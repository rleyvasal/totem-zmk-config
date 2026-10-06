"""Compile the actual journal startup path against native settings mocks."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define __packed __attribute__((packed))
#define IS_ENABLED(x) 1
#define RING_CAP 4
#define BLOCK_EVENTS 2
#define JOURNAL_SLOTS 4
#define PERSIST_EVERY 2
#define PERSIST_MIN_INTERVAL_MS 10000
#define SETTINGS_KEY_PREFIX "th/dlog"
#define K_FOREVER -1
#define K_MSEC(x) (x)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define LOG_WRN(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define snprintk snprintf
struct k_mutex { bool locked; };
#define K_MUTEX_DEFINE(name) struct k_mutex name
struct k_work_delayable { int unused; };
static struct k_work_delayable persist_work;
static int schedules, saves, wait_ms;
static int64_t now=100;
static bool loading, inject_event;
static int64_t k_uptime_get(void) { return now; }
static uint32_t k_uptime_get_32(void) { return (uint32_t)now; }
static void k_mutex_lock(struct k_mutex *mu, int timeout) {
    assert(timeout==K_FOREVER && !mu->locked); mu->locked=true;
}
static void k_mutex_unlock(struct k_mutex *mu) { assert(mu->locked); mu->locked=false; }
static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
    uint32_t crc=0; for(size_t i=0; i<len; i++) crc=crc*33+data[i]; return crc;
}
/* ACTUAL_STATE */
typedef int (*settings_read_cb)(void *, void *, size_t);
static void totem_host_event_log_record(uint8_t, int8_t, int8_t, uint8_t, uint8_t, uint8_t);
static struct totem_diag_block written;
static int scheduled(struct k_work_delayable *work, int delay) {
    assert(work==&persist_work && !ring_mu.locked && !loading);
    schedules++; wait_ms=delay; return 0;
}
static int k_work_schedule(struct k_work_delayable *w, int d) {
    return scheduled(w,d);
}
static int k_work_reschedule(struct k_work_delayable *w, int d) {
    return scheduled(w,d);
}
static int settings_save_one(const char *key, const void *data, size_t len) {
    assert(journal_ready && !loading && !ring_mu.locked && persist_mu.locked);
    assert(len==sizeof(written) && strncmp(key,"th/dlog/",8)==0);
    memcpy(&written,data,len); saves++;
    if(inject_event) { inject_event=false; totem_host_event_log_record(9,-1,-1,0,0,0); }
    return 0;
}
/* ACTUAL_FUNCTIONS */
static int read_block(void *arg, void *out, size_t len) { memcpy(out,arg,len); return (int)len; }
static void load_block(unsigned slot, unsigned journal, unsigned first) {
    struct totem_diag_block block={.magic=BLOCK_MAGIC,.version=BLOCK_VERSION,
        .slot=slot,.count=2,.journal_seq=journal,.first_event_seq=first};
    block.crc=crc32_ieee((const uint8_t *)&block,offsetof(struct totem_diag_block,crc));
    char key[10]; snprintf(key,sizeof(key),"dlog/%u",slot);
    assert(hevt_settings_set(key,sizeof(block),read_block,&block)==0);
}
int main(int argc, char **argv) {
    assert(argc==2);
    loading=true;
    totem_host_event_log_record(14,-1,-1,1,0,0);
    totem_host_event_log_record(15,-1,-1,2,0,0);
    totem_host_event_log_persist();
    assert(!journal_ready && schedules==0 && saves==0 && ring_seq==2);
    bool empty=strcmp(argv[1],"empty")==0;
    if(!empty) {
        load_block(2,20,1000);
        /* Events during loading must not jump to a partially restored tail. */
        totem_host_event_log_record(16,-1,-1,0,0,0);
        load_block(0,18,900);
        assert(ring_seq==3 && schedules==0 && next_slot==3);
    }
    bool wrap=strcmp(argv[1],"wrap")==0;
    if(wrap) {
        for(int i=0;i<4;i++) totem_host_event_log_record(20+i,-1,-1,0,0,0);
        assert(ring_count==4 && schedules==0);
    }
    /* All settings callbacks, including synchronous Bluetooth commands, finish
     * before the journal can occupy the system queue waiting for settings. */
    assert(!journal_ready && schedules==0 && saves==0);
    loading=false;
    zmk_settings_loaded();
    uint32_t tail=empty ? 0 : 1001;
    assert(journal_ready && ring_seq==tail+ring_count && schedules==1 && saves==0);
    unsigned seq=ring_seq;
    zmk_settings_loaded();
    assert(ring_seq==seq); /* No double rebasing. */
    inject_event=!wrap && !empty;
    totem_host_event_log_persist();
    assert(saves==1 && written.first_event_seq==tail+1 && written.count==2);
    assert(written.journal_seq==(empty ? 1 : 21) && written.slot==(empty ? 0 : 3));
    assert(written.ev[0].type==(wrap ? 20 : 14));
    assert(written.ev[1].type==(wrap ? 21 : 15));
    if(!empty) {
        assert(restored_valid[0] && restored_blocks[0].journal_seq==18);
        assert(restored_valid[2] && restored_blocks[2].journal_seq==20);
        totem_host_event_log_persist();
        assert(saves==1 && wait_ms==10000); /* Existing write rate limit. */
        now+=10000;
        totem_host_event_log_persist();
        assert(saves==2 && written.first_event_seq==tail+3 && written.count==2);
        assert(written.ev[0].type==(wrap ? 22 : 16));
        assert(written.ev[1].type==(wrap ? 23 : 9));
        assert(persisted_event_seq==ring_seq);
    }
    assert(!ring_mu.locked && !persist_mu.locked);
    return 0;
}
"""


class DiagnosticStartupTests(unittest.TestCase):
    def test_old_firmware_without_startup_hook_is_rejected(self):
        source = (Path(__file__).resolve().parents[1] / "src/host_event_log.c").read_text()
        start = source.index("#if IS_ENABLED(CONFIG_SETTINGS) && !defined(")
        guard = source[start:source.index("#endif", start) + len("#endif")]
        for hook in (False, True):
            with self.subTest(hook=hook):
                definitions = "#define CONFIG_SETTINGS 1\n#define IS_ENABLED(x) x\n"
                if hook:
                    definitions += "#define ZMK_SETTINGS_LOADED_HOOK_VERSION 1\n"
                result = subprocess.run(
                    [os.environ.get("CC", "cc"), "-E", "-x", "c", "-"],
                    input=definitions + guard, text=True, capture_output=True, timeout=10,
                )
                self.assertEqual(result.returncode == 0, hook, result.stderr)
                if not hook:
                    self.assertIn("post-settings-load hook", result.stderr)

    def test_restore_precedes_writes_and_preserves_early_events(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/host_event_log.c").read_text()
        state = source[source.index("struct totem_host_event {"):
                       source.index("static struct totem_host_event dump_tmp")]
        boundaries = [
            ("static void schedule_persist_coalesced(",
             "static void persist_work_handler(struct k_work *work) {"),
            ("void totem_host_event_log_persist(void) {", "static int dump_send("),
            ("static int hevt_settings_set(", "SETTINGS_STATIC_HANDLER_DEFINE(totem_hevt"),
        ]
        functions = []
        for start_marker, end_marker in boundaries:
            start = source.index(start_marker)
            functions.append(source[start:source.index(end_marker, start)])
        self.assertIn('hevt_settings_set, NULL, NULL);', source)
        harness = HARNESS.replace("/* ACTUAL_STATE */", state)
        harness = harness.replace("/* ACTUAL_FUNCTIONS */", "\n".join(functions))
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_startup")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            for scenario in ("saved", "empty", "wrap"):
                with self.subTest(scenario=scenario):
                    subprocess.run([executable, scenario], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
