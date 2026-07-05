/*
 * Reject BLE host connections that aren't the currently-selected profile.
 *
 * ZMK uses a single BLE identity for all profiles and advertises openly, so any
 * host ever bonded to any profile can connect while the keyboard is advertising.
 * That let other previously-paired computers grab the keyboard in the background
 * -> connect/drop churn, battery drain, and cross-wake.
 *
 * This registers an ADDITIONAL connection callback (Zephyr calls it alongside
 * ZMK's own) and, on the central half only, immediately disconnects any host
 * that is not the active profile's bonded peer. No device addresses are
 * hard-coded: the active profile is read from ZMK at connect time, so switching
 * profiles (&bt BT_SEL n) or pairing a new device just works.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>

#include <zmk/ble.h>

/* Central (left) half only: on the right half the split link to the central has
 * role PERIPHERAL and would be misread as a "host". Compile to nothing there. */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

LOG_MODULE_REGISTER(active_profile_only, CONFIG_ZMK_LOG_LEVEL);

static void active_profile_only_connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;

    if (err) {
        return;
    }

    if (bt_conn_get_info(conn, &info) != 0) {
        return;
    }

    /* On the central, a host link has role PERIPHERAL; the split link to the
     * right half has role CENTRAL. Only ever touch host links. */
    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    /* No bond in this slot yet -> pairing mode, accept the new host. */
    if (zmk_ble_active_profile_is_open()) {
        return;
    }

    const bt_addr_le_t *peer = bt_conn_get_dst(conn);

    /* Bonded hosts resolve to their identity address here, which is what the
     * profile stores, so this compare works for public and bonded-RPA hosts. */
    if (bt_addr_le_cmp(peer, zmk_ble_active_profile_addr()) == 0) {
        return; /* this IS the selected device -- keep it */
    }

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(peer, addr, sizeof(addr));
    LOG_INF("Rejecting non-active-profile host %s", addr);
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

BT_CONN_CB_DEFINE(active_profile_only_cb) = {
    .connected = active_profile_only_connected,
};

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
