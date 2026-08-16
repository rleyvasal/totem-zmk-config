/*
 * Print the HID endpoint whenever it changes.
 *
 * Which transport is actually carrying keystrokes has never been visible in any
 * capture we have taken, and it cannot be inferred by typing: keystrokes look
 * identical to the user over USB and BLE.
 *
 * It is worse than merely invisible, because ZMK falls back. get_selected_transport()
 * in zmk/app/src/endpoints.c prefers what you asked for, but silently uses the other
 * transport when the preferred one is not ready. So "I forced BLE, turned Bluetooth
 * off, and typing still worked" proves nothing -- BLE went not-ready and USB took
 * over, exactly as designed. That test cost us a round trip on 2026-07-26.
 *
 * Hence two values, not one:
 *   preferred - what &out asked for (persisted in settings, survives reboot)
 *   selected  - what is actually carrying HID right now, after fallback
 * They differ precisely when the transport you chose is unavailable, which is the
 * case worth seeing.
 *
 * printk, not LOG_*: this has to work on the production image where CONFIG_LOG is
 * off (see TOTEM_USB_CONSOLE).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>

#define EP_STR_LEN 16

static struct zmk_endpoint_instance last_selected;
static struct zmk_endpoint_instance last_preferred;
static bool have_last;

static void totem_print_endpoints(const char *why, bool force) {
    char sel[EP_STR_LEN];
    char pref[EP_STR_LEN];

    struct zmk_endpoint_instance selected = zmk_endpoint_get_selected();
    struct zmk_endpoint_instance preferred = zmk_endpoint_get_preferred();

    if (!force && have_last && zmk_endpoint_instance_eq(selected, last_selected) &&
        zmk_endpoint_instance_eq(preferred, last_preferred)) {
        return;
    }

    last_selected = selected;
    last_preferred = preferred;
    have_last = true;

    if (zmk_endpoint_instance_to_str(selected, sel, sizeof(sel)) < 0) {
        sel[0] = '?';
        sel[1] = '\0';
    }
    if (zmk_endpoint_instance_to_str(preferred, pref, sizeof(pref)) < 0) {
        pref[0] = '?';
        pref[1] = '\0';
    }

    /* The FALLBACK marker is the point of the whole file: it is the only way to tell
     * "my &out press did nothing" from "it worked, but the transport is down". */
    printk("totem_ep %s selected=%s preferred=%s%s\n", why, sel, pref,
           zmk_endpoint_instance_eq(selected, preferred) ? "" : "  FALLBACK");
}

/* Polled, because there is no event for a PREFERRED change.
 * raise_zmk_endpoint_changed() in zmk/app/src/endpoints.c fires only when the
 * SELECTED instance differs, so with BLE down, &out OUT_BLE / OUT_USB move the
 * preference while selected stays USB -- no event, and the keypress looks like it did
 * nothing. That is precisely the confusion this file exists to remove, so the
 * preference has to be sampled. Cost is two getters and a comparison per second, and
 * only a real change prints; deep sleep is off anyway (CONFIG_ZMK_SLEEP=n). */
static void totem_endpoint_poll_handler(struct k_work *work) {
    totem_print_endpoints("changed", false);
    k_work_reschedule(k_work_delayable_from_work(work), K_MSEC(1000));
}

static K_WORK_DELAYABLE_DEFINE(totem_endpoint_poll_work, totem_endpoint_poll_handler);

static int totem_endpoint_changed_listener(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *ev = as_zmk_endpoint_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    totem_print_endpoints("changed", false);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_endpoint_log, totem_endpoint_changed_listener);
ZMK_SUBSCRIPTION(totem_endpoint_log, zmk_endpoint_changed);

/* Also report once at startup: the preferred endpoint is restored from settings, so
 * a BLE preference set days ago is in force at boot with nothing on screen to say so.
 * Deferred for the same reason as the profile dump -- settings_load() runs in ZMK's
 * main(), after every SYS_INIT level, so reading at init would report the default
 * rather than the stored value. */
static void totem_endpoint_boot_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    totem_print_endpoints("boot", true);
}

static K_WORK_DELAYABLE_DEFINE(totem_endpoint_boot_work, totem_endpoint_boot_work_handler);

static int totem_endpoint_log_init(void) {
    k_work_schedule(&totem_endpoint_boot_work,
                    K_MSEC(CONFIG_TOTEM_BOOT_PROFILE_DUMP_DELAY_MS));
    /* Start polling after the boot report so the first sample is the baseline. */
    k_work_schedule(&totem_endpoint_poll_work,
                    K_MSEC(CONFIG_TOTEM_BOOT_PROFILE_DUMP_DELAY_MS + 1000));
    return 0;
}

SYS_INIT(totem_endpoint_log_init, APPLICATION, 99);
