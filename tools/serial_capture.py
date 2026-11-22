#!/usr/bin/env python3
"""Capture serial log, optionally send a CLI line first. UTF-8-aware filtering."""
import argparse
import sys
import time

import serial


def printable_ratio(s: str) -> float:
    if not s:
        return 0.0
    return sum(ch.isprintable() or ch in "\t" for ch in s) / len(s)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem5C941020181")
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--send", default=None, help="line to send after 2s")
    ap.add_argument("--filter", default=None, help="only emit lines containing this substring")
    args = ap.parse_args()

    with serial.Serial(args.port, 115200, timeout=0.2) as ser:
        deadline = time.monotonic() + args.seconds
        sent = False
        buf = b""
        while time.monotonic() < deadline:
            if args.send and not sent and time.monotonic() > deadline - args.seconds + 2:
                ser.write(b"\n")
                time.sleep(0.1)
                ser.write(args.send.encode() + b"\n")
                ser.flush()
                print(f">>> sent: {args.send}", flush=True)
                sent = True
            data = ser.read(4096)
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip()
                if printable_ratio(line) < 0.7:
                    continue
                if args.filter and args.filter not in line:
                    continue
                print(line, flush=True)


if __name__ == "__main__":
    main()