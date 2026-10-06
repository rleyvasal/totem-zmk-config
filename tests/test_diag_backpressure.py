"""Verify retained dumps wait only briefly when nonblocking TX is full."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <assert.h>
#include <errno.h>
#include <string.h>
#define CONFIG_TOTEM_STUDIO_CONSOLE 1
#define CONFIG_ZMK_STUDIO_CONSOLE 0
#define IS_ENABLED(x) x
#define K_MSEC(x) (x)
static int calls, sleeps, ready_after, terminal_error;
static int totem_studio_send_diag_line(const char *line) {
    assert(strcmp(line,"record")==0); calls++;
    return calls<=ready_after ? -EAGAIN : terminal_error;
}
static void k_sleep(int ms) { assert(ms==5); sleeps++; }
/* FUNCTION */
int main(void) {
    ready_after=2;
    assert(dump_send("record")==0 && calls==3 && sleeps==2);
    calls=sleeps=0; ready_after=100;
    assert(dump_send("record")==-EAGAIN && calls==40 && sleeps==40);
    calls=sleeps=ready_after=0; terminal_error=-ENOTCONN;
    assert(dump_send("record")==-ENOTCONN && calls==1 && sleeps==0);
    return 0;
}
"""


class DiagnosticBackpressureTests(unittest.TestCase):
    def test_dump_retries_are_bounded_and_disconnect_returns_immediately(self):
        source = (Path(__file__).resolve().parents[1] / "src/host_event_log.c").read_text()
        start = source.index("static int dump_send(")
        function = source[start:source.index("static int dump_data(", start)]
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_dump_backpressure")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-x", "c", "-", "-o", executable],
                input=HARNESS.replace("/* FUNCTION */", function),
                text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
