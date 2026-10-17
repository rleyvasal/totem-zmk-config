/* Aggregate metadata only: no keycodes, report bodies, or per-key flash writes. */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <totem_host_event_log.h>
#include <totem_hid_report_diagnostics.h>

enum hid_stage {
    RELEASE_OK, RELEASE_FAILED, QUEUED, QUEUE_FAILED, DEQUEUED, EVICTED,
    NO_HOST_DROP, NOTIFY_ACCEPTED, NOTIFY_FAILED, UNSUBSCRIBED,
    ALL_UP_QUEUED, ALL_UP_ACCEPTED, ALL_UP_EVICTED, ALL_UP_FAILED, HID_STAGE_COUNT,
};

struct hid_count {
    uint32_t last_ms;
    uint16_t total;
    uint8_t error;
    bool dirty;
};
static struct hid_count counts[ZMK_BLE_PROFILE_COUNT][HID_STAGE_COUNT];
static struct k_spinlock count_lock;
extern struct k_msgq zmk_hog_keyboard_msgq;
extern struct k_work_q hog_work_q;
/* Only accessed by the HOG worker; never retains report contents. */
static bool dequeued_report, all_up_report, keyboard_notify;
static int notify_profile;

static void flush_counts(struct k_work *work) {
    ARG_UNUSED(work);
    bool changed = false;
    for (int profile = 0; profile < ZMK_BLE_PROFILE_COUNT; profile++) {
        for (int stage = 0; stage < HID_STAGE_COUNT; stage++) {
            k_spinlock_key_t key = k_spin_lock(&count_lock);
            struct hid_count count = counts[profile][stage];
            counts[profile][stage].dirty = false;
            counts[profile][stage].error = 0;
            k_spin_unlock(&count_lock, key);
            if (!count.dirty) {
                continue;
            }
            changed = true;
            totem_host_event_log_record_timing(count.last_ms, TOTEM_HEVT_HID_COUNT,
                profile, -1, stage, count.total & 0xff, count.total >> 8);
            if (count.error) {
                totem_host_event_log_record_timing(count.last_ms, TOTEM_HEVT_HID_ERROR,
                    profile, -1, stage, count.error, 0);
            }
        }
    }
    if (changed) {
        /* Existing journal rate limit still applies; no persistence in hooks. */
        totem_host_event_log_persist();
    }
}
static K_WORK_DELAYABLE_DEFINE(summary_work, flush_counts);

static void count_event(int profile, enum hid_stage stage, int err) {
    if (profile < 0 || profile >= ZMK_BLE_PROFILE_COUNT) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&count_lock);
    struct hid_count *count = &counts[profile][stage];
    count->total++;
    count->last_ms = k_uptime_get_32();
    count->dirty = true;
    if (err) {
        count->error = MIN(err < 0 ? -err : err, UINT8_MAX);
    }
    k_spin_unlock(&count_lock, key);
    /* schedule, not reschedule: continuous typing cannot postpone the summary. */
    k_work_schedule(&summary_work, K_SECONDS(5));
}

static bool all_up(const struct zmk_hid_keyboard_report_body *report) {
    if (report->modifiers) {
        return false;
    }
    for (size_t i = 0; i < sizeof(report->keys); i++) {
        if (report->keys[i]) {
            return false;
        }
    }
    return true;
}

int __real_zmk_hid_release(uint32_t usage);
int __wrap_zmk_hid_release(uint32_t usage) {
    int profile = zmk_ble_active_profile_index();
    bool trace = ZMK_HID_USAGE_PAGE(usage) == HID_USAGE_KEY &&
                 zmk_endpoint_get_selected().transport != ZMK_TRANSPORT_USB;
    int err = __real_zmk_hid_release(usage);
    if (trace) {
        count_event(profile, err < 0 ? RELEASE_FAILED : RELEASE_OK, err < 0 ? err : 0);
    }
    return err;
}

int __real_z_impl_k_msgq_put(struct k_msgq *queue, const void *data, k_timeout_t timeout);
int __wrap_z_impl_k_msgq_put(struct k_msgq *queue, const void *data, k_timeout_t timeout) {
    if (queue != &zmk_hog_keyboard_msgq) {
        return __real_z_impl_k_msgq_put(queue, data, timeout);
    }
    int profile = zmk_ble_active_profile_index();
    bool released = all_up(data);
    int err = __real_z_impl_k_msgq_put(queue, data, timeout);
    count_event(profile, err ? QUEUE_FAILED : QUEUED, err);
    if (!err && released) {
        count_event(profile, ALL_UP_QUEUED, 0);
    }
    return err;
}

int __real_z_impl_k_msgq_get(struct k_msgq *queue, void *data, k_timeout_t timeout);
int __wrap_z_impl_k_msgq_get(struct k_msgq *queue, void *data, k_timeout_t timeout) {
    int err = __real_z_impl_k_msgq_get(queue, data, timeout);
    if (queue == &zmk_hog_keyboard_msgq) {
        bool worker = k_current_get() == k_work_queue_thread_get(&hog_work_q);
        if (worker) {
            dequeued_report = !err;
        }
        if (!err) {
            int profile = zmk_ble_active_profile_index();
            bool released = all_up(data);
            count_event(profile, worker ? DEQUEUED : EVICTED, 0);
            if (worker) {
                all_up_report = released;
            } else if (released) {
                count_event(profile, ALL_UP_EVICTED, 0);
            }
        }
    }
    return err;
}

struct bt_conn *__real_zmk_ble_active_profile_conn(void);
struct bt_conn *__wrap_zmk_ble_active_profile_conn(void) {
    struct bt_conn *conn = __real_zmk_ble_active_profile_conn();
    if (!conn && k_current_get() == k_work_queue_thread_get(&hog_work_q) && dequeued_report) {
        count_event(zmk_ble_active_profile_index(), NO_HOST_DROP, 0);
        dequeued_report = false;
    }
    return conn;
}

void totem_hid_report_observed(struct bt_conn *conn, bool subscribed) {
    notify_profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    keyboard_notify = true;
    dequeued_report = false;
    if (!subscribed) {
        count_event(notify_profile, UNSUBSCRIBED, 0);
    }
}

int __real_bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params);
int __wrap_bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params) {
    bool trace = k_current_get() == k_work_queue_thread_get(&hog_work_q) && keyboard_notify;
    if (!trace) {
        return __real_bt_gatt_notify_cb(conn, params);
    }
    keyboard_notify = false;
    int err = __real_bt_gatt_notify_cb(conn, params);
    count_event(notify_profile, err ? NOTIFY_FAILED : NOTIFY_ACCEPTED, err);
    if (all_up_report) {
        count_event(notify_profile, err ? ALL_UP_FAILED : ALL_UP_ACCEPTED, err);
    }
    return err;
}
