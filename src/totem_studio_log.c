/*
 * Mux printk onto the Studio RPC UART as framed 'L' payloads so one CDC
 * carries both keymap RPC and the host/BLE log.
 *
 * USB only, and only while the host has turned the stream on ('C' '1').
 * Bluetooth HID never gets log frames. Default is off so USB typing stays
 * reliable until the configurator enables the panel.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/net_buf.h>

#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/ble.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

#include <totem_studio_log.h>
#include <totem_usb_quiet.h>

int zmk_rpc_tx_raw_payload(const uint8_t *payload, size_t len);
void zmk_studio_uart_rearm(void);
bool zmk_studio_uart_host_open(void);

#if IS_ENABLED(CONFIG_TOTEM_STUDIO_CONSOLE) || IS_ENABLED(CONFIG_ZMK_STUDIO_CONSOLE)

#define LINE_MAX 120
#define RING_LEN 32
#define LOG_PREFIX ((uint8_t)'L')
#define BAT_PREFIX ((uint8_t)'B')

static char assembling[LINE_MAX];
static uint8_t assembling_len;

static char ring[RING_LEN][LINE_MAX];
static uint8_t ring_len;
static uint8_t ring_head; /* next write */
static bool dtr_was_up;
static bool usb_log_on;
static uint8_t replay_off;

K_MSGQ_DEFINE(totem_studio_log_q, LINE_MAX, RING_LEN, 4);

static void totem_studio_log_heartbeat(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(totem_studio_log_hb, totem_studio_log_heartbeat);

static void totem_studio_log_flush(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(totem_studio_log_work, totem_studio_log_flush);

static void ring_store(const char *line) {
    strncpy(ring[ring_head], line, LINE_MAX - 1);
    ring[ring_head][LINE_MAX - 1] = '\0';
    ring_head = (uint8_t)((ring_head + 1) % RING_LEN);
    if (ring_len < RING_LEN) {
        ring_len++;
    }
}

static int send_line(const char *line) {
    uint8_t payload[LINE_MAX + 1];
    size_t n = strnlen(line, LINE_MAX - 1);

    payload[0] = LOG_PREFIX;
    memcpy(&payload[1], line, n);
    return zmk_rpc_tx_raw_payload(payload, n + 1);
}

static bool cdc_host_listening(void) {
#if DT_HAS_CHOSEN(zmk_studio_rpc_uart)
    const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(zmk_studio_rpc_uart));
    uint32_t dtr = 0;

    if (!device_is_ready(uart)) {
        return false;
    }
    if (uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr) != 0) {
        return true;
    }
    return dtr != 0;
#else
    return true;
#endif
}

static bool studio_cdc_ready(void) {
    if (zmk_endpoint_get_selected().transport != ZMK_TRANSPORT_USB) {
        return false;
    }
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (!zmk_usb_is_hid_ready()) {
        return false;
    }
#endif
    return cdc_host_listening() || zmk_studio_uart_host_open();
}

static bool studio_tx_ready(void) { return usb_log_on && studio_cdc_ready(); }

int totem_studio_send_battery(const char *line) {
    uint8_t payload[LINE_MAX + 1];
    size_t n;

    if (!line || !line[0] || !studio_cdc_ready()) {
        return -EAGAIN;
    }
    n = strnlen(line, LINE_MAX - 1);
    payload[0] = BAT_PREFIX;
    memcpy(&payload[1], line, n);
    return zmk_rpc_tx_raw_payload(payload, n + 1);
}

void zmk_studio_control_payload(const uint8_t *payload, size_t len) {
    if (!payload || len < 2 || payload[0] != (uint8_t)'C') {
        return;
    }
    bool on = payload[1] == (uint8_t)'1';

    if (!on) {
        usb_log_on = true;
        (void)send_line("totem_log usb=0");
        usb_log_on = false;
        dtr_was_up = false;
        (void)k_work_cancel_delayable(&totem_studio_log_hb);
        return;
    }
    usb_log_on = true;
    dtr_was_up = false;
    replay_off = 0;
    (void)k_work_schedule(&totem_studio_log_work, K_NO_WAIT);
    (void)k_work_schedule(&totem_studio_log_hb, K_SECONDS(10));
}

static int8_t read_conn_rssi(struct bt_conn *conn) {
    uint16_t handle;
    struct net_buf *buf, *rsp = NULL;
    struct bt_hci_cp_read_rssi *cp;
    struct bt_hci_rp_read_rssi *rp;
    int8_t rssi = 127;

    if (bt_hci_get_conn_handle(conn, &handle) != 0) {
        return rssi;
    }
    buf = bt_hci_cmd_create(BT_HCI_OP_READ_RSSI, sizeof(*cp));
    if (!buf) {
        return rssi;
    }
    cp = net_buf_add(buf, sizeof(*cp));
    cp->handle = sys_cpu_to_le16(handle);
    if (bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp) != 0) {
        return rssi;
    }
    rp = (void *)rsp->data;
    rssi = rp->rssi;
    net_buf_unref(rsp);
    return rssi;
}

struct rssi_acc {
    char line[LINE_MAX];
    size_t n;
    uint8_t peers;
    uint8_t hosts;
    uint8_t split;
};

static void rssi_one(struct bt_conn *conn, void *user) {
    struct rssi_acc *acc = user;
    struct bt_conn_info info;
    int idx;
    int8_t rssi;
    int wrote;

    if (bt_conn_get_info(conn, &info) != 0 || info.state != BT_CONN_STATE_CONNECTED) {
        return;
    }
    rssi = read_conn_rssi(conn);
    idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (idx < 0 && info.role == BT_CONN_ROLE_CENTRAL) {
        acc->split++;
    } else if (idx >= 0) {
        acc->hosts++;
    }
    if (acc->n >= sizeof(acc->line) - 16) {
        return;
    }
    if (idx < 0 && info.role == BT_CONN_ROLE_CENTRAL) {
        if (rssi == 127) {
            wrote = snprintk(acc->line + acc->n, sizeof(acc->line) - acc->n, " split=--");
        } else {
            wrote = snprintk(acc->line + acc->n, sizeof(acc->line) - acc->n, " split=%d", (int)rssi);
        }
    } else if (idx >= 0) {
        if (rssi == 127) {
            wrote = snprintk(acc->line + acc->n, sizeof(acc->line) - acc->n, " idx%d=--", idx);
        } else {
            wrote = snprintk(acc->line + acc->n, sizeof(acc->line) - acc->n, " idx%d=%d", idx,
                             (int)rssi);
        }
    } else {
        wrote = snprintk(acc->line + acc->n, sizeof(acc->line) - acc->n, " other=%d", (int)rssi);
    }
    if (wrote > 0) {
        acc->n += (size_t)wrote;
        acc->peers++;
    }
}

static void totem_studio_log_heartbeat(struct k_work *work) {
    ARG_UNUSED(work);
    struct rssi_acc acc = {.n = 0, .peers = 0};
    const char *hid = "none";
    const char *adv = "off";
    enum zmk_transport t;

    if (!usb_log_on) {
        return;
    }
    t = zmk_endpoint_get_selected().transport;
    if (t == ZMK_TRANSPORT_USB) {
        hid = "usb";
    } else if (t == ZMK_TRANSPORT_BLE) {
        hid = "ble";
    }
    adv = zmk_ble_totem_adv_state();
    bt_conn_foreach(BT_CONN_TYPE_LE, rssi_one, &acc);
    printk("totem_log t=%us usb=1 hid=%s adv=%s hosts=%u split=%u\n", k_uptime_get_32() / 1000, hid,
           adv, acc.hosts, acc.split);
    if (acc.peers == 0) {
        printk("totem_ble rssi none\n");
    } else {
        printk("totem_ble rssi%s\n", acc.line);
    }
    (void)k_work_schedule(&totem_studio_log_hb, K_SECONDS(10));
}

static void drain_msgq(void) {
    char line[LINE_MAX];

    while (k_msgq_get(&totem_studio_log_q, line, K_NO_WAIT) == 0) {
    }
}

static void replay_ring(void) {
    uint8_t start = (uint8_t)((ring_head + RING_LEN - ring_len) % RING_LEN);
    uint8_t n = 0;

    while (replay_off < ring_len && n < 4) {
        (void)send_line(ring[(start + replay_off) % RING_LEN]);
        replay_off++;
        n++;
    }
    if (replay_off < ring_len) {
        (void)k_work_schedule(&totem_studio_log_work, K_MSEC(20));
    }
}

static void totem_studio_log_flush(struct k_work *work) {
    ARG_UNUSED(work);
    char line[LINE_MAX];

    if (!usb_log_on) {
        dtr_was_up = false;
        drain_msgq();
        return;
    }
    if (!studio_tx_ready()) {
        dtr_was_up = false;
        drain_msgq();
        (void)k_work_schedule(&totem_studio_log_work, K_MSEC(500));
        return;
    }
    if (!dtr_was_up) {
        dtr_was_up = true;
        replay_off = 0;
        zmk_studio_uart_rearm();
        drain_msgq();
    }
    if (replay_off < ring_len) {
        replay_ring();
        return;
    }
    uint8_t n = 0;
    while (n < 4 && k_msgq_get(&totem_studio_log_q, line, K_NO_WAIT) == 0) {
        (void)send_line(line);
        n++;
    }
    if (k_msgq_num_used_get(&totem_studio_log_q) > 0) {
        (void)k_work_schedule(&totem_studio_log_work, K_MSEC(20));
    }
}

static void commit_line(void) {
    assembling[assembling_len] = '\0';
    if (assembling_len == 0) {
        return;
    }
    ring_store(assembling);
    assembling_len = 0;
    if (!usb_log_on) {
        return;
    }
    (void)k_msgq_put(&totem_studio_log_q, assembling, K_NO_WAIT);
    (void)k_work_schedule(&totem_studio_log_work, K_NO_WAIT);
}

static int totem_printk_char(int c) {
    if (c == '\r') {
        return c;
    }
    if (c == '\n') {
        commit_line();
        return c;
    }
    if (assembling_len < LINE_MAX - 1) {
        assembling[assembling_len++] = (char)c;
    } else {
        commit_line();
        assembling[assembling_len++] = (char)c;
    }
    return c;
}

static int totem_studio_log_endpoint(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *ev = as_zmk_endpoint_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (zmk_endpoint_get_selected().transport != ZMK_TRANSPORT_USB) {
        usb_log_on = false;
        dtr_was_up = false;
        (void)k_work_cancel_delayable(&totem_studio_log_hb);
    }
    (void)k_work_schedule(&totem_studio_log_work, K_NO_WAIT);
    return ZMK_EV_EVENT_BUBBLE;
}

static int totem_studio_log_usb(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (!zmk_usb_is_powered()) {
        usb_log_on = false;
        dtr_was_up = false;
        (void)k_work_cancel_delayable(&totem_studio_log_hb);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif
    zmk_studio_uart_rearm();
    (void)k_work_schedule(&totem_studio_log_work, K_MSEC(50));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_studio_log, totem_studio_log_endpoint);
ZMK_SUBSCRIPTION(totem_studio_log, zmk_endpoint_changed);
ZMK_LISTENER(totem_studio_log_usb, totem_studio_log_usb);
ZMK_SUBSCRIPTION(totem_studio_log_usb, zmk_usb_conn_state_changed);

static int totem_studio_log_init(void) {
    __printk_hook_install(totem_printk_char);
    (void)k_work_schedule(&totem_studio_log_work, K_MSEC(200));
    return 0;
}

SYS_INIT(totem_studio_log_init, APPLICATION, 0);

#endif
