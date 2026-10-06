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
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/settings.h>

#include <totem_host_event_log.h>
#include <totem_studio_log.h>

LOG_MODULE_REGISTER(host_event_log, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_TOTEM_HOST_EVENT_LOG)

#if IS_ENABLED(CONFIG_SETTINGS) && !defined(ZMK_SETTINGS_LOADED_HOOK_VERSION)
#error "Persistent diagnostics require a ZMK revision with the post-settings-load hook"
#endif

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
static bool journal_ready = !IS_ENABLED(CONFIG_SETTINGS);

/* Static init: settings handlers and BLE callbacks may run before SYS_INIT. */
static K_MUTEX_DEFINE(ring_mu);
static K_MUTEX_DEFINE(persist_mu);

/* Static buffers — never put the full blob on a call stack. */
static struct totem_diag_block persist_block;
static struct totem_diag_block restored_blocks[JOURNAL_SLOTS];
static bool restored_valid[JOURNAL_SLOTS];
static struct totem_host_event dump_tmp[RING_CAP];
static struct totem_diag_block dump_blocks[JOURNAL_SLOTS];
static bool dump_valid[JOURNAL_SLOTS];
static K_MUTEX_DEFINE(dump_mu);
static struct k_work_q dump_queue;
K_THREAD_STACK_DEFINE(dump_stack, 1536);

static void dump_work_handler(struct k_work *work);
static void persist_work_handler(struct k_work *work);

static K_WORK_DEFINE(dump_work, dump_work_handler);
static K_WORK_DELAYABLE_DEFINE(persist_work, persist_work_handler);

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

static void schedule_persist_coalesced(bool urgent) {
    k_mutex_lock(&ring_mu, K_FOREVER);
    persist_requested = true;
    if (!journal_ready) {
        k_mutex_unlock(&ring_mu);
        return;
    }
    int64_t now = k_uptime_get();
    int64_t elapsed = now - last_persist_uptime_ms;
    int32_t wait_ms = urgent ? 0 : PERSIST_MIN_INTERVAL_MS;

    if (last_persist_uptime_ms != 0 && elapsed < PERSIST_MIN_INTERVAL_MS) {
        wait_ms = (int32_t)(PERSIST_MIN_INTERVAL_MS - elapsed);
    }
    k_mutex_unlock(&ring_mu);
    /* Coalesce: many thrash events → one delayed flash write. */
    if (urgent) {
        (void)k_work_reschedule(&persist_work, K_MSEC(wait_ms));
    } else {
        (void)k_work_schedule(&persist_work, K_MSEC(wait_ms));
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
    /* The post-load hook places early events after the saved journal tail. */
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
    schedule_persist_coalesced(need_persist);
}

static void persist_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    totem_host_event_log_persist();
}

void totem_host_event_log_persist(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    k_mutex_lock(&persist_mu, K_FOREVER);
    int64_t now = k_uptime_get();
    k_mutex_lock(&ring_mu, K_FOREVER);
    if (!journal_ready) {
        k_mutex_unlock(&ring_mu);
        k_mutex_unlock(&persist_mu);
        return;
    }
    if (last_persist_uptime_ms != 0 && (now - last_persist_uptime_ms) < PERSIST_MIN_INTERVAL_MS) {
        /* Too soon (e.g. dump requested during thrash) — defer. */
        k_mutex_unlock(&ring_mu);
        k_mutex_unlock(&persist_mu);
        schedule_persist_coalesced(true);
        return;
    }

    uint32_t first_available = ring_count ? ring_seq - ring_count + 1 : ring_seq + 1;
    uint32_t first_new = persisted_event_seq + 1;
    if (first_new < first_available) {
        first_new = first_available;
    }
    if (first_new > ring_seq) {
        persist_requested = false;
        k_mutex_unlock(&ring_mu);
        k_mutex_unlock(&persist_mu);
        return;
    }
    uint32_t pending = ring_seq - first_new + 1;
    uint16_t count = (uint16_t)MIN(pending, (uint32_t)BLOCK_EVENTS);
    /* Write the oldest available pending records first; subsequent blocks
     * preserve their sequence instead of silently skipping to the tail. */
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
    uint32_t last_copied_seq = first_new + count - 1;
    persist_requested = false;
    /* Copy under lock; write after unlock so BLE callbacks are not blocked on flash. */
    k_mutex_unlock(&ring_mu);

    /* crc is at the end of the fixed-size struct. Always write the complete
     * block, including unused zeroed event slots and the actual CRC. */
    int err = settings_save_one(key, &persist_block, sizeof(persist_block));
    k_mutex_lock(&ring_mu, K_FOREVER);
    last_persist_uptime_ms = k_uptime_get();
    if (err) {
        LOG_WRN("totem_diag persist failed err=%d count=%u", err, count);
        persist_requested = true;
    } else {
        persisted_event_seq = last_copied_seq;
        restored_blocks[slot] = persist_block;
        restored_valid[slot] = true;
        next_slot = (uint8_t)((slot + 1) % JOURNAL_SLOTS);
        LOG_DBG("totem_diag persisted slot=%u count=%u seq=%u", slot, count, journal_seq);
    }
    bool more_pending = persist_requested || ring_seq > persisted_event_seq;
    k_mutex_unlock(&ring_mu);
    k_mutex_unlock(&persist_mu);
    if (more_pending) {
        schedule_persist_coalesced(true);
    }
#else
    LOG_WRN("totem_ble hevt persist skipped (SETTINGS off)");
#endif
}

static int dump_send(const char *line) {
#if IS_ENABLED(CONFIG_TOTEM_STUDIO_CONSOLE) || IS_ENABLED(CONFIG_ZMK_STUDIO_CONSOLE)
    /* Only the dedicated dump queue waits for TX space; live logging never waits. */
    for (int attempt = 0; attempt < 40; attempt++) {
        int err = totem_studio_send_diag_line(line);
        if (err != -EAGAIN) {
            return err;
        }
        k_sleep(K_MSEC(5));
    }
    return -EAGAIN;
#else
    printk("%s\n", line);
    return 0;
#endif
}

static int dump_data(uint32_t id, uint32_t *ordinal, const char *detail) {
    char line[120];
    int len = snprintk(line, sizeof(line), "totem_diag d=%u n=%u %s", id, *ordinal, detail);
    if (len < 0 || len >= sizeof(line)) {
        return -EMSGSIZE;
    }
    int err = dump_send(line);
    if (err == 0) {
        (*ordinal)++;
    }
    return err;
}

static int dump_event(uint32_t id, uint32_t *ordinal, const struct totem_host_event *e,
                      char source, uint8_t slot, uint32_t seq) {
    char detail[90];
    int len = snprintk(detail, sizeof(detail),
                       "e s=%c%u q=%u t=%u k=%u i=%d a=%d r=%u w=%u x=%u", source,
                       slot, seq, e->uptime_ms, e->type, (int)e->idx, (int)e->active,
                       e->reason, e->thrash_win, e->extra);
    if (len < 0 || len >= sizeof(detail)) {
        return -EMSGSIZE;
    }
    return dump_data(id, ordinal, detail);
}

void totem_host_event_log_dump(void) {
    uint16_t n = 0;
    uint32_t seq;
    uint8_t start_slot;
    uint32_t id = k_uptime_get_32();
    uint32_t expected = 0;
    uint32_t ordinal = 0;
    uint32_t persistent_events = 0;
    uint8_t blocks = 0;
    char line[120];
    char detail[90];
    int len;
    int err;

    k_mutex_lock(&dump_mu, K_FOREVER);

    k_mutex_lock(&ring_mu, K_FOREVER);
    ring_get_ordered(dump_tmp, &n);
    seq = ring_seq;
    memcpy(dump_blocks, restored_blocks, sizeof(dump_blocks));
    memcpy(dump_valid, restored_valid, sizeof(dump_valid));
    start_slot = next_slot;
    k_mutex_unlock(&ring_mu);

    for (uint8_t slot = 0; slot < JOURNAL_SLOTS; slot++) {
        if (dump_valid[slot]) {
            blocks++;
            persistent_events += dump_blocks[slot].count;
        }
    }
    expected = blocks + persistent_events + n;
    len = snprintk(line, sizeof(line),
                   "totem_diag begin id=%u lines=%u blocks=%u persisted=%u ram=%u seq=%u",
                   id, expected, blocks, persistent_events, n, seq);
    if (len < 0 || len >= sizeof(line) || dump_send(line) != 0) {
        goto out;
    }

    /* Each data line has a consecutive ordinal. A receiver must observe all
     * ordinals and the matching end marker before accepting the dump. */
    for (uint8_t off = 0; off < JOURNAL_SLOTS; off++) {
        uint8_t slot = (uint8_t)((start_slot + off) % JOURNAL_SLOTS);
        if (!dump_valid[slot]) {
            continue;
        }
        const struct totem_diag_block *block = &dump_blocks[slot];
        len = snprintk(detail, sizeof(detail), "b s=%u j=%u first=%u count=%u", slot,
                       block->journal_seq, block->first_event_seq, block->count);
        if (len < 0 || len >= sizeof(detail) || dump_data(id, &ordinal, detail) != 0) {
            goto out;
        }
        for (uint8_t i = 0; i < block->count; i++) {
            err = dump_event(id, &ordinal, &block->ev[i], 'p', slot,
                             block->first_event_seq + i);
            if (err != 0) {
                goto out;
            }
        }
    }
    for (uint16_t i = 0; i < n; i++) {
        err = dump_event(id, &ordinal, &dump_tmp[i], 'r', 0, seq - n + i + 1);
        if (err != 0) {
            goto out;
        }
    }
    if (ordinal == expected) {
        len = snprintk(line, sizeof(line), "totem_diag end id=%u lines=%u status=ok", id,
                       ordinal);
        if (len >= 0 && len < sizeof(line)) {
            (void)dump_send(line);
        }
    }

out:
    k_mutex_unlock(&dump_mu);
}

static void dump_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    totem_host_event_log_dump();
}

void totem_host_event_log_request_dump(void) { (void)k_work_submit_to_queue(&dump_queue, &dump_work); }

static int host_diag_dump_init(void) {
    k_work_queue_start(&dump_queue, dump_stack, K_THREAD_STACK_SIZEOF(dump_stack),
                       K_PRIO_PREEMPT(10), NULL);
    return 0;
}

SYS_INIT(host_diag_dump_init, APPLICATION, 1);

#if IS_ENABLED(CONFIG_SETTINGS)
static int hevt_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strncmp(name, "dlog/", 5) != 0 || name[5] < '0' || name[5] > '9' || name[6] != '\0') {
        return -ENOENT;
    }
    uint8_t slot = (uint8_t)(name[5] - '0');
    if (slot >= JOURNAL_SLOTS || len != sizeof(struct totem_diag_block)) {
        return 0;
    }

    struct totem_diag_block *block = &restored_blocks[slot];
    memset(block, 0, sizeof(*block));
    int rc = read_cb(cb_arg, block, len);
    if (rc < 0) {
        return rc;
    }
    uint32_t crc = crc32_ieee((const uint8_t *)block, offsetof(struct totem_diag_block, crc));
    if (block->magic != BLOCK_MAGIC || block->version != BLOCK_VERSION || block->slot != slot ||
        block->count == 0 || block->count > BLOCK_EVENTS || rc != sizeof(*block) ||
        block->crc != crc) {
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

void zmk_settings_loaded(void) {
    k_mutex_lock(&ring_mu, K_FOREVER);
    if (!journal_ready) {
        /* main() calls this after all settings callbacks and their lock release. */
        ring_seq = persisted_event_seq + ring_count;
        journal_ready = true;
    }
    bool pending = ring_seq > persisted_event_seq;
    k_mutex_unlock(&ring_mu);
    if (pending) {
        schedule_persist_coalesced(true);
    }
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
    totem_host_event_log_request_dump();
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
