"""Compile actual radio observers; passthrough, END classification and isolation."""
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
#define CONFIG_BT_CTLR_LE_ENC 1
#define BT_CONN_ROLE_PERIPHERAL 1
#define PDU_DATA_LLID_CTRL 3
#define PDU_DATA_LLCTRL_TYPE_TERMINATE_IND 2
#define NRF_RADIO ((void *)0x1234)
#define NRF_RADIO_EVENT_END 1
typedef void (*radio_isr_cb_t)(void *);
struct lll_conn { uint16_t handle; uint8_t role,enc_tx; };
struct ccm { unsigned untouched; };
struct pdu_data { uint8_t ll_id,len; struct { uint8_t opcode; } llctrl; };
static bool armed[6], end_event;
static unsigned setups[6], completed[6], uncompleted[6];
static unsigned callback_calls, ccm_calls, packet_calls, reset_calls;
static radio_isr_cb_t expected_cb;
static void *expected_param,*expected_packet,*expected_plain,*cipher;
static struct ccm *expected_ccm;
void lll_conn_isr_rx(void *p) { (void)p; }
void lll_conn_isr_tx(void *p) { (void)p; }
static void final_done(void *p) { (void)p; }
static void unrelated(void *p) { (void)p; }
bool totem_handoff_tx_armed(uint16_t h) { return h<6 && armed[h]; }
void totem_handoff_tx_setup(uint16_t h) {
    assert(h<6); if (armed[h]) setups[h]++;
}
void totem_handoff_tx_result(uint16_t h,bool end) {
    assert(h<6); if (armed[h]) { if(end) completed[h]++; else uncompleted[h]++; }
}
static bool nrf_radio_event_check(void *radio,unsigned event) {
    assert(radio==NRF_RADIO && event==NRF_RADIO_EVENT_END); return end_event;
}
void __real_radio_isr_set(radio_isr_cb_t cb,void *param) {
    assert(cb==expected_cb && param==expected_param); callback_calls++;
}
void *__real_radio_ccm_tx_pkt_set(struct ccm *cnf,void *packet) {
    assert(cnf==expected_ccm && packet==expected_plain); ccm_calls++; return cipher;
}
void __real_radio_pkt_tx_set(void *packet) {
    assert(packet==expected_packet); packet_calls++;
}
void __real_radio_status_reset(void) { end_event=false; reset_calls++; }
/* ACTUAL_SOURCE */
static void callback(radio_isr_cb_t cb,void *param) {
    expected_cb=cb; expected_param=param; __wrap_radio_isr_set(cb,param);
}
static void packet(void *p) { expected_packet=p; __wrap_radio_pkt_tx_set(p); }
int main(void) {
    struct lll_conn host={1,1,0}, split={2,0,0}, other={3,1,0};
    struct pdu_data term={3,2,{2}}, data={2,2,{2}}, short_ctrl={3,1,{2}};
    /* No armed handoff: host, split and unknown event contexts stay silent. */
    callback(lll_conn_isr_rx,&host); packet(&term);
    end_event=true; __wrap_radio_status_reset(); assert(!setups[1]);
    armed[1]=true; callback(lll_conn_isr_rx,&host);
    /* Private final-TX callback retains context; read END before it is reset. */
    callback(final_done,&host); packet(&term);
    assert(setups[1]==1 && !completed[1]); end_event=true;
    __wrap_radio_status_reset(); __wrap_radio_status_reset();
    assert(completed[1]==1 && !uncompleted[1] && !end_event);
    /* A continuing TX ISR and repeated termination transmission also count. */
    callback(lll_conn_isr_tx,&host); packet(&term); end_event=true;
    __wrap_radio_status_reset(); assert(setups[1]==2 && completed[1]==2);
    /* Setup canceled before END is never reported as a completed transmit. */
    packet(&term); __wrap_radio_status_reset(); assert(uncompleted[1]==1);
    packet(&data); end_event=true; __wrap_radio_status_reset();
    packet(&short_ctrl); end_event=true; __wrap_radio_status_reset();
    assert(setups[1]==3 && completed[1]==2);
    /* Switching to RX resolves a canceled setup before a later RX END. */
    packet(&term); callback(lll_conn_isr_rx,&host);
    end_event=true; __wrap_radio_status_reset();
    assert(uncompleted[1]==2 && completed[1]==2);
    /* A different radio event invalidates the connection context. */
    packet(&term); callback(unrelated,&other);
    packet(&term); end_event=true; __wrap_radio_status_reset();
    assert(setups[1]==5 && uncompleted[1]==3 && completed[1]==2);
    armed[2]=true; callback(lll_conn_isr_rx,&split);
    packet(&term); end_event=true; __wrap_radio_status_reset(); assert(!setups[2]);
    callback(lll_conn_isr_rx,&other);
    packet(&term); end_event=true; __wrap_radio_status_reset(); assert(!setups[3]);
    /* A handle disarmed/reused after setup cannot append old completions. */
    callback(lll_conn_isr_rx,&host); packet(&term); armed[1]=false;
    end_event=true; __wrap_radio_status_reset(); assert(completed[1]==2);
    armed[1]=true;
#if defined(CONFIG_BT_CTLR_LE_ENC)
    /* Ciphertext is opaque: never inspect its bytes for a control opcode. */
    host.enc_tx=1; callback(lll_conn_isr_rx,&host);
    struct ccm ccm={0xdead}; expected_ccm=&ccm;
    cipher=(void *)0x5678; expected_plain=&term;
    assert(__wrap_radio_ccm_tx_pkt_set(&ccm,&term)==cipher);
    unsigned before=setups[1]; packet(cipher); end_event=true;
    __wrap_radio_status_reset();
    assert(setups[1]==before+1 && completed[1]==3 && ccm.untouched==0xdead);
    /* An unmatched or already consumed CCM result cannot create a TX record. */
    packet(cipher); end_event=true; __wrap_radio_status_reset();
    assert(setups[1]==before+1);
    expected_plain=&data; __wrap_radio_ccm_tx_pkt_set(&ccm,&data);
    packet(cipher); end_event=true; __wrap_radio_status_reset();
    assert(setups[1]==before+1 && ccm_calls==2);
    /* Changing event context discards a stale encrypted-packet association. */
    expected_plain=&term; __wrap_radio_ccm_tx_pkt_set(&ccm,&term);
    callback(unrelated,&other); callback(lll_conn_isr_rx,&host);
    packet(cipher); end_event=true; __wrap_radio_status_reset();
    assert(setups[1]==before+1 && ccm_calls==3);
    assert(callback_calls==12 && packet_calls==16 && reset_calls==16);
#else
    assert(callback_calls==9 && packet_calls==12 && reset_calls==12);
#endif
    return 0;
}
"""


class HandoffRadioTxTests(unittest.TestCase):
    def test_radio_end_passthrough_and_handoff_isolation(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/handoff_radio_tx.c").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "radio_tx")
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
