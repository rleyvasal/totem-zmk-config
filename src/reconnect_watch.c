/*
 * Reconnect watch: after a profile switch (or soft reselect), ensure the
 * selected host actually comes up. Also arms a *light* ladder when the active
 * host disconnects mid-session (peer-mapped only), without full reselect.
 *
 * Ladder (one step per timeout, never auto-clears bonds):
 *   1) FULL (BT_SEL): prof_select / reselect kick.
 *      LIGHT (active-down): densify boost + open ads; never prof_select.
 *   2) Force-evict any non-active host still connected.
 *   3) Final connection-table verification. Never disconnect an active-profile
 *      connection merely because a second helper temporarily disagrees.
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

#include <totem_reconnect_watch.h>
#include <totem_host_event_log.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

LOG_MODULE_REGISTER(reconnect_watch, CONFIG_ZMK_LOG_LEVEL);

#define RECONNECT_WATCH_STEP_SEC CONFIG_TOTEM_RECONNECT_WATCH_SEC

/* First recovery step waits out the post-switch boost window when boost is on,
 * so we do not abort an in-flight host reconnect (e.g. slow Windows) with a
 * mid-window reselect. Later steps use RECONNECT_WATCH_STEP_SEC. */
static int reconnect_watch_first_delay_sec(void) {
#if IS_ENABLED(CONFIG_TOTEM_ADV_BOOST)
    if (CONFIG_TOTEM_ADV_BOOST_SEC > RECONNECT_WATCH_STEP_SEC) {
        return CONFIG_TOTEM_ADV_BOOST_SEC;
    }
#endif
    return RECONNECT_WATCH_STEP_SEC;
}

/* Totem helpers from patches/zmk-ble.patch — declared in <zmk/ble.h> on the fork. */

enum reconnect_step {
    RECONNECT_STEP_NONE = 0,
    RECONNECT_STEP_READV,
    RECONNECT_STEP_EVICT,
    RECONNECT_STEP_ZOMBIE,
};

static enum reconnect_step next_step;
/* true ⇒ light step 1 (active-down / storm); false ⇒ full reselect (BT_SEL). */
static bool ladder_from_active_down;
static struct k_work_delayable reconnect_watch_work;
static struct k_work active_down_arm_work;

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
    LOG_WRN("totem_ble watch step=2 evict addr=%s idx=%d", addr, idx);
    int err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    if (err) {
        LOG_WRN("totem_ble watch step=2 disconnect failed err=%d", err);
    }
}

static void reconnect_watch_reset(void) {
    next_step = RECONNECT_STEP_NONE;
    ladder_from_active_down = false;
    k_work_cancel_delayable(&reconnect_watch_work);
}

static void reconnect_watch_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (zmk_ble_active_profile_is_open()) {
        LOG_DBG("totem_ble watch stop: open (pairing)");
        reconnect_watch_reset();
        return;
    }

    if (zmk_ble_active_profile_is_connected()) {
        LOG_INF("totem_ble watch done: active connected");
        reconnect_watch_reset();
        return;
    }

    struct host_conn_scan scan = {0};
    bt_conn_foreach(BT_CONN_TYPE_LE, scan_host_conns, &scan);

    /* scan_host_conns only records a profile match after bt_conn_get_info()
     * reports BT_CONN_STATE_CONNECTED. That is stronger evidence than the
     * identity-address helper, which can temporarily find a stale connection
     * object while a macOS RPA connection is live. Never run recovery against
     * a connection we have independently verified as active. */
    if (scan.active_match != NULL) {
        LOG_INF("totem_ble watch done: verified active connection");
        reconnect_watch_reset();
        host_conn_scan_release(&scan);
        return;
    }

    LOG_WRN("totem_ble watch active_down=%d profile=%d hosts=%d step=%d", ladder_from_active_down,
            zmk_ble_active_profile_index(), scan.host_count, next_step);

    switch (next_step) {
    case RECONNECT_STEP_READV:
        /* Always densify/kick ads — never prof_select here. Reselect soft-recovery
         * is only for intentional same-profile BT_SEL; auto reselect at ~8s was
         * aborting slow host reconnects and making switches feel >10s. */
        LOG_WRN("totem_ble watch step=1 mode=%s densify profile=%d",
                ladder_from_active_down ? "light" : "full", zmk_ble_active_profile_index());
        totem_host_event_log_record(TOTEM_HEVT_WATCH_STEP, (int8_t)zmk_ble_active_profile_index(),
                                    (int8_t)zmk_ble_active_profile_index(), 1,
                                    (uint8_t)scan.host_count,
                                    ladder_from_active_down ? 1 : 0);
#if IS_ENABLED(CONFIG_TOTEM_ADV_BOOST)
        zmk_ble_totem_adv_boost_rearm();
#else
        zmk_ble_totem_kick_open_adv();
#endif
        next_step = RECONNECT_STEP_EVICT;
        k_work_schedule(&reconnect_watch_work, K_SECONDS(RECONNECT_WATCH_STEP_SEC));
        break;

    case RECONNECT_STEP_EVICT:
        LOG_WRN("totem_ble watch step=2 force-evict non-active hosts profile=%d",
                zmk_ble_active_profile_index());
        totem_host_event_log_record(TOTEM_HEVT_WATCH_STEP, (int8_t)zmk_ble_active_profile_index(),
                                    (int8_t)zmk_ble_active_profile_index(), 2,
                                    (uint8_t)scan.host_count,
                                    ladder_from_active_down ? 1 : 0);
        bt_conn_foreach(BT_CONN_TYPE_LE, drop_non_active_hosts, NULL);
        next_step = RECONNECT_STEP_ZOMBIE;
        k_work_schedule(&reconnect_watch_work, K_SECONDS(RECONNECT_WATCH_STEP_SEC));
        break;

    case RECONNECT_STEP_ZOMBIE:
        /* Never drop unmapped (idx < 0) hosts. A connected active-profile
         * match was handled above and must never be treated as a zombie. */
        totem_host_event_log_record(TOTEM_HEVT_WATCH_STEP, (int8_t)zmk_ble_active_profile_index(),
                                    (int8_t)zmk_ble_active_profile_index(), 3,
                                    (uint8_t)scan.host_count,
                                    ladder_from_active_down ? 1 : 0);
        LOG_WRN("totem_ble watch step=3 leave unmapped/pairing alone");
        reconnect_watch_reset();
        break;

    default:
        reconnect_watch_reset();
        break;
    }

    host_conn_scan_release(&scan);
}

/* BT_SEL / profile_changed: recovery ladder (step1 = densify only, no reselect). */
static void reconnect_watch_arm_full(void) {
    if (zmk_ble_active_profile_is_open() || zmk_ble_active_profile_is_connected()) {
        reconnect_watch_reset();
        return;
    }
    ladder_from_active_down = false;
    next_step = RECONNECT_STEP_READV;
    int delay = reconnect_watch_first_delay_sec();
    LOG_INF("totem_ble watch arm mode=full profile=%d first_delay_sec=%d",
            zmk_ble_active_profile_index(), delay);
    totem_host_event_log_record(TOTEM_HEVT_WATCH_ARM, (int8_t)zmk_ble_active_profile_index(),
                                (int8_t)zmk_ble_active_profile_index(), (uint8_t)delay, 0,
                                0 /* full */);
    k_work_reschedule(&reconnect_watch_work, K_SECONDS(delay));
}

/* Active-down / optional storm: light ladder — never prof_select in step 1. */
static void reconnect_watch_arm_light(void) {
    if (zmk_ble_active_profile_is_open() || zmk_ble_active_profile_is_connected()) {
        reconnect_watch_reset();
        return;
    }
    if (zmk_ble_totem_ads_suppressed()) {
        LOG_INF("totem_ble active_down skip: ads_suppressed");
        return;
    }
    if (next_step != RECONNECT_STEP_NONE) {
        LOG_DBG("totem_ble active_down skip: ladder already armed step=%d", next_step);
        return;
    }
    ladder_from_active_down = true;
    next_step = RECONNECT_STEP_READV;
    LOG_INF("totem_ble watch arm mode=light profile=%d step_sec=%d",
            zmk_ble_active_profile_index(), RECONNECT_WATCH_STEP_SEC);
    totem_host_event_log_record(TOTEM_HEVT_WATCH_ARM, (int8_t)zmk_ble_active_profile_index(),
                                (int8_t)zmk_ble_active_profile_index(), RECONNECT_WATCH_STEP_SEC, 0,
                                1 /* light */);
    k_work_reschedule(&reconnect_watch_work, K_SECONDS(RECONNECT_WATCH_STEP_SEC));
}

/* Public export for exclusive_host storm (if STORM_ARM_WATCH=y): ALWAYS light. */
void totem_reconnect_watch_arm_if_needed(void) {
    reconnect_watch_arm_light();
}

static void active_down_arm_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!IS_ENABLED(CONFIG_TOTEM_RECONNECT_WATCH_ON_ACTIVE_DOWN)) {
        return;
    }
    if (zmk_ble_active_profile_is_open()) {
        return;
    }
    /* Re-check at fire time — not only in disconnected callback. */
    if (zmk_ble_totem_ads_suppressed()) {
        LOG_INF("totem_ble active_down skip: ads_suppressed");
        return;
    }
    if (zmk_ble_active_profile_is_connected()) {
        return;
    }
    if (next_step != RECONNECT_STEP_NONE) {
        LOG_DBG("totem_ble active_down skip: ladder already armed step=%d", next_step);
        return;
    }

    LOG_WRN("totem_ble active_down arm profile=%d source=peer_disc",
            zmk_ble_active_profile_index());
    totem_host_event_log_record(TOTEM_HEVT_ACTIVE_DOWN_ARM,
                                (int8_t)zmk_ble_active_profile_index(),
                                (int8_t)zmk_ble_active_profile_index(), 0, 0, 0);
    reconnect_watch_arm_light();
}

static int reconnect_watch_profile_changed(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    reconnect_watch_arm_full();
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
        LOG_INF("totem_ble watch cancel: active host connected");
        reconnect_watch_reset();
    }
}

static void reconnect_watch_disconnected(struct bt_conn *conn, uint8_t reason) {
    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    int active = zmk_ble_active_profile_index();

    LOG_INF("totem_ble watch disc idx=%d active=%d disc_reason=0x%02x", idx, active, reason);

    /* Peer-mapped only: never arm on background thrash or unresolved RPA. */
    if (idx < 0 || idx != active) {
        return;
    }

    if (!IS_ENABLED(CONFIG_TOTEM_RECONNECT_WATCH_ON_ACTIVE_DOWN)) {
        return;
    }

    /* Defer so conn table / is_connected() match ZMK's deferred update_advertising. */
    k_work_submit(&active_down_arm_work);
}

static void reconnect_watch_security_changed(struct bt_conn *conn, bt_security_t level,
                                             enum bt_security_err err) {
    ARG_UNUSED(conn);
    ARG_UNUSED(err);
    if (level >= BT_SECURITY_L2 && zmk_ble_active_profile_is_connected()) {
        reconnect_watch_reset();
    }
}

BT_CONN_CB_DEFINE(reconnect_watch_cb) = {
    .connected = reconnect_watch_connected,
    .disconnected = reconnect_watch_disconnected,
    .security_changed = reconnect_watch_security_changed,
};

static int reconnect_watch_init(void) {
    k_work_init_delayable(&reconnect_watch_work, reconnect_watch_work_handler);
    k_work_init(&active_down_arm_work, active_down_arm_work_handler);
    return 0;
}

SYS_INIT(reconnect_watch_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
