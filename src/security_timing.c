/* Timing only. Preserve buffers, ownership, return values and security policy. */
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci_types.h>
#include <totem_host_event_log.h>
#include <totem_handoff_diagnostics.h>
#include "conn_internal.h"
#include "hci_core.h"
#include "l2cap_internal.h"
#include "smp.h"

static void encryption_observed(sys_slist_t *list, const struct net_buf *buf, uint8_t stage) {
    if (list != &bt_dev.rx_queue || !buf || bt_buf_get_type(buf) != BT_BUF_EVT ||
        buf->len < sizeof(struct bt_hci_evt_hdr)) {
        return;
    }
    const struct bt_hci_evt_hdr *hdr = (const void *)buf->data;
    if (buf->len < sizeof(*hdr) + hdr->len) {
        return;
    }
    const uint8_t *payload = buf->data + sizeof(*hdr);
    if (hdr->evt == BT_HCI_EVT_LE_META_EVENT &&
        hdr->len >= sizeof(struct bt_hci_evt_le_meta_event) +
                    sizeof(struct bt_hci_evt_le_ltk_request) &&
        payload[0] == BT_HCI_EVT_LE_LTK_REQUEST) {
        uint16_t handle = sys_get_le16(payload + sizeof(struct bt_hci_evt_le_meta_event));
        totem_diagnostic_timing_capture(k_uptime_get_32(), TOTEM_HEVT_SECURITY_EXCHANGE,
            (int8_t)handle, (int8_t)(handle >> 8), 6 + stage, 0, 0);
        return;
    }
    bool refresh = hdr->evt == BT_HCI_EVT_ENCRYPT_KEY_REFRESH_COMPLETE;
    if (hdr->evt != BT_HCI_EVT_ENCRYPT_CHANGE && !refresh) {
        return;
    }
    size_t required = refresh ? sizeof(struct bt_hci_evt_encrypt_key_refresh_complete) :
                                sizeof(struct bt_hci_evt_encrypt_change);
    if (hdr->len < required || buf->len < sizeof(*hdr) + hdr->len) {
        return;
    }
    uint16_t handle = sys_get_le16(payload + offsetof(struct bt_hci_evt_encrypt_change, handle));
    totem_diagnostic_timing_capture(k_uptime_get_32(), TOTEM_HEVT_ENCRYPT_EVENT,
        (int8_t)handle, (int8_t)(handle >> 8), stage + (refresh ? 2 : 0),
        payload[0], refresh ? UINT8_MAX : payload[offsetof(struct bt_hci_evt_encrypt_change, encrypt)]);
}

int __real_bt_l2cap_send_pdu(struct bt_l2cap_le_chan *chan, struct net_buf *buf,
                            bt_conn_tx_cb_t cb, void *user_data);
int __wrap_bt_l2cap_send_pdu(struct bt_l2cap_le_chan *chan, struct net_buf *buf,
                            bt_conn_tx_cb_t cb, void *user_data) {
    struct bt_conn *conn = chan->chan.conn;
    bool trace = conn && chan->tx.cid == BT_L2CAP_CID_SMP && buf->len == 2 &&
                 buf->data[0] == BT_SMP_CMD_SECURITY_REQUEST &&
                 conn->type == BT_CONN_TYPE_LE && conn->role == BT_CONN_ROLE_PERIPHERAL;
    /* The real send may consume the buffer. Retain only its connection handle. */
    uint16_t handle = trace ? conn->handle : 0;
    if (trace) {
        totem_diagnostic_timing_capture(k_uptime_get_32(), TOTEM_HEVT_SECURITY_EXCHANGE,
            (int8_t)handle, (int8_t)(handle >> 8), 0, 0, 0);
    }
    int err = __real_bt_l2cap_send_pdu(chan, buf, cb, user_data);
    if (trace) {
        totem_diagnostic_timing_capture(k_uptime_get_32(), TOTEM_HEVT_SECURITY_EXCHANGE,
            (int8_t)handle, (int8_t)(handle >> 8), 1, err < 0 ? 1 : err > 0 ? 2 : 0,
            (uint8_t)MIN(err < 0 ? -err : err, UINT8_MAX));
    }
    return err;
}

void __real_net_buf_slist_put(sys_slist_t *list, struct net_buf *buf);
void __wrap_net_buf_slist_put(sys_slist_t *list, struct net_buf *buf) {
    encryption_observed(list, buf, 0);
    __real_net_buf_slist_put(list, buf);
}

struct net_buf *__real_net_buf_slist_get(sys_slist_t *list);
struct net_buf *__wrap_net_buf_slist_get(sys_slist_t *list) {
    struct net_buf *buf = __real_net_buf_slist_get(list);
    encryption_observed(list, buf, 1);
    return buf;
}

static void security_observed(struct bt_conn *conn, uint8_t stage, uint8_t hci_status) {
    if (conn->type == BT_CONN_TYPE_LE && conn->role == BT_CONN_ROLE_PERIPHERAL) {
        totem_diagnostic_timing_capture(k_uptime_get_32(), TOTEM_HEVT_SECURITY_TIMING,
            (int8_t)conn->handle, (int8_t)(conn->handle >> 8), stage, hci_status, conn->sec_level);
    }
}

void __real_bt_conn_security_changed(struct bt_conn *conn, uint8_t hci_err,
                                      enum bt_security_err err);
void __wrap_bt_conn_security_changed(struct bt_conn *conn, uint8_t hci_err,
                                      enum bt_security_err err) {
    security_observed(conn, 0, hci_err);
    __real_bt_conn_security_changed(conn, hci_err, err);
    security_observed(conn, 2, hci_err);
}

void __real_bt_l2cap_security_changed(struct bt_conn *conn, uint8_t hci_status);
void __wrap_bt_l2cap_security_changed(struct bt_conn *conn, uint8_t hci_status) {
    __real_bt_l2cap_security_changed(conn, hci_status);
    security_observed(conn, 1, hci_status);
}
