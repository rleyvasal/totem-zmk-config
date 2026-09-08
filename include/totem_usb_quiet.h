#pragma once

#include <stdbool.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

/* True when the left half is on USB power/HID. Split BLE to the right half
 * must stay up; host advertising and host connections must not. */
static inline bool totem_usb_owns_hid(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}
