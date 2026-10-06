#!/usr/bin/env python3
"""Request and verify a complete Totem diagnostic dump over Studio USB CDC.

Requires pyserial. Usage: python tools/read_diag_dump.py /dev/cu.usbmodem101
Only a matching begin/end pair with every numbered data line is accepted.
"""

import argparse
import re
import sys
import time

SOF, ESC, EOF = 0xAB, 0xAC, 0xAD
BEGIN = re.compile(r"^totem_diag begin id=(\d+) lines=(\d+) blocks=(\d+) persisted=(\d+) ram=(\d+) seq=(\d+)$")
DATA = re.compile(r"^totem_diag d=(\d+) n=(\d+) (.+)$")
END = re.compile(r"^totem_diag end id=(\d+) lines=(\d+) status=ok$")


def frame(payload):
    result = bytearray([SOF])
    for byte in payload:
        if byte in (SOF, ESC, EOF):
            result.append(ESC)
        result.append(byte)
    result.append(EOF)
    return bytes(result)


class Deframer:
    def __init__(self):
        self.active = False
        self.escaped = False
        self.payload = bytearray()

    def feed(self, chunk):
        for byte in chunk:
            if not self.active:
                if byte == SOF:
                    self.active = True
                    self.payload.clear()
                continue
            if self.escaped:
                self.payload.append(byte)
                self.escaped = False
            elif byte == ESC:
                self.escaped = True
            elif byte == SOF:
                self.payload.clear()
            elif byte == EOF:
                yield bytes(self.payload)
                self.active = False
                self.payload.clear()
            else:
                self.payload.append(byte)
            if len(self.payload) > 512:
                self.active = False
                self.escaped = False
                self.payload.clear()


class DumpVerifier:
    def __init__(self):
        self.dump_id = None
        self.expected = None
        self.next_line = 0
        self.error = None
        self.complete = False

    def feed(self, line):
        begin = BEGIN.match(line)
        if begin:
            self.dump_id = int(begin[1])
            self.expected = int(begin[2])
            self.next_line = 0
            self.error = None
            self.complete = False
            if self.expected != int(begin[3]) + int(begin[4]) + int(begin[5]):
                self.error = "begin counts disagree"
            return True
        if self.dump_id is None:
            return False
        data = DATA.match(line)
        if data and int(data[1]) == self.dump_id:
            ordinal = int(data[2])
            if ordinal != self.next_line and self.error is None:
                self.error = f"expected line {self.next_line}, got {ordinal}"
            self.next_line = ordinal + 1
            return True
        end = END.match(line)
        if end and int(end[1]) == self.dump_id:
            if int(end[2]) != self.expected or self.next_line != self.expected:
                self.error = self.error or f"expected {self.expected} lines, got {self.next_line}"
            self.complete = self.error is None
            return True
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="Studio USB CDC port, e.g. /dev/cu.usbmodem101")
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    try:
        import serial
    except ImportError as exc:
        parser.error(f"pyserial is required: {exc}")

    verifier = DumpVerifier()
    decoder = Deframer()
    with serial.Serial(args.port, 115200, timeout=0.2, write_timeout=2) as port:
        port.dtr = True
        port.write(frame(b"D1"))
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            for payload in decoder.feed(port.read(256)):
                if not payload.startswith(b"L"):
                    continue
                line = payload[1:].decode("utf-8", "replace")
                recognized = verifier.feed(line)
                if recognized:
                    print(line)
                if verifier.complete:
                    print(f"Complete diagnostic dump: {verifier.expected} lines", file=sys.stderr)
                    return 0
                if recognized and END.match(line):
                    print(f"Incomplete diagnostic dump: {verifier.error}", file=sys.stderr)
                    return 1

    reason = verifier.error or "missing matching end marker"
    print(f"Incomplete diagnostic dump: {reason}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
