/*
 * Battery logging for both halves, with timestamps, over the USB console.
 *
 * Why not just read Mighty Mitts (or any host-side battery UI): ZMK derives charge
 * from the ADC voltage on the battery divider, and lithium_ion_mv_to_pct() in
 * zmk/app/src/battery.c returns 100 for anything >= 4200 mV. While USB is plugged in
 * the cell is held at charge voltage, so it reads ~100% regardless of real charge.
 * That is why the right half showed 0%, then 100% the moment it was plugged in.
 *
 * ANY battery reading taken while that half is on USB is meaningless. The half being
 * measured must be on its own cell. Print left=usb in that case (do not pretend 100%
 * is SoC). PROXY publishes the right-half level as a second BAS for Mighty Mitts.
 * The LEFT half can be on USB (that is where the console is) while the RIGHT half
 * runs on battery -- the central receives the peripheral's level over the split link.
 *
 * Two sources:
 *   zmk_battery_state_changed             -- this half (central / left)
 *   zmk_peripheral_battery_state_changed  -- the peripheral (right), via split
 * Both fire only on CHANGE, and ZMK samples once a minute, so a slow drain produces
 * long silences. The periodic tick exists so the log has regular samples to fit a
 * slope against rather than sparse step edges.
 *
 * printk, not LOG_*: this must work on the production image where CONFIG_LOG is off.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_BATTERY)
#include <zmk_studio_log/battery.h>
#endif
#if IS_ENABLED(CONFIG_TOTEM_HOST_EVENT_LOG)
#include <zmk/ble.h>
#include <totem_host_event_log.h>
#endif

/* -1 until the peripheral has reported at least once. Distinguishes "right half has
 * not been heard from" (split link down, or it is off) from "right half is at 0%",
 * which look identical if you print an unsigned 0. */
static int peripheral_soc = -1;

/* Rate limit. The peripheral battery event can storm: a failing split read retries
 * and each attempt raises an event, and 2026-07-30 produced ~22 in two seconds, all
 * reporting 0%. Printing one line per event turns that into a burst of printk to a
 * USB CDC console -- slow I/O on the event path, so the diagnostic starts causing the
 * latency it is supposed to measure. Ticks and boot always print; event-driven lines
 * print at most once a second, and repeats of an unchanged value are dropped. */
#define BATT_EVENT_MIN_INTERVAL_MS 1000

#define LEVELS_MAX 48

static int64_t last_event_print_ms;
static char last_printed[LEVELS_MAX];

static void format_levels(char *buf, size_t cap);

static bool batt_event_should_print(void) {
    char levels[LEVELS_MAX];
    int64_t now = k_uptime_get();

    format_levels(levels, sizeof(levels));
    if (strcmp(levels, last_printed) == 0) {
        return false;
    }
    if (last_event_print_ms != 0 && (now - last_event_print_ms) < BATT_EVENT_MIN_INTERVAL_MS) {
        return false;
    }

    last_event_print_ms = now;
    strcpy(last_printed, levels);
    return true;
}

/* ZMK only fills zmk_battery_millivolts() in LITHIUM_VOLTAGE fetch mode; this
 * board uses STATE_OF_CHARGE mode, where it stays 0. The voltage-divider driver
 * still keeps the millivolts of its last fetch, so read that (no ADC access; ZMK's
 * own periodic fetch keeps it fresh). */
static uint16_t battery_mv(void) {
    uint16_t mv = zmk_battery_millivolts();

#if DT_HAS_CHOSEN(zmk_battery)
    if (mv == 0) {
        const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
        struct sensor_value v;

        if (device_is_ready(dev) && sensor_channel_get(dev, SENSOR_CHAN_GAUGE_VOLTAGE, &v) == 0) {
            mv = (uint16_t)(v.val1 * 1000 + v.val2 / 1000);
        }
    }
#endif
    return mv;
}

static bool left_on_usb(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

/* Levels as the 'K' battery frame carries them (zmk-studio-log), e.g.
 * "left=78 usb right=15": on USB, NN usb is the cell level (not charge voltage),
 * NN usb full once the charger is done. */
static void format_levels(char *buf, size_t cap) {
#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_BATTERY)
    (void)zmk_studio_log_battery_snapshot(buf, cap);
#else
    if (peripheral_soc < 0) {
        snprintk(buf, cap, "left=%u right=--", zmk_battery_reported_soc());
    } else {
        snprintk(buf, cap, "left=%u right=%d", zmk_battery_reported_soc(), peripheral_soc);
    }
#endif
}

static void totem_batt_print(const char *why, bool with_mv) {
    uint32_t up_s = k_uptime_get_32() / 1000;
    char levels[LEVELS_MAX];
    uint16_t mv = battery_mv();

    format_levels(levels, sizeof(levels));
    if (with_mv && mv > 0) {
        if (peripheral_soc < 0) {
            printk("totem_batt %s t=%us %s %umV  (no report yet)\n", why, up_s, levels, mv);
        } else {
            printk("totem_batt %s t=%us %s %umV\n", why, up_s, levels, mv);
        }
    } else if (peripheral_soc < 0) {
        printk("totem_batt %s t=%us %s  (no report yet)\n", why, up_s, levels);
    } else {
        printk("totem_batt %s t=%us %s\n", why, up_s, levels);
    }
}

static int totem_batt_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *pev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (pev != NULL) {
        peripheral_soc = pev->state_of_charge;
        if (batt_event_should_print()) {
            totem_batt_print("periph", false);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_BATTERY_CHARGE_STATUS)
    const struct zmk_peripheral_battery_charging_changed *cev =
        as_zmk_peripheral_battery_charging_changed(eh);

    if (cev != NULL) {
        if (batt_event_should_print()) {
            totem_batt_print("periph", false);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    if (as_zmk_battery_state_changed(eh) != NULL && batt_event_should_print()) {
        totem_batt_print("local", false);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_battery_log, totem_batt_listener);
ZMK_SUBSCRIPTION(totem_battery_log, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(totem_battery_log, zmk_peripheral_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_BATTERY_CHARGE_STATUS)
ZMK_SUBSCRIPTION(totem_battery_log, zmk_peripheral_battery_charging_changed);
#endif

static void totem_batt_tick_handler(struct k_work *work) {
    /* Deliberately bypasses the rate limit: the tick IS the measurement, and a flat
     * battery over a quiet hour must still produce samples. */
    totem_batt_print("tick", true);
    k_work_reschedule(k_work_delayable_from_work(work),
                      K_SECONDS(CONFIG_TOTEM_BATTERY_LOG_INTERVAL_SEC));
}

static K_WORK_DELAYABLE_DEFINE(totem_batt_tick, totem_batt_tick_handler);

#if IS_ENABLED(CONFIG_TOTEM_HOST_EVENT_LOG) && CONFIG_TOTEM_DIAG_HEARTBEAT_MIN > 0
/* Journal heartbeat (k=47). The printk tick above only reaches an attached console;
 * after an unexplained reset the journal is the only evidence, so it also needs a
 * periodic power sample. Packing is documented at TOTEM_HEVT_HEARTBEAT. */
static void totem_batt_heartbeat_handler(struct k_work *work) {
    uint16_t word = MIN(battery_mv(), 0x7FFF);

    if (left_on_usb()) {
        word |= 0x8000;
    }
    totem_host_event_log_record(TOTEM_HEVT_HEARTBEAT, (int8_t)zmk_ble_active_profile_index(),
                                (int8_t)peripheral_soc, zmk_battery_state_of_charge(),
                                (uint8_t)word, (uint8_t)(word >> 8));
    k_work_reschedule(k_work_delayable_from_work(work),
                      K_MINUTES(CONFIG_TOTEM_DIAG_HEARTBEAT_MIN));
}

static K_WORK_DELAYABLE_DEFINE(totem_batt_heartbeat, totem_batt_heartbeat_handler);

static void totem_batt_heartbeat_start(void) {
    /* First sample after ZMK's first battery read, so boot voltage is recorded. */
    k_work_schedule(&totem_batt_heartbeat, K_SECONDS(60));
}
#else
static void totem_batt_heartbeat_start(void) {}
#endif

static int totem_battery_log_init(void) {
    /* First tick deliberately early: a drain measurement wants a starting point
     * shortly after boot, not one interval later. */
    k_work_schedule(&totem_batt_tick, K_SECONDS(10));
    totem_batt_heartbeat_start();
    return 0;
}

SYS_INIT(totem_battery_log_init, APPLICATION, 99);
