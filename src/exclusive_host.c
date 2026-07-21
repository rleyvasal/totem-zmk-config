/*
 * Exclusive host: keep only the active profile's computer connected.
 *
 * Known non-active profiles (idx >= 0 && idx != active) are dropped as soon as
 * they are identified -- without waiting for encryption. That stops Windows
 * thrashing open ads while Mac is selected.
 *
 * CRITICAL: idx < 0 (unresolved address) is NEVER dropped, even on the force
 * fallback. That state is:
 *   - a brand-new pairing (profile not stored yet), or
 *   - macOS RPA before identity resolve.
 * Force-dropping idx < 0 after a short settle (tried at 500 ms) killed pairing:
 * Windows showed a PIN then disconnected; macOS spun forever. Leave unknowns
 * alone; identity_resolved / security_changed will classify them.
 *
 * Profile switch: immediate eviction of known non-active established links.
 * Central-only.
 *
 * Logging uses stable totem_ble tokens for dual-host triage (see DEBUGGING-NOTES).
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

/* Only used to re-check RPA → profile mapping after a short delay; does not
 * force-drop unresolved peers (see drop_if_non_active_host). */
#define EXCLUSIVE_HOST_SETTLE_MS 2000

/* --- thrash detect (log / counters; storm actions gated separately) --- */

#if IS_ENABLED(CONFIG_TOTEM_THRASH_DETECT)
#define THRASH_RING_SIZE 16
#define THRASH_WINDOW_MS (CONFIG_TOTEM_THRASH_WINDOW_SEC * 1000)
#define CLASS_A_WINDOW_MS 60000
#define CLASS_A_FAIL_THRESHOLD 3

static int64_t thrash_ring[THRASH_RING_SIZE];
static uint8_t thrash_ring_head; /* next write index */
static uint8_t thrash_ring_count;

static int64_t class_a_fail_ts[CLASS_A_FAIL_THRESHOLD];
static uint8_t class_a_fail_count;

static uint32_t thrash_win_count(void) {
    int64_t now = k_uptime_get();
    uint32_t n = 0;

    for (uint8_t i = 0; i < thrash_ring_count; i++) {
        /* Head-backward: most recent first; stop when outside window. */
        uint8_t idx = (uint8_t)((thrash_ring_head + THRASH_RING_SIZE - 1 - i) % THRASH_RING_SIZE);
        if (now - thrash_ring[idx] > THRASH_WINDOW_MS) {
            break;
        }
        n++;
    }
    return n;
}

static void thrash_note_bg_evict(void) {
    thrash_ring[thrash_ring_head] = k_uptime_get();
    thrash_ring_head = (uint8_t)((thrash_ring_head + 1) % THRASH_RING_SIZE);
    if (thrash_ring_count < THRASH_RING_SIZE) {
        thrash_ring_count++;
    }

    uint32_t win = thrash_win_count();
    LOG_INF("totem_ble thrash_win count=%u window_sec=%d", win, CONFIG_TOTEM_THRASH_WINDOW_SEC);
}

static void thrash_clear(void) {
    thrash_ring_head = 0;
    thrash_ring_count = 0;
}

static void class_a_note_auth_event(bool is_fail) {
    if (!is_fail) {
        class_a_fail_count = 0;
        return;
    }

    int64_t now = k_uptime_get();
    /* Drop stale fails outside 60 s window. */
    uint8_t kept = 0;
    for (uint8_t i = 0; i < class_a_fail_count; i++) {
        if (now - class_a_fail_ts[i] <= CLASS_A_WINDOW_MS) {
            class_a_fail_ts[kept++] = class_a_fail_ts[i];
        }
    }
    class_a_fail_count = kept;

    if (class_a_fail_count < CLASS_A_FAIL_THRESHOLD) {
        class_a_fail_ts[class_a_fail_count++] = now;
    } else {
        /* Shift and append */
        for (uint8_t i = 1; i < CLASS_A_FAIL_THRESHOLD; i++) {
            class_a_fail_ts[i - 1] = class_a_fail_ts[i];
        }
        class_a_fail_ts[CLASS_A_FAIL_THRESHOLD - 1] = now;
    }

    uint32_t win = thrash_win_count();
    if (class_a_fail_count >= CLASS_A_FAIL_THRESHOLD && win < 2) {
        LOG_WRN("totem_ble CLASS_A_SUSPECT profile=%d auth_fails=%u thrash_win=%u",
                zmk_ble_active_profile_index(), class_a_fail_count, win);
    }
}
#else
static void thrash_note_bg_evict(void) {}
static void thrash_clear(void) {}
static void class_a_note_auth_event(bool is_fail) { ARG_UNUSED(is_fail); }
static uint32_t thrash_win_count(void) { return 0; }
#endif /* CONFIG_TOTEM_THRASH_DETECT */

static void log_host_conn_disc(struct bt_conn *conn, uint8_t disc_reason) {
    char addr[BT_ADDR_LE_STR_LEN];
    struct bt_conn_info info;
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    int active = zmk_ble_active_profile_index();
    bool active_up = zmk_ble_active_profile_is_connected();
    int role = -1;
    if (bt_conn_get_info(conn, &info) == 0) {
        role = info.role;
    }
    LOG_INF("totem_ble disc addr=%s role=%d idx=%d active=%d active_up=%d disc_reason=0x%02x "
            "thrash_win=%u",
            addr, role, idx, active, active_up, disc_reason, thrash_win_count());
}

static void log_host_conn_event(const char *tag, struct bt_conn *conn, uint8_t extra) {
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
    LOG_INF("totem_ble %s addr=%s role=%d idx=%d active=%d active_up=%d sec=%d extra=0x%02x", tag,
            addr, role, idx, active, active_up, (int)sec, extra);
}

/* data = force: historically bypassed the L2 wait for *known* non-active hosts.
 * Unknown (idx < 0) is always spared. */
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

    /* Unresolved / mid-pair / Mac RPA: never drop. Force must not override. */
    if (idx < 0) {
        return;
    }
    if (idx == active) {
        return;
    }

    /* Known non-active profile: drop immediately (no L2 wait). force unused for
     * this path but kept for API compatibility with the delayed fallback. */
    ARG_UNUSED(force);

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    int err = bt_conn_disconnect(conn, CONFIG_TOTEM_EXCLUSIVE_DISCONNECT_REASON);
    if (err) {
        LOG_WRN("totem_ble bg_evict_fail addr=%s idx=%d err=%d", addr, idx, err);
        return;
    }

    thrash_note_bg_evict();
    LOG_INF("totem_ble bg_evict addr=%s idx=%d disc_reason=0x%02x thrash_win=%u", addr, idx,
            CONFIG_TOTEM_EXCLUSIVE_DISCONNECT_REASON, thrash_win_count());
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
    /* Re-scan only: may catch a host whose address just became matchable. */
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
    LOG_ERR("totem_ble bond_heal clear profile=%d (host must Forget + re-pair)",
            zmk_ble_active_profile_index());
    active_auth_fail_streak = 0;
    zmk_ble_clear_bonds();
}

static K_WORK_DEFINE(bond_heal_clear_active_work, bond_heal_clear_active_work_handler);

static void bond_heal_note_auth_failure(const char *why) {
    if (++active_auth_fail_streak < CONFIG_TOTEM_BOND_HEAL_THRESHOLD) {
        LOG_WRN("totem_ble bond_heal auth_fail %u/%u (%s)", active_auth_fail_streak,
                CONFIG_TOTEM_BOND_HEAL_THRESHOLD, why);
        return;
    }
    LOG_ERR("totem_ble bond_heal threshold (%s) -- scheduling bond clear", why);
    k_work_submit(&bond_heal_clear_active_work);
}

static void bond_heal_note_auth_ok(void) {
    if (active_auth_fail_streak != 0) {
        LOG_INF("totem_ble bond_heal clear streak (%u)", active_auth_fail_streak);
        active_auth_fail_streak = 0;
    }
}
#endif /* CONFIG_TOTEM_BOND_HEAL */

static void exclusive_host_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        char addr[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
        int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
        LOG_INF("totem_ble connect_fail addr=%s idx=%d err=0x%02x", addr, idx, err);
        return;
    }
    log_host_conn_event("connected", conn, 0);
    /* Immediate try: known background hosts drop without waiting for L2. */
    k_work_submit(&exclusive_host_evict_work);
    exclusive_host_schedule_retry();
    k_work_reschedule(&exclusive_host_fallback_work, K_MSEC(EXCLUSIVE_HOST_SETTLE_MS));
}

static void exclusive_host_security_changed(struct bt_conn *conn, bt_security_t level,
                                            enum bt_security_err err) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    int active = zmk_ble_active_profile_index();

    if (err) {
        LOG_INF("totem_ble security_fail addr=%s idx=%d active=%d security_err=%d level=%d", addr,
                idx, active, (int)err, (int)level);
    } else {
        LOG_INF("totem_ble security_ok addr=%s idx=%d active=%d security_err=0 level=%d", addr, idx,
                active, (int)level);
    }

    if (idx == active) {
        if (err) {
            class_a_note_auth_event(true);
        } else if (level >= BT_SECURITY_L2) {
            class_a_note_auth_event(false);
        }
    }

#if IS_ENABLED(CONFIG_TOTEM_BOND_HEAL)
    if (idx == active) {
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
    log_host_conn_disc(conn, reason);

    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (idx == zmk_ble_active_profile_index() && reason == BT_HCI_ERR_AUTH_FAIL) {
        class_a_note_auth_event(true);
    }

#if IS_ENABLED(CONFIG_TOTEM_BOND_HEAL)
    if (reason == BT_HCI_ERR_AUTH_FAIL && idx == zmk_ble_active_profile_index()) {
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
    LOG_INF("totem_ble identity_resolved rpa=%s id=%s idx=%d active=%d active_up=%d", rpa_s, id_s,
            idx, zmk_ble_active_profile_index(), zmk_ble_active_profile_is_connected());
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
    thrash_clear();
    class_a_note_auth_event(false);
    LOG_INF("totem_ble profile_changed active=%d connected=%d open=%d",
            zmk_ble_active_profile_index(), zmk_ble_active_profile_is_connected(),
            zmk_ble_active_profile_is_open());
    exclusive_host_evict_all(false);
    exclusive_host_schedule_retry();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_exclusive_host, exclusive_host_profile_changed);
ZMK_SUBSCRIPTION(totem_exclusive_host, zmk_ble_active_profile_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
