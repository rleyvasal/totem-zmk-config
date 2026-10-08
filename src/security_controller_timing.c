/* Timing metadata only. Never retain encryption payloads or key material. */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <totem_host_event_log.h>
#include <totem_handoff_diagnostics.h>
#include "hal/ccm.h"
#include "util/memq.h"
#include "util/dbuf.h"
#include "pdu_df.h"
#include "lll/pdu_vendor.h"
#include "pdu.h"
#include "lll.h"
#include "lll_conn.h"
#include "ull_tx_queue.h"
#include "ull_conn_types.h"

static void exchange_capture(struct ll_conn *conn, uint8_t stage, uint8_t code, uint8_t extra) {
    if (conn->lll.role == BT_CONN_ROLE_PERIPHERAL) {
        uint16_t handle = conn->lll.handle;
        totem_diagnostic_timing_capture(k_uptime_get_32(), TOTEM_HEVT_SECURITY_EXCHANGE,
            (int8_t)handle, (int8_t)(handle >> 8), stage, code, extra);
    }
}

static bool encryption_pdu(const struct pdu_data *pdu) {
    if (pdu->ll_id != PDU_DATA_LLID_CTRL || pdu->len < 1) {
        return false;
    }
    switch (pdu->llctrl.opcode) {
    case PDU_DATA_LLCTRL_TYPE_ENC_REQ:
        return pdu->len == PDU_DATA_LLCTRL_LEN(enc_req);
    case PDU_DATA_LLCTRL_TYPE_ENC_RSP:
        return pdu->len == PDU_DATA_LLCTRL_LEN(enc_rsp);
    case PDU_DATA_LLCTRL_TYPE_START_ENC_REQ:
        return pdu->len == PDU_DATA_LLCTRL_LEN(start_enc_req);
    case PDU_DATA_LLCTRL_TYPE_START_ENC_RSP:
        return pdu->len == PDU_DATA_LLCTRL_LEN(start_enc_rsp);
    default:
        return false;
    }
}

void totem_security_tx_queued(struct ll_conn *conn, struct node_tx *tx) {
    const struct pdu_data *pdu = (const void *)tx->pdu;
    if (encryption_pdu(pdu)) {
        exchange_capture(conn, 3, pdu->llctrl.opcode, 0);
    }
}

void __real_ull_cp_rx(struct ll_conn *conn, memq_link_t *link, struct node_rx_pdu *rx);
void __wrap_ull_cp_rx(struct ll_conn *conn, memq_link_t *link, struct node_rx_pdu *rx) {
    const struct pdu_data *pdu = (const void *)rx->pdu;
    if (encryption_pdu(pdu)) {
        exchange_capture(conn, 2, pdu->llctrl.opcode, 0);
    }
    __real_ull_cp_rx(conn, link, rx);
}

uint8_t __real_ull_cp_ltk_req_reply(struct ll_conn *conn, const uint8_t ltk[16]);
uint8_t __wrap_ull_cp_ltk_req_reply(struct ll_conn *conn, const uint8_t ltk[16]) {
    exchange_capture(conn, 4, 0, 1);
    uint8_t result = __real_ull_cp_ltk_req_reply(conn, ltk);
    exchange_capture(conn, 5, result, 1);
    return result;
}

uint8_t __real_ull_cp_ltk_req_neq_reply(struct ll_conn *conn);
uint8_t __wrap_ull_cp_ltk_req_neq_reply(struct ll_conn *conn) {
    exchange_capture(conn, 4, 0, 0);
    uint8_t result = __real_ull_cp_ltk_req_neq_reply(conn);
    exchange_capture(conn, 5, result, 0);
    return result;
}
