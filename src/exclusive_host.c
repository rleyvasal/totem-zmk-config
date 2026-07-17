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
 * case where a background host's address resolves a moment later.
 *
 * Handshake safety: a link that has not reached encryption (< BT_SECURITY_L2)
 * is not evicted by the normal paths. Cutting a connection mid LE-encryption /
 * SMP handshake is how keyboard and host can end up holding mismatched bonds
 * (the 2026-07-16 dual-host outage: both machines needed forget + re-pair), so
 * eviction of a fresh connection waits for the security_changed callback --
 * encryption settled, success or failure. A forced fallback pass still reaps a
 * background link that never starts encryption at all: a bonded background
 * host encrypts in well under a second, so nothing legitimate is still
 * unencrypted after the settle window.
 *
 * Profile switch: evict immediately (same context as the profile-changed
 * event). Those are established, already-encrypted links, and deferring left
 * the previous host connected while ZMK already advertised for the new
 * profile, which made the just-dropped machine fight the target host for the
 * link and stretched reconnect to multi-second delays. A rare mid-handshake
 * link at switch time is skipped by the security guard and cleaned up by its
 * own security_changed / the fallback.
 *
 * bt_conn_disconnect() never runs inside a Zephyr stack callback -- the
 * connected / security_changed / disconnected paths that need to act defer
 * through the system work queue (except the profile-changed path, which is
 * already outside the stack's critical connect path and needs immediate
 * eviction for switch latency).
 *
 * Bond heal (CONFIG_TOTEM_BOND_HEAL): track authentication failures (HCI 0x05 /
 * security_changed errors) on the *active* profile. After N failures in a row,
 * clear that profile's bond on the keyboard so we stop sitting in the macOS
 * "Connected + battery but no keystrokes" zombie state. The host still needs
 * Forget + re-pair once -- but the keyboard side self-heals instead of
 * flapping forever with a rotten key.
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

/* A bonded background host reaches encryption well under this. A link still
 * below L2 afterwards is not a real handshake -> the fallback force-evicts it. */
#define EXCLUSIVE_HOST_SETTLE_MS 2000

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
/*
 * Consecutive auth failures on the *active* profile. Reset on a clean security
 * level promotion for that profile. Threshold via Kconfig.
 */
static uint8_t active_auth_fail_streak;

static void bond_heal_clear_active_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    LOG_ERR("Bond heal: clearing active profile %d after repeated auth failures "
            "(host must Forget + re-pair)",
            zmk_ble_active_profile_index());
    active_auth_fail_streak = 0;
    /* Clears keyboard-side bond for the active profile and restarts advertising
     * as an open pairable profile. Host still needs Forget if it holds the
     * mismatched key -- otherwise it will fail again immediately. */
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

/* A new host connected -> no eviction yet: it may be mid-handshake (see header).
 * Arm the forced fallback so a link that never encrypts still gets reaped.
 * reschedule (not schedule): a connect near the window's edge must get a full
 * settle window; postponement can't shield a host, because every link that
 * finishes its handshake is evicted from security_changed anyway. */
static void exclusive_host_connected(struct bt_conn *conn, uint8_t err) {
    ARG_UNUSED(conn);
    if (err) {
        return;
    }
    k_work_reschedule(&exclusive_host_fallback_work, K_MSEC(EXCLUSIVE_HOST_SETTLE_MS));
}

/* Encryption settled (success OR failure) -> safe to evict a non-active host.
 * Deferred via work queue; the retry covers late RPA resolution. */
static void exclusive_host_security_changed(struct bt_conn *conn, bt_security_t level,
                                            enum bt_security_err err) {
#if IS_ENABLED(CONFIG_TOTEM_BOND_HEAL)
    /* Only the active profile's security outcome feeds the heal streak --
     * background hosts are expected to encrypt then get dropped. */
    if (zmk_ble_profile_index(bt_conn_get_dst(conn)) == zmk_ble_active_profile_index()) {
        if (err) {
            bond_heal_note_auth_failure("security_changed");
        } else if (level >= BT_SECURITY_L2) {
            bond_heal_note_auth_ok();
        }
    }
#else
    ARG_UNUSED(conn);
    ARG_UNUSED(level);
    ARG_UNUSED(err);
#endif
    k_work_submit(&exclusive_host_evict_work);
    exclusive_host_schedule_retry();
}

static void exclusive_host_disconnected(struct bt_conn *conn, uint8_t reason) {
#if IS_ENABLED(CONFIG_TOTEM_BOND_HEAL)
    /* 0x05 = authentication failure (key mismatch). Count only for the active
     * profile so a flapping background host cannot trigger a clear. */
    if (reason == BT_HCI_ERR_AUTH_FAIL &&
        zmk_ble_profile_index(bt_conn_get_dst(conn)) == zmk_ble_active_profile_index()) {
        bond_heal_note_auth_failure("disconnect 0x05");
    }
#else
    ARG_UNUSED(conn);
    ARG_UNUSED(reason);
#endif
}

BT_CONN_CB_DEFINE(exclusive_host_cb) = {
    .connected = exclusive_host_connected,
    .disconnected = exclusive_host_disconnected,
    .security_changed = exclusive_host_security_changed,
};

/* Profile switched -> drop the previous computer immediately so the new profile
 * is not competing with a still-live background link for radio / host attention. */
static int exclusive_host_profile_changed(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
#if IS_ENABLED(CONFIG_TOTEM_BOND_HEAL)
    /* Failures on the previous profile must not clear the newly selected one. */
    active_auth_fail_streak = 0;
#endif
    exclusive_host_evict_all(false);
    exclusive_host_schedule_retry();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_exclusive_host, exclusive_host_profile_changed);
ZMK_SUBSCRIPTION(totem_exclusive_host, zmk_ble_active_profile_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
