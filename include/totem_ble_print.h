/*
 * Mirror totem_ble LOG_* lines to printk so the Studio board CDC (usbmodem101)
 * can stream them without enabling CONFIG_LOG.
 */
#pragma once

#include <zephyr/sys/printk.h>

#define TOTEM_BLE_INF(fmt, ...)                                                                        \
    do {                                                                                               \
        LOG_INF(fmt, ##__VA_ARGS__);                                                                   \
        printk(fmt "\n", ##__VA_ARGS__);                                                               \
    } while (0)

#define TOTEM_BLE_WRN(fmt, ...)                                                                        \
    do {                                                                                               \
        LOG_WRN(fmt, ##__VA_ARGS__);                                                                   \
        printk(fmt "\n", ##__VA_ARGS__);                                                               \
    } while (0)

#define TOTEM_BLE_ERR(fmt, ...)                                                                        \
    do {                                                                                               \
        LOG_ERR(fmt, ##__VA_ARGS__);                                                                   \
        printk(fmt "\n", ##__VA_ARGS__);                                                               \
    } while (0)
