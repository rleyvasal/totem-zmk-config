#pragma once

#include <stdbool.h>

#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

/* True when HID reports are going to USB. Host BLE may still advertise;
 * exclusive-host must not evict (that storm is what stalls USB typing). */
static inline bool totem_hid_is_usb(void) {
    return zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB;
}

/* Cable is in, even if ZMK has not selected USB HID yet (endpoint NONE
 * while BLE profile 0 is down). Eviction and ad-dark must not run. */
static inline bool totem_usb_cable_up(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}
