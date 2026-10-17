"""Check recovery timing using the actual reconnect-watch functions."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define IS_ENABLED(x) x
#define CONFIG_TOTEM_ADV_BOOST 1
#define CONFIG_TOTEM_ADV_BOOST_SEC 20
#define RECONNECT_WATCH_STEP_SEC 8
#define K_SECONDS(x) (x)
#define ARG_UNUSED(x) ((void)(x))
#define TOTEM_BLE_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define totem_host_event_log_record(...) ((void)0)
#define ZMK_EV_EVENT_BUBBLE 0
typedef int zmk_event_t;
static int profile, schedules, delay;
static bool usb, connected, open_profile, suppressed;
static int reconnect_watch_work, watch_profile_index;
static int next_step;
static bool ladder_from_active_down;
enum { RECONNECT_STEP_NONE, RECONNECT_STEP_READV };
static bool totem_hid_is_usb(void) { return usb; }
static bool totem_usb_cable_up(void) { return usb; }
static bool zmk_ble_totem_ads_suppressed(void) { return suppressed; }
static bool zmk_ble_active_profile_is_open(void) { return open_profile; }
static bool zmk_ble_active_profile_is_connected(void) { return connected; }
static int zmk_ble_active_profile_index(void) { return profile; }
static void reconnect_watch_reset(void) { next_step=RECONNECT_STEP_NONE; }
static int k_work_reschedule(int *work, int seconds) {
    assert(work==&reconnect_watch_work); schedules++; delay=seconds; return 0;
}
/* FUNCTIONS */
int main(void) {
    profile=2; /* Restore a nonzero profile before its first host connection. */
    assert(reconnect_watch_settings_commit()==0 && watch_profile_index==2);
    reconnect_watch_profile_changed(NULL);
    assert(schedules==0);

    /* Both orders of the disconnect callbacks must preserve eight seconds. */
    reconnect_watch_arm_light();
    reconnect_watch_profile_changed(NULL);
    assert(schedules==1 && delay==8 && ladder_from_active_down);
    reconnect_watch_reset(); schedules=0;
    reconnect_watch_profile_changed(NULL);
    reconnect_watch_arm_light();
    assert(schedules==1 && delay==8 && ladder_from_active_down);

    /* A real host switch replaces the old ladder with the full recovery. */
    profile=0;
    reconnect_watch_profile_changed(NULL);
    assert(schedules==2 && delay==20 && !ladder_from_active_down);
    reconnect_watch_profile_changed(NULL);
    assert(schedules==2);

    /* Switching while USB is active or the host is up cancels recovery. */
    usb=true; profile=1;
    reconnect_watch_profile_changed(NULL);
    assert(schedules==2 && next_step==RECONNECT_STEP_NONE);
    usb=false; connected=true; profile=2;
    reconnect_watch_profile_changed(NULL);
    assert(schedules==2 && next_step==RECONNECT_STEP_NONE);
    return 0;
}
"""


class ReconnectWatchTests(unittest.TestCase):
    def test_link_notifications_do_not_replace_light_recovery(self):
        source = (Path(__file__).resolve().parents[1] / "src/reconnect_watch.c").read_text()
        functions = []
        for start_marker, end_marker in (
            ("static int reconnect_watch_first_delay_sec(", "/* Totem helpers"),
            ("static void reconnect_watch_arm_full(", "/* Public export"),
            ("static int reconnect_watch_profile_changed(", "ZMK_LISTENER(totem_reconnect_watch"),
            ("static int reconnect_watch_settings_commit(", "SETTINGS_STATIC_HANDLER_DEFINE(totem_watch"),
        ):
            start = source.index(start_marker)
            functions.append(source[start:source.index(end_marker, start)])
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_reconnect")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=HARNESS.replace("/* FUNCTIONS */", "\n".join(functions)),
                text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
