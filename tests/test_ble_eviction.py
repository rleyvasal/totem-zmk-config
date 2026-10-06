"""Compile and exercise the actual BLE policy callbacks with native mocks."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ARG_UNUSED(x) (void)(x)
#define IS_ENABLED(x) BOND_HEAL
#define K_MSEC(x) (x)
#define ZMK_EV_EVENT_BUBBLE 0
#define TOTEM_HEVT_CONN_FAIL 1
#define TOTEM_HEVT_CONN 2
#define TOTEM_HEVT_PROFILE_CHANGED 3
#define BT_ADDR_LE_STR_LEN 32
#define EXCLUSIVE_HOST_SETTLE_MS 2000
typedef int zmk_event_t;
struct k_work { int unused; };
struct bt_conn { int profile; };
static int last_active_profile=-1, profile=2, active_auth_fail_streak=7;
static int clears, retries, evictions, submissions;
static int exclusive_host_evict_work, exclusive_host_fallback_work;
static bool forced;
static int zmk_ble_active_profile_index(void) { return profile; }
static bool zmk_ble_active_profile_is_connected(void) { return true; }
static bool zmk_ble_active_profile_is_open(void) { return false; }
static const struct bt_conn *bt_conn_get_dst(struct bt_conn *conn) { return conn; }
static int zmk_ble_profile_index(const struct bt_conn *conn) { return conn->profile; }
static void bt_addr_le_to_str(const struct bt_conn *conn, char *out, size_t size) {
    (void)conn; assert(size); out[0]='\0';
}
static void fake_log(const char *format, ...) { (void)format; }
#define TOTEM_BLE_INF(...) fake_log(__VA_ARGS__)
static void log_host_conn_event(const char *tag, struct bt_conn *conn, uint8_t extra) {
    (void)tag; (void)conn; (void)extra;
}
static void totem_host_event_log_record(int kind, int idx, int active, int reason, int win, int extra) {
    (void)kind; (void)idx; (void)active; (void)reason; (void)win; (void)extra;
}
static int thrash_win_count(void) { return 0; }
static void thrash_clear(void) { clears++; }
static void bg_clear_ignores(void) { clears++; }
static void class_a_note_auth_event(bool fail) { assert(!fail); clears++; }
static void exclusive_host_evict_all(bool force) { forced=force; evictions++; }
static void exclusive_host_schedule_retry(void) { retries++; }
static void k_work_submit(int *work) { (void)work; submissions++; }
static void k_work_reschedule(int *work, int timeout) { (void)work; assert(timeout==2000); }
/* ACTUAL_CALLBACKS */
int main(void) {
    /* First link notification establishes the baseline without clearing state. */
    exclusive_host_profile_changed(NULL);
    assert(last_active_profile==2 && clears==0 && !forced && active_auth_fail_streak==7);
    exclusive_host_profile_changed(NULL);
    assert(clears==0 && !forced && active_auth_fail_streak==7);

    /* A connection establishes the restored index before the first key press. */
    last_active_profile=-1;
    struct bt_conn split={.profile=-19};
    exclusive_host_connected(&split, 0);
    assert(last_active_profile==2 && clears==0 && submissions==1);
    struct bt_conn host={.profile=2};
    exclusive_host_connected(&host, 0);
    assert(clears==0 && active_auth_fail_streak==7 && submissions==2);

    /* The first genuine switch still clears safeguards and requests force. */
    profile=0; exclusive_host_profile_changed(NULL);
    assert(clears==3 && forced && last_active_profile==0);
    assert(active_auth_fail_streak==(BOND_HEAL ? 0 : 7));

    active_auth_fail_streak=5;
    exclusive_host_profile_changed(NULL);
    assert(clears==3 && !forced && active_auth_fail_streak==5);
    host.profile=0; exclusive_host_connected(&host, 0);
    assert(clears==3 && active_auth_fail_streak==5 && submissions==3);
    exclusive_host_connected(&host, 5);
    assert(clears==3 && submissions==3);

    forced=true; exclusive_host_fallback_work_handler(NULL);
    assert(!forced && clears==3 && evictions==5 && retries==7);
    return 0;
}
"""


class BleEvictionTests(unittest.TestCase):
    def test_reconnects_preserve_safeguards_and_real_switches_reset_them(self):
        source = (Path(__file__).resolve().parents[1] / "src/exclusive_host.c").read_text()
        boundaries = [
            ("static void exclusive_host_fallback_work_handler(", "\nstatic K_WORK_DEFINE"),
            ("static void exclusive_host_connected(", "\nstatic void exclusive_host_security_changed("),
            ("static int exclusive_host_profile_changed(", "\nZMK_LISTENER"),
        ]
        callbacks = []
        for start_marker, end_marker in boundaries:
            start = source.index(start_marker)
            callbacks.append(source[start:source.index(end_marker, start)])
        harness = HARNESS.replace("/* ACTUAL_CALLBACKS */", "\n".join(callbacks))
        with tempfile.TemporaryDirectory() as directory:
            for bond_heal in (0, 1):
                with self.subTest(bond_heal=bond_heal):
                    executable = str(Path(directory) / f"test_eviction_{bond_heal}")
                    subprocess.run(
                        [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                         f"-DBOND_HEAL={bond_heal}", "-x", "c", "-", "-o", executable],
                        input=harness, text=True, check=True, timeout=30,
                    )
                    subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
