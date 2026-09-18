/*
 * Studio UART RPC only starts RX when the selected HID endpoint is USB
 * (zmk/app/src/studio/rpc.c refresh_selected_transport). A Mac that is still
 * on BLE can open the CDC port and get silence. On a Studio image, prefer USB
 * whenever the cable is up so RPC listens without turning Bluetooth off.
 *
 * Unplugging USB lets ZMK fall back to BLE as usual and restarts host ads
 * for the profile chosen while the cable was in. Do not churn advertising
 * on BT_SEL while USB is up (nRF52840 USB HID stalls).
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

#include <totem_host_event_log.h>

void zmk_studio_uart_rearm(void);

#if IS_ENABLED(CONFIG_ZMK_STUDIO) && IS_ENABLED(CONFIG_ZMK_USB)

static void totem_studio_prefer_usb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(totem_studio_usb_work, totem_studio_prefer_usb);

static void totem_studio_prefer_usb(struct k_work *work) {
    static uint8_t tries;
    int err;
    int selected;

    ARG_UNUSED(work);

    if (!zmk_usb_is_powered()) {
        tries = 0;
        /* Cable out: apply the profile selected while USB was up. */
        zmk_ble_totem_wake_ads();
        return;
    }
    zmk_studio_uart_rearm();
    err = zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB);
    selected = (int)zmk_endpoint_get_selected().transport;
    if (selected == (int)ZMK_TRANSPORT_USB) {
        tries = 0;
        return;
    }
    if (tries < 20) {
        tries++;
        if (tries == 1 || tries == 20) {
            printk("totem_studio prefer_usb err=%d selected=%d try=%u\n", err, selected, tries);
        }
        k_work_reschedule(&totem_studio_usb_work, K_MSEC(250));
    }
}

static void totem_studio_usb_kick(void) {
    k_work_reschedule(&totem_studio_usb_work, K_MSEC(200));
}

static int totem_studio_usb_listener(const zmk_event_t *eh) {
    const struct zmk_usb_conn_state_changed *usb = as_zmk_usb_conn_state_changed(eh);
    const struct zmk_endpoint_changed *endpoint = as_zmk_endpoint_changed(eh);
    if (usb != NULL) {
        totem_diag_log_record(TOTEM_HEVT_USB, -1, -1, (uint8_t)usb->conn_state,
                              (uint8_t)zmk_endpoint_get_selected().transport);
    } else if (endpoint != NULL) {
        totem_diag_log_record(TOTEM_HEVT_USB, -1, -1, 0xFF,
                              (uint8_t)endpoint->endpoint.transport);
    }
    totem_studio_usb_kick();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_studio_usb, totem_studio_usb_listener);
ZMK_SUBSCRIPTION(totem_studio_usb, zmk_usb_conn_state_changed);
ZMK_SUBSCRIPTION(totem_studio_usb, zmk_endpoint_changed);

static int totem_studio_usb_init(void) {
    k_work_schedule(&totem_studio_usb_work, K_MSEC(1500));
    return 0;
}

SYS_INIT(totem_studio_usb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif
