/*
 * One host at a time: keep only the currently-selected profile's computer
 * connected, so keystrokes never leak to another paired machine.
 *
 * ZMK does NOT disconnect the previously-connected computer when you switch
 * profiles, so two hosts can be connected at once and input bleeds across (e.g.
 * Ctrl+C meant for one Mac also waking the other). This module, on the central
 * half:
 *   - when you switch profiles, disconnects any OTHER profile's computer that is
 *     still connected in the background, and
 *   - when a new host connects, drops it if it belongs to a non-active profile.
 *
 * RPA-safe: it only ever disconnects a host whose address matches a KNOWN,
 * different profile (`zmk_ble_profile_index` >= 0 and != active). An address that
 * doesn't match any stored profile -- e.g. a fresh resolvable-private-address from
 * your active Mac on reconnect -- is left ALONE (fail open). That's the difference
 * from the earlier version, which raw-compared to the active address and could
 * wedge the legit host ("connected but grayed out") when the address didn't match.
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

LOG_MODULE_REGISTER(active_profile_only, CONFIG_ZMK_LOG_LEVEL);

/* Disconnect this connection only if it's a host (role peripheral on the central)
 * that belongs to a KNOWN profile other than the active one. */
static void drop_if_non_active_host(struct bt_conn *conn, void *data) {
    struct bt_conn_info info;

    if (bt_conn_get_info(conn, &info) != 0) {
        return;
    }
    /* The split link to the right half is role CENTRAL; only touch host links. */
    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    /* idx < 0: address matches no stored profile (fresh RPA / new device) -> leave
     * it alone. Only drop a host that IS a known, non-active profile. */
    if (idx < 0 || idx == zmk_ble_active_profile_index()) {
        return;
    }

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Disconnecting non-active-profile host %s (profile %d)", addr, idx);
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

/* A new host connected -> drop it immediately if it's a known non-active profile. */
static void active_profile_only_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        return;
    }
    drop_if_non_active_host(conn, NULL);
}

BT_CONN_CB_DEFINE(active_profile_only_cb) = {
    .connected = active_profile_only_connected,
};

/* Profile switched -> drop any other-profile computer still connected so only the
 * newly-selected one stays. */
static int active_profile_only_switched(const zmk_event_t *eh) {
    bt_conn_foreach(BT_CONN_TYPE_LE, drop_if_non_active_host, NULL);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_active_profile_only, active_profile_only_switched);
ZMK_SUBSCRIPTION(totem_active_profile_only, zmk_ble_active_profile_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
