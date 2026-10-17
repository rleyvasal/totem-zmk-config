"""Compile the real security observers; verify timestamps and unchanged calls."""
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
#include <errno.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define BT_BUF_EVT 1
#define BT_CONN_TYPE_LE 1
#define BT_CONN_ROLE_PERIPHERAL 1
#define BT_HCI_EVT_ENCRYPT_CHANGE 8
#define BT_HCI_EVT_ENCRYPT_KEY_REFRESH_COMPLETE 48
#define BT_HCI_EVT_LE_META_EVENT 62
#define BT_HCI_EVT_LE_LTK_REQUEST 5
#define BT_L2CAP_CID_SMP 6
#define BT_SMP_CMD_SECURITY_REQUEST 11
#define TOTEM_HEVT_ENCRYPT_EVENT 36
#define TOTEM_HEVT_SECURITY_TIMING 37
#define TOTEM_HEVT_SECURITY_EXCHANGE 38
typedef struct { unsigned unused; } sys_slist_t;
struct { sys_slist_t rx_queue; } bt_dev;
struct net_buf { uint8_t *data; size_t len; unsigned type,ref; };
struct bt_hci_evt_hdr { uint8_t evt,len; };
struct __attribute__((packed)) bt_hci_evt_encrypt_change {
    uint8_t status; uint16_t handle; uint8_t encrypt;
};
struct __attribute__((packed)) bt_hci_evt_encrypt_key_refresh_complete {
    uint8_t status; uint16_t handle;
};
struct __attribute__((packed)) bt_hci_evt_le_meta_event { uint8_t subevent; };
struct __attribute__((packed)) bt_hci_evt_le_ltk_request {
    uint16_t handle; uint64_t rand; uint16_t ediv;
};
struct bt_conn { uint8_t type,role,sec_level; uint16_t handle; };
struct bt_l2cap_le_chan {
    struct { struct bt_conn *conn; } chan;
    struct { uint16_t cid; } tx;
};
typedef void (*bt_conn_tx_cb_t)(struct bt_conn *,void *);
enum bt_security_err { BT_SECURITY_ERR_SUCCESS, BT_SECURITY_ERR_AUTH_FAIL };
static uint32_t now;
static uint32_t k_uptime_get_32(void) { return now; }
static unsigned bt_buf_get_type(const struct net_buf *buf) { return buf->type; }
static uint16_t sys_get_le16(const uint8_t *p) { return p[0] | (p[1]<<8); }
struct observed { uint32_t t; uint8_t type; uint16_t handle; uint8_t r,w,x; };
static struct observed events[32];
static unsigned recorded,puts,gets,sec_calls,l2cap_calls;
static sys_slist_t *expected_list;
static struct net_buf *expected_buf,*get_result;
static struct bt_conn *expected_conn;
static uint8_t expected_hci;
static enum bt_security_err expected_error;
static struct bt_l2cap_le_chan *expected_chan;
static int send_result;
static unsigned send_calls;
static bt_conn_tx_cb_t expected_cb;
static void *expected_user;
int __real_bt_l2cap_send_pdu(struct bt_l2cap_le_chan *chan,struct net_buf *buf,
                            bt_conn_tx_cb_t cb,void *user) {
    assert(chan==expected_chan && buf==expected_buf && cb==expected_cb && user==expected_user);
    send_calls++; now+=3;
    if (!send_result) { buf->data=NULL; buf->len=0; }
    return send_result;
}
static void totem_diagnostic_timing_capture(uint32_t t,uint8_t type,int8_t i,int8_t a,
                                           uint8_t r,uint8_t w,uint8_t x) {
    assert(recorded<32);
    events[recorded++]=(struct observed){t,type,(uint8_t)i|((uint8_t)a<<8),r,w,x};
}
void __real_net_buf_slist_put(sys_slist_t *list,struct net_buf *buf) {
    assert(list==expected_list && buf==expected_buf); puts++;
}
struct net_buf *__real_net_buf_slist_get(sys_slist_t *list) {
    assert(list==expected_list); gets++; return get_result;
}
void __wrap_bt_l2cap_security_changed(struct bt_conn *conn,uint8_t status);
void __real_bt_l2cap_security_changed(struct bt_conn *conn,uint8_t status) {
    assert(conn==expected_conn && status==expected_hci); l2cap_calls++; now+=80;
}
void __real_bt_conn_security_changed(struct bt_conn *conn,uint8_t status,enum bt_security_err err) {
    assert(conn==expected_conn && status==expected_hci && err==expected_error); sec_calls++;
    __wrap_bt_l2cap_security_changed(conn,status); now+=20;
}
/* ACTUAL_SOURCE */
int main(void) {
    uint8_t bytes[]={8,4,0,0xab,1,1}; uint8_t original[6]; memcpy(original,bytes,6);
    struct net_buf buf={bytes,6,BT_BUF_EVT,3}; expected_buf=&buf; get_result=&buf;
    expected_list=&bt_dev.rx_queue;
    now=100; __wrap_net_buf_slist_put(expected_list,&buf);
    assert(recorded==1 && puts==1 && events[0].t==100 && events[0].r==0);
    assert(events[0].handle==0x1ab && !events[0].w && events[0].x==1);
    now=2500; assert(__wrap_net_buf_slist_get(expected_list)==&buf);
    assert(recorded==2 && gets==1 && events[1].t==2500 && events[1].r==1);
    assert(buf.len==6 && buf.ref==3 && memcmp(bytes,original,6)==0);
    bytes[0]=48; bytes[1]=3; bytes[2]=5; buf.len=5;
    __wrap_net_buf_slist_put(expected_list,&buf); __wrap_net_buf_slist_get(expected_list);
    assert(events[2].r==2 && events[2].w==5 && events[2].x==255);
    assert(events[3].r==3 && events[3].handle==0x1ab);
    unsigned before=recorded;
    /* Unrelated list/type/event, malformed lengths and an empty queue pass through. */
    sys_slist_t unrelated={0}; expected_list=&unrelated;
    __wrap_net_buf_slist_put(expected_list,&buf); __wrap_net_buf_slist_get(expected_list);
    expected_list=&bt_dev.rx_queue; buf.type=2; __wrap_net_buf_slist_put(expected_list,&buf);
    buf.type=BT_BUF_EVT; bytes[0]=9; __wrap_net_buf_slist_put(expected_list,&buf);
    bytes[0]=8; bytes[1]=4; buf.len=5; __wrap_net_buf_slist_put(expected_list,&buf);
    bytes[1]=3; __wrap_net_buf_slist_put(expected_list,&buf);
    buf.len=1; __wrap_net_buf_slist_put(expected_list,&buf);
    get_result=NULL; assert(__wrap_net_buf_slist_get(expected_list)==NULL);
    assert(recorded==before && puts==8 && gets==4);
    struct bt_conn conn={1,1,4,0x1ab}; expected_conn=&conn;
    now=3000; __wrap_bt_conn_security_changed(&conn,0,BT_SECURITY_ERR_SUCCESS);
    assert(recorded==before+3 && sec_calls==1 && l2cap_calls==1);
    assert(events[before].type==37 && events[before].r==0 && events[before].t==3000);
    assert(events[before+1].r==1 && events[before+1].t==3080);
    assert(events[before+2].r==2 && events[before+2].t==3100);
    assert(events[before].handle==0x1ab && events[before].x==4);
    expected_hci=5; expected_error=BT_SECURITY_ERR_AUTH_FAIL; conn.sec_level=1;
    __wrap_bt_conn_security_changed(&conn,5,BT_SECURITY_ERR_AUTH_FAIL);
    assert(events[recorded-1].w==5 && events[recorded-1].x==1);
    before=recorded; conn.role=0;
    __wrap_bt_conn_security_changed(&conn,5,BT_SECURITY_ERR_AUTH_FAIL);
    conn.role=1; conn.type=2;
    __wrap_bt_conn_security_changed(&conn,5,BT_SECURITY_ERR_AUTH_FAIL);
    assert(recorded==before && sec_calls==4 && l2cap_calls==4);
    /* LTK metadata boundaries: key/rand/ediv remain untouched and unrecorded. */
    uint8_t ltk_evt[]={62,13,5,0xab,1,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xbb,0xbb};
    uint8_t ltk_original[sizeof(ltk_evt)]; memcpy(ltk_original,ltk_evt,sizeof(ltk_evt));
    buf=(struct net_buf){ltk_evt,sizeof(ltk_evt),BT_BUF_EVT,3}; get_result=&buf;
    now=5000; __wrap_net_buf_slist_put(expected_list,&buf);
    now=5100; assert(__wrap_net_buf_slist_get(expected_list)==&buf);
    assert(events[before].type==38 && events[before].r==6 && events[before].t==5000);
    assert(events[before+1].r==7 && events[before+1].t==5100);
    assert(events[before].handle==0x1ab && !events[before].w && !events[before].x);
    assert(buf.ref==3 && !memcmp(ltk_evt,ltk_original,sizeof(ltk_evt)));
    before=recorded; buf.len--; __wrap_net_buf_slist_put(expected_list,&buf);
    buf.len++; ltk_evt[2]=6; __wrap_net_buf_slist_put(expected_list,&buf);
    assert(recorded==before);
    /* Request submission passes through success/errors and consumed buffers. */
    conn=(struct bt_conn){1,1,1,0x1ab};
    struct bt_l2cap_le_chan chan={{&conn},{6}}; expected_chan=&chan;
    uint8_t req[]={11,0xee}; buf=(struct net_buf){req,2,2,1}; now=6000;
    assert(__wrap_bt_l2cap_send_pdu(&chan,&buf,NULL,NULL)==0);
    assert(events[before].r==0 && events[before].t==6000);
    assert(events[before+1].r==1 && events[before+1].t==6003 && !events[before+1].w);
    assert(buf.data==NULL && events[before+1].handle==0x1ab);
    buf=(struct net_buf){req,2,2,1}; send_result=-ENOMEM;
    assert(__wrap_bt_l2cap_send_pdu(&chan,&buf,NULL,NULL)==-ENOMEM);
    assert(events[recorded-1].w==1 && events[recorded-1].x==ENOMEM && req[1]==0xee);
    before=recorded; chan.chan.conn=NULL;
    __wrap_bt_l2cap_send_pdu(&chan,&buf,NULL,NULL);
    chan.chan.conn=&conn; conn.role=0; __wrap_bt_l2cap_send_pdu(&chan,&buf,NULL,NULL);
    conn.role=1; chan.tx.cid=4; __wrap_bt_l2cap_send_pdu(&chan,&buf,NULL,NULL);
    chan.tx.cid=6; buf.len=1; __wrap_bt_l2cap_send_pdu(&chan,&buf,NULL,NULL);
    buf.len=2; req[0]=1; __wrap_bt_l2cap_send_pdu(&chan,&buf,NULL,NULL);
    assert(recorded==before && send_calls==7);
    return 0;
}
"""


class SecurityTimingTests(unittest.TestCase):
    def test_queue_boundaries_security_stages_and_passthrough(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/security_timing.c").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_security")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=HARNESS.replace("/* ACTUAL_SOURCE */", source),
                text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
