#!/usr/bin/env python3
"""Send `wechat login` over the console UART and capture the QR URL.

Usage: python wechat_qr_login.py [port] [--cmd "wechat status"] [--seconds 90]
Prints all serial lines; writes each QR_URL payload to qr_url.txt (latest wins).
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="/dev/cu.usbmodem5C941020181")
    ap.add_argument("--cmd", default="wechat login")
    ap.add_argument("--seconds", type=int, default=90)
    args = ap.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=0.2)
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write((args.cmd + "\n").encode())
    ser.flush()
    print(f">>> {args.cmd}", flush=True)

    deadline = time.time() + args.seconds
    buf = b""
    got_qr = False
    while time.time() < deadline:
        data = ser.read(4096)
        if not data:
            if got_qr and args.cmd.startswith("wechat login"):
                break
            continue
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip()
            print(text, flush=True)
            idx = text.find("QR_URL: ")
            if idx >= 0:
                url = text[idx + len("QR_URL: "):].strip()
                with open("qr_url.txt", "w") as f:
                    f.write(url)
                got_qr = True
    ser.close()
    if got_qr:
        print("QR URL saved to qr_url.txt", flush=True)


if __name__ == "__main__":
    main()