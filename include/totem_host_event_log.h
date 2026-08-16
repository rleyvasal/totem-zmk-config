/*
 * Multi-profile host event ring: capture BLE host connect/disconnect/security/
 * thrash/watch events for any profile index (not dual-host specific).
 *
 * Dump over USB serial via &host_log_dump (combo). Persistence via settings so
 * the ring can outlive a soft reboot / firmware flash (without settings-reset).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Event kinds stored in the ring (stable numeric IDs for dumps). */
enum totem_host_evt {
    TOTEM_HEVT_NONE = 0,
    TOTEM_HEVT_DISC = 1,
    TOTEM_HEVT_CONN = 2,
    TOTEM_HEVT_CONN_FAIL = 3,
    TOTEM_HEVT_SEC_OK = 4,
    TOTEM_HEVT_SEC_FAIL = 5,
    TOTEM_HEVT_BG_EVICT = 6,
    TOTEM_HEVT_ACTIVE_DOWN_ARM = 7,
    TOTEM_HEVT_WATCH_ARM = 8,
    TOTEM_HEVT_WATCH_STEP = 9,
    TOTEM_HEVT_PROFILE_CHANGED = 10,
    TOTEM_HEVT_IDENTITY = 11,
    TOTEM_HEVT_CLASS_A_SUSPECT = 12,
    TOTEM_HEVT_THRASH_WIN = 13,
    /* Boot record. `reason` carries the compressed reset cause (TOTEM_RR_* in
     * src/totem_watchdog.c); idx/active are -1. A TOTEM_RR_WATCHDOG bit here means
     * the previous boot ended in a hang the firmware could not recover from. */
    TOTEM_HEVT_BOOT = 14,
    /* Post-mortem from the previous boot (see totem_fault.h). `idx` is the task
     * watchdog channel or -1, `reason` the K_ERR_* code, `extra` the fault kind. */
    TOTEM_HEVT_FAULT = 15,
};

/**
 * Record one host event. Safe to call from BLE callbacks / work queues.
 * @param type  enum totem_host_evt
 * @param idx   peer profile index, or -1 if unknown/unresolved
 * @param active active profile index
 * @param reason HCI disc_reason, security_err, watch step, thrash count, etc.
 * @param thrash_win current thrash window count (0 if N/A)
 * @param extra  free byte (e.g. watch mode 0=full 1=light, security level)
 */
#if IS_ENABLED(CONFIG_TOTEM_HOST_EVENT_LOG)
void totem_host_event_log_record(uint8_t type, int8_t idx, int8_t active, uint8_t reason,
                                 uint8_t thrash_win, uint8_t extra);

/** Print ring oldest→newest via printk (USB CDC when logging / console enabled). */
void totem_host_event_log_dump(void);

/** Force-save ring to settings now (also done periodically). */
void totem_host_event_log_persist(void);
#else
static inline void totem_host_event_log_record(uint8_t type, int8_t idx, int8_t active,
                                               uint8_t reason, uint8_t thrash_win, uint8_t extra) {
    (void)type;
    (void)idx;
    (void)active;
    (void)reason;
    (void)thrash_win;
    (void)extra;
}
static inline void totem_host_event_log_dump(void) {}
static inline void totem_host_event_log_persist(void) {}
#endif

#ifdef __cplusplus
}
#endif
