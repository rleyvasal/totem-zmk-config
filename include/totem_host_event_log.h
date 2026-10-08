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
    /* USB/HID state transition. reason is the USB event state; extra is the
     * selected transport (or a compact endpoint state). */
    TOTEM_HEVT_USB = 16,
    /* Host advertising intent/result. reason is the requested advertising
     * state; extra is a compact reason or error code. */
    TOTEM_HEVT_ADV = 17,
    /* Split central-to-peripheral link transition. reason is the HCI reason
     * or security error; extra carries role/security state. */
    TOTEM_HEVT_SPLIT = 18,
    /* A coalesced repeat count; reason identifies the underlying event and
     * extra is the number of suppressed repeats. */
    TOTEM_HEVT_REPEAT = 19,
    TOTEM_HEVT_HID_SUBSCRIBED = 20,
    /* First send attempt; extra=1 if keyboard notifications are subscribed. */
    TOTEM_HEVT_HID_FIRST_ATTEMPT = 21,
    /* extra=0: local send completion; extra=1: send failed, reason=positive errno. */
    TOTEM_HEVT_HID_TX_RESULT = 22,
    /* reason=zmk_ble_adv_stage; thrash_win=0 success, 1 POSIX, 2 HCI;
     * extra=absolute API result. No journal format change. */
    TOTEM_HEVT_ADV_RESULT = 23,
    /* First local security API call: reason=requested level/flags,
     * extra=current level at entry. Not proof an SMP packet was sent. */
    TOTEM_HEVT_SEC_REQUEST = 24,
    /* Its return: reason=requested level/flags; w=0 success, 1 POSIX, 2 HCI;
     * extra=absolute result. Security completion remains event 4/5. */
    TOTEM_HEVT_SEC_REQUEST_RESULT = 25,
    /* Settings write entry/return: reason=key category (1 journal, 2 profile
     * selection, 3 profile data, 4 bonds, 5 other); w=pair ID; end extra=errno.
     * Original timestamps are retained; these records never request a save. */
    TOTEM_HEVT_FLASH_BEGIN = 26,
    TOTEM_HEVT_FLASH_END = 27,
    /* Controller->host disconnect event before host cleanup/callbacks.
     * idx/active carry the low/high handle bytes, reason=HCI reason,
     * w=HCI status, extra=selected profile. Not an over-the-air timestamp. */
    TOTEM_HEVT_HCI_DISC = 28,
    /* Connection handle mapping: idx=profile, active=selection,
     * reason/extra=low/high handle bytes. */
    TOTEM_HEVT_CONN_HANDLE = 29,
    /* Timing inbox overflow: reason=number of dropped records (up to 255). */
    TOTEM_HEVT_TIMING_DROPPED = 30,
    /* Coalesced HID pipeline count: idx=observed profile; reason=stage;
     * w/extra=low/high cumulative count (16-bit, wraps, resets on boot).
     * Timestamp is the latest observation, not the summary flush time. */
    TOTEM_HEVT_HID_COUNT = 31,
    /* Same profile/stage, w=last positive errno, extra=0. No key identity. */
    TOTEM_HEVT_HID_ERROR = 32,
    /* Handoff-only controller diagnostics. i/a always contain handle low/high.
     * Parameters: r=0 interval (1.25 ms), 1 latency, 2 timeout (10 ms); w/x=value.
     * Termination: r=0 request (x=reason), 1 API return (w=error class,x=result),
     * 2 control PDU queued (not necessarily transmitted), 3 link-layer ACK.
     * 4 confirmed local termination timer expiry (w=0,x=controller reason).
     * Radio summary: r=0 completed events, 1 no valid CRC/non-aborted packet,
     * 2 aborted events, 3 skipped events; w/x=saturating 16-bit count.
     * r=4 last valid RX before request, 5 last valid RX at disconnect:
     * t=that event's captured time, x=1 known or 0 unknown (t=snapshot time).
     * r=6 first event without valid RX during handoff, x=aborted flag.
     * r=7 completed RX packets (trx_cnt sum, not TX/ACK count),
     * 8 non-aborted events with no RX, 9 non-aborted received-but-invalid events,
     * 10/11 events reporting MIC pass/fail; all w/x=saturating 16-bit counts.
     * MIC counts are event summaries, not per-packet; none is not failure. */
    TOTEM_HEVT_HANDOFF_PARAM = 33,
    TOTEM_HEVT_HANDOFF_TERM = 34,
    TOTEM_HEVT_HANDOFF_RADIO = 35,
    /* Encryption event received at host RX queue/removed for processing.
     * i/a=handle low/high, r=0 queued, 1 dequeued; 2/3 for key refresh.
     * w=HCI status, x=encryption enabled byte (255 for key refresh).
     * Includes split events without doing a connection lookup in the RX path. */
    TOTEM_HEVT_ENCRYPT_EVENT = 36,
    /* LE host security processing: i/a=handle low/high, r=0 entry,
     * 1 after L2CAP encryption callbacks, 2 return; w=HCI status,
     * x=security level at this boundary. Event 4/5 is application observation. */
    TOTEM_HEVT_SECURITY_TIMING = 37,
    /* Security exchange: i/a=handle low/high. r=0 SMP request submission,
     * 1 submission return (w=error class,x=absolute result),
     * 2 controller RX / 3 TX queued (w=LL opcode),
     * 4 key reply entry / 5 return (w=HCI result,x=1 positive/0 negative),
     * 6/7 host LTK request arrival/dequeue. No key or payload retained. */
    TOTEM_HEVT_SECURITY_EXCHANGE = 38,
    /* nRF52840 handoff-only termination TX: i/a=handle low/high.
     * r=0 radio packet setups, 1 observed hardware TX ENDs, 2 setups without END;
     * w/x=saturating 16-bit count. r=3 first setup, 4 first END, 5 last END:
     * t=captured uptime, x=1 known or 0 unknown (snapshot timestamp instead).
     * Setup is not transmission; END is not proof of reception or ACK.
     * Repeated ENDs describe retransmissions of this termination procedure. */
    TOTEM_HEVT_HANDOFF_TX = 39,
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

/** Append a captured timestamp from a worker, without scheduling persistence. */
void totem_host_event_log_record_timing(uint32_t uptime_ms, uint8_t type, int8_t idx,
                                       int8_t active, uint8_t reason, uint8_t pair, uint8_t extra);

/** Stream a numbered journal/RAM snapshot (framed Studio CDC or plain printk). */
void totem_host_event_log_dump(void);

/** Queue a dump from a transport/control callback; never prints inline there. */
void totem_host_event_log_request_dump(void);

/** Request an immediate settings save, subject to the flash rate limit. */
void totem_host_event_log_persist(void);

/** Record a non-host diagnostic transition using the shared compact schema. */
static inline void totem_diag_log_record(uint8_t type, int8_t idx, int8_t active, uint8_t reason,
                                         uint8_t extra) {
    totem_host_event_log_record(type, idx, active, reason, 0, extra);
}
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
static inline void totem_host_event_log_request_dump(void) {}
static inline void totem_host_event_log_persist(void) {}
static inline void totem_diag_log_record(uint8_t type, int8_t idx, int8_t active, uint8_t reason,
                                         uint8_t extra) {
    (void)type;
    (void)idx;
    (void)active;
    (void)reason;
    (void)extra;
}
#endif

#ifdef __cplusplus
}
#endif
