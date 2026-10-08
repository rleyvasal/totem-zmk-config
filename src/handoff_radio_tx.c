/* nRF52840 radio observations only: no packet modification or ISR logging. */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <hal/nrf_radio.h>
#include <totem_handoff_diagnostics.h>
#include "hal/ccm.h"
#include "util/memq.h"
#include "util/dbuf.h"
#include "pdu_df.h"
#include "lll/pdu_vendor.h"
#include "pdu.h"
#include "lll.h"
#include "lll_conn.h"
#include "hal/radio.h"

/* The radio is shared by host, split and advertising events. Associate it only
 * with a known connection ISR, and invalidate it on a different event context.
 * Keep the same context for the connection's private final-TX/done callback. */
static struct {
    void *param;
    uint16_t handle;
    bool active, encrypted, pending;
    void *encrypted_packet;
    bool encrypted_term;
} tx_radio;

static bool is_termination(const struct pdu_data *pdu) {
    return pdu->ll_id == PDU_DATA_LLID_CTRL &&
           pdu->len >= 2 && pdu->llctrl.opcode == PDU_DATA_LLCTRL_TYPE_TERMINATE_IND;
}

static void finish_pending(void) {
    if (tx_radio.pending) {
        /* Read END directly: radio_is_done() has timer-accounting side effects
         * on some controller configurations. Never clear the hardware event. */
        bool completed = nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END);
        totem_handoff_tx_result(tx_radio.handle, completed);
        tx_radio.pending = false;
    }
}

void __real_radio_isr_set(radio_isr_cb_t cb, void *param);
void __wrap_radio_isr_set(radio_isr_cb_t cb, void *param) {
    if (cb == lll_conn_isr_rx || cb == lll_conn_isr_tx) {
        /* A new receive phase cannot complete an earlier TX. Resolve it before
         * a later RX END could otherwise be mistaken for a pending TX END. */
        if (cb == lll_conn_isr_rx) {
            finish_pending();
        }
        if (param != tx_radio.param) {
            finish_pending();
            tx_radio.encrypted_packet = NULL;
        }
        struct lll_conn *lll = param;
        tx_radio.param = param;
        tx_radio.handle = lll->handle;
        tx_radio.active = lll->role == BT_CONN_ROLE_PERIPHERAL &&
                          totem_handoff_tx_armed(lll->handle);
#if defined(CONFIG_BT_CTLR_LE_ENC)
        tx_radio.encrypted = lll->enc_tx;
#else
        tx_radio.encrypted = false;
#endif
    } else if (param != tx_radio.param) {
        finish_pending();
        tx_radio.active = false;
        tx_radio.encrypted_packet = NULL;
        tx_radio.param = NULL;
    }
    __real_radio_isr_set(cb, param);
}

#if defined(CONFIG_BT_CTLR_LE_ENC)
void *__real_radio_ccm_tx_pkt_set(struct ccm *cnf, void *packet);
void *__wrap_radio_ccm_tx_pkt_set(struct ccm *cnf, void *packet) {
    /* Inspect just the plaintext control opcode before CCM; retain no payload,
     * key, encryption counter, or pointer to the plaintext packet. */
    bool term = tx_radio.active && is_termination(packet);
    void *encrypted = __real_radio_ccm_tx_pkt_set(cnf, packet);
    tx_radio.encrypted_packet = encrypted;
    tx_radio.encrypted_term = term;
    return encrypted;
}
#endif

void __real_radio_pkt_tx_set(void *packet);
void __wrap_radio_pkt_tx_set(void *packet) {
    bool term = tx_radio.active && (tx_radio.encrypted ?
        tx_radio.encrypted_packet == packet && tx_radio.encrypted_term :
        is_termination(packet));
    tx_radio.encrypted_packet = NULL;
    finish_pending();
    __real_radio_pkt_tx_set(packet);
    if (term) {
        totem_handoff_tx_setup(tx_radio.handle);
        tx_radio.pending = true;
    }
}

void __real_radio_status_reset(void);
void __wrap_radio_status_reset(void) {
    /* Covers both the continuing TX ISR and the private final-TX/done ISR,
     * before either clears END. A canceled setup with END=0 is not a TX. */
    finish_pending();
    __real_radio_status_reset();
}
