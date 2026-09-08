/*
 * While USB is providing HID, drop computer BLE links and keep the split.
 *
 * Host advertising is refused in ZMK ble.c (update_advertising / ads_suppressed)
 * whenever the cable is up. This module tears down already-open host connections
 * so the nRF52 radio is not also serving Mac/Windows connection events that
 * starve USB HID.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>

#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>

#include <totem_usb_quiet.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IS_ENABLED(CONFIG_TOTEM_USB_QUIET_HOST)

static void drop_host_conn(struct bt_conn *conn, void *data) {
    ARG_UNUSED(data);
    struct bt_conn_info info;

    if (bt_conn_get_info(conn, &info) != 0) {
        return;
    }
    /* Peripheral role = a computer connected to us. Central role = the right half. */
    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    int err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    printk("totem_ble usb_quiet drop host addr=%s err=%d\n", addr, err);
}

static void usb_host_quiet_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!totem_usb_owns_hid()) {
        printk("totem_ble usb_quiet host_ble=on (cable gone)\n");
        return;
    }

    bt_conn_foreach(BT_CONN_TYPE_LE, drop_host_conn, NULL);
    printk("totem_ble usb_quiet host_ble=off split=keep\n");
}

static K_WORK_DELAYABLE_DEFINE(usb_host_quiet_work, usb_host_quiet_work_handler);

static int usb_host_quiet_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    k_work_reschedule(&usb_host_quiet_work, K_MSEC(50));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(totem_usb_host_quiet, usb_host_quiet_listener);
ZMK_SUBSCRIPTION(totem_usb_host_quiet, zmk_usb_conn_state_changed);

static int usb_host_quiet_init(void) {
    k_work_schedule(&usb_host_quiet_work, K_MSEC(400));
    return 0;
}

SYS_INIT(usb_host_quiet_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif
