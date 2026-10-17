"""Compile the actual observers with mocked controller calls, never radio policy."""
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
#include <string.h>
#include <errno.h>
#define CONFIG_BT_MAX_CONN 6
#define CONFIG_BT_CTLR_LE_ENC 1
#define LLL_CONN_MIC_NONE 0
#define LLL_CONN_MIC_PASS 1
#define LLL_CONN_MIC_FAIL 2
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define CONTAINER_OF(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
#define BT_CONN_TYPE_LE 1
#define BT_CONN_ROLE_PERIPHERAL 1
#define BT_CONN_STATE_CONNECTED 2
#define PDU_DATA_LLID_CTRL 3
#define PDU_DATA_LLCTRL_TYPE_TERMINATE_IND 2
#define TOTEM_HEVT_HANDOFF_PARAM 33
#define TOTEM_HEVT_HANDOFF_TERM 34
#define TOTEM_HEVT_HANDOFF_RADIO 35
#define TOTEM_HEVT_HANDOFF_TX 39
enum { PROC_UNKNOWN, PROC_TERMINATE };
struct bt_conn { uint16_t handle; int profile; };
struct bt_conn_info {
    unsigned type,role,state;
    struct { uint16_t interval,latency,timeout; } le;
};
struct ll_conn {
    unsigned ull;
    struct { uint16_t handle,lazy_prepare,latency_event; uint8_t role; } lll;
    struct { struct { uint16_t prt_expire; } local; } llcp;
};
struct pdu_data { uint8_t ll_id; struct { uint8_t opcode; } llctrl; };
struct node_tx { uint8_t pdu[4]; };
struct proc_ctx { unsigned proc; };
struct node_rx_event_done {
    void *param;
    struct { bool crc_valid,is_aborted; uint16_t trx_cnt; uint8_t mic_state; } extra;
};
struct k_spinlock { bool held; };
typedef int k_spinlock_key_t;
static k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) {
    assert(!lock->held); lock->held=true; return 0;
}
static void k_spin_unlock(struct k_spinlock *lock,k_spinlock_key_t key) {
    assert(lock->held && key==0); lock->held=false;
}
static uint32_t now;
static bool usb;
static unsigned selected=0;
static int info_error, handle_error, disconnect_result;
static struct bt_conn_info info={1,1,2,{12,30,400}};
static uint32_t k_uptime_get_32(void) { return now; }
static int bt_conn_get_info(struct bt_conn *conn,struct bt_conn_info *out) {
    assert(conn); *out=info; return info_error;
}
static const void *bt_conn_get_dst(struct bt_conn *conn) { return &conn->profile; }
static int zmk_ble_profile_index(const void *dst) { return *(const int *)dst; }
static int zmk_ble_active_profile_index(void) { return selected; }
static bool zmk_usb_is_powered(void) { return usb; }
static int bt_hci_get_conn_handle(struct bt_conn *conn,uint16_t *out) {
    *out=conn->handle; return handle_error;
}
struct observed { uint32_t t; uint8_t type; uint16_t handle; uint8_t r,w,x; };
static struct observed events[128];
static unsigned recorded,disc_calls,enqueue_calls,ack_calls,done_calls;
static struct bt_conn *expected_bt;
static struct ll_conn *expected_ll;
static struct node_tx *expected_tx;
static struct proc_ctx *expected_ctx;
static struct node_rx_event_done *expected_done;
static void totem_diagnostic_timing_capture(uint32_t t,uint8_t type,int8_t i,int8_t a,
                                           uint8_t r,uint8_t w,uint8_t x) {
    assert(recorded<128);
    events[recorded++]=(struct observed){t,type,(uint8_t)i|((uint8_t)a<<8),r,w,x};
}
int __real_bt_conn_disconnect(struct bt_conn *conn,uint8_t reason) {
    assert(conn==expected_bt && reason==0x13); disc_calls++; return disconnect_result;
}
void __real_llcp_tx_enqueue(struct ll_conn *conn,struct node_tx *tx) {
    assert(conn==expected_ll && tx==expected_tx); enqueue_calls++;
}
static unsigned security_tx_calls;
void totem_security_tx_queued(struct ll_conn *conn,struct node_tx *tx) {
    assert(conn==expected_ll && tx==expected_tx); security_tx_calls++;
}
void __real_llcp_lp_comm_tx_ack(struct ll_conn *conn,struct proc_ctx *ctx,struct node_tx *tx) {
    assert(conn==expected_ll && tx==expected_tx && ctx==expected_ctx); ack_calls++;
}
void __real_ull_conn_done(struct node_rx_event_done *done) {
    assert(done==expected_done); done_calls++;
}
static unsigned timer_calls;
static uint16_t timer_elapsed, timer_after;
static uint8_t timer_reason, *expected_reason;
static int timer_result;
static struct proc_ctx *llcp_lr_peek(struct ll_conn *conn) {
    assert(conn==expected_ll); return expected_ctx;
}
int __real_ull_cp_prt_elapse(struct ll_conn *conn, uint16_t elapsed, uint8_t *reason) {
    assert(conn==expected_ll && elapsed==timer_elapsed && reason==expected_reason);
    timer_calls++;
    conn->llcp.local.prt_expire=timer_after; *reason=timer_reason;
    return timer_result;
}
/* ACTUAL_SOURCE */
int main(void) {
    struct bt_conn bt={1,2}; expected_bt=&bt;
    struct ll_conn ll={.lll={1,0,0,1}}; expected_ll=&ll;
    struct node_tx tx={{3,2}}; expected_tx=&tx;
    struct proc_ctx ctx={0}; expected_ctx=&ctx;
    struct node_rx_event_done done={&ll.ull,{true,false,2,LLL_CONN_MIC_PASS}}; expected_done=&done;
    totem_handoff_connected(1);
    now=80; __wrap_ull_conn_done(&done);
    assert(recorded==0 && done_calls==1);
    now=100; assert(__wrap_bt_conn_disconnect(&bt,0x13)==0);
    assert(disc_calls==1 && recorded==6 && radios[1].armed);
    assert(events[0].type==34 && events[0].r==0 && events[0].x==0x13);
    assert(events[1].type==33 && events[1].w==12);
    assert(events[2].w==30 && events[3].w==144 && events[3].x==1);
    assert(events[4].type==35 && events[4].r==4 && events[4].t==80 && events[4].x==1);
    assert(events[5].type==34 && events[5].r==1 && events[5].x==0);
    now=101; __wrap_llcp_tx_enqueue(&ll,&tx);
    assert(enqueue_calls==1 && events[6].r==2);
    now=105; __wrap_llcp_lp_comm_tx_ack(&ll,&ctx,&tx);
    assert(ack_calls==1 && events[7].r==3);
    /* TX observations coalesce until disconnect, without ISR capture work. */
    unsigned tx_before=recorded;
    assert(totem_handoff_tx_armed(1) && !totem_handoff_tx_armed(65535));
    now=106; totem_handoff_tx_setup(1);
    now=107; totem_handoff_tx_result(1,true);
    now=108; totem_handoff_tx_setup(1); totem_handoff_tx_result(1,false);
    now=109; totem_handoff_tx_setup(1); totem_handoff_tx_result(1,true);
    totem_handoff_tx_setup(65535); totem_handoff_tx_result(65535,true);
    assert(recorded==tx_before);
    tx.pdu[1]=9; unsigned before=recorded;
    __wrap_llcp_tx_enqueue(&ll,&tx); __wrap_llcp_lp_comm_tx_ack(&ll,&ctx,&tx);
    assert(recorded==before && enqueue_calls==2 && ack_calls==2); tx.pdu[1]=2;
    now=110; __wrap_ull_conn_done(&done);
    done.extra.crc_valid=false; ll.lll.lazy_prepare=2; ll.lll.latency_event=3;
    done.extra.trx_cnt=0; done.extra.mic_state=LLL_CONN_MIC_NONE;
    now=120; __wrap_ull_conn_done(&done);
    assert(events[recorded-1].r==6 && events[recorded-1].x==0);
    /* Received-but-invalid is separate from silence; no per-event log flood. */
    unsigned quiet=recorded; done.extra.trx_cnt=1; done.extra.mic_state=LLL_CONN_MIC_FAIL;
    ll.lll.lazy_prepare=ll.lll.latency_event=0;
    now=125; __wrap_ull_conn_done(&done); assert(recorded==quiet);
    before=recorded; done.extra.is_aborted=true; ll.lll.lazy_prepare=0;
    done.extra.trx_cnt=0; done.extra.mic_state=LLL_CONN_MIC_NONE;
    ll.lll.latency_event=0; now=130; __wrap_ull_conn_done(&done);
    assert(recorded==before && done_calls==5);
    now=140; totem_handoff_disconnected(1);
    assert(!radios[1].armed && recorded==before+16);
    assert(events[before].r==0 && events[before].w==4);
    assert(events[before+1].r==1 && events[before+1].w==3);
    assert(events[before+2].r==2 && events[before+2].w==1);
    assert(events[before+3].r==3 && events[before+3].w==5);
    assert(events[before+4].r==7 && events[before+4].w==3);
    assert(events[before+5].r==8 && events[before+5].w==1);
    assert(events[before+6].r==9 && events[before+6].w==1);
#if defined(CONFIG_BT_CTLR_LE_ENC)
    assert(events[before+7].r==10 && events[before+7].w==1);
    assert(events[before+8].r==11 && events[before+8].w==1);
#else
    assert(events[before+7].w==0 && events[before+8].w==0);
#endif
    assert(events[before+9].r==5 && events[before+9].t==110 && events[before+9].x==1);
    assert(events[before+10].type==39 && events[before+10].r==0 && events[before+10].w==3);
    assert(events[before+11].r==1 && events[before+11].w==2);
    assert(events[before+12].r==2 && events[before+12].w==1);
    assert(events[before+13].r==3 && events[before+13].t==106 && events[before+13].x==1);
    assert(events[before+14].r==4 && events[before+14].t==107 && events[before+14].x==1);
    assert(events[before+15].r==5 && events[before+15].t==109 && events[before+15].x==1);
    assert(!radios[1].rx_packets && !radios[1].no_rx && !radios[1].invalid_rx);
    assert(!radios[1].mic_pass && !radios[1].mic_fail);
    assert(!radios[1].tx_setups && !radios[1].tx_completed && !radios[1].tx_uncompleted);
    totem_handoff_tx_setup(1); totem_handoff_tx_result(1,true);
    assert(!radios[1].tx_setups && !radios[1].tx_completed);
    before=recorded; __wrap_llcp_tx_enqueue(&ll,&tx);
    totem_handoff_disconnected(1); assert(recorded==before);
    /* USB, selected host, split, unknown identity, non-connected and API errors. */
    usb=true; __wrap_bt_conn_disconnect(&bt,0x13); usb=false;
    bt.profile=0; __wrap_bt_conn_disconnect(&bt,0x13);
    bt.profile=-1; __wrap_bt_conn_disconnect(&bt,0x13); bt.profile=2;
    info.role=0; __wrap_bt_conn_disconnect(&bt,0x13); info.role=1;
    info.state=3; __wrap_bt_conn_disconnect(&bt,0x13); info.state=2;
    info_error=-EINVAL; __wrap_bt_conn_disconnect(&bt,0x13); info_error=0;
    handle_error=-EINVAL; __wrap_bt_conn_disconnect(&bt,0x13); handle_error=0;
    assert(recorded==before && disc_calls==8);
    disconnect_result=-EIO; assert(__wrap_bt_conn_disconnect(&bt,0x13)==-EIO);
    assert(!radios[1].armed && events[recorded-1].w==1 && events[recorded-1].x==EIO);
    totem_handoff_connected(1); disconnect_result=0; now=200;
    __wrap_bt_conn_disconnect(&bt,0x13);
    assert(events[recorded-2].r==4 && events[recorded-2].t==200 && !events[recorded-2].x);
    now=210; totem_handoff_disconnected(1);
    assert(events[recorded-1].r==5 && events[recorded-1].t==210 && !events[recorded-1].x);
    assert(add_count(65535,1)==65535 && add_count(65530,20)==65535);
    before=recorded; ll.lll.role=0; __wrap_ull_conn_done(&done);
    ll.lll.role=1; ll.lll.handle=65535; __wrap_ull_conn_done(&done);
    totem_handoff_connected(65535); totem_handoff_disconnected(65535);
    assert(recorded==before);
    assert(security_tx_calls==enqueue_calls);
    /* Only an actual local termination timeout may emit stage 4. */
    ll.lll.handle=1; ll.lll.role=1; ctx.proc=PROC_TERMINATE;
    totem_handoff_connected(1); now=300;
    __wrap_bt_conn_disconnect(&bt,0x13);
    static uint8_t reason; expected_reason=&reason;
    const struct {
        uint16_t local, elapsed, after;
        int result;
        uint8_t reason, proc, role;
        uint16_t handle;
        bool armed, marker;
    } cases[] = {
        {8,2,6,0,0,PROC_TERMINATE,1,1,true,false},
        {6,6,6,-ETIMEDOUT,0x16,PROC_TERMINATE,1,1,true,true},
        {4,6,4,-ETIMEDOUT,0x16,PROC_TERMINATE,1,1,true,true},
        {0,2,0,-ETIMEDOUT,0x22,PROC_TERMINATE,1,1,true,false},
        /* Local shrinks to <= elapsed, but only the remote timer expired. */
        {6,3,3,-ETIMEDOUT,0x22,PROC_TERMINATE,1,1,true,false},
        {2,2,2,-ETIMEDOUT,0x22,PROC_UNKNOWN,1,1,true,false},
        {2,2,2,-EIO,0x16,PROC_TERMINATE,1,1,true,false},
        {2,2,2,-ETIMEDOUT,0x16,PROC_TERMINATE,0,1,true,false},
        {2,2,2,-ETIMEDOUT,0x16,PROC_TERMINATE,1,65535,true,false},
        {2,2,2,-ETIMEDOUT,0x16,PROC_TERMINATE,1,1,false,false},
    };
    for (unsigned i=0; i<ARRAY_SIZE(cases); i++) {
        ll.llcp.local.prt_expire=cases[i].local;
        ll.lll.handle=cases[i].handle; ll.lll.role=cases[i].role;
        ctx.proc=cases[i].proc; radios[1].armed=cases[i].armed;
        timer_elapsed=cases[i].elapsed; timer_after=cases[i].after;
        timer_result=cases[i].result; timer_reason=cases[i].reason;
        before=recorded; now=310+i;
        assert(__wrap_ull_cp_prt_elapse(&ll,timer_elapsed,&reason)==timer_result);
        assert(timer_calls==i+1 && reason==timer_reason);
        assert(ll.llcp.local.prt_expire==timer_after);
        assert(recorded==before+cases[i].marker);
        if (cases[i].marker) {
            assert(events[before].type==34 && events[before].r==4);
            assert(events[before].handle==1 && events[before].t==now);
            assert(events[before].w==0 && events[before].x==timer_reason);
        }
    }
    return 0;
}
"""


class HandoffDiagnosticsTests(unittest.TestCase):
    def test_handoff_only_counts_timestamps_and_passthrough(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/handoff_diagnostics.c").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_handoff")
            for encryption in (True, False):
                with self.subTest(encryption=encryption):
                    harness = HARNESS if encryption else HARNESS.replace(
                        "#define CONFIG_BT_CTLR_LE_ENC 1", "")
                    subprocess.run(
                        [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                         "-x", "c", "-", "-o", executable],
                        input=harness.replace("/* ACTUAL_SOURCE */", source),
                        text=True, check=True, timeout=30,
                    )
                    subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
