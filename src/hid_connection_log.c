#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/atomic.h>
#include <zmk/ble.h>
#include <zmk/hog.h>
#include <totem_host_event_log.h>
#include <totem_hid_report_diagnostics.h>

void zmk_ble_advertising_observed(uint8_t stage, int err) {
    int active = zmk_ble_active_profile_index();
    totem_host_event_log_record(TOTEM_HEVT_ADV_RESULT, active, active, stage,
                                err < 0 ? 1 : err > 0 ? 2 : 0,
                                err < 0 ? -err : err);
}

static ATOMIC_DEFINE(first_attempts, CONFIG_BT_MAX_CONN);
static ATOMIC_DEFINE(first_subscriptions, CONFIG_BT_MAX_CONN);
static ATOMIC_DEFINE(first_errors, CONFIG_BT_MAX_CONN);
static ATOMIC_DEFINE(tracked_sends, CONFIG_BT_MAX_CONN);
static ATOMIC_DEFINE(first_security_requests, CONFIG_BT_MAX_CONN);

int __real_bt_conn_set_security(struct bt_conn *conn, bt_security_t sec);

int __wrap_bt_conn_set_security(struct bt_conn *conn, bt_security_t sec) {
    bool trace = !atomic_test_and_set_bit(first_security_requests, bt_conn_index(conn));
    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    int active = zmk_ble_active_profile_index();
    if (trace) {
        totem_diag_log_record(TOTEM_HEVT_SEC_REQUEST, idx, active, sec,
                              bt_conn_get_security(conn));
    }
    int err = __real_bt_conn_set_security(conn, sec);
    if (trace) {
        totem_host_event_log_record(TOTEM_HEVT_SEC_REQUEST_RESULT, idx, active, sec,
                                    err < 0 ? 1 : err > 0 ? 2 : 0,
                                    err < 0 ? -err : err);
    }
    return err;
}

void zmk_hog_subscription_observed(struct bt_conn *conn) {
    if (!atomic_test_and_set_bit(first_subscriptions, bt_conn_index(conn))) {
        totem_diag_log_record(TOTEM_HEVT_HID_SUBSCRIBED,
                             zmk_ble_profile_index(bt_conn_get_dst(conn)),
                             zmk_ble_active_profile_index(), 0, 0);
    }
}

bool zmk_hog_keyboard_report_attempted(struct bt_conn *conn, bool subscribed) {
    totem_hid_report_observed(conn, subscribed);
    if (!atomic_test_and_set_bit(first_attempts, bt_conn_index(conn))) {
        totem_diag_log_record(TOTEM_HEVT_HID_FIRST_ATTEMPT,
                             zmk_ble_profile_index(bt_conn_get_dst(conn)),
                             zmk_ble_active_profile_index(), 0, subscribed);
    }
    /* Only one completion callback in flight; stop requesting it after success. */
    return !atomic_test_and_set_bit(tracked_sends, bt_conn_index(conn));
}

void zmk_hog_keyboard_report_result(struct bt_conn *conn, int err) {
    unsigned int slot = bt_conn_index(conn);
    if (err) {
        atomic_clear_bit(tracked_sends, slot);
        if (atomic_test_and_set_bit(first_errors, slot)) {
            return;
        }
    }
    totem_diag_log_record(TOTEM_HEVT_HID_TX_RESULT,
                         zmk_ble_profile_index(bt_conn_get_dst(conn)),
                         zmk_ble_active_profile_index(), err ? -err : 0, err != 0);
}

static void hid_log_connected(struct bt_conn *conn, uint8_t err) {
    if (!err) {
        uint16_t handle;
        if (!bt_hci_get_conn_handle(conn, &handle)) {
            totem_host_event_log_record_timing(k_uptime_get_32(), TOTEM_HEVT_CONN_HANDLE,
                                              zmk_ble_profile_index(bt_conn_get_dst(conn)),
                                              zmk_ble_active_profile_index(), handle & 0xff,
                                              0, handle >> 8);
        }
        atomic_clear_bit(first_attempts, bt_conn_index(conn));
        atomic_clear_bit(first_subscriptions, bt_conn_index(conn));
        atomic_clear_bit(first_errors, bt_conn_index(conn));
        atomic_clear_bit(tracked_sends, bt_conn_index(conn));
    }
}

static void security_log_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    atomic_clear_bit(first_security_requests, bt_conn_index(conn));
}

BT_CONN_CB_DEFINE(hid_connection_log_cb) = {
    .connected = hid_log_connected, .disconnected = security_log_disconnected};
