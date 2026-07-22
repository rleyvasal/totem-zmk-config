/*
 * Lazy inactive host: multi-link dual-host without exclusive-host disconnect.
 *
 * Goal: keep bonded computers connected so BT_SEL is near-instant after both
 * have linked once, while requesting *lazy* LE connection parameters on any
 * host that is not the active profile (lower radio duty → less battery, less
 * chance of disturbing a sleeping machine). Active profile gets snappy params.
 *
 * HID still only goes to the active profile (ZMK hog). Central-only.
 *
 * Does not disconnect background hosts. Pair with exclusive-host OFF.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IS_ENABLED(CONFIG_TOTEM_LAZY_INACTIVE_HOST)

LOG_MODULE_REGISTER(lazy_inactive_host, CONFIG_ZMK_LOG_LEVEL);

/* Units: interval = 1.25 ms, latency = events, timeout = 10 ms. */
#define ACTIVE_INT_MIN CONFIG_BT_PERIPHERAL_PREF_MIN_INT
#define ACTIVE_INT_MAX CONFIG_BT_PERIPHERAL_PREF_MAX_INT
#define ACTIVE_LATENCY 0
#define ACTIVE_TIMEOUT 400 /* 4 s */

#define LAZY_INT_MIN CONFIG_TOTEM_LAZY_HOST_MIN_INT
#define LAZY_INT_MAX CONFIG_TOTEM_LAZY_HOST_MAX_INT
#define LAZY_LATENCY CONFIG_TOTEM_LAZY_HOST_LATENCY
#define LAZY_TIMEOUT CONFIG_TOTEM_LAZY_HOST_TIMEOUT

static void apply_params(struct bt_conn *conn, bool snappy) {
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }
    if (info.state != BT_CONN_STATE_CONNECTED) {
        return;
    }
    if (bt_conn_get_security(conn) < BT_SECURITY_L2) {
        return; /* wait for encryption */
    }

    struct bt_le_conn_param param;
    if (snappy) {
        param = *BT_LE_CONN_PARAM(ACTIVE_INT_MIN, ACTIVE_INT_MAX, ACTIVE_LATENCY, ACTIVE_TIMEOUT);
    } else {
        param = *BT_LE_CONN_PARAM(LAZY_INT_MIN, LAZY_INT_MAX, LAZY_LATENCY, LAZY_TIMEOUT);
    }

    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    int err = bt_conn_le_param_update(conn, &param);
    if (err && err != -EALREADY) {
        LOG_WRN("totem_ble lazy_host param_update idx=%d snappy=%d err=%d", idx, (int)snappy, err);
    } else {
        LOG_INF("totem_ble lazy_host idx=%d mode=%s int=%u-%u lat=%u", idx,
                snappy ? "active" : "lazy", param.interval_min, param.interval_max, param.latency);
    }
}

static void update_one_host(struct bt_conn *conn, void *data) {
    ARG_UNUSED(data);
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }
    if (info.state != BT_CONN_STATE_CONNECTED) {
        return;
    }

    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    /* Unresolved RPA: leave default params until identity resolves. */
    if (idx < 0) {
        return;
    }

    bool snappy = (idx == zmk_ble_active_profile_index());
    apply_params(conn, snappy);
}

static void lazy_host_refresh(void) {
    bt_conn_foreach(BT_CONN_TYPE_LE, update_one_host, NULL);
}

static void lazy_host_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    lazy_host_refresh();
}

static K_WORK_DELAYABLE_DEFINE(lazy_host_work, lazy_host_work_handler);

static void lazy_host_schedule(void) {
    /* Defer slightly so exclusive/profile switch settles and encryption can finish. */
    k_work_reschedule(&lazy_host_work, K_MSEC(100));
}

static int lazy_host_profile_changed(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    LOG_INF("totem_ble lazy_host profile_changed active=%d", zmk_ble_active_profile_index());
    lazy_host_schedule();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_lazy_inactive_host, lazy_host_profile_changed);
ZMK_SUBSCRIPTION(totem_lazy_inactive_host, zmk_ble_active_profile_changed);

static void lazy_host_connected(struct bt_conn *conn, uint8_t err) {
    ARG_UNUSED(conn);
    if (err) {
        return;
    }
    lazy_host_schedule();
}

static void lazy_host_security_changed(struct bt_conn *conn, bt_security_t level,
                                       enum bt_security_err err) {
    ARG_UNUSED(conn);
    ARG_UNUSED(err);
    if (level >= BT_SECURITY_L2) {
        lazy_host_schedule();
    }
}

static void lazy_host_identity_resolved(struct bt_conn *conn, const bt_addr_le_t *rpa,
                                        const bt_addr_le_t *identity) {
    ARG_UNUSED(conn);
    ARG_UNUSED(rpa);
    ARG_UNUSED(identity);
    lazy_host_schedule();
}

BT_CONN_CB_DEFINE(lazy_inactive_host_cb) = {
    .connected = lazy_host_connected,
    .security_changed = lazy_host_security_changed,
    .identity_resolved = lazy_host_identity_resolved,
};

#endif /* CENTRAL && TOTEM_LAZY_INACTIVE_HOST */
