/*
 * Multi-profile host event ring + USB-serial dump behavior.
 *
 * Captures host BLE events for any profile (idx/active are free integers).
 * RAM ring is primary; optional settings snapshot survives reboot / UF2 flash
 * without settings-reset so you can capture on production then dump on a
 * logging build (or production with ZMK_USB_LOGGING).
 *
 * SAFETY (boot brick fix):
 * - Mutex/work items are statically initialized (settings load can run before
 *   APPLICATION SYS_INIT and must not lock an uninit mutex).
 * - Persist/load/dump use static buffers (not ~1.5KB stack frames that can hard
 *   fault the main/settings stack on nRF52840).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <totem_host_event_log.h>

LOG_MODULE_REGISTER(host_event_log, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_TOTEM_HOST_EVENT_LOG)

#define RING_CAP CONFIG_TOTEM_HOST_EVENT_LOG_SIZE
#define SETTINGS_KEY "th/hevt"
#define PERSIST_EVERY CONFIG_TOTEM_HOST_EVENT_LOG_PERSIST_EVERY

struct totem_host_event {
    uint32_t uptime_ms;
    uint8_t type;
    int8_t idx;
    int8_t active;
    uint8_t reason;
    uint8_t thrash_win;
    uint8_t extra;
} __packed;

/* On-disk blob: small header + packed events (oldest→newest order at save). */
struct totem_host_event_blob {
    uint16_t magic;
    uint16_t count;
    uint32_t seq;
    struct totem_host_event ev[RING_CAP];
} __packed;

#define BLOB_MAGIC 0x4845 /* 'H''E' */

static struct totem_host_event ring[RING_CAP];
static uint16_t ring_head;  /* next write */
static uint16_t ring_count; /* 0..RING_CAP */
static uint32_t ring_seq;
static uint16_t events_since_persist;

/* Static init: settings handlers and BLE callbacks may run before SYS_INIT. */
static K_MUTEX_DEFINE(ring_mu);

/* Static buffers — never put the full blob on a call stack. */
static struct totem_host_event_blob persist_blob;
static struct totem_host_event dump_tmp[RING_CAP];

static void dump_work_handler(struct k_work *work);
static void persist_work_handler(struct k_work *work);

static K_WORK_DEFINE(dump_work, dump_work_handler);
static K_WORK_DEFINE(persist_work, persist_work_handler);

static const char *evt_name(uint8_t type) {
    switch (type) {
    case TOTEM_HEVT_DISC:
        return "disc";
    case TOTEM_HEVT_CONN:
        return "conn";
    case TOTEM_HEVT_CONN_FAIL:
        return "conn_fail";
    case TOTEM_HEVT_SEC_OK:
        return "sec_ok";
    case TOTEM_HEVT_SEC_FAIL:
        return "sec_fail";
    case TOTEM_HEVT_BG_EVICT:
        return "bg_evict";
    case TOTEM_HEVT_ACTIVE_DOWN_ARM:
        return "active_down";
    case TOTEM_HEVT_WATCH_ARM:
        return "watch_arm";
    case TOTEM_HEVT_WATCH_STEP:
        return "watch_step";
    case TOTEM_HEVT_PROFILE_CHANGED:
        return "prof_chg";
    case TOTEM_HEVT_IDENTITY:
        return "identity";
    case TOTEM_HEVT_CLASS_A_SUSPECT:
        return "class_a";
    case TOTEM_HEVT_THRASH_WIN:
        return "thrash_win";
    default:
        return "unknown";
    }
}

static void ring_get_ordered(struct totem_host_event *out, uint16_t *out_count) {
    uint16_t n = ring_count;
    *out_count = n;
    if (n == 0) {
        return;
    }
    /* Oldest first */
    uint16_t start = (uint16_t)((ring_head + RING_CAP - n) % RING_CAP);
    for (uint16_t i = 0; i < n; i++) {
        out[i] = ring[(start + i) % RING_CAP];
    }
}

void totem_host_event_log_record(uint8_t type, int8_t idx, int8_t active, uint8_t reason,
                                 uint8_t thrash_win, uint8_t extra) {
    struct totem_host_event e = {
        .uptime_ms = k_uptime_get_32(),
        .type = type,
        .idx = idx,
        .active = active,
        .reason = reason,
        .thrash_win = thrash_win,
        .extra = extra,
    };

    k_mutex_lock(&ring_mu, K_FOREVER);
    ring[ring_head] = e;
    ring_head = (uint16_t)((ring_head + 1) % RING_CAP);
    if (ring_count < RING_CAP) {
        ring_count++;
    }
    ring_seq++;
    events_since_persist++;
    bool do_persist = (events_since_persist >= PERSIST_EVERY);
    if (do_persist) {
        events_since_persist = 0;
    }
    k_mutex_unlock(&ring_mu);

    if (do_persist) {
        k_work_submit(&persist_work);
    }
}

static void persist_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    totem_host_event_log_persist();
}

void totem_host_event_log_persist(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    k_mutex_lock(&ring_mu, K_FOREVER);
    memset(&persist_blob, 0, sizeof(persist_blob));
    persist_blob.magic = BLOB_MAGIC;
    ring_get_ordered(persist_blob.ev, &persist_blob.count);
    persist_blob.seq = ring_seq;
    uint16_t count = persist_blob.count;
    uint32_t seq = persist_blob.seq;
    size_t len = offsetof(struct totem_host_event_blob, ev) +
                 (size_t)count * sizeof(struct totem_host_event);
    /* Copy length under lock; write after unlock to avoid holding mutex during flash. */
    k_mutex_unlock(&ring_mu);

    int err = settings_save_one(SETTINGS_KEY, &persist_blob, len);
    if (err) {
        LOG_WRN("totem_ble hevt persist failed err=%d count=%u", err, count);
    } else {
        LOG_DBG("totem_ble hevt persisted count=%u seq=%u", count, seq);
    }
#else
    LOG_WRN("totem_ble hevt persist skipped (SETTINGS off)");
#endif
}

static void dump_one(const struct totem_host_event *e, uint16_t i) {
    printk("totem_ble hevt i=%u t_ms=%u type=%s(%u) idx=%d active=%d reason=0x%02x "
           "thrash_win=%u extra=0x%02x\n",
           i, e->uptime_ms, evt_name(e->type), e->type, (int)e->idx, (int)e->active, e->reason,
           e->thrash_win, e->extra);
}

void totem_host_event_log_dump(void) {
    uint16_t n = 0;
    uint32_t seq;

    k_mutex_lock(&ring_mu, K_FOREVER);
    ring_get_ordered(dump_tmp, &n);
    seq = ring_seq;
    k_mutex_unlock(&ring_mu);

    printk("\n===== totem_ble HOST_EVENT_LOG dump begin count=%u seq=%u cap=%u =====\n", n, seq,
           RING_CAP);
    for (uint16_t i = 0; i < n; i++) {
        dump_one(&dump_tmp[i], i);
    }
    printk("===== totem_ble HOST_EVENT_LOG dump end =====\n\n");

    totem_host_event_log_persist();
}

static void dump_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    totem_host_event_log_dump();
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int hevt_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "hevt", &next) && !next) {
        if (len == 0 || len > sizeof(struct totem_host_event_blob)) {
            return -EINVAL;
        }

        /* Static buffer: settings load runs on a small stack. */
        memset(&persist_blob, 0, sizeof(persist_blob));
        int rc = read_cb(cb_arg, &persist_blob, MIN(len, sizeof(persist_blob)));
        if (rc < 0) {
            return rc;
        }
        if (persist_blob.magic != BLOB_MAGIC || persist_blob.count > RING_CAP) {
            LOG_WRN("totem_ble hevt settings invalid magic/count");
            return 0;
        }

        k_mutex_lock(&ring_mu, K_FOREVER);
        ring_head = 0;
        ring_count = 0;
        for (uint16_t i = 0; i < persist_blob.count; i++) {
            ring[ring_head] = persist_blob.ev[i];
            ring_head = (uint16_t)((ring_head + 1) % RING_CAP);
            ring_count++;
        }
        ring_seq = persist_blob.seq;
        events_since_persist = 0;
        k_mutex_unlock(&ring_mu);
        LOG_INF("totem_ble hevt restored count=%u seq=%u", persist_blob.count, persist_blob.seq);
        return 0;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(totem_hevt, "th", NULL, hevt_settings_set, NULL, NULL);
#endif /* CONFIG_SETTINGS */

/* --- dump behavior: &host_log_dump --- */

#define DT_DRV_COMPAT zmk_behavior_totem_host_log_dump

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_dump_pressed(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    LOG_WRN("totem_ble hevt dump requested");
    k_work_submit(&dump_work);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_dump_released(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api host_log_dump_driver_api = {
    .binding_pressed = on_dump_pressed,
    .binding_released = on_dump_released,
    .locality = BEHAVIOR_LOCALITY_EVENT_SOURCE,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &host_log_dump_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY */

#endif /* CONFIG_TOTEM_HOST_EVENT_LOG */
