/*
 * Totem extras on top of the zmk-studio-log module, which owns the Studio log
 * stream ('L' lines, 'C' on/off, history replay) and, on this image, restart
 * diagnostics.
 *
 * What stays Totem-specific:
 *   - 'K' battery snapshots for the configurator header (was 'B', which can
 *     start a protobuf message and is refused by zmk_rpc_send_raw_frame()).
 *     A 'G' '1' request from the host asks for one right away.
 *   - 'D' "Load retained log": dumps the Totem host event journal, then turns
 *     the live log on. Not registered when the module's own black box is
 *     enabled, since that claims 'D' too.
 *   - The 10 s totem_log / totem_ble rssi heartbeat while the log is on.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/studio/rpc.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

#include <zmk_studio_log/studio_log.h>

#include <totem_host_event_log.h>
#include <totem_studio_log.h>

#define BATTERY_PREFIX ((uint8_t)'K')
#define BATTERY_REQUEST_PREFIX ((uint8_t)'G')
#define DUMP_PREFIX ((uint8_t)'D')
#define HEARTBEAT_PERIOD K_SECONDS(10)

static bool usb_hid_ready(void) {
    if (zmk_endpoint_get_selected().transport != ZMK_TRANSPORT_USB) {
        return false;
    }
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_hid_ready();
#else
    return false;
#endif
}

int totem_studio_send_battery(const char *line) {
    if (!line || !line[0] || !usb_hid_ready()) {
        return -EAGAIN;
    }
    return zmk_studio_log_send_frame(BATTERY_PREFIX, line, strlen(line));
}

int totem_studio_send_diag_line(const char *line) {
    if (!line || !line[0]) {
        return -EMSGSIZE;
    }
    if (!usb_hid_ready()) {
        return -EAGAIN;
    }
    return zmk_studio_log_send_line(line);
}

#if IS_ENABLED(CONFIG_TOTEM_BATTERY_LOG) || IS_ENABLED(CONFIG_ZMK_BATTERY_LOG)
static void battery_request_cb(const uint8_t *payload, size_t len) {
    if (len >= 2 && payload[1] == (uint8_t)'1') {
        totem_battery_send_now();
    }
}

ZMK_RPC_RAW_FRAME_HANDLER(totem_battery_request, BATTERY_REQUEST_PREFIX, battery_request_cb);
#endif

#if IS_ENABLED(CONFIG_TOTEM_HOST_EVENT_LOG) && !IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_BLACK_BOX)
/* Works while host BLE is suppressed and does not rely on a key combo reaching ZMK. */
static void dump_frame_cb(const uint8_t *payload, size_t len) {
    if (len < 2 || payload[1] != (uint8_t)'1') {
        return;
    }
    totem_host_event_log_request_dump();
    zmk_studio_log_set_streaming(true);
}

ZMK_RPC_RAW_FRAME_HANDLER(totem_diag_dump, DUMP_PREFIX, dump_frame_cb);
#endif

/* ---- heartbeat ---- */

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
    char line[CONFIG_ZMK_STUDIO_LOG_LINE_MAX];
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
            wrote =
                snprintk(acc->line + acc->n, sizeof(acc->line) - acc->n, " split=%d", (int)rssi);
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

static void heartbeat_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_work_cb);

/* Runs every 10 s and prints only while the configurator has the log on. */
static void heartbeat_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    struct rssi_acc acc = {0};
    const char *hid = "none";
    enum zmk_transport t;

    (void)k_work_schedule(&heartbeat_work, HEARTBEAT_PERIOD);
    if (!zmk_studio_log_is_streaming()) {
        return;
    }

    t = zmk_endpoint_get_selected().transport;
    if (t == ZMK_TRANSPORT_USB) {
        hid = "usb";
    } else if (t == ZMK_TRANSPORT_BLE) {
        hid = "ble";
    }
    bt_conn_foreach(BT_CONN_TYPE_LE, rssi_one, &acc);
    printk("totem_log t=%us usb=1 hid=%s adv=%s hosts=%u split=%u\n", k_uptime_get_32() / 1000, hid,
           zmk_ble_totem_adv_state(), acc.hosts, acc.split);
    if (acc.peers == 0) {
        printk("totem_ble rssi none\n");
    } else {
        printk("totem_ble rssi%s\n", acc.line);
    }
}

static int totem_studio_extras_init(void) {
    (void)k_work_schedule(&heartbeat_work, HEARTBEAT_PERIOD);
    return 0;
}

SYS_INIT(totem_studio_extras_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
