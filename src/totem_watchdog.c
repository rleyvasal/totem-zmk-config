/*
 * Hardware watchdog + reset-reason capture.
 *
 * Why this exists (2026-07-26): the left half was found powered but not executing
 * -- no USB enumeration, no BLE advertising, no keypress handling, for five hours.
 * Battery was ~80%, the charge LED was lit, and the same cable flashed the board
 * minutes later, so power/cable/battery are all ruled out. Nothing in the firmware
 * could recover it: every recovery path we had (ADV_RECONCILE, RECONNECT_WATCH,
 * even sys_reboot in RECOVERY_REBOOT) is *software*, and software does not run on a
 * core that has stopped. Only a hardware timer can break that state.
 *
 * The duration argues for a hang (deadlock / blocked workqueue / interrupt storm)
 * rather than a crash: a Zephyr fatal error would have taken the reset path within
 * seconds and left a working keyboard. A hang has no handler at all.
 *
 * Design notes:
 *   - nRF52840 WDT keeps running in SYSTEM ON sleep, which is what we need: the
 *     keyboard is idle almost all the time, and a hang while idle is exactly the
 *     failure being caught.
 *   - The WDT cannot be stopped once started (nRF hardware). That is a feature
 *     here, but it means the feed path must be extremely boring: one thread, no
 *     locks, no allocation, nothing that can itself block.
 *   - Feeding from a LOW priority thread is deliberate. If a high-priority thread
 *     spins or the system workqueue wedges, a high-priority feeder would happily
 *     keep petting the dog while the keyboard is useless. Starving the feeder is
 *     the signal we want.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/device.h>
#include <zephyr/sys/printk.h>

#if IS_ENABLED(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#include <totem_host_event_log.h>

LOG_MODULE_REGISTER(totem_watchdog, CONFIG_ZMK_LOG_LEVEL);

/* --- reset reason ------------------------------------------------------------
 *
 * Read once at boot, before anything can clear it. Recorded into the host event
 * ring so it survives into the next dump: after an incident the first question is
 * "did the watchdog fire, or did it brown out, or was this a normal power-on?" and
 * without this the answer is unknowable.
 */

#if IS_ENABLED(CONFIG_TOTEM_RESET_REASON_CAPTURE) && IS_ENABLED(CONFIG_HWINFO)

/* Compressed to one byte for the ring's `reason` field. Bit order is ours, not
 * the hardware's -- hwinfo already normalises across SoCs. */
#define TOTEM_RR_PIN 0x01
#define TOTEM_RR_SOFTWARE 0x02
#define TOTEM_RR_WATCHDOG 0x04
#define TOTEM_RR_BROWNOUT 0x08
#define TOTEM_RR_POR 0x10
#define TOTEM_RR_DEBUG 0x20
#define TOTEM_RR_LOW_POWER 0x40
#define TOTEM_RR_OTHER 0x80

static uint8_t totem_reset_flags;

static uint8_t compress_reset_cause(uint32_t cause) {
    uint8_t out = 0;

    if (cause & RESET_PIN) {
        out |= TOTEM_RR_PIN;
    }
    if (cause & RESET_SOFTWARE) {
        out |= TOTEM_RR_SOFTWARE;
    }
    if (cause & RESET_WATCHDOG) {
        out |= TOTEM_RR_WATCHDOG;
    }
    if (cause & RESET_BROWNOUT) {
        out |= TOTEM_RR_BROWNOUT;
    }
    if (cause & RESET_POR) {
        out |= TOTEM_RR_POR;
    }
    if (cause & RESET_DEBUG) {
        out |= TOTEM_RR_DEBUG;
    }
    if (cause & RESET_LOW_POWER_WAKE) {
        out |= TOTEM_RR_LOW_POWER;
    }
    if (cause != 0 && out == 0) {
        out |= TOTEM_RR_OTHER;
    }

    return out;
}

static void totem_capture_reset_reason(void) {
    uint32_t cause = 0;
    int err = hwinfo_get_reset_cause(&cause);

    if (err) {
        LOG_WRN("totem_wdt reset_reason unavailable err=%d", err);
        return;
    }

    totem_reset_flags = compress_reset_cause(cause);

    /* printk, not LOG_*: this must be readable on a console-only production build
     * where the logging subsystem is compiled out (see TOTEM_USB_CONSOLE). */
    printk("totem_wdt boot reset_cause=0x%08x flags=0x%02x%s\n", cause, totem_reset_flags,
           (totem_reset_flags & TOTEM_RR_WATCHDOG) ? " WATCHDOG-FIRED" : "");

    if (totem_reset_flags & TOTEM_RR_WATCHDOG) {
        LOG_ERR("totem_wdt previous boot ended in a WATCHDOG RESET -- the firmware hung");
    }

    /* idx/active are meaningless for a boot event; -1 keeps them out of the way. */
    totem_host_event_log_record(TOTEM_HEVT_BOOT, -1, -1, totem_reset_flags, 0, 0);
    totem_host_event_log_persist();

    /* Clear so the *next* boot reports its own cause and not a sticky union of
     * every reset this board has ever taken (nRF RESETREAS is cumulative). */
    hwinfo_clear_reset_cause();
}

#else /* capture disabled, or no HWINFO backend on this SoC */

static void totem_capture_reset_reason(void) {}

#endif

/* --- watchdog ---------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_TOTEM_WATCHDOG)

#define WDT_TIMEOUT_MS CONFIG_TOTEM_WATCHDOG_TIMEOUT_MS
/* Feed well inside the window so a merely slow tick never trips it. A third of the
 * timeout tolerates two consecutive missed feeds before the dog bites. */
#define WDT_FEED_INTERVAL_MS (WDT_TIMEOUT_MS / 3)

#define WDT_FEEDER_STACK_SIZE 512
/* Lowest practical priority: see the header comment. If this thread cannot get
 * scheduled for WDT_TIMEOUT_MS, the keyboard is not doing useful work either. */
#define WDT_FEEDER_PRIORITY (CONFIG_NUM_PREEMPT_PRIORITIES - 1)

static const struct device *const wdt_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

K_THREAD_STACK_DEFINE(wdt_feeder_stack, WDT_FEEDER_STACK_SIZE);
static struct k_thread wdt_feeder_thread;

static void wdt_feeder(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        wdt_feed(wdt_dev, wdt_channel);
        k_msleep(WDT_FEED_INTERVAL_MS);
    }
}

static int totem_watchdog_init(void) {
    totem_capture_reset_reason();

    if (wdt_dev == NULL) {
        LOG_ERR("totem_wdt no watchdog0 alias; watchdog DISABLED");
        return 0;
    }
    if (!device_is_ready(wdt_dev)) {
        LOG_ERR("totem_wdt watchdog device not ready; watchdog DISABLED");
        return 0;
    }

    struct wdt_timeout_cfg cfg = {
        .window =
            {
                .min = 0,
                .max = WDT_TIMEOUT_MS,
            },
        /* No callback: nRF gives only ~61 us between the interrupt and the reset,
         * far too little to persist anything. The reset reason on the next boot is
         * the record, which is why RESET_REASON_CAPTURE is the useful half. */
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,
    };

    wdt_channel = wdt_install_timeout(wdt_dev, &cfg);
    if (wdt_channel < 0) {
        LOG_ERR("totem_wdt install failed err=%d; watchdog DISABLED", wdt_channel);
        return 0;
    }

    /* WDT_OPT_PAUSE_HALTED_BY_DBG only: do NOT pause while the CPU is idle. The
     * hang we are chasing looks exactly like an idle CPU to the hardware. */
    int err = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (err) {
        LOG_ERR("totem_wdt setup failed err=%d; watchdog DISABLED", err);
        return 0;
    }

    k_thread_create(&wdt_feeder_thread, wdt_feeder_stack, K_THREAD_STACK_SIZEOF(wdt_feeder_stack),
                    wdt_feeder, NULL, NULL, NULL, WDT_FEEDER_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&wdt_feeder_thread, "totem_wdt");

    LOG_INF("totem_wdt armed timeout=%d ms feed=%d ms channel=%d", WDT_TIMEOUT_MS,
            WDT_FEED_INTERVAL_MS, wdt_channel);
    return 0;
}

#else /* !TOTEM_WATCHDOG: still capture the reset reason */

static int totem_watchdog_init(void) {
    totem_capture_reset_reason();
    return 0;
}

#endif /* CONFIG_TOTEM_WATCHDOG */

/* APPLICATION level, late: the event ring must have loaded from settings first, or
 * the boot record lands in a ring that is about to be overwritten by the restore. */
SYS_INIT(totem_watchdog_init, APPLICATION, 99);
