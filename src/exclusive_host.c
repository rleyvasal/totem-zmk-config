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
 * legitimate reconnect of the active host. A short delayed re-check covers the
 * case where a background host's address resolves a moment later.
 *
 * Profile switch: evict immediately (same context as the profile-changed event).
 * Deferring that path left the previous host connected while ZMK already
 * advertised for the new profile, which made the just-dropped machine fight the
 * target host for the link and stretched reconnect to multi-second delays.
 *
 * Connected callback: still deferred via work queue — bt_conn_disconnect() from
 * inside Zephyr's connected callback is unsafe while the stack finishes setup.
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

/* Re-check after RPA / identity resolution may have completed. */
#define EXCLUSIVE_HOST_RETRY_MS 150

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
    int err = bt_conn_disconnect(conn, CONFIG_TOTEM_EXCLUSIVE_DISCONNECT_REASON);
    if (err) {
        LOG_WRN("bt_conn_disconnect(%s) failed (err %d)", addr, err);
    }
}

static void exclusive_host_evict_all(void) {
    bt_conn_foreach(BT_CONN_TYPE_LE, drop_if_non_active_host, NULL);
}

static void exclusive_host_evict_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    exclusive_host_evict_all();
}

static void exclusive_host_retry_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    exclusive_host_evict_all();
}

static K_WORK_DEFINE(exclusive_host_evict_work, exclusive_host_evict_work_handler);
static K_WORK_DELAYABLE_DEFINE(exclusive_host_retry_work, exclusive_host_retry_work_handler);

static void exclusive_host_schedule_retry(void) {
    /* schedule (not reschedule): a flapping background host must not keep
     * postponing the RPA re-check forever. */
    k_work_schedule(&exclusive_host_retry_work, K_MSEC(EXCLUSIVE_HOST_RETRY_MS));
}

/* A new host connected -> defer eviction (never disconnect inside connected). */
static void exclusive_host_connected(struct bt_conn *conn, uint8_t err) {
    ARG_UNUSED(conn);
    if (err) {
        return;
    }
    k_work_submit(&exclusive_host_evict_work);
    exclusive_host_schedule_retry();
}

BT_CONN_CB_DEFINE(exclusive_host_cb) = {
    .connected = exclusive_host_connected,
};

/* Profile switched -> drop the previous computer immediately so the new profile
 * is not competing with a still-live background link for radio / host attention. */
static int exclusive_host_profile_changed(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    exclusive_host_evict_all();
    exclusive_host_schedule_retry();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_exclusive_host, exclusive_host_profile_changed);
ZMK_SUBSCRIPTION(totem_exclusive_host, zmk_ble_active_profile_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
