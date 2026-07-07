/*
 * Idle disconnect: after the keyboard has gone CONFIG_TOTEM_IDLE_DISCONNECT_MIN
 * minutes without a keypress, disconnect the active host.
 *
 * Why: macOS keeps its BLE link alive during sleep (for wake-on-keystroke), so the
 * keyboard otherwise stays connected-idle to it overnight and the advertising
 * throttle never engages -- the throttle only pauses advertising while the active
 * host is DISCONNECTED. Dropping the idle host hands things to the throttle: an
 * asleep/away host does not reconnect, advertising pauses, and idle drain falls to
 * the throttled rate.
 *
 * If the host is actually present (awake) it just reconnects -- cleanly, the same
 * path the exclusive-host module relies on -- so the only cost is the first
 * keystroke after a long idle, lost while the host reconnects. Pairs with
 * CONFIG_TOTEM_ADV_THROTTLE (which then pauses the advertising the disconnect frees
 * up). Central-only.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

LOG_MODULE_REGISTER(idle_disconnect, CONFIG_ZMK_LOG_LEVEL);

static struct k_work_delayable idle_disconnect_work;

static void idle_disconnect_work_handler(struct k_work *work) {
    if (!zmk_ble_active_profile_is_connected()) {
        /* Already gone -> the advertising throttle is handling it. */
        return;
    }

    struct bt_conn *conn = zmk_ble_active_profile_conn();
    if (conn == NULL) {
        return;
    }

    LOG_INF("Active host idle for %d min; disconnecting to let advertising pause",
            CONFIG_TOTEM_IDLE_DISCONNECT_MIN);
    /* 0x13 (remote user terminated) -- the reason proven to let macOS reconnect and
     * type cleanly (see the exclusive-host module). */
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    bt_conn_unref(conn);
}

/* Any keypress (either half; the central sees right-half presses over the split
 * link) resets the idle countdown -- activity means the host is present. */
static int idle_disconnect_keypress(const zmk_event_t *eh) {
    k_work_reschedule(&idle_disconnect_work, K_MINUTES(CONFIG_TOTEM_IDLE_DISCONNECT_MIN));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_idle_disconnect, idle_disconnect_keypress);
ZMK_SUBSCRIPTION(totem_idle_disconnect, zmk_position_state_changed);

/* Re-arm once the active host (re)connects, so a host that reconnects without any
 * typing is still re-evaluated and eventually dropped again once it is truly away. */
static void idle_disconnect_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        return;
    }
    if (zmk_ble_active_profile_is_connected()) {
        k_work_reschedule(&idle_disconnect_work, K_MINUTES(CONFIG_TOTEM_IDLE_DISCONNECT_MIN));
    }
}

BT_CONN_CB_DEFINE(idle_disconnect_cb) = {
    .connected = idle_disconnect_connected,
};

static int idle_disconnect_init(void) {
    k_work_init_delayable(&idle_disconnect_work, idle_disconnect_work_handler);
    return 0;
}

SYS_INIT(idle_disconnect_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
