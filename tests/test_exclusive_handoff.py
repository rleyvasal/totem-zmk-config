"""The profile event must not issue controller commands on the key thread."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define IS_ENABLED(x) x
#define CONFIG_TOTEM_BOND_HEAL 0
#define ARG_UNUSED(x) ((void)(x))
#define TOTEM_BLE_INF(...) ((void)0)
#define ZMK_EV_EVENT_BUBBLE 0
#define TOTEM_HEVT_PROFILE_CHANGED 10
typedef int zmk_event_t;
static int last_active_profile = -1;
static int active, queued, retries, resets;
static int exclusive_host_evict_work;
static int zmk_ble_active_profile_index(void) { return active; }
static bool zmk_ble_active_profile_is_connected(void) { return false; }
static bool zmk_ble_active_profile_is_open(void) { return false; }
static void thrash_clear(void) { resets++; }
static void bg_clear_ignores(void) {}
static void class_a_note_auth_event(bool failed) { assert(!failed); }
static void totem_host_event_log_record(int event, int index, int selected,
                                       int reason, int count, int extra) {
    assert(event == 10 && index == active && selected == active);
    assert(reason == 0 && count == 0 && extra == 0);
}
static void k_work_submit(int *work) {
    assert(work == &exclusive_host_evict_work); queued++;
}
static void exclusive_host_schedule_retry(void) { retries++; }
/* ACTUAL_LISTENER */
int main(void) {
    active = 2; exclusive_host_profile_changed(NULL);
    active = 0; exclusive_host_profile_changed(NULL);
    exclusive_host_profile_changed(NULL); /* Link event is not a new switch. */
    assert(queued == 3 && retries == 3 && resets == 1);
    return 0;
}
"""


class ExclusiveHandoffTests(unittest.TestCase):
    def test_profile_listener_only_queues_eviction(self):
        source = (Path(__file__).resolve().parents[1] / "src/exclusive_host.c").read_text()
        start = source.index("static int exclusive_host_profile_changed(")
        listener = source[start:source.index("\nZMK_LISTENER", start)]
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_exclusive_handoff")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=HARNESS.replace("/* ACTUAL_LISTENER */", listener),
                text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
