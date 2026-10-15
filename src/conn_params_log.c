/*
 * Print every connection's parameters when they change, for the split link and
 * the host link alike. The connection interval and peripheral latency decide most
 * of the radio power, and neither was visible on the production image: the split
 * link's idle interval (CONFIG_ZMK_SPLIT_BLE_IDLE_INT) is only checkable here, and
 * the host decides the host link's values, whatever the keyboard requests.
 *
 * role=0: this half runs the link (the split link). role=1: the host runs it.
 * interval is in 1.25 ms units, timeout in 10 ms units. Event-driven only.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(conn_params_log, CONFIG_ZMK_LOG_LEVEL);
#include <totem_ble_print.h>

static void print_params(struct bt_conn *conn, const char *why) {
    struct bt_conn_info info;
    char addr[BT_ADDR_LE_STR_LEN];

    if (bt_conn_get_info(conn, &info) != 0 || info.type != BT_CONN_TYPE_LE) {
        return;
    }
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    TOTEM_BLE_INF("totem_ble conn_params %s addr=%s role=%u interval=%u latency=%u timeout=%u", why,
                  addr, info.role, info.le.interval, info.le.latency, info.le.timeout);
}

static void conn_params_connected(struct bt_conn *conn, uint8_t err) {
    if (!err) {
        print_params(conn, "connected");
    }
}

static void conn_params_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                                uint16_t timeout) {
    ARG_UNUSED(interval);
    ARG_UNUSED(latency);
    ARG_UNUSED(timeout);
    print_params(conn, "updated");
}

BT_CONN_CB_DEFINE(conn_params_log_cb) = {
    .connected = conn_params_connected,
    .le_param_updated = conn_params_updated,
};
