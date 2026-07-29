/*
 * Crash capture + post-mortem reporting. See include/totem_fault.h for why.
 *
 * Layer 1 of the freeze investigation: distinguish a CRASH from a HANG, and for a
 * crash produce a program counter that maps to a source line.
 *
 * The distinction matters because the two need opposite fixes, and until now we had
 * no way to tell them apart. Silence here on a boot that follows a watchdog reset is
 * itself the answer: the fatal handler was never reached, so nothing faulted -- the
 * core was hung, not crashed.
 */

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/crc.h>
#include <string.h>

#include <totem_fault.h>
#include <totem_host_event_log.h>

#define TOTEM_FAULT_MAGIC 0x544F4654u /* "TOFT" */

/* __noinit: not zeroed at startup, so it carries across a soft reset. */
static __noinit struct totem_fault_record fault_rec;

/* Everything from `kind` onward; magic and crc are the envelope. */
#define FAULT_CRC_OFFSET offsetof(struct totem_fault_record, kind)
#define FAULT_CRC_LEN (sizeof(struct totem_fault_record) - FAULT_CRC_OFFSET)

static uint32_t fault_crc(const struct totem_fault_record *r) {
    return crc32_ieee(((const uint8_t *)r) + FAULT_CRC_OFFSET, FAULT_CRC_LEN);
}

static bool fault_valid(const struct totem_fault_record *r) {
    return r->magic == TOTEM_FAULT_MAGIC && r->crc == fault_crc(r);
}

static void fault_seal(struct totem_fault_record *r) {
    r->magic = TOTEM_FAULT_MAGIC;
    r->crc = fault_crc(r);
}

/* Best-effort thread identification. CONFIG_THREAD_NAME is selected by
 * TOTEM_FAULT_CAPTURE, but k_thread_name_get() can still return NULL for threads
 * that were never named -- the raw pointer is then the only handle we have, and it
 * is still resolvable against the .elf's static thread objects. */
static void fault_fill_thread(struct totem_fault_record *r) {
    k_tid_t tid = k_current_get();

    r->thread = (uint32_t)(uintptr_t)tid;
    r->label[0] = '\0';

#if IS_ENABLED(CONFIG_THREAD_NAME)
    const char *name = k_thread_name_get(tid);

    if (name != NULL && name[0] != '\0') {
        strncpy(r->label, name, sizeof(r->label) - 1);
        r->label[sizeof(r->label) - 1] = '\0';
    }
#endif
}

void totem_fault_note_task_wdt(int channel_id, const char *label) {
    memset(&fault_rec, 0, sizeof(fault_rec));
    fault_rec.kind = TOTEM_FAULT_TASK_WDT;
    fault_rec.channel = (int8_t)channel_id;
    fault_rec.reason = 0;
    fault_rec.uptime_ms = k_uptime_get_32();
    fault_fill_thread(&fault_rec);

    /* The channel label is more informative than whatever thread happened to be
     * interrupted by the timer ISR, so it wins the label field. */
    if (label != NULL) {
        strncpy(fault_rec.label, label, sizeof(fault_rec.label) - 1);
        fault_rec.label[sizeof(fault_rec.label) - 1] = '\0';
    }

    fault_seal(&fault_rec);
}

#if IS_ENABLED(CONFIG_TOTEM_FAULT_CAPTURE)
/* Overrides Zephyr's weak default, which would otherwise halt the CPU (or reboot,
 * depending on CONFIG_RESET_ON_FATAL_ERROR) without leaving anything behind. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    memset(&fault_rec, 0, sizeof(fault_rec));
    fault_rec.kind = TOTEM_FAULT_CRASH;
    fault_rec.channel = -1;
    fault_rec.reason = (uint16_t)reason;
    fault_rec.uptime_ms = k_uptime_get_32();
    fault_fill_thread(&fault_rec);

#if defined(CONFIG_CPU_CORTEX_M)
    /* esf is NULL for non-exception fatals (kernel panic, k_oops). */
    if (esf != NULL) {
        fault_rec.pc = esf->basic.pc;
        fault_rec.lr = esf->basic.lr;
    }
#else
    ARG_UNUSED(esf);
#endif

    fault_seal(&fault_rec);

    /* Print immediately as well: if a console happens to be attached this is the
     * fastest path to the answer, and it costs nothing if nobody is listening. */
    printk("\n*** totem_fault CRASH reason=%u pc=0x%08x lr=0x%08x thread=%s(0x%08x) "
           "uptime=%u ms ***\n",
           reason, fault_rec.pc, fault_rec.lr, fault_rec.label, fault_rec.thread,
           fault_rec.uptime_ms);
    printk("*** rebooting; run addr2line against the matching zmk.elf for pc ***\n");

    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}
#endif /* CONFIG_TOTEM_FAULT_CAPTURE */

static const char *fault_reason_str(uint16_t reason) {
    switch (reason) {
    case K_ERR_CPU_EXCEPTION:
        return "CPU_EXCEPTION";
    case K_ERR_SPURIOUS_IRQ:
        return "SPURIOUS_IRQ";
    case K_ERR_STACK_CHK_FAIL:
        return "STACK_OVERFLOW";
    case K_ERR_KERNEL_OOPS:
        return "KERNEL_OOPS";
    case K_ERR_KERNEL_PANIC:
        return "KERNEL_PANIC";
    default:
        return "UNKNOWN";
    }
}

void totem_fault_report_and_clear(void) {
    if (!fault_valid(&fault_rec)) {
        /* Cold boot, or nothing has faulted since the last report. Not an error --
         * say so explicitly, because "no line printed" is otherwise ambiguous with
         * "this feature is not compiled in". */
        printk("totem_fault none\n");
        return;
    }

    switch (fault_rec.kind) {
    case TOTEM_FAULT_CRASH:
        printk("totem_fault CRASH reason=%u(%s) pc=0x%08x lr=0x%08x thread=%s(0x%08x) "
               "at_uptime=%u ms\n",
               fault_rec.reason, fault_reason_str(fault_rec.reason), fault_rec.pc, fault_rec.lr,
               fault_rec.label, fault_rec.thread, fault_rec.uptime_ms);
        break;
    case TOTEM_FAULT_TASK_WDT:
        printk("totem_fault HANG task_wdt channel=%d (%s) stopped feeding; "
               "at_uptime=%u ms\n",
               fault_rec.channel, fault_rec.label, fault_rec.uptime_ms);
        break;
    default:
        printk("totem_fault unknown kind=%u\n", fault_rec.kind);
        break;
    }

    totem_host_event_log_record(TOTEM_HEVT_FAULT, (int8_t)fault_rec.channel, -1,
                                (uint8_t)fault_rec.reason, 0, fault_rec.kind);
    totem_host_event_log_persist();

    /* Report exactly once: leaving it valid would make every later boot look like it
     * had just crashed. */
    fault_rec.magic = 0;
    fault_rec.crc = 0;
}
