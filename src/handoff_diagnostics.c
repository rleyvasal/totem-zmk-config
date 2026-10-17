/* Observe host handoff only. Never print, persist, or change controller state. */
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zmk/ble.h>
#include <zmk/usb.h>
#include <totem_host_event_log.h>
#include <totem_handoff_diagnostics.h>

/* Use actual controller types rather than reproducing private layouts. */
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
#include "ull_llcp_internal.h"

struct handoff_radio {
    bool armed, rx_known, first_bad;
    uint32_t last_rx;
    uint16_t done, bad, aborted, skipped;
    uint16_t rx_packets, no_rx, invalid_rx, mic_pass, mic_fail;
    uint16_t tx_setups, tx_completed, tx_uncompleted;
    uint32_t first_tx_setup, first_tx_end, last_tx_end;
};
static struct handoff_radio radios[CONFIG_BT_MAX_CONN];
static struct k_spinlock radio_lock;

static void capture(uint32_t time, uint8_t type, uint16_t handle,
                    uint8_t stage, uint8_t low, uint8_t high) {
    totem_diagnostic_timing_capture(time, type, (int8_t)handle,
                                    (int8_t)(handle >> 8), stage, low, high);
}

static void capture_value(uint8_t type, uint16_t handle, uint8_t stage, uint16_t value) {
    capture(k_uptime_get_32(), type, handle, stage, value & 0xff, value >> 8);
}

bool totem_handoff_tx_armed(uint16_t handle) {
    if (handle >= ARRAY_SIZE(radios)) {
        return false;
    }
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    bool armed = radios[handle].armed;
    k_spin_unlock(&radio_lock, key);
    return armed;
}

static uint16_t add_count(uint16_t value, uint32_t count);

void totem_handoff_tx_setup(uint16_t handle) {
    if (handle >= ARRAY_SIZE(radios)) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    struct handoff_radio *radio = &radios[handle];
    if (radio->armed) {
        if (!radio->tx_setups) {
            radio->first_tx_setup = k_uptime_get_32();
        }
        radio->tx_setups = add_count(radio->tx_setups, 1);
    }
    k_spin_unlock(&radio_lock, key);
}

void totem_handoff_tx_result(uint16_t handle, bool completed) {
    if (handle >= ARRAY_SIZE(radios)) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    struct handoff_radio *radio = &radios[handle];
    if (radio->armed) {
        if (completed) {
            uint32_t now = k_uptime_get_32();
            if (!radio->tx_completed) {
                radio->first_tx_end = now;
            }
            radio->last_tx_end = now;
            radio->tx_completed = add_count(radio->tx_completed, 1);
        } else {
            radio->tx_uncompleted = add_count(radio->tx_uncompleted, 1);
        }
    }
    k_spin_unlock(&radio_lock, key);
}

void totem_handoff_connected(uint16_t handle) {
    if (handle >= ARRAY_SIZE(radios)) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    radios[handle] = (struct handoff_radio){0};
    k_spin_unlock(&radio_lock, key);
}

void totem_handoff_disconnected(uint16_t handle) {
    if (handle >= ARRAY_SIZE(radios)) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    struct handoff_radio radio = radios[handle];
    radios[handle] = (struct handoff_radio){0};
    k_spin_unlock(&radio_lock, key);
    if (!radio.armed) {
        return;
    }
    capture_value(TOTEM_HEVT_HANDOFF_RADIO, handle, 0, radio.done);
    capture_value(TOTEM_HEVT_HANDOFF_RADIO, handle, 1, radio.bad);
    capture_value(TOTEM_HEVT_HANDOFF_RADIO, handle, 2, radio.aborted);
    capture_value(TOTEM_HEVT_HANDOFF_RADIO, handle, 3, radio.skipped);
    capture_value(TOTEM_HEVT_HANDOFF_RADIO, handle, 7, radio.rx_packets);
    capture_value(TOTEM_HEVT_HANDOFF_RADIO, handle, 8, radio.no_rx);
    capture_value(TOTEM_HEVT_HANDOFF_RADIO, handle, 9, radio.invalid_rx);
    capture_value(TOTEM_HEVT_HANDOFF_RADIO, handle, 10, radio.mic_pass);
    capture_value(TOTEM_HEVT_HANDOFF_RADIO, handle, 11, radio.mic_fail);
    capture(radio.rx_known ? radio.last_rx : k_uptime_get_32(),
            TOTEM_HEVT_HANDOFF_RADIO, handle, 5, 0, radio.rx_known);
    capture_value(TOTEM_HEVT_HANDOFF_TX, handle, 0, radio.tx_setups);
    capture_value(TOTEM_HEVT_HANDOFF_TX, handle, 1, radio.tx_completed);
    capture_value(TOTEM_HEVT_HANDOFF_TX, handle, 2, radio.tx_uncompleted);
    capture(radio.tx_setups ? radio.first_tx_setup : k_uptime_get_32(),
            TOTEM_HEVT_HANDOFF_TX, handle, 3, 0, radio.tx_setups != 0);
    capture(radio.tx_completed ? radio.first_tx_end : k_uptime_get_32(),
            TOTEM_HEVT_HANDOFF_TX, handle, 4, 0, radio.tx_completed != 0);
    capture(radio.tx_completed ? radio.last_tx_end : k_uptime_get_32(),
            TOTEM_HEVT_HANDOFF_TX, handle, 5, 0, radio.tx_completed != 0);
}

int __real_bt_conn_disconnect(struct bt_conn *conn, uint8_t reason);

int __wrap_bt_conn_disconnect(struct bt_conn *conn, uint8_t reason) {
    struct bt_conn_info info;
    uint16_t handle;
    bool handoff = false;
    if (!bt_conn_get_info(conn, &info) && info.type == BT_CONN_TYPE_LE &&
        info.role == BT_CONN_ROLE_PERIPHERAL && info.state == BT_CONN_STATE_CONNECTED &&
        !zmk_usb_is_powered()) {
        int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
        if (profile >= 0 && profile != zmk_ble_active_profile_index() &&
            !bt_hci_get_conn_handle(conn, &handle) && handle < ARRAY_SIZE(radios)) {
            k_spinlock_key_t key = k_spin_lock(&radio_lock);
            struct handoff_radio previous = radios[handle];
            radios[handle] = (struct handoff_radio){
                .armed = true, .rx_known = previous.rx_known, .last_rx = previous.last_rx};
            k_spin_unlock(&radio_lock, key);
            handoff = true;
            capture(k_uptime_get_32(), TOTEM_HEVT_HANDOFF_TERM, handle, 0, 0, reason);
            capture_value(TOTEM_HEVT_HANDOFF_PARAM, handle, 0, info.le.interval);
            capture_value(TOTEM_HEVT_HANDOFF_PARAM, handle, 1, info.le.latency);
            capture_value(TOTEM_HEVT_HANDOFF_PARAM, handle, 2, info.le.timeout);
            capture(previous.rx_known ? previous.last_rx : k_uptime_get_32(),
                    TOTEM_HEVT_HANDOFF_RADIO, handle, 4, 0, previous.rx_known);
        }
    }
    int err = __real_bt_conn_disconnect(conn, reason);
    if (handoff) {
        capture(k_uptime_get_32(), TOTEM_HEVT_HANDOFF_TERM, handle, 1,
                err < 0 ? 1 : err > 0 ? 2 : 0,
                (uint8_t)MIN(err < 0 ? -err : err, UINT8_MAX));
        if (err) {
            k_spinlock_key_t key = k_spin_lock(&radio_lock);
            radios[handle].armed = false;
            k_spin_unlock(&radio_lock, key);
        }
    }
    return err;
}

static void termination_observed(struct ll_conn *conn, struct node_tx *tx, uint8_t stage) {
    uint16_t handle = conn->lll.handle;
    const struct pdu_data *pdu = (const void *)tx->pdu;
    if (handle >= ARRAY_SIZE(radios) || pdu->ll_id != PDU_DATA_LLID_CTRL ||
        pdu->llctrl.opcode != PDU_DATA_LLCTRL_TYPE_TERMINATE_IND) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    bool armed = radios[handle].armed;
    k_spin_unlock(&radio_lock, key);
    if (armed) {
        capture(k_uptime_get_32(), TOTEM_HEVT_HANDOFF_TERM, handle, stage, 0, 0);
    }
}

void __real_llcp_tx_enqueue(struct ll_conn *conn, struct node_tx *tx);
void __wrap_llcp_tx_enqueue(struct ll_conn *conn, struct node_tx *tx) {
    termination_observed(conn, tx, 2);
    totem_security_tx_queued(conn, tx);
    __real_llcp_tx_enqueue(conn, tx);
}

void __real_llcp_lp_comm_tx_ack(struct ll_conn *conn, struct proc_ctx *ctx, struct node_tx *tx);
void __wrap_llcp_lp_comm_tx_ack(struct ll_conn *conn, struct proc_ctx *ctx, struct node_tx *tx) {
    termination_observed(conn, tx, 3);
    __real_llcp_lp_comm_tx_ack(conn, ctx, tx);
}

int __real_ull_cp_prt_elapse(struct ll_conn *conn, uint16_t elapsed_event, uint8_t *error_code);
int __wrap_ull_cp_prt_elapse(struct ll_conn *conn, uint16_t elapsed_event, uint8_t *error_code) {
    /* Snapshot before Zephyr decrements the timer; a remote timeout must not
     * be mistaken for a local timer that merely became small this event. */
    uint16_t local_expire = conn->llcp.local.prt_expire;
    int result = __real_ull_cp_prt_elapse(conn, elapsed_event, error_code);
    uint16_t handle = conn->lll.handle;
    if (result != -ETIMEDOUT || !local_expire || local_expire > elapsed_event ||
        conn->lll.role != BT_CONN_ROLE_PERIPHERAL || handle >= ARRAY_SIZE(radios)) {
        return result;
    }
    /* The pinned implementation leaves the procedure allocated for its caller
     * to clean up, so inspect only its type after the confirmed timeout. */
    struct proc_ctx *ctx = llcp_lr_peek(conn);
    if (ctx && ctx->proc == PROC_TERMINATE) {
        k_spinlock_key_t key = k_spin_lock(&radio_lock);
        bool armed = radios[handle].armed;
        k_spin_unlock(&radio_lock, key);
        if (armed) {
            capture(k_uptime_get_32(), TOTEM_HEVT_HANDOFF_TERM, handle, 4, 0, *error_code);
        }
    }
    return result;
}

static uint16_t add_count(uint16_t value, uint32_t count) {
    return MIN((uint32_t)value + count, UINT16_MAX);
}

void __real_ull_conn_done(struct node_rx_event_done *done);
void __wrap_ull_conn_done(struct node_rx_event_done *done) {
    struct ll_conn *conn = CONTAINER_OF(done->param, struct ll_conn, ull);
    uint16_t handle = conn->lll.handle;
    if (handle < ARRAY_SIZE(radios) && conn->lll.role == BT_CONN_ROLE_PERIPHERAL) {
        uint32_t now = k_uptime_get_32();
        bool valid = done->extra.crc_valid && !done->extra.is_aborted;
        bool first_bad = false;
        k_spinlock_key_t key = k_spin_lock(&radio_lock);
        struct handoff_radio *radio = &radios[handle];
        if (valid) {
            radio->rx_known = true;
            radio->last_rx = now;
        }
        if (radio->armed) {
            radio->done = add_count(radio->done, 1);
            radio->bad = add_count(radio->bad, !valid);
            radio->aborted = add_count(radio->aborted, done->extra.is_aborted);
            radio->rx_packets = add_count(radio->rx_packets, done->extra.trx_cnt);
            radio->no_rx = add_count(radio->no_rx,
                !done->extra.is_aborted && !done->extra.trx_cnt);
            radio->invalid_rx = add_count(radio->invalid_rx,
                !done->extra.is_aborted && done->extra.trx_cnt && !done->extra.crc_valid);
#if defined(CONFIG_BT_CTLR_LE_ENC)
            radio->mic_pass = add_count(radio->mic_pass, done->extra.mic_state == LLL_CONN_MIC_PASS);
            radio->mic_fail = add_count(radio->mic_fail, done->extra.mic_state == LLL_CONN_MIC_FAIL);
#endif
            radio->skipped = add_count(radio->skipped,
                conn->lll.lazy_prepare +
#if defined(CONFIG_BT_CTLR_CONN_META)
                (conn->common.is_must_expire ? 0 : conn->lll.latency_event));
#else
                conn->lll.latency_event);
#endif
            first_bad = !valid && !radio->first_bad;
            radio->first_bad |= !valid;
        }
        k_spin_unlock(&radio_lock, key);
        if (first_bad) {
            capture(now, TOTEM_HEVT_HANDOFF_RADIO, handle, 6, 0, done->extra.is_aborted);
        }
    }
    __real_ull_conn_done(done);
}
