"""Compile controller observers; verify filtering, passthrough and no key access."""
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
#include <string.h>
#define BT_CONN_ROLE_PERIPHERAL 1
#define TOTEM_HEVT_SECURITY_EXCHANGE 38
#define PDU_DATA_LLID_CTRL 3
#define PDU_DATA_LLCTRL_TYPE_ENC_REQ 3
#define PDU_DATA_LLCTRL_TYPE_ENC_RSP 4
#define PDU_DATA_LLCTRL_TYPE_START_ENC_REQ 5
#define PDU_DATA_LLCTRL_TYPE_START_ENC_RSP 6
#define enc_req_len 23
#define enc_rsp_len 13
#define start_enc_req_len 1
#define start_enc_rsp_len 1
#define PDU_DATA_LLCTRL_LEN(name) name##_len
struct ll_conn { struct { uint16_t handle; uint8_t role; } lll; };
struct pdu_data { uint8_t ll_id,len; struct { uint8_t opcode; } llctrl; };
struct node_tx { uint8_t pdu[32]; };
struct node_rx_pdu { uint8_t pdu[32]; };
typedef struct { unsigned unused; } memq_link_t;
static uint32_t now;
static uint32_t k_uptime_get_32(void) { return now; }
struct observed { uint32_t t; uint16_t handle; uint8_t r,w,x; };
static struct observed events[32];
static unsigned recorded,rx_calls,key_calls,negative_calls;
static uint8_t result;
static struct ll_conn *expected_conn;
static struct node_rx_pdu *expected_rx;
static memq_link_t *expected_link;
static const uint8_t *expected_key;
static void totem_diagnostic_timing_capture(uint32_t t,uint8_t type,int8_t i,int8_t a,
                                           uint8_t r,uint8_t w,uint8_t x) {
    assert(type==38 && recorded<32);
    events[recorded++]=(struct observed){t,(uint8_t)i|((uint8_t)a<<8),r,w,x};
}
void __real_ull_cp_rx(struct ll_conn *conn,memq_link_t *link,struct node_rx_pdu *rx) {
    assert(conn==expected_conn && link==expected_link && rx==expected_rx); rx_calls++;
}
uint8_t __real_ull_cp_ltk_req_reply(struct ll_conn *conn,const uint8_t key[16]) {
    /* An invalid key pointer makes any observer key access fail this test. */
    assert(conn==expected_conn && key==expected_key); key_calls++; now+=7; return result;
}
uint8_t __real_ull_cp_ltk_req_neq_reply(struct ll_conn *conn) {
    assert(conn==expected_conn); negative_calls++; now+=2; return result;
}
/* ACTUAL_SOURCE */
int main(void) {
    struct ll_conn conn={{0x1ab,1}}; expected_conn=&conn;
    struct node_rx_pdu rx={{3,23,3}}; expected_rx=&rx;
    memq_link_t link={0}; expected_link=&link;
    struct node_tx tx={{3,13,4}};
    uint8_t original[32]; memcpy(original,rx.pdu,32);
    now=100; __wrap_ull_cp_rx(&conn,&link,&rx);
    assert(recorded==1 && events[0].r==2 && events[0].w==3 && events[0].t==100);
    assert(events[0].handle==0x1ab && !memcmp(original,rx.pdu,32));
    now=120; totem_security_tx_queued(&conn,&tx);
    assert(events[1].r==3 && events[1].w==4 && events[1].t==120);
    rx.pdu[1]=1; rx.pdu[2]=6; __wrap_ull_cp_rx(&conn,&link,&rx);
    tx.pdu[1]=1; tx.pdu[2]=5; totem_security_tx_queued(&conn,&tx);
    assert(events[2].w==6 && events[3].w==5);
    unsigned before=recorded;
    rx.pdu[1]=0; __wrap_ull_cp_rx(&conn,&link,&rx);
    rx.pdu[1]=1; rx.pdu[2]=3; __wrap_ull_cp_rx(&conn,&link,&rx);
    rx.pdu[2]=2; __wrap_ull_cp_rx(&conn,&link,&rx);
    rx.pdu[2]=6; rx.pdu[0]=1; __wrap_ull_cp_rx(&conn,&link,&rx);
    conn.lll.role=0; rx.pdu[0]=3; __wrap_ull_cp_rx(&conn,&link,&rx);
    totem_security_tx_queued(&conn,&tx);
    assert(recorded==before && rx_calls==7);
    conn.lll.role=1; expected_key=(const uint8_t *)(uintptr_t)1; now=200;
    assert(__wrap_ull_cp_ltk_req_reply(&conn,expected_key)==0);
    assert(events[before].r==4 && events[before].x==1 && events[before].t==200);
    assert(events[before+1].r==5 && events[before+1].x==1 && events[before+1].t==207);
    result=12; assert(__wrap_ull_cp_ltk_req_neq_reply(&conn)==12);
    assert(events[recorded-1].r==5 && events[recorded-1].w==12 && !events[recorded-1].x);
    before=recorded; conn.lll.role=0;
    assert(__wrap_ull_cp_ltk_req_reply(&conn,expected_key)==12);
    assert(__wrap_ull_cp_ltk_req_neq_reply(&conn)==12);
    assert(recorded==before && key_calls==2 && negative_calls==2);
    return 0;
}
"""


class SecurityControllerTimingTests(unittest.TestCase):
    def test_controller_boundaries_and_passthrough(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/security_controller_timing.c").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_security_controller")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=HARNESS.replace("/* ACTUAL_SOURCE */", source),
                text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
