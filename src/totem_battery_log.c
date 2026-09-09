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

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

#include <totem_studio_log.h>

__attribute__((weak)) int totem_studio_send_battery(const char *line) {
    ARG_UNUSED(line);
    return -ENOTSUP;
}

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

static int64_t last_event_print_ms;
static int last_printed_local = -1;
static int last_printed_periph = -2;

static bool batt_event_should_print(void) {
    int local = zmk_battery_state_of_charge();
    int64_t now = k_uptime_get();

    if (local == last_printed_local && peripheral_soc == last_printed_periph) {
        return false;
    }
    if (last_event_print_ms != 0 && (now - last_event_print_ms) < BATT_EVENT_MIN_INTERVAL_MS) {
        return false;
    }

    last_event_print_ms = now;
    last_printed_local = local;
    last_printed_periph = peripheral_soc;
    return true;
}

static bool left_on_usb(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

/* Compact levels for the Studio 'B' frame and for printk. While USB is in,
 * left=NN usb is the last cell reading (not charge voltage). */
static void format_levels(char *buf, size_t cap) {
    int cell = zmk_battery_last_cell_soc();

    if (left_on_usb()) {
        if (cell >= 0) {
            if (peripheral_soc < 0) {
                snprintk(buf, cap, "left=%d usb right=--", cell);
            } else {
                snprintk(buf, cap, "left=%d usb right=%d", cell, peripheral_soc);
            }
        } else if (peripheral_soc < 0) {
            snprintk(buf, cap, "left=usb right=--");
        } else {
            snprintk(buf, cap, "left=usb right=%d", peripheral_soc);
        }
        return;
    }
    if (peripheral_soc < 0) {
        snprintk(buf, cap, "left=%u right=--", zmk_battery_state_of_charge());
    } else {
        snprintk(buf, cap, "left=%u right=%d", zmk_battery_state_of_charge(), peripheral_soc);
    }
}

static void totem_batt_push_studio(void) {
    char levels[40];

    format_levels(levels, sizeof(levels));
    (void)totem_studio_send_battery(levels);
}

static void totem_batt_print(const char *why, bool with_mv) {
    uint32_t up_s = k_uptime_get_32() / 1000;
    char levels[40];
    uint16_t mv = zmk_battery_millivolts();

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
    totem_batt_push_studio();
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

    if (as_zmk_battery_state_changed(eh) != NULL && batt_event_should_print()) {
        totem_batt_print("local", false);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_battery_log, totem_batt_listener);
ZMK_SUBSCRIPTION(totem_battery_log, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(totem_battery_log, zmk_peripheral_battery_state_changed);

static void totem_batt_tick_handler(struct k_work *work) {
    /* Deliberately bypasses the rate limit: the tick IS the measurement, and a flat
     * battery over a quiet hour must still produce samples. */
    totem_batt_print("tick", true);
    k_work_reschedule(k_work_delayable_from_work(work),
                      K_SECONDS(CONFIG_TOTEM_BATTERY_LOG_INTERVAL_SEC));
}

static K_WORK_DELAYABLE_DEFINE(totem_batt_tick, totem_batt_tick_handler);

static void totem_batt_studio_handler(struct k_work *work) {
    char levels[40];
    int rc;

    if (!left_on_usb()) {
        k_work_reschedule(k_work_delayable_from_work(work), K_SECONDS(10));
        return;
    }
    format_levels(levels, sizeof(levels));
    rc = totem_studio_send_battery(levels);
    /* Retry sooner until the configurator has DTR so Connect shows L/R chips
     * without Enable log or the 5 min printk tick. */
    k_work_reschedule(k_work_delayable_from_work(work), rc == 0 ? K_SECONDS(10) : K_SECONDS(2));
}

static K_WORK_DELAYABLE_DEFINE(totem_batt_studio, totem_batt_studio_handler);

static int totem_battery_log_init(void) {
    /* First tick deliberately early: a drain measurement wants a starting point
     * shortly after boot, not one interval later. */
    k_work_schedule(&totem_batt_tick, K_SECONDS(10));
    k_work_schedule(&totem_batt_studio, K_SECONDS(2));
    return 0;
}

SYS_INIT(totem_battery_log_init, APPLICATION, 99);
