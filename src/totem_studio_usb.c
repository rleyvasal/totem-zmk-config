/*
 * Studio UART RPC only starts RX when the selected HID endpoint is USB
 * (zmk/app/src/studio/rpc.c refresh_selected_transport). A Mac that is still
 * on BLE can open the CDC port and get silence. On a Studio image, prefer USB
 * whenever the cable is up so RPC listens without turning Bluetooth off.
 *
 * Unplugging USB lets ZMK fall back to BLE as usual.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO) && IS_ENABLED(CONFIG_ZMK_USB)

static void totem_studio_prefer_usb(struct k_work *work) {
    ARG_UNUSED(work);

    if (!zmk_usb_is_powered()) {
        return;
    }
    if (zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB) {
        return;
    }

    int err = zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB);
    printk("totem_studio prefer_usb err=%d selected=%d\n", err,
           (int)zmk_endpoint_get_selected().transport);
}

static K_WORK_DELAYABLE_DEFINE(totem_studio_usb_work, totem_studio_prefer_usb);

static void totem_studio_usb_kick(void) {
    k_work_reschedule(&totem_studio_usb_work, K_MSEC(200));
}

static int totem_studio_usb_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
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
