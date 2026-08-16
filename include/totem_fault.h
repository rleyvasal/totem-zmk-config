/*
 * Post-mortem record that survives a reset.
 *
 * The 2026-07-26 freeze left no evidence at all: the left half sat powered but not
 * executing for five hours and the only thing recoverable afterwards was "it was
 * dead". A hardware watchdog fixes the recovery but says nothing about the cause.
 * This is the cause half.
 *
 * Two producers, one consumer:
 *   - k_sys_fatal_error_handler() (src/totem_fault.c) -- a CRASH. Records the
 *     Zephyr K_ERR_* reason plus the faulting PC/LR, which addr2line resolves to a
 *     source line against the matching zmk.elf.
 *   - the task watchdog callback (src/totem_watchdog.c) -- a HANG. Records which
 *     software channel stopped being fed, i.e. which subsystem stopped running.
 *   - totem_fault_report_and_clear() prints whichever happened at the next boot.
 *
 * Storage is a __noinit RAM struct: nRF52 keeps RAM powered across a soft reset and
 * Zephyr does not zero __noinit, so the record survives a watchdog or software
 * reset. It does NOT survive a power cut or brownout, hence the magic + CRC -- after
 * a cold start the bytes are garbage and must be rejected rather than reported as a
 * fault that never happened.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum totem_fault_kind {
    TOTEM_FAULT_NONE = 0,
    TOTEM_FAULT_CRASH = 1,    /* k_sys_fatal_error_handler fired */
    TOTEM_FAULT_TASK_WDT = 2, /* a task watchdog channel expired */
};

/* Field order matters: the CRC covers everything from `kind` onward. */
struct totem_fault_record {
    uint32_t magic;
    uint32_t crc;
    uint8_t kind;
    int8_t channel;   /* task watchdog channel id, -1 when N/A */
    uint16_t reason;  /* K_ERR_* for a crash, 0 for a hang */
    uint32_t pc;      /* faulting program counter (crash only) */
    uint32_t lr;      /* link register (crash only) */
    uint32_t uptime_ms;
    uint32_t thread;  /* k_current_get(), for when the name is unavailable */
    char label[20];   /* thread name, or the watchdog channel label */
};

/** Record a hang: `label` names the subsystem whose channel stopped feeding. */
void totem_fault_note_task_wdt(int channel_id, const char *label);

/** printk any stored record, then invalidate it so it is reported exactly once. */
void totem_fault_report_and_clear(void);

#ifdef __cplusplus
}
#endif
