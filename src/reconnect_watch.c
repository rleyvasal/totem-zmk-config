/*
 * Reconnect watch: after a profile switch (or soft reselect), ensure the
 * selected host actually comes up. Open advertising + exclusive-host eviction
 * are necessary but not sufficient when a background host races the link:
 * advertising can fail while another peer is connected, or the active host can
 * stall mid-reconnect.
 *
 * Ladder (one step per timeout, never auto-clears bonds):
 *   1) Restart open advertising + re-arm boost (if the patch provides it).
 *   2) Force-evict any non-active host still connected, then advertise again.
 *   3) If a peripheral host is connected but ZMK still does not see the active
 *      profile as connected (zombie / RPA lag), disconnect that host and
 *      re-advertise so a clean reconnect can complete.
 *
 * Central-only. No address filters. Does not pause advertising "for thrash"
 * (that regressed dual-host switching).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

LOG_MODULE_REGISTER(reconnect_watch, CONFIG_ZMK_LOG_LEVEL);

#define RECONNECT_WATCH_STEP_SEC CONFIG_TOTEM_RECONNECT_WATCH_SEC

enum reconnect_step {
    RECONNECT_STEP_NONE = 0,
    RECONNECT_STEP_READV,
    RECONNECT_STEP_EVICT,
    RECONNECT_STEP_ZOMBIE,
};

static enum reconnect_step next_step;
static struct k_work_delayable reconnect_watch_work;

struct host_conn_scan {
    struct bt_conn *active_match;
    struct bt_conn *any_host;
    int host_count;
};

static void scan_host_conns(struct bt_conn *conn, void *data) {
    struct host_conn_scan *scan = data;
    struct bt_conn_info info;

    if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }
    if (info.state != BT_CONN_STATE_CONNECTED) {
        return;
    }

    scan->host_count++;
    if (scan->any_host == NULL) {
        scan->any_host = bt_conn_ref(conn);
    }

    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (idx == zmk_ble_active_profile_index() && scan->active_match == NULL) {
        scan->active_match = bt_conn_ref(conn);
    }
}

static void host_conn_scan_release(struct host_conn_scan *scan) {
    if (scan->active_match) {
        bt_conn_unref(scan->active_match);
        scan->active_match = NULL;
    }
    if (scan->any_host) {
        bt_conn_unref(scan->any_host);
        scan->any_host = NULL;
    }
}

static void drop_non_active_hosts(struct bt_conn *conn, void *data) {
    ARG_UNUSED(data);
    struct bt_conn_info info;

    if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (idx < 0 || idx == zmk_ble_active_profile_index()) {
        return;
    }
    if (bt_conn_get_security(conn) < BT_SECURITY_L2) {
        return;
    }

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_WRN("Reconnect watch: evicting background host %s (profile %d)", addr, idx);
    int err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    if (err) {
        LOG_WRN("Reconnect watch: disconnect failed (err %d)", err);
    }
}

static void reconnect_watch_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (zmk_ble_active_profile_is_open()) {
        LOG_DBG("Reconnect watch: active profile open (pairing); stop");
        next_step = RECONNECT_STEP_NONE;
        return;
    }

    if (zmk_ble_active_profile_is_connected()) {
        LOG_INF("Reconnect watch: active profile connected; done");
        next_step = RECONNECT_STEP_NONE;
        return;
    }

    struct host_conn_scan scan = {0};
    bt_conn_foreach(BT_CONN_TYPE_LE, scan_host_conns, &scan);

    LOG_WRN("Reconnect watch: active profile %d still down (hosts=%d, step=%d)",
            zmk_ble_active_profile_index(), scan.host_count, next_step);

    switch (next_step) {
    case RECONNECT_STEP_READV:
        /* Re-select the active profile: with TOTEM_RESELECT_RECONNECT this
         * clears throttle, re-arms boost, and restarts open advertising even
         * when nothing is connected yet. Without reselect it is a no-op and
         * we still advance the ladder. */
        LOG_WRN("Reconnect watch step1: kick advertising via prof_select(%d)",
                zmk_ble_active_profile_index());
        (void)zmk_ble_prof_select((uint8_t)zmk_ble_active_profile_index());
        next_step = RECONNECT_STEP_EVICT;
        k_work_schedule(&reconnect_watch_work, K_SECONDS(RECONNECT_WATCH_STEP_SEC));
        break;

    case RECONNECT_STEP_EVICT:
        LOG_WRN("Reconnect watch step2: force-evict non-active hosts");
        bt_conn_foreach(BT_CONN_TYPE_LE, drop_non_active_hosts, NULL);
        next_step = RECONNECT_STEP_ZOMBIE;
        k_work_schedule(&reconnect_watch_work, K_SECONDS(RECONNECT_WATCH_STEP_SEC));
        break;

    case RECONNECT_STEP_ZOMBIE:
        /* Connected peripheral that does not map to the active profile index
         * via ZMK's address table, or maps but active_profile_is_connected is
         * still false — drop it so advertising can serve a clean reconnect. */
        if (scan.any_host != NULL && scan.active_match == NULL) {
            char addr[BT_ADDR_LE_STR_LEN];
            bt_addr_le_to_str(bt_conn_get_dst(scan.any_host), addr, sizeof(addr));
            LOG_WRN("Reconnect watch step3: dropping unmapped host %s for clean reconnect",
                    addr);
            (void)bt_conn_disconnect(scan.any_host, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        } else if (scan.active_match != NULL && !zmk_ble_active_profile_is_connected()) {
            char addr[BT_ADDR_LE_STR_LEN];
            bt_addr_le_to_str(bt_conn_get_dst(scan.active_match), addr, sizeof(addr));
            LOG_WRN("Reconnect watch step3: active maps %s but ZMK not connected; soft drop",
                    addr);
            (void)bt_conn_disconnect(scan.active_match, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        } else {
            LOG_WRN("Reconnect watch step3: no zombie conn; host must scan (still advertising)");
        }
        next_step = RECONNECT_STEP_NONE;
        break;

    default:
        next_step = RECONNECT_STEP_NONE;
        break;
    }

    host_conn_scan_release(&scan);
}

static void reconnect_watch_arm(void) {
    if (zmk_ble_active_profile_is_open()) {
        k_work_cancel_delayable(&reconnect_watch_work);
        next_step = RECONNECT_STEP_NONE;
        return;
    }
    if (zmk_ble_active_profile_is_connected()) {
        k_work_cancel_delayable(&reconnect_watch_work);
        next_step = RECONNECT_STEP_NONE;
        return;
    }

    next_step = RECONNECT_STEP_READV;
    LOG_INF("Reconnect watch: armed for profile %d (%d s steps)",
            zmk_ble_active_profile_index(), RECONNECT_WATCH_STEP_SEC);
    k_work_reschedule(&reconnect_watch_work, K_SECONDS(RECONNECT_WATCH_STEP_SEC));
}

static int reconnect_watch_profile_changed(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    reconnect_watch_arm();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_reconnect_watch, reconnect_watch_profile_changed);
ZMK_SUBSCRIPTION(totem_reconnect_watch, zmk_ble_active_profile_changed);

static void reconnect_watch_connected(struct bt_conn *conn, uint8_t err) {
    ARG_UNUSED(conn);
    if (err) {
        return;
    }
    if (zmk_ble_active_profile_is_connected()) {
        k_work_cancel_delayable(&reconnect_watch_work);
        next_step = RECONNECT_STEP_NONE;
        LOG_INF("Reconnect watch: active host connected; cancelled");
    }
}

static void reconnect_watch_security_changed(struct bt_conn *conn, bt_security_t level,
                                             enum bt_security_err err) {
    ARG_UNUSED(conn);
    ARG_UNUSED(err);
    if (level >= BT_SECURITY_L2 && zmk_ble_active_profile_is_connected()) {
        k_work_cancel_delayable(&reconnect_watch_work);
        next_step = RECONNECT_STEP_NONE;
    }
}

BT_CONN_CB_DEFINE(reconnect_watch_cb) = {
    .connected = reconnect_watch_connected,
    .security_changed = reconnect_watch_security_changed,
};

static int reconnect_watch_init(void) {
    k_work_init_delayable(&reconnect_watch_work, reconnect_watch_work_handler);
    return 0;
}

SYS_INIT(reconnect_watch_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
