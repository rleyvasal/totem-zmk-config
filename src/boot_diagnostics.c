/* Boot-only observers: no transport changes and no flash writes at early init. */
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <totem_boot_diagnostics.h>
#include <totem_host_event_log.h>
#if IS_ENABLED(CONFIG_SOC_NRF52840)
#include <helpers/nrfx_reset_reason.h>
#endif

#define BOOT_RAM_MAGIC 0x5442524du
static __noinit struct {
    uint32_t magic;
    uint32_t inverse;
} boot_ram;

static struct {
    uint32_t count;
    uint32_t crc;
} boot_counter;
static uint32_t raw_reset;
static uint8_t raw_available;
static uint8_t ram_survived;
static uint8_t counter_status;
static bool counter_pending;
static bool boot_loaded;
static int reset_read_result;
static uint8_t fault_status;
static uint32_t saved_previous_uptime;
static int8_t saved_previous_profile = -1;
static int8_t saved_previous_endpoint = -1;

void totem_boot_diagnostics_reset_result(int result) { reset_read_result = result; }
void totem_boot_diagnostics_fault_status(uint8_t status) { fault_status = status; }

void totem_boot_diagnostics_format(char *line, size_t len, bool previous) {
    if (previous) {
        snprintk(line, len, "totem_boot previous uptime=%u profile=%d endpoint=%d",
                 saved_previous_uptime, saved_previous_profile, saved_previous_endpoint);
    } else {
        snprintk(line, len,
                 "totem_boot current count=%u raw=%08x available=%u read=%d ram=%u fault=%u counter=%u",
                 boot_counter.count, raw_reset, raw_available, reset_read_result,
                 ram_survived, fault_status, counter_status);
    }
}

static int boot_capture_early(void) {
#if IS_ENABLED(CONFIG_SOC_NRF52840)
    raw_reset = nrfx_reset_reason_get();
    raw_available = 1;
#endif
    ram_survived = boot_ram.magic == BOOT_RAM_MAGIC &&
                   boot_ram.inverse == ~BOOT_RAM_MAGIC;
    boot_ram.magic = BOOT_RAM_MAGIC;
    boot_ram.inverse = ~BOOT_RAM_MAGIC;
    return 0;
}

SYS_INIT(boot_capture_early, PRE_KERNEL_1, 0);

static uint32_t counter_crc(void) {
    return crc32_ieee((const uint8_t *)&boot_counter.count, sizeof(boot_counter.count));
}

static void boot_record_word(uint8_t kind, uint32_t value, uint8_t status) {
    totem_host_event_log_record(kind, (int8_t)value, (int8_t)(value >> 8), status,
                                (uint8_t)(value >> 16), (uint8_t)(value >> 24));
}

void totem_boot_diagnostics_loaded(uint32_t previous_uptime, int8_t previous_profile,
                                   int8_t previous_endpoint) {
    if (boot_loaded) {
        return;
    }
    boot_loaded = true;
    saved_previous_uptime = previous_uptime;
    saved_previous_profile = previous_profile;
    saved_previous_endpoint = previous_endpoint;
    boot_counter.count++;
    boot_counter.crc = counter_crc();
    counter_pending = true;
    boot_record_word(TOTEM_HEVT_BOOT_RAW, raw_reset, raw_available);
    boot_record_word(TOTEM_HEVT_BOOT_COUNTER, boot_counter.count, counter_status);
    boot_record_word(TOTEM_HEVT_BOOT_PREVIOUS_UPTIME, previous_uptime, 0);
    totem_host_event_log_record(TOTEM_HEVT_BOOT_PREVIOUS_STATE, previous_profile,
                                previous_endpoint, 0, 0, 0);
    totem_host_event_log_record(TOTEM_HEVT_BOOT_RAM_STATUS, -1, -1, ram_survived, 0, 0);
}

int totem_boot_diagnostics_persist(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    if (counter_pending) {
        int err = settings_save_one("tboot/count", &boot_counter, sizeof(boot_counter));
        if (err) {
            return err;
        }
        counter_pending = false;
    }
#endif
    return 0;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int boot_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
    if (strcmp(name, "count") != 0) {
        return -ENOENT;
    }
    if (boot_loaded) {
        return 0;
    }
    if (len != sizeof(boot_counter)) {
        counter_status = 2;
        return 0;
    }
    int rc = read_cb(cb_arg, &boot_counter, len);
    if (rc != sizeof(boot_counter) || boot_counter.crc != counter_crc()) {
        memset(&boot_counter, 0, sizeof(boot_counter));
        counter_status = 2;
    } else {
        counter_status = 1;
    }
    return rc < 0 ? rc : 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(totem_boot, "tboot", NULL, boot_settings_set, NULL, NULL);
#endif
