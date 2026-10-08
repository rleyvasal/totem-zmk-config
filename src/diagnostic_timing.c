/* Timing only: never print, save settings, or wait in the controller path. */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zmk/ble.h>
#include <totem_host_event_log.h>
#include <totem_handoff_diagnostics.h>
/* The pinned Zephyr priority HCI handler calls this internal state setter. */
#include "conn_internal.h"

/* HCI disconnect + radio/TX summaries + ACK/timeout fit in one burst. */
#define TIMING_CAP 24
struct timing_record {
    uint32_t uptime_ms;
    uint8_t type;
    int8_t idx, active;
    uint8_t reason, pair, extra;
};
static struct timing_record timing_inbox[TIMING_CAP];
static struct k_spinlock timing_lock;
static uint8_t timing_head, timing_count;
static uint8_t timing_dropped;
static atomic_t write_sequence;

static void timing_drain(struct k_work *work) {
    ARG_UNUSED(work);
    for (;;) {
        k_spinlock_key_t key = k_spin_lock(&timing_lock);
        if (!timing_count) {
            uint8_t dropped = timing_dropped;
            timing_dropped = 0;
            k_spin_unlock(&timing_lock, key);
            if (dropped) {
                totem_host_event_log_record_timing(k_uptime_get_32(),
                    TOTEM_HEVT_TIMING_DROPPED, -1, -1, dropped, 0, 0);
            }
            return;
        }
        struct timing_record record = timing_inbox[timing_head];
        timing_head = (timing_head + 1) % TIMING_CAP;
        timing_count--;
        k_spin_unlock(&timing_lock, key);
        totem_host_event_log_record_timing(record.uptime_ms, record.type, record.idx,
                                          record.active, record.reason, record.pair, record.extra);
    }
}
static K_WORK_DEFINE(timing_work, timing_drain);

void totem_diagnostic_timing_capture(uint32_t uptime_ms, uint8_t type, int8_t idx,
                                      int8_t active, uint8_t reason, uint8_t pair, uint8_t extra) {
    struct timing_record record = {uptime_ms, type, idx, active, reason, pair, extra};
    k_spinlock_key_t key = k_spin_lock(&timing_lock);
    if (timing_count < TIMING_CAP) {
        timing_inbox[(timing_head + timing_count) % TIMING_CAP] = record;
        timing_count++;
    } else if (timing_dropped < UINT8_MAX) {
        timing_dropped++;
    }
    k_spin_unlock(&timing_lock, key);
    k_work_submit(&timing_work);
}

static void timing_capture(uint8_t type, int8_t idx, int8_t active,
                            uint8_t reason, uint8_t pair, uint8_t extra) {
    totem_diagnostic_timing_capture(k_uptime_get_32(), type, idx, active, reason, pair, extra);
}

int __real_settings_save_one(const char *name, const void *value, size_t len);

int __wrap_settings_save_one(const char *name, const void *value, size_t len) {
    uint8_t category = !strncmp(name, "th/dlog/", 8) ? 1 :
                       !strcmp(name, "ble/active_profile") ? 2 :
                       !strncmp(name, "ble/profiles/", 13) ? 3 :
                       !strncmp(name, "bt/", 3) ? 4 : 5;
    uint8_t pair = (uint8_t)atomic_inc(&write_sequence);
    int8_t active = zmk_ble_active_profile_index();
    timing_capture(TOTEM_HEVT_FLASH_BEGIN, -1, active, category, pair, 0);
    int err = __real_settings_save_one(name, value, len);
    timing_capture(TOTEM_HEVT_FLASH_END, -1, active, category, pair,
                    (uint8_t)MIN(err < 0 ? -err : err, UINT8_MAX));
    return err;
}

void __real_bt_conn_set_state(struct bt_conn *conn, bt_conn_state_t state);

void __wrap_bt_conn_set_state(struct bt_conn *conn, bt_conn_state_t state) {
#if IS_ENABLED(CONFIG_BT_LL_SW_SPLIT)
    if (conn->type == BT_CONN_TYPE_LE && state == BT_CONN_CONNECTED) {
        totem_handoff_connected(conn->handle);
    }
#endif
    /* Called by hci_disconn_complete_prio() after it copies the HCI reason,
     * before TX flushing, deferred cleanup, and application callbacks. */
    if (conn->type == BT_CONN_TYPE_LE && state == BT_CONN_DISCONNECT_COMPLETE) {
        timing_capture(TOTEM_HEVT_HCI_DISC, (int8_t)(conn->handle & 0xff),
                        (int8_t)(conn->handle >> 8), conn->err, 0,
                        zmk_ble_active_profile_index());
#if IS_ENABLED(CONFIG_BT_LL_SW_SPLIT)
        totem_handoff_disconnected(conn->handle);
#endif
    }
    __real_bt_conn_set_state(conn, state);
}
