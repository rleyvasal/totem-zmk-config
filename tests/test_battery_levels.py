"""Compile the actual battery line formatter: each half on USB prints NN usb."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define IS_ENABLED(x) x
#define CONFIG_ZMK_USB 1
#define snprintk snprintf
static bool usb_powered;
static int cell = -1;
static uint8_t live_soc;
static bool zmk_usb_is_powered(void) { return usb_powered; }
static int zmk_battery_last_cell_soc(void) { return cell; }
static uint8_t zmk_battery_state_of_charge(void) { return live_soc; }
static int peripheral_soc = -1;
static bool peripheral_charging;
/* ACTUAL_FORMAT */
static void expect(const char *want) {
    char levels[48];
    format_levels(levels, sizeof(levels));
    if (strcmp(levels, want) != 0) {
        fprintf(stderr, "got \"%s\", want \"%s\"\n", levels, want);
        assert(0);
    }
}
int main(void) {
    live_soc = 64;
    expect("left=64 right=--");
    peripheral_soc = 15;
    expect("left=64 right=15");
    peripheral_charging = true;
    expect("left=64 right=15 usb");
    usb_powered = true;
    expect("left=usb right=15 usb");
    cell = 78;
    expect("left=78 usb right=15 usb");
    peripheral_charging = false;
    expect("left=78 usb right=15");
    /* Charging but never reported a level: still unknown. */
    peripheral_soc = -1; peripheral_charging = true;
    expect("left=78 usb right=--");
    /* Widest values still fit the 48-byte buffers used by the callers. */
    live_soc = 100; usb_powered = false; peripheral_soc = 100;
    expect("left=100 right=100 usb");
    return 0;
}
"""


class BatteryLevelsTests(unittest.TestCase):
    def test_right_half_on_usb_prints_usb(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/totem_battery_log.c").read_text()
        start = source.index("static bool left_on_usb(void) {")
        end = source.index("\nstatic void totem_batt_push_studio(", start)
        harness = HARNESS.replace("/* ACTUAL_FORMAT */", source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_battery_levels")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
