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
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>

#if IS_ENABLED(CONFIG_TOTEM_BOOT_PROFILE_DUMP)
#include <zephyr/bluetooth/addr.h>
#include <zmk/ble.h>
#endif

#if IS_ENABLED(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#include <totem_fault.h>
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
        printk("totem_wdt reset_reason unavailable err=%d\n", err);
        return;
    }

    totem_reset_flags = compress_reset_cause(cause);

    /* printk, not LOG_*: this must be readable on a console-only production build
     * where the logging subsystem is compiled out (see TOTEM_USB_CONSOLE). */
    printk("totem_wdt boot reset_cause=0x%08x flags=0x%02x%s\n", cause, totem_reset_flags,
           (totem_reset_flags & TOTEM_RR_WATCHDOG) ? " WATCHDOG-FIRED" : "");

    if (totem_reset_flags & TOTEM_RR_WATCHDOG) {
        printk("totem_wdt previous boot ended in a WATCHDOG RESET -- the firmware hung\n");
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

/* --- stored profiles ---------------------------------------------------------
 *
 * "Which host does each profile actually hold?" answered every BLE incident this
 * month -- active=4 on 07-25, and all-zeroes on profile 0 (bonds gone) on 07-26 --
 * but the only source was ZMK's `Loaded ... address for profile N`, a LOG_DBG.
 * TOTEM_USB_CONSOLE deliberately leaves CONFIG_LOG off, so on a production image
 * that line does not exist and the question needed a reflash to a debug build to
 * answer. printk it here instead, so the most useful diagnostic is available on the
 * firmware actually being used.
 *
 * TIMING (got this wrong once -- 2026-07-26): profiles are NOT readable from any
 * SYS_INIT. zmk_ble_init() at SYS_INIT(APPLICATION, 50) only calls
 * settings_register(&profiles_handler); the actual settings_load() happens in ZMK's
 * main() (app/src/main.c), which runs after every SYS_INIT level has completed. A
 * dump at APPLICATION/99 therefore prints 00:00:00:00:00:00 for every profile and
 * reports EMPTY on a keyboard whose bonds are perfectly intact -- exactly the false
 * alarm this feature exists to detect, which is how it was caught: the dump said
 * profile 0 was empty while the user was typing over BLE on profile 0.
 *
 * So it runs from delayed work instead, well after main() has loaded settings.
 */

#if IS_ENABLED(CONFIG_TOTEM_BOOT_PROFILE_DUMP)
static void totem_dump_profiles(void) {
    int active = zmk_ble_active_profile_index();

    printk("totem_prof active=%d count=%d\n", active, ZMK_BLE_PROFILE_COUNT);

    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        bt_addr_le_t *addr = zmk_ble_profile_address((uint8_t)i);
        char addr_str[BT_ADDR_LE_STR_LEN];
        bool empty = (addr == NULL) || (bt_addr_le_cmp(addr, BT_ADDR_LE_ANY) == 0);

        if (addr == NULL) {
            snprintk(addr_str, sizeof(addr_str), "(null)");
        } else {
            bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
        }

        /* EMPTY on the active profile is the 07-25 trap (exclusive host evicts the
         * only bonded computer); EMPTY on a profile whose host still shows the
         * keyboard as paired is the 07-26 asymmetric bond and needs a re-pair. */
        printk("totem_prof %d%s %s%s\n", i, (i == active) ? "*" : " ", addr_str,
               empty ? "  EMPTY" : "");
    }
}

static void totem_profile_dump_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    totem_dump_profiles();
}

static K_WORK_DELAYABLE_DEFINE(totem_profile_dump_work, totem_profile_dump_work_handler);

/* Scheduled from SYS_INIT but deliberately fired late: settings_load() runs in main(),
 * after all SYS_INIT levels. The delay only has to outlast that, and a few seconds also
 * gives USB time to enumerate so the line is not printed into a console nobody is
 * attached to yet. */
static void totem_schedule_profile_dump(void) {
    k_work_schedule(&totem_profile_dump_work, K_MSEC(CONFIG_TOTEM_BOOT_PROFILE_DUMP_DELAY_MS));
}
#else
static void totem_schedule_profile_dump(void) {}
#endif

/* --- watchdog ---------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_TOTEM_WATCHDOG)

/* Layer 2: Zephyr's task watchdog on top of the hardware WDT.
 *
 * A single feeder can only ever say "the firmware stopped". Multiple software
 * channels, each fed by a different execution context, say WHICH context stopped --
 * and that is the difference between a diagnosis and a shrug:
 *
 *   sched   - the lowest-priority thread. Stops when the scheduler stops running
 *             normal threads at all (a spinning higher-priority thread, or a core
 *             that has stopped scheduling).
 *   sysworkq- a self-rescheduling delayed work item. Stops when the system
 *             workqueue wedges. This is the prime suspect for the 07-26 freeze:
 *             most ZMK and BLE work runs here, and one blocking call in a work
 *             handler stalls everything behind it while ISRs keep running.
 *   timer   - a k_timer expiry (ISR context). Keeps feeding even when threads are
 *             starved, so "only timer survived" localises the fault to thread
 *             scheduling rather than the whole core.
 *
 * task_wdt drives the hardware WDT itself (task_wdt_init installs its own timeout),
 * so this must not also call wdt_install_timeout.
 */

#define WDT_TIMEOUT_MS CONFIG_TOTEM_WATCHDOG_TIMEOUT_MS
/* Feed well inside the window so a merely slow tick never trips it. A third of the
 * timeout tolerates two consecutive missed feeds before the dog bites. */
#define WDT_FEED_INTERVAL_MS (WDT_TIMEOUT_MS / 3)

#define WDT_FEEDER_STACK_SIZE 512
/* Lowest practical priority: see the header comment. If this thread cannot get
 * scheduled for WDT_TIMEOUT_MS, the keyboard is not doing useful work either. */
#define WDT_FEEDER_PRIORITY (CONFIG_NUM_PREEMPT_PRIORITIES - 1)

static const struct device *const wdt_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));

static int chan_sched = -1;
static int chan_workq = -1;
static int chan_timer = -1;

K_THREAD_STACK_DEFINE(wdt_feeder_stack, WDT_FEEDER_STACK_SIZE);
static struct k_thread wdt_feeder_thread;

static const char *chan_label(int channel_id) {
    if (channel_id == chan_sched) {
        return "sched";
    }
    if (channel_id == chan_workq) {
        return "sysworkq";
    }
    if (channel_id == chan_timer) {
        return "timer";
    }
    return "unknown";
}

/* ISR CONTEXT. task_wdt_trigger() is a k_timer expiry function, so this runs from
 * the system timer interrupt: no blocking, no flash writes, no settings_save. Record
 * to __noinit RAM and reset; totem_fault_report_and_clear() prints it next boot. */
static void totem_task_wdt_cb(int channel_id, void *user_data) {
    ARG_UNUSED(user_data);

    const char *label = chan_label(channel_id);

    totem_fault_note_task_wdt(channel_id, label);
    printk("\n*** totem_wdt HANG: channel %d (%s) stopped feeding; resetting ***\n", channel_id,
           label);

    sys_reboot(SYS_REBOOT_COLD);
}

static void wdt_feeder(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        task_wdt_feed(chan_sched);
        k_msleep(WDT_FEED_INTERVAL_MS);
    }
}

static void wdt_workq_handler(struct k_work *work) {
    task_wdt_feed(chan_workq);
    k_work_reschedule(k_work_delayable_from_work(work), K_MSEC(WDT_FEED_INTERVAL_MS));
}

static K_WORK_DELAYABLE_DEFINE(wdt_workq_work, wdt_workq_handler);

/* ISR context; task_wdt_feed() takes a spinlock, so this is safe here. */
static void wdt_timer_handler(struct k_timer *t) {
    ARG_UNUSED(t);
    task_wdt_feed(chan_timer);
}

static K_TIMER_DEFINE(wdt_timer, wdt_timer_handler, NULL);

static int totem_watchdog_init(void) {
    totem_capture_reset_reason();
    totem_fault_report_and_clear();
    totem_schedule_profile_dump();

    if (wdt_dev == NULL) {
        printk("totem_wdt no watchdog0 alias; watchdog DISABLED\n");
        return 0;
    }
    if (!device_is_ready(wdt_dev)) {
        printk("totem_wdt watchdog device not ready; watchdog DISABLED\n");
        return 0;
    }

    int err = task_wdt_init(wdt_dev);

    if (err) {
        printk("totem_wdt task_wdt_init failed err=%d; watchdog DISABLED\n", err);
        return 0;
    }

    chan_sched = task_wdt_add(WDT_TIMEOUT_MS, totem_task_wdt_cb, NULL);
    chan_workq = task_wdt_add(WDT_TIMEOUT_MS, totem_task_wdt_cb, NULL);
    chan_timer = task_wdt_add(WDT_TIMEOUT_MS, totem_task_wdt_cb, NULL);

    if (chan_sched < 0 || chan_workq < 0 || chan_timer < 0) {
        printk("totem_wdt task_wdt_add failed (%d/%d/%d); watchdog DISABLED\n", chan_sched,
               chan_workq, chan_timer);
        return 0;
    }

    k_thread_create(&wdt_feeder_thread, wdt_feeder_stack, K_THREAD_STACK_SIZEOF(wdt_feeder_stack),
                    wdt_feeder, NULL, NULL, NULL, WDT_FEEDER_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&wdt_feeder_thread, "totem_wdt");

    k_work_reschedule(&wdt_workq_work, K_MSEC(WDT_FEED_INTERVAL_MS));
    k_timer_start(&wdt_timer, K_MSEC(WDT_FEED_INTERVAL_MS), K_MSEC(WDT_FEED_INTERVAL_MS));

    printk("totem_wdt armed timeout=%d ms feed=%d ms channels sched=%d sysworkq=%d timer=%d\n",
           WDT_TIMEOUT_MS, WDT_FEED_INTERVAL_MS, chan_sched, chan_workq, chan_timer);
    return 0;
}

#else /* !TOTEM_WATCHDOG: still capture the reset reason */

static int totem_watchdog_init(void) {
    totem_capture_reset_reason();
    totem_fault_report_and_clear();
    totem_schedule_profile_dump();
    return 0;
}

#endif /* CONFIG_TOTEM_WATCHDOG */

/* APPLICATION level, late: the event ring must have loaded from settings first, or
 * the boot record lands in a ring that is about to be overwritten by the restore. */
SYS_INIT(totem_watchdog_init, APPLICATION, 99);
