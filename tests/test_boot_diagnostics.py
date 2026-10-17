"""Compile boot-only observers against native hardware/settings mocks."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#define snprintk snprintf
#define IS_ENABLED(x) 1
#define __noinit
#define SYS_INIT(...)
#define SETTINGS_STATIC_HANDLER_DEFINE(...)
typedef int (*settings_read_cb)(void *, void *, size_t);
static int saves, records, fail_save;
static uint8_t kinds[32], reasons[32];
static uint32_t words[32];
static uint32_t nrfx_reset_reason_get(void) { return 0x12345678; }
static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
    uint32_t crc=0; for (size_t i=0;i<len;i++) crc=crc*33+data[i]; return crc;
}
static void totem_host_event_log_record(uint8_t k,int8_t i,int8_t a,uint8_t r,uint8_t w,uint8_t x) {
    kinds[records]=k; reasons[records]=r;
    words[records++]=(uint8_t)i|((uint32_t)(uint8_t)a<<8)|((uint32_t)w<<16)|((uint32_t)x<<24);
}
static int settings_save_one(const char *key,const void *data,size_t len) {
    assert(strcmp(key,"tboot/count")==0 && data && len==8); saves++;
    return fail_save ? -EIO : 0;
}
/* ENUM */
/* SOURCE */
static int read_value(void *arg,void *out,size_t len) { memcpy(out,arg,len); return (int)len; }
static int failed_read(void *arg,void *out,size_t len) {
    (void)arg; (void)out; (void)len; return -EIO;
}
int main(void) {
    assert(boot_capture_early()==0 && raw_reset==0x12345678 && !ram_survived);
    assert(records==0 && saves==0);
    assert(boot_capture_early()==0 && ram_survived && saves==0);
    boot_ram.inverse=0; assert(boot_capture_early()==0 && !ram_survived);
    boot_counter.count=7; boot_counter.crc=counter_crc();
    uint32_t saved[2]={boot_counter.count,boot_counter.crc};
    assert(boot_settings_set("count",8,read_value,saved)==0 && counter_status==1);
    totem_boot_diagnostics_loaded(123456,-1,-1);
    assert(boot_counter.count==8 && records==5 && saves==0);
    assert(kinds[0]==40 && words[0]==0x12345678 && reasons[0]==1);
    assert(kinds[1]==43 && words[1]==8 && reasons[1]==1);
    assert(kinds[2]==44 && words[2]==123456);
    totem_boot_diagnostics_reset_result(-EIO);
    totem_boot_diagnostics_fault_status(2);
    char line[120];
    totem_boot_diagnostics_format(line,sizeof(line),false);
    assert(strstr(line,"raw=12345678") && strstr(line,"read=-5") && strstr(line,"fault=2"));
    totem_boot_diagnostics_format(line,sizeof(line),true);
    assert(strstr(line,"uptime=123456 profile=-1 endpoint=-1"));
    totem_boot_diagnostics_loaded(0,0,0); assert(records==5 && boot_counter.count==8);
    assert(boot_settings_set("count",8,read_value,saved)==0 && boot_counter.count==8);
    fail_save=1; assert(totem_boot_diagnostics_persist()==-EIO && counter_pending);
    fail_save=0; assert(totem_boot_diagnostics_persist()==0 && !counter_pending);
    assert(totem_boot_diagnostics_persist()==0 && saves==2);
    boot_loaded=false; saved[1]^=1;
    assert(boot_settings_set("count",8,read_value,saved)==0 && counter_status==2 && boot_counter.count==0);
    assert(boot_settings_set("count",7,read_value,saved)==0 && counter_status==2);
    assert(boot_settings_set("count",8,failed_read,saved)==-EIO && boot_counter.count==0);
    return 0;
}
'''

class BootDiagnosticsTests(unittest.TestCase):
    def test_fault_record_validation_is_reported_before_clear(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/totem_fault.c").read_text()
        helpers = source[source.index("#define FAULT_CRC_OFFSET"):
                         source.index("static void fault_seal")]
        start = source.index("void totem_fault_report_and_clear(void) {")
        observer = source[start:source.index("    if (!fault_valid(&fault_rec))", start)] + "}\n"
        harness = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include "totem_fault.h"
#define IS_ENABLED(x) 1
#define TOTEM_FAULT_MAGIC 0x544F4654u
#define TOTEM_HEVT_BOOT_FAULT_STATUS 42
static struct totem_fault_record fault_rec;
static uint8_t observed, reported_kind;
static uint32_t crc32_ieee(const uint8_t *p,size_t n) {
    uint32_t c=0; for(size_t i=0;i<n;i++) c=c*33+p[i]; return c;
}
static void totem_boot_diagnostics_fault_status(uint8_t status) { observed=status; }
static void totem_host_event_log_record(uint8_t k,int8_t i,int8_t a,uint8_t r,uint8_t w,uint8_t x) {
    assert(k==42 && i==-1 && a==-1 && r==observed && x==0); reported_kind=w;
}
/* HELPERS */
/* OBSERVER */
int main(void) {
    totem_fault_report_and_clear(); assert(observed==0 && reported_kind==0);
    fault_rec.magic=1; totem_fault_report_and_clear(); assert(observed==1);
    fault_rec.magic=TOTEM_FAULT_MAGIC; fault_rec.kind=1; fault_rec.crc=123;
    totem_fault_report_and_clear(); assert(observed==2 && reported_kind==0);
    fault_rec.crc=fault_crc(&fault_rec);
    totem_fault_report_and_clear(); assert(observed==3 && reported_kind==1);
    assert(fault_rec.magic==TOTEM_FAULT_MAGIC);
    return 0;
}
'''
        harness = harness.replace("/* HELPERS */", helpers).replace("/* OBSERVER */", observer)
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "fault_status_test")
            subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                            "-I", str(root / "include"), "-x", "c", "-", "-o", executable],
                           input=harness, text=True, check=True, timeout=30)
            subprocess.run([executable], check=True, timeout=3)

    def test_module_crash_is_imported_but_never_overwrites_a_hang(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/totem_fault.c").read_text()
        helpers = source[source.index("#define FAULT_CRC_OFFSET"):
                         source.index("static void fault_fill_thread")]
        start = source.index("static void fault_import_module_crash(void) {")
        importer = source[start:source.index("\n}\n", start) + 3]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "totem_fault.h"
#define TOTEM_FAULT_MAGIC 0x544F4654u
struct zmk_studio_log_fault {
    uint32_t reason, pc, lr, uptime_ms, thread;
    char thread_name[16];
};
static struct totem_fault_record fault_rec;
static bool module_has_crash;
static uint32_t crc32_ieee(const uint8_t *p, size_t n) {
    uint32_t c = 0; for (size_t i = 0; i < n; i++) c = c * 33 + p[i]; return c;
}
static bool zmk_studio_log_boot_fault(struct zmk_studio_log_fault *out) {
    if (module_has_crash) {
        *out = (struct zmk_studio_log_fault){.reason = 3, .pc = 0x1234, .lr = 0x5678,
                                             .uptime_ms = 9000, .thread = 0xabc,
                                             .thread_name = "sysworkq"};
    }
    return module_has_crash;
}
/* HELPERS */
/* IMPORTER */
int main(void) {
    /* No crash anywhere: nothing is invented. */
    fault_import_module_crash();
    assert(!fault_valid(&fault_rec));

    /* A module crash becomes a sealed Totem CRASH record with the same facts. */
    module_has_crash = true;
    fault_import_module_crash();
    assert(fault_valid(&fault_rec) && fault_rec.kind == TOTEM_FAULT_CRASH);
    assert(fault_rec.reason == 3 && fault_rec.pc == 0x1234 && fault_rec.lr == 0x5678);
    assert(fault_rec.uptime_ms == 9000 && fault_rec.channel == -1);
    assert(strcmp(fault_rec.label, "sysworkq") == 0);

    /* A task watchdog HANG noted by Totem wins: it explains the reset, the crash cannot. */
    memset(&fault_rec, 0, sizeof(fault_rec));
    fault_rec.kind = TOTEM_FAULT_TASK_WDT; fault_rec.channel = 1;
    fault_seal(&fault_rec);
    fault_import_module_crash();
    assert(fault_rec.kind == TOTEM_FAULT_TASK_WDT && fault_rec.channel == 1);
    return 0;
}
'''
        harness = harness.replace("/* HELPERS */", helpers)
        harness = harness.replace("/* IMPORTER */", importer)
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "fault_import_test")
            subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", "-I", str(root / "include"),
                            "-x", "c", "-", "-o", executable],
                           input=harness, text=True, check=True, timeout=30)
            subprocess.run([executable], check=True, timeout=3)

    def test_early_capture_validation_counter_and_deferred_write(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/boot_diagnostics.c").read_text()
        source = source[source.index("#define BOOT_RAM_MAGIC"):]
        header = (root / "include/totem_host_event_log.h").read_text()
        enum = header[header.index("enum totem_host_evt {"):header.index("};")+2]
        harness = HARNESS.replace("/* ENUM */", enum).replace("/* SOURCE */", source)
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "boot_test")
            subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                            "-x", "c", "-", "-o", executable], input=harness,
                           text=True, check=True, timeout=30)
            subprocess.run([executable], check=True, timeout=3)

if __name__ == "__main__":
    unittest.main()
