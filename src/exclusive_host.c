/*
 * Exclusive host: keep only the active profile's computer connected. When you
 * switch profiles, disconnect any other computer still connected in the
 * background, and evict a non-active host shortly after it connects, so only
 * the selected computer stays connected.
 *
 * Generic -- no per-host configuration. The active profile is whatever you have
 * selected; everything else is background and gets evicted. Nothing here knows or
 * cares which slot is a Mac vs a PC.
 *
 * A connection whose address does NOT resolve to a stored profile (idx < 0) is
 * left alone: that is a host mid-resolution -- e.g. a macOS resolvable private
 * address before the IRK match completes -- and dropping it would kill a
 * legitimate reconnect of the active host. A short delayed re-check covers the
 * case where a background host's address resolves a moment later. The
 * identity_resolved callback also re-runs eviction once Zephyr has the identity.
 *
 * Handshake safety: a link that has not reached encryption (< BT_SECURITY_L2)
 * is not evicted by the normal paths. Cutting a connection mid LE-encryption /
 * SMP handshake is how keyboard and host can end up holding mismatched bonds
 * (the 2026-07-16 dual-host outage: both machines needed forget + re-pair), so
 * eviction of a fresh connection waits for the security_changed callback --
 * encryption settled, success or failure. A forced fallback pass still reaps a
 * background link that never starts encryption at all.
 *
 * Profile switch: evict immediately (same context as the profile-changed
 * event). Established links are dropped so the new profile is not competing
 * with a still-live background peer.
 *
 * Bond heal (CONFIG_TOTEM_BOND_HEAL, default n): optional auto-clear of the
 * active profile after repeated pure auth failures. Keep off under dual-host
 * thrash -- transient security errors must not wipe a good bond.
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

/* A bonded background host reaches encryption well under this. A link still
 * below L2 afterwards is not a real handshake -> the fallback force-evicts it. */
#define EXCLUSIVE_HOST_SETTLE_MS 2000

static void log_host_conn(const char *tag, struct bt_conn *conn, uint8_t extra) {
    char addr[BT_ADDR_LE_STR_LEN];
    struct bt_conn_info info;
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    int active = zmk_ble_active_profile_index();
    bool active_up = zmk_ble_active_profile_is_connected();
    bt_security_t sec = bt_conn_get_security(conn);
    int role = -1;
    if (bt_conn_get_info(conn, &info) == 0) {
        role = info.role;
    }
    LOG_INF("%s %s role=%d idx=%d active=%d active_up=%d sec=%d extra=0x%02x", tag, addr, role, idx,
            active, active_up, (int)sec, extra);
}

/* Disconnect this connection only if it's a host link (role peripheral on the
 * central) that belongs to a KNOWN profile other than the active one. `data`
 * carries the force flag: when false, a not-yet-encrypted link is spared. */
static void drop_if_non_active_host(struct bt_conn *conn, void *data) {
    bool force = (bool)(uintptr_t)data;
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
     * own reconnect. idx == active: this is the selected host -> keep. Never
     * overridden by force. */
    if (idx < 0 || idx == zmk_ble_active_profile_index()) {
        return;
    }

    /* Below L2 = encryption hasn't settled -> likely mid-handshake. Evicting now
     * risks leaving keyboard and host with mismatched bonds; security_changed
     * will trigger the eviction the moment the handshake finishes, and the
     * forced fallback reaps links that never encrypt. */
    if (!force && bt_conn_get_security(conn) < BT_SECURITY_L2) {
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

static void exclusive_host_evict_all(bool force) {
    bt_conn_foreach(BT_CONN_TYPE_LE, drop_if_non_active_host, (void *)(uintptr_t)force);
}

static void exclusive_host_evict_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    exclusive_host_evict_all(false);
}

static void exclusive_host_retry_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    exclusive_host_evict_all(false);
}

static void exclusive_host_fallback_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    exclusive_host_evict_all(true);
}

static K_WORK_DEFINE(exclusive_host_evict_work, exclusive_host_evict_work_handler);
static K_WORK_DELAYABLE_DEFINE(exclusive_host_retry_work, exclusive_host_retry_work_handler);
static K_WORK_DELAYABLE_DEFINE(exclusive_host_fallback_work, exclusive_host_fallback_work_handler);

static void exclusive_host_schedule_retry(void) {
    /* schedule (not reschedule): a flapping background host must not keep
     * postponing the RPA re-check forever. */
    k_work_schedule(&exclusive_host_retry_work, K_MSEC(EXCLUSIVE_HOST_RETRY_MS));
}

#if IS_ENABLED(CONFIG_TOTEM_BOND_HEAL)
static uint8_t active_auth_fail_streak;

static void bond_heal_clear_active_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    LOG_ERR("Bond heal: clearing active profile %d after repeated auth failures "
            "(host must Forget + re-pair)",
            zmk_ble_active_profile_index());
    active_auth_fail_streak = 0;
    zmk_ble_clear_bonds();
}

static K_WORK_DEFINE(bond_heal_clear_active_work, bond_heal_clear_active_work_handler);

static void bond_heal_note_auth_failure(const char *why) {
    if (++active_auth_fail_streak < CONFIG_TOTEM_BOND_HEAL_THRESHOLD) {
        LOG_WRN("Bond heal: active profile auth failure %u/%u (%s)", active_auth_fail_streak,
                CONFIG_TOTEM_BOND_HEAL_THRESHOLD, why);
        return;
    }
    LOG_ERR("Bond heal: threshold reached (%s) -- scheduling bond clear", why);
    k_work_submit(&bond_heal_clear_active_work);
}

static void bond_heal_note_auth_ok(void) {
    if (active_auth_fail_streak != 0) {
        LOG_INF("Bond heal: active profile security OK; clearing fail streak (%u)",
                active_auth_fail_streak);
        active_auth_fail_streak = 0;
    }
}
#endif /* CONFIG_TOTEM_BOND_HEAL */

static void exclusive_host_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        log_host_conn("connect_fail", conn, err);
        return;
    }
    log_host_conn("connected", conn, 0);
    k_work_reschedule(&exclusive_host_fallback_work, K_MSEC(EXCLUSIVE_HOST_SETTLE_MS));
}

static void exclusive_host_security_changed(struct bt_conn *conn, bt_security_t level,
                                            enum bt_security_err err) {
    log_host_conn(err ? "security_fail" : "security_ok", conn, err ? (uint8_t)err : (uint8_t)level);

#if IS_ENABLED(CONFIG_TOTEM_BOND_HEAL)
    if (zmk_ble_profile_index(bt_conn_get_dst(conn)) == zmk_ble_active_profile_index()) {
        if (err) {
            bond_heal_note_auth_failure("security_changed");
        } else if (level >= BT_SECURITY_L2) {
            bond_heal_note_auth_ok();
        }
    }
#endif
    k_work_submit(&exclusive_host_evict_work);
    exclusive_host_schedule_retry();
}

static void exclusive_host_disconnected(struct bt_conn *conn, uint8_t reason) {
    log_host_conn("disconnected", conn, reason);
#if IS_ENABLED(CONFIG_TOTEM_BOND_HEAL)
    if (reason == BT_HCI_ERR_AUTH_FAIL &&
        zmk_ble_profile_index(bt_conn_get_dst(conn)) == zmk_ble_active_profile_index()) {
        bond_heal_note_auth_failure("disconnect 0x05");
    }
#endif
}

/* macOS RPA: once Zephyr resolves identity, re-run eviction so a background
 * host that only just became matchable is dropped, and the active host is not
 * left stuck as idx<0 fail-open forever if it was mis-classified. */
static void exclusive_host_identity_resolved(struct bt_conn *conn, const bt_addr_le_t *rpa,
                                             const bt_addr_le_t *identity) {
    char rpa_s[BT_ADDR_LE_STR_LEN];
    char id_s[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(rpa, rpa_s, sizeof(rpa_s));
    bt_addr_le_to_str(identity, id_s, sizeof(id_s));
    int idx = zmk_ble_profile_index(identity);
    LOG_INF("identity_resolved rpa=%s id=%s idx=%d active=%d active_up=%d", rpa_s, id_s, idx,
            zmk_ble_active_profile_index(), zmk_ble_active_profile_is_connected());
    ARG_UNUSED(conn);
    k_work_submit(&exclusive_host_evict_work);
    exclusive_host_schedule_retry();
}

BT_CONN_CB_DEFINE(exclusive_host_cb) = {
    .connected = exclusive_host_connected,
    .disconnected = exclusive_host_disconnected,
    .security_changed = exclusive_host_security_changed,
    .identity_resolved = exclusive_host_identity_resolved,
};

static int exclusive_host_profile_changed(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
#if IS_ENABLED(CONFIG_TOTEM_BOND_HEAL)
    active_auth_fail_streak = 0;
#endif
    LOG_INF("profile_changed -> active=%d connected=%d", zmk_ble_active_profile_index(),
            zmk_ble_active_profile_is_connected());
    exclusive_host_evict_all(false);
    exclusive_host_schedule_retry();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_exclusive_host, exclusive_host_profile_changed);
ZMK_SUBSCRIPTION(totem_exclusive_host, zmk_ble_active_profile_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
