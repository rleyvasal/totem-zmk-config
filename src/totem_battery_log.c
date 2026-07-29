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
 * measured must be on its own cell. The LEFT half can be on USB (that is where the
 * console is) while the RIGHT half runs on battery -- the central receives the
 * peripheral's level over the split link, so this still yields a valid curve for the
 * right half.
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

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

/* -1 until the peripheral has reported at least once. Distinguishes "right half has
 * not been heard from" (split link down, or it is off) from "right half is at 0%",
 * which look identical if you print an unsigned 0. */
static int peripheral_soc = -1;

static void totem_batt_print(const char *why) {
    uint32_t up_s = k_uptime_get_32() / 1000;

    if (peripheral_soc < 0) {
        printk("totem_batt %s t=%us left=%u%% right=--  (no report yet)\n", why, up_s,
               zmk_battery_state_of_charge());
    } else {
        printk("totem_batt %s t=%us left=%u%% right=%d%%\n", why, up_s,
               zmk_battery_state_of_charge(), peripheral_soc);
    }
}

static int totem_batt_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *pev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (pev != NULL) {
        peripheral_soc = pev->state_of_charge;
        totem_batt_print("periph");
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (as_zmk_battery_state_changed(eh) != NULL) {
        totem_batt_print("local");
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_battery_log, totem_batt_listener);
ZMK_SUBSCRIPTION(totem_battery_log, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(totem_battery_log, zmk_peripheral_battery_state_changed);

static void totem_batt_tick_handler(struct k_work *work) {
    totem_batt_print("tick");
    k_work_reschedule(k_work_delayable_from_work(work),
                      K_SECONDS(CONFIG_TOTEM_BATTERY_LOG_INTERVAL_SEC));
}

static K_WORK_DELAYABLE_DEFINE(totem_batt_tick, totem_batt_tick_handler);

static int totem_battery_log_init(void) {
    /* First tick deliberately early: a drain measurement wants a starting point
     * shortly after boot, not one interval later. */
    k_work_schedule(&totem_batt_tick, K_SECONDS(10));
    return 0;
}

SYS_INIT(totem_battery_log_init, APPLICATION, 99);
