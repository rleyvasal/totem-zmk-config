"""Exercise the actual metadata-only wrappers without changing pipeline behavior."""

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
#include <errno.h>
#include <string.h>
#define ARG_UNUSED(x) ((void)(x))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ZMK_BLE_PROFILE_COUNT 3
#define HID_USAGE_KEY 7
#define ZMK_HID_USAGE_PAGE(x) (((x)>>16)&0xff)
#define TOTEM_HEVT_HID_COUNT 31
#define TOTEM_HEVT_HID_ERROR 32
#define K_SECONDS(x) ((x)*1000)
typedef int k_timeout_t;
struct k_work { void (*handler)(struct k_work *); };
struct k_work_delayable { struct k_work work; bool scheduled; };
#define K_WORK_DELAYABLE_DEFINE(name,fn) struct k_work_delayable name={{fn},false}
struct k_spinlock { bool locked; };
typedef int k_spinlock_key_t;
static k_spinlock_key_t k_spin_lock(struct k_spinlock *l) {
    assert(!l->locked); l->locked=true; return 0;
}
static void k_spin_unlock(struct k_spinlock *l,k_spinlock_key_t key) {
    assert(l->locked && !key); l->locked=false;
}
static unsigned now=100, schedules, saves, emitted;
static unsigned deadline;
static uint32_t k_uptime_get_32(void) { return now; }
static int k_work_schedule(struct k_work_delayable *w,int delay) {
    schedules++; if(!w->scheduled) {w->scheduled=true; deadline=now+delay;} return 1;
}
struct k_msgq { int id; };
struct k_work_q { int thread; };
struct k_msgq zmk_hog_keyboard_msgq={1};
struct k_work_q hog_work_q={2};
static int current_thread=1;
static int k_current_get(void) { return current_thread; }
static int k_work_queue_thread_get(struct k_work_q *q) { return q->thread; }
struct bt_conn { int profile; };
struct bt_gatt_notify_params { void (*func)(void); const void *data; };
struct zmk_hid_keyboard_report_body { uint8_t modifiers, reserved, keys[6]; };
enum { ZMK_TRANSPORT_NONE, ZMK_TRANSPORT_USB, ZMK_TRANSPORT_BLE };
struct endpoint { int transport; };
static int active=2, transport=ZMK_TRANSPORT_BLE;
static int zmk_ble_active_profile_index(void) { return active; }
static struct endpoint zmk_endpoint_get_selected(void) { return (struct endpoint){transport}; }
static const int *bt_conn_get_dst(struct bt_conn *conn) { return &conn->profile; }
static int zmk_ble_profile_index(const int *dst) { return *dst; }
static unsigned releases, puts, gets, notifies, connections;
static int release_result, put_result, get_result, notify_result;
static uint32_t expected_usage;
static const void *expected_data;
static struct zmk_hid_keyboard_report_body returned_report;
static struct bt_conn host={2}, *returned_conn=&host;
int __real_zmk_hid_release(uint32_t usage) {
    assert(usage==expected_usage); releases++; return release_result;
}
int __real_z_impl_k_msgq_put(struct k_msgq *q,const void *data,k_timeout_t timeout) {
    assert(q && data==expected_data && timeout==100); puts++; return put_result;
}
int __real_z_impl_k_msgq_get(struct k_msgq *q,void *data,k_timeout_t timeout) {
    assert(q && timeout==0); gets++; if(!get_result) memcpy(data,&returned_report,sizeof(returned_report));
    return get_result;
}
struct bt_conn *__real_zmk_ble_active_profile_conn(void) { connections++; return returned_conn; }
int __real_bt_gatt_notify_cb(struct bt_conn *conn,struct bt_gatt_notify_params *p) {
    assert(conn==&host && p && p->data==expected_data); notifies++; return notify_result;
}
struct event { uint32_t t; uint8_t k,r,w,x; int i,a; };
static struct event events[100];
static void totem_host_event_log_record_timing(uint32_t t,uint8_t k,int i,int a,
                                             uint8_t r,uint8_t w,uint8_t x) {
    assert(!emitted || emitted<100); events[emitted++]=(struct event){t,k,r,w,x,i,a};
}
static void totem_host_event_log_persist(void) { saves++; }
/* ACTUAL_SOURCE */
static unsigned total(unsigned stage) { return counts[2][stage].total; }
int main(void) {
    expected_usage=(7<<16)|8;
    assert(__wrap_zmk_hid_release(expected_usage)==0 && releases==1);
    assert(total(RELEASE_OK)==1 && emitted==0 && saves==0 && deadline==5100);
    now=200; release_result=-EINVAL;
    assert(__wrap_zmk_hid_release(expected_usage)==-EINVAL && total(RELEASE_FAILED)==1);
    assert(deadline==5100); /* Continuous input does not postpone the flush. */
    transport=ZMK_TRANSPORT_USB;
    __wrap_zmk_hid_release(expected_usage); assert(total(RELEASE_FAILED)==1);
    transport=ZMK_TRANSPORT_BLE; expected_usage=(12<<16)|8;
    __wrap_zmk_hid_release(expected_usage); assert(total(RELEASE_FAILED)==1);
    struct zmk_hid_keyboard_report_body report={0}; expected_data=&report;
    assert(__wrap_z_impl_k_msgq_put(&zmk_hog_keyboard_msgq,&report,100)==0);
    assert(total(QUEUED)==1 && total(ALL_UP_QUEUED)==1);
    put_result=-EAGAIN;
    assert(__wrap_z_impl_k_msgq_put(&zmk_hog_keyboard_msgq,&report,100)==-EAGAIN);
    assert(total(QUEUE_FAILED)==1 && total(QUEUED)==1);
    assert(__wrap_z_impl_k_msgq_get(&zmk_hog_keyboard_msgq,&report,0)==0);
    assert(total(EVICTED)==1 && total(ALL_UP_EVICTED)==1);
    current_thread=2;
    assert(__wrap_z_impl_k_msgq_get(&zmk_hog_keyboard_msgq,&report,0)==0);
    assert(total(DEQUEUED)==1);
    assert(__wrap_zmk_ble_active_profile_conn()==&host);
    totem_hid_report_observed(&host,true);
    struct bt_gatt_notify_params params={NULL,&report};
    assert(__wrap_bt_gatt_notify_cb(&host,&params)==0);
    assert(total(NOTIFY_ACCEPTED)==1 && total(ALL_UP_ACCEPTED)==1 && !params.func);
    /* A later failure is counted, not suppressed after the first successful send. */
    __wrap_z_impl_k_msgq_get(&zmk_hog_keyboard_msgq,&report,0);
    totem_hid_report_observed(&host,false); notify_result=-ENOTCONN; now=300;
    assert(__wrap_bt_gatt_notify_cb(&host,&params)==-ENOTCONN);
    assert(total(NOTIFY_FAILED)==1 && total(ALL_UP_FAILED)==1 && total(UNSUBSCRIBED)==1);
    returned_report.keys[0]=8; /* Same metadata for any nonzero key identity. */
    __wrap_z_impl_k_msgq_get(&zmk_hog_keyboard_msgq,&report,0);
    totem_hid_report_observed(&host,true);
    __wrap_bt_gatt_notify_cb(&host,&params);
    assert(total(NOTIFY_FAILED)==2 && total(ALL_UP_FAILED)==1);
    returned_report.keys[0]=9;
    __wrap_z_impl_k_msgq_get(&zmk_hog_keyboard_msgq,&report,0);
    returned_conn=NULL;
    assert(!__wrap_zmk_ble_active_profile_conn() && total(NO_HOST_DROP)==1);
    __wrap_zmk_ble_active_profile_conn(); assert(total(NO_HOST_DROP)==1);
    get_result=-ENOMSG;
    __wrap_z_impl_k_msgq_get(&zmk_hog_keyboard_msgq,&report,0);
    assert(total(DEQUEUED)==4 && total(EVICTED)==1);
    unsigned before=schedules;
    struct k_msgq unrelated={3}; current_thread=1;
    __wrap_z_impl_k_msgq_put(&unrelated,&report,100);
    __wrap_z_impl_k_msgq_get(&unrelated,&report,0);
    __wrap_bt_gatt_notify_cb(&host,&params);
    assert(schedules==before && puts==3 && releases==4 && notifies==4);
    assert(!saves && !emitted); now=6000; flush_counts(&summary_work.work);
    assert(saves==1 && emitted>0);
    bool error_seen=false, release_seen=false;
    for(unsigned i=0;i<emitted;i++) {
        assert(events[i].i==2 && events[i].a==-1 && events[i].t<=300);
        if(events[i].k==32 && events[i].r==NOTIFY_FAILED) {
            assert(events[i].w==ENOTCONN); error_seen=true;
        }
        if(events[i].k==31 && events[i].r==RELEASE_OK) {
            assert(events[i].t==100 && events[i].w==1 && !events[i].x); release_seen=true;
        }
    }
    assert(error_seen && release_seen);
    before=emitted; flush_counts(&summary_work.work); assert(emitted==before && saves==1);
    counts[2][QUEUED].total=UINT16_MAX; put_result=0;
    __wrap_z_impl_k_msgq_put(&zmk_hog_keyboard_msgq,&report,100);
    assert(!total(QUEUED) && counts[2][QUEUED].dirty);
    return 0;
}
"""


class HidReportDiagnosticsTests(unittest.TestCase):
    def test_pipeline_counters_and_unchanged_calls(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/hid_report_diagnostics.c").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_hid_diagnostics")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=HARNESS.replace("/* ACTUAL_SOURCE */", source),
                text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
