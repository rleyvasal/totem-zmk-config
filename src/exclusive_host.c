/*
 * Exclusive host: keep only the active profile's computer connected.
 *
 * Known non-active profiles (e.g. Windows while Mac is selected) are dropped
 * as soon as they are identified -- without waiting for encryption to finish.
 * Waiting for L2 let the wrong host thrash for hundreds of ms per cycle,
 * hold the only free connection slot, and starve the selected machine
 * (symptoms: Win flap + Mac greyed out after a clean dual pair).
 *
 * Unresolved addresses (idx < 0, typical macOS RPA before identity resolve)
 * are still left alone until identity_resolved / security_changed / the
 * forced fallback, so we never kill the active host mid-RPA.
 *
 * Profile switch: immediate eviction of known non-active established links.
 *
 * Bond heal remains optional and off by default.
 *
 * Central-only.
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

#define EXCLUSIVE_HOST_RETRY_MS 150

/* Force-drop still-unencrypted background links that never identified. Short:
 * known hosts are already dropped early; this only reaps stuck unknowns. */
#define EXCLUSIVE_HOST_SETTLE_MS 500

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

/* data = force: when true, also drop unresolved (idx<0) non-active attempts that
 * are still below L2 after the settle window (stuck ghosts). */
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
    int active = zmk_ble_active_profile_index();

    if (idx == active) {
        return;
    }

    if (idx < 0) {
        /* Unresolved RPA: only the forced fallback may drop, and only once
         * encryption never started -- protects an active Mac mid-resolve. */
        if (!force) {
            return;
        }
        if (bt_conn_get_security(conn) >= BT_SECURITY_L2) {
            /* Encrypted but unknown: leave it; identity_resolved should map it. */
            return;
        }
    }

    /* Known non-active profile (idx >= 0 && idx != active): drop immediately.
     * Do not wait for L2 -- that was the thrash window. */

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Disconnecting background host %s (profile %d, reason 0x%02x%s)", addr, idx,
            CONFIG_TOTEM_EXCLUSIVE_DISCONNECT_REASON, force ? ", force" : "");
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
    /* Immediate try: known background hosts drop without waiting for L2. */
    k_work_submit(&exclusive_host_evict_work);
    exclusive_host_schedule_retry();
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
