/*
 * Exclusive host: keep only the active profile's computer connected. When you
 * switch profiles, disconnect any other computer still connected in the
 * background, and drop a non-active host the moment it connects, so only the
 * selected computer stays connected.
 *
 * Generic -- no per-host configuration. The active profile is whatever you have
 * selected; everything else is background and gets evicted. Nothing here knows or
 * cares which slot is a Mac vs a PC.
 *
 * A connection whose address does NOT resolve to a stored profile (idx < 0) is
 * left alone: that is a host mid-resolution -- e.g. a macOS resolvable private
 * address before the IRK match completes -- and dropping it would kill a
 * legitimate reconnect of the active host.
 *
 * The HCI reason used for the disconnect is configurable
 * (CONFIG_TOTEM_EXCLUSIVE_DISCONNECT_REASON) so it can be tuned for hosts that
 * mishandle input after a plain user-terminated disconnect.
 *
 * Central-only: the split link to the right half is role central and is skipped;
 * only host links (role peripheral on the central) are touched.
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

LOG_MODULE_REGISTER(exclusive_host, CONFIG_ZMK_LOG_LEVEL);

/* Disconnect this connection only if it's a host link (role peripheral on the
 * central) that belongs to a KNOWN profile other than the active one. */
static void drop_if_non_active_host(struct bt_conn *conn, void *data) {
    struct bt_conn_info info;

    if (bt_conn_get_info(conn, &info) != 0) {
        return;
    }
    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    /* idx < 0: address doesn't resolve to a stored profile yet (host still
     * resolving / fresh RPA) -> leave it alone so we never kill the active host's
     * own reconnect. idx == active: this is the selected host -> keep. */
    if (idx < 0 || idx == zmk_ble_active_profile_index()) {
        return;
    }

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Disconnecting background host %s (profile %d, reason 0x%02x)", addr, idx,
            CONFIG_TOTEM_EXCLUSIVE_DISCONNECT_REASON);
    bt_conn_disconnect(conn, CONFIG_TOTEM_EXCLUSIVE_DISCONNECT_REASON);
}

/* A new host connected -> drop it immediately if it's a non-active profile. */
static void exclusive_host_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        return;
    }
    drop_if_non_active_host(conn, NULL);
}

BT_CONN_CB_DEFINE(exclusive_host_cb) = {
    .connected = exclusive_host_connected,
};

/* Profile switched -> drop any other-profile computer still connected in the
 * background so only the newly-selected one stays. */
static int exclusive_host_profile_changed(const zmk_event_t *eh) {
    bt_conn_foreach(BT_CONN_TYPE_LE, drop_if_non_active_host, NULL);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_exclusive_host, exclusive_host_profile_changed);
ZMK_SUBSCRIPTION(totem_exclusive_host, zmk_ble_active_profile_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
