#pragma once

#include <stdbool.h>

#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>

/* True when HID reports are going to USB. Host BLE may still advertise;
 * exclusive-host must not evict (that storm is what stalls USB typing). */
static inline bool totem_hid_is_usb(void) {
    return zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB;
}
