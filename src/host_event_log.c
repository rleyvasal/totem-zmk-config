/*
 * Persistent diagnostic black box + USB-serial dump behavior.
 *
 * Captures USB, advertising, BLE host/split, recovery and fault events. RAM
 * is primary; compact CRC-checked settings blocks rotate so recent history
 * survives reboot / UF2 flash without settings-reset.
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
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <totem_host_event_log.h>

LOG_MODULE_REGISTER(host_event_log, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_TOTEM_HOST_EVENT_LOG)

#define RING_CAP CONFIG_TOTEM_HOST_EVENT_LOG_SIZE
#define SETTINGS_KEY_PREFIX "th/dlog"
#define PERSIST_EVERY CONFIG_TOTEM_HOST_EVENT_LOG_PERSIST_EVERY
#define BLOCK_EVENTS CONFIG_TOTEM_HOST_EVENT_LOG_BLOCK_SIZE
#define JOURNAL_SLOTS CONFIG_TOTEM_HOST_EVENT_LOG_SLOTS
/* Flash NVS under dual-host thrash can stall the central for long enough that
 * BLE + USB HID look dead until power-cycle. Never persist more often than this. */
#define PERSIST_MIN_INTERVAL_MS CONFIG_TOTEM_HOST_EVENT_LOG_PERSIST_MIN_MS

struct totem_host_event {
    uint32_t uptime_ms;
    uint8_t type;
    int8_t idx;
    int8_t active;
    uint8_t reason;
    uint8_t thrash_win;
    uint8_t extra;
} __packed;

/* One settings value per slot. A write is valid only when its CRC covers the
 * complete header and event payload; an interrupted flash write is ignored on
 * the next boot and the previous slot remains available. */
struct totem_diag_block {
    uint32_t magic;
    uint16_t version;
    uint8_t slot;
    uint8_t count;
    uint32_t journal_seq;
    uint32_t first_event_seq;
    struct totem_host_event ev[BLOCK_EVENTS];
    uint32_t crc;
} __packed;

#define BLOCK_MAGIC 0x54444C47u /* 'TDLG' */
#define BLOCK_VERSION 1

static struct totem_host_event ring[RING_CAP];
static uint16_t ring_head;  /* next write */
static uint16_t ring_count; /* 0..RING_CAP */
static uint32_t ring_seq;
static uint32_t persisted_event_seq;
static uint32_t journal_seq;
static uint8_t next_slot;
static uint16_t events_since_persist;
static int64_t last_persist_uptime_ms;
static bool persist_requested;

/* Static init: settings handlers and BLE callbacks may run before SYS_INIT. */
static K_MUTEX_DEFINE(ring_mu);

/* Static buffers — never put the full blob on a call stack. */
static struct totem_diag_block persist_block;
static struct totem_diag_block restored_blocks[JOURNAL_SLOTS];
static bool restored_valid[JOURNAL_SLOTS];
static struct totem_host_event dump_tmp[RING_CAP];

static void dump_work_handler(struct k_work *work);
static void persist_work_handler(struct k_work *work);

static K_WORK_DEFINE(dump_work, dump_work_handler);
static K_WORK_DELAYABLE_DEFINE(persist_work, persist_work_handler);

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
    case TOTEM_HEVT_BOOT:
        return "boot";
    case TOTEM_HEVT_FAULT:
        return "fault";
    case TOTEM_HEVT_USB:
        return "usb";
    case TOTEM_HEVT_ADV:
        return "adv";
    case TOTEM_HEVT_SPLIT:
        return "split";
    case TOTEM_HEVT_REPEAT:
        return "repeat";
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

static void schedule_persist_coalesced(void) {
    persist_requested = true;
    int64_t now = k_uptime_get();
    int64_t elapsed = now - last_persist_uptime_ms;
    int32_t wait_ms = 0;

    if (last_persist_uptime_ms != 0 && elapsed < PERSIST_MIN_INTERVAL_MS) {
        wait_ms = (int32_t)(PERSIST_MIN_INTERVAL_MS - elapsed);
    }
    /* Coalesce: many thrash events → one delayed flash write. */
    k_work_reschedule(&persist_work, K_MSEC(wait_ms));
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
    /* Continue event ordering across boots after settings restored the latest
     * persisted sequence. */
    if (ring_seq < persisted_event_seq) {
        ring_seq = persisted_event_seq;
    }
    ring[ring_head] = e;
    ring_head = (uint16_t)((ring_head + 1) % RING_CAP);
    if (ring_count < RING_CAP) {
        ring_count++;
    }
    ring_seq++;
    events_since_persist++;
    bool need_persist = (events_since_persist >= PERSIST_EVERY);
    if (need_persist) {
        events_since_persist = 0;
    }
    k_mutex_unlock(&ring_mu);

    /* A quiet failure may produce only one record, so also schedule a bounded
     * time-based flush. The work item coalesces storms into one write. */
    ARG_UNUSED(need_persist);
    schedule_persist_coalesced();
}

static void persist_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!persist_requested) {
        return;
    }
    totem_host_event_log_persist();
}

void totem_host_event_log_persist(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    int64_t now = k_uptime_get();
    if (last_persist_uptime_ms != 0 && (now - last_persist_uptime_ms) < PERSIST_MIN_INTERVAL_MS) {
        /* Too soon (e.g. dump requested during thrash) — defer. */
        schedule_persist_coalesced();
        return;
    }

    k_mutex_lock(&ring_mu, K_FOREVER);
    uint32_t first_available = ring_count ? ring_seq - ring_count + 1 : ring_seq + 1;
    uint32_t first_new = persisted_event_seq + 1;
    if (first_new < first_available) {
        first_new = first_available;
    }
    if (first_new > ring_seq) {
        persist_requested = false;
        k_mutex_unlock(&ring_mu);
        return;
    }
    uint32_t pending = ring_seq - first_new + 1;
    uint16_t count = (uint16_t)MIN(pending, (uint32_t)BLOCK_EVENTS);
    /* Preserve the newest records if a long storm exceeded one block. */
    first_new = ring_seq - count + 1;
    uint16_t start = (uint16_t)((ring_head + RING_CAP - ring_count) % RING_CAP);

    memset(&persist_block, 0, sizeof(persist_block));
    persist_block.magic = BLOCK_MAGIC;
    persist_block.version = BLOCK_VERSION;
    persist_block.slot = next_slot;
    persist_block.count = (uint8_t)count;
    persist_block.journal_seq = ++journal_seq;
    persist_block.first_event_seq = first_new;
    for (uint16_t i = 0; i < count; i++) {
        uint32_t event_seq = first_new + i;
        uint16_t offset = (uint16_t)(event_seq - first_available);
        persist_block.ev[i] = ring[(start + offset) % RING_CAP];
    }
    persist_block.crc = crc32_ieee((const uint8_t *)&persist_block,
                                   offsetof(struct totem_diag_block, crc));
    uint8_t slot = next_slot;
    char key[20];
    snprintk(key, sizeof(key), SETTINGS_KEY_PREFIX "/%u", slot);
    size_t len = offsetof(struct totem_diag_block, ev) +
                 (size_t)count * sizeof(struct totem_host_event) + sizeof(persist_block.crc);
    persist_requested = false;
    /* Copy under lock; write after unlock so BLE callbacks are not blocked on flash. */
    k_mutex_unlock(&ring_mu);

    int err = settings_save_one(key, &persist_block, len);
    last_persist_uptime_ms = k_uptime_get();
    if (err) {
        LOG_WRN("totem_diag persist failed err=%d count=%u", err, count);
    } else {
        k_mutex_lock(&ring_mu, K_FOREVER);
        persisted_event_seq = ring_seq;
        restored_blocks[slot] = persist_block;
        restored_valid[slot] = true;
        next_slot = (uint8_t)((slot + 1) % JOURNAL_SLOTS);
        k_mutex_unlock(&ring_mu);
        LOG_DBG("totem_diag persisted slot=%u count=%u seq=%u", slot, count, journal_seq);
    }
#else
    LOG_WRN("totem_ble hevt persist skipped (SETTINGS off)");
    persist_requested = false;
#endif
}

static void dump_one(const struct totem_host_event *e, uint16_t i) {
    printk("totem_ble hevt i=%u t_ms=%u type=%s(%u) idx=%d active=%d reason=0x%02x "
           "thrash_win=%u extra=0x%02x\n",
           i, e->uptime_ms, evt_name(e->type), e->type, (int)e->idx, (int)e->active, e->reason,
           e->thrash_win, e->extra);
}

static void dump_persistent(void) {
    printk("===== totem_diag persistent journal begin slots=%u =====\n", JOURNAL_SLOTS);
    /* next_slot is the oldest candidate after a complete rotation. */
    for (uint8_t off = 0; off < JOURNAL_SLOTS; off++) {
        uint8_t slot = (uint8_t)((next_slot + off) % JOURNAL_SLOTS);
        if (!restored_valid[slot]) {
            continue;
        }
        const struct totem_diag_block *block = &restored_blocks[slot];
        printk("totem_diag block slot=%u seq=%u first=%u count=%u\n", slot, block->journal_seq,
               block->first_event_seq, block->count);
        for (uint8_t i = 0; i < block->count; i++) {
            dump_one(&block->ev[i], i);
        }
    }
    printk("===== totem_diag persistent journal end =====\n");
}

void totem_host_event_log_dump(void) {
    uint16_t n = 0;
    uint32_t seq;

    k_mutex_lock(&ring_mu, K_FOREVER);
    ring_get_ordered(dump_tmp, &n);
    seq = ring_seq;
    k_mutex_unlock(&ring_mu);

    dump_persistent();
    printk("\n===== totem_diag RAM ring begin count=%u seq=%u cap=%u =====\n", n, seq,
           RING_CAP);
    for (uint16_t i = 0; i < n; i++) {
        dump_one(&dump_tmp[i], i);
    }
    printk("===== totem_diag RAM ring end =====\n\n");

    totem_host_event_log_persist();
}

static void dump_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    totem_host_event_log_dump();
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int hevt_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strncmp(name, "dlog/", 5) != 0 || name[5] < '0' || name[5] > '9' || name[6] != '\0') {
        return -ENOENT;
    }
    uint8_t slot = (uint8_t)(name[5] - '0');
    if (slot >= JOURNAL_SLOTS || len < offsetof(struct totem_diag_block, ev) + sizeof(uint32_t) ||
        len > sizeof(struct totem_diag_block)) {
        return 0;
    }

    struct totem_diag_block *block = &restored_blocks[slot];
    memset(block, 0, sizeof(*block));
    int rc = read_cb(cb_arg, block, len);
    if (rc < 0) {
        return rc;
    }
    size_t expected = offsetof(struct totem_diag_block, ev) +
                      (size_t)block->count * sizeof(struct totem_host_event) + sizeof(block->crc);
    uint32_t crc = crc32_ieee((const uint8_t *)block, offsetof(struct totem_diag_block, crc));
    if (block->magic != BLOCK_MAGIC || block->version != BLOCK_VERSION || block->slot != slot ||
        block->count > BLOCK_EVENTS || len != expected || block->crc != crc) {
        LOG_WRN("totem_diag ignored invalid journal slot=%u", slot);
        return 0;
    }
    restored_valid[slot] = true;
    if (block->journal_seq >= journal_seq) {
        journal_seq = block->journal_seq;
        next_slot = (uint8_t)((slot + 1) % JOURNAL_SLOTS);
    }
    uint32_t last_event_seq = block->first_event_seq + block->count - 1;
    if (last_event_seq > persisted_event_seq) {
        persisted_event_seq = last_event_seq;
    }
    return 0;
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
