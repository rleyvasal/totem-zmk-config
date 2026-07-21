/*
 * Public export for optional thrash-storm → light reconnect-watch arm.
 * Always light ladder (never full reselect / prof_select).
 */
#pragma once

#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_TOTEM_RECONNECT_WATCH)
void totem_reconnect_watch_arm_if_needed(void);
#else
static inline void totem_reconnect_watch_arm_if_needed(void) {}
#endif
