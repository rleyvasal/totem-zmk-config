"""Compile actual timing adapters: no controller-path writes or inline draining."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define ARG_UNUSED(x) ((void)(x))
#define IS_ENABLED(x) 0
#define MIN(a,b) ((a)<(b)?(a):(b))
#define TOTEM_HEVT_FLASH_BEGIN 26
#define TOTEM_HEVT_FLASH_END 27
#define TOTEM_HEVT_HCI_DISC 28
#define TOTEM_HEVT_TIMING_DROPPED 30
#define BT_CONN_TYPE_LE 1
typedef enum { BT_CONN_CONNECTED, BT_CONN_DISCONNECT_COMPLETE } bt_conn_state_t;
struct bt_conn { uint8_t type, err; uint16_t handle; };
struct k_spinlock { bool locked; };
typedef int k_spinlock_key_t;
struct k_work { void (*handler)(struct k_work *); };
#define K_WORK_DEFINE(name,handler) struct k_work name = {handler}
typedef unsigned atomic_t;
static unsigned atomic_inc(atomic_t *v) { return (*v)++; }
static uint32_t now;
static uint32_t k_uptime_get_32(void) { return now; }
static int zmk_ble_active_profile_index(void) { return 2; }
static k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) {
    assert(!lock->locked); lock->locked=true; return 0;
}
static void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) {
    assert(lock->locked && key==0); lock->locked=false;
}
static unsigned queued, recorded, writes, state_calls;
static bool inside_real_call;
static int k_work_submit(struct k_work *work) { assert(work); queued++; return 1; }
struct observed { uint32_t t; uint8_t type; int8_t idx, active; uint8_t r,w,x; };
static struct observed events[40];
static void totem_host_event_log_record_timing(uint32_t t, uint8_t type, int8_t idx,
                                             int8_t active, uint8_t r, uint8_t w, uint8_t x) {
    assert(!inside_real_call && recorded<40);
    events[recorded++] = (struct observed){t,type,idx,active,r,w,x};
}
static const char *expected_name;
static const void *expected_value;
static int save_result;
int __real_settings_save_one(const char *name, const void *value, size_t len) {
    assert(name==expected_name && value==expected_value && len==3);
    writes++; inside_real_call=true; now+=2300; inside_real_call=false;
    return save_result;
}
void __real_bt_conn_set_state(struct bt_conn *conn, bt_conn_state_t state) {
    assert(conn && (state==BT_CONN_CONNECTED || state==BT_CONN_DISCONNECT_COMPLETE));
    state_calls++; now+=1;
}
/* ACTUAL_SOURCE */
int main(void) {
    expected_value="abc"; expected_name="th/dlog/0"; now=100;
    assert(__wrap_settings_save_one(expected_name,expected_value,3)==0);
    assert(writes==1 && recorded==0 && timing_count==2);
    struct bt_conn conn={BT_CONN_TYPE_LE,22,0x1ab}; now=2500;
    __wrap_bt_conn_set_state(&conn,BT_CONN_DISCONNECT_COMPLETE);
    assert(state_calls==1 && recorded==0 && timing_count==3);
    now=5000; timing_drain(&timing_work);
    assert(recorded==3 && events[0].t==100 && events[1].t==2400);
    assert(events[0].type==26 && events[1].type==27 && events[0].w==events[1].w);
    assert(events[0].r==1 && events[1].x==0 && events[2].t==2500);
    assert(events[2].type==28 && (uint8_t)events[2].idx==0xab);
    assert(events[2].active==1 && events[2].r==22 && events[2].x==2);
    unsigned before=queued;
    __wrap_bt_conn_set_state(&conn,BT_CONN_CONNECTED);
    conn.type=2; __wrap_bt_conn_set_state(&conn,BT_CONN_DISCONNECT_COMPLETE);
    assert(queued==before && state_calls==3);
    const char *names[]={"ble/active_profile","ble/profiles/0","bt/keys/0/peer","other"};
    for(unsigned i=0;i<4;i++) {
        expected_name=names[i]; save_result=-ENOSPC;
        assert(__wrap_settings_save_one(expected_name,expected_value,3)==-ENOSPC);
        timing_drain(&timing_work);
        assert(events[recorded-2].r==i+2 && events[recorded-1].x==ENOSPC);
    }
    recorded=0; conn.type=BT_CONN_TYPE_LE;
    for(unsigned i=0;i<TIMING_CAP+2;i++) __wrap_bt_conn_set_state(&conn,BT_CONN_DISCONNECT_COMPLETE);
    timing_drain(&timing_work);
    assert(recorded==TIMING_CAP+1 && events[TIMING_CAP].type==30);
    assert(events[TIMING_CAP].r==2 && writes==5 && timing_count==0);
    return 0;
}
"""


class DiagnosticTimingTests(unittest.TestCase):
    def test_timestamps_preserved_and_wrapped_calls_unchanged(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/diagnostic_timing.c").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_timing")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=HARNESS.replace("/* ACTUAL_SOURCE */", source),
                text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
