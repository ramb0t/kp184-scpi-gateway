#!/usr/bin/env python3
"""Listen on the KP184's RS485 bus and print each frame. Never transmits.

Run it on a second RS485 adapter wired in parallel with the gateway, to see
whether the gateway's requests reach the bus and whether the load answers.

Usage: kp184_sniff.py [PORT] [--baud 57600]      (PORT defaults to /dev/ttyUSB0)
"""
import argparse
import os
import select
import sys
import termios
import time

REGS = {0x0001: "model", 0x010E: "input", 0x0110: "mode", 0x0112: "CV mV", 0x0116: "CC mA",
        0x011A: "CR 0.1ohm", 0x011E: "CW 0.01W", 0x0122: "U mV", 0x0126: "I mA",
        0x0300: "status", 0x0301: "status long"}


def crc16(data: bytes) -> bytes:
    r = 0xFFFF
    for x in data:
        r ^= x
        for _ in range(8):
            r = (r >> 1) ^ 0xA001 if r & 1 else r >> 1
    return r.to_bytes(2, "little")


def describe(f: bytes) -> str:
    if len(f) < 5 or crc16(f[:-2]) != f[-2:]:
        return "?? bad CRC or fragment"
    func = f[1]
    if func == 3 and len(f) == 8:
        reg = int.from_bytes(f[2:4], "big")
        return f"request  read  {REGS.get(reg, hex(reg))}"
    if func == 6 and len(f) == 13:
        reg = int.from_bytes(f[2:4], "big")
        return f"request  write {REGS.get(reg, hex(reg))} = {int.from_bytes(f[7:11], 'big')}"
    if func == 3 and len(f) == 9:
        return f"reply    value {int.from_bytes(f[3:7], 'big')} (low 16 bits {int.from_bytes(f[5:7], 'big')})"
    if func == 3 and len(f) in (23, 37):
        d = f[3:]
        return (f"reply    status input={'on' if d[0] & 1 else 'off'} "
                f"{int.from_bytes(d[2:5], 'big')} mV {int.from_bytes(d[5:8], 'big')} mA")
    if func == 6 and len(f) == 8:
        return f"reply    write ack {REGS.get(int.from_bytes(f[2:4], 'big'), '?')} = {int.from_bytes(f[4:6], 'big')}"
    return "?? valid CRC, unknown layout"


def split(buf: bytes) -> list:
    """A USB adapter often delivers a request and its reply as one chunk: cut at the request."""
    for n in (8, 13):
        if len(buf) > n and buf[1] in (3, 6) and crc16(buf[:n - 2]) == buf[n - 2:n]:
            return [buf[:n], buf[n:]]
    return [buf]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", nargs="?", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=57600)
    args = ap.parse_args()

    fd = os.open(args.port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[4] = a[5] = getattr(termios, f"B{args.baud}")
    a[6][termios.VMIN] = a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIFLUSH)

    gap = max(0.003, 40 / args.baud)        # frames are separated by silence; USB adds a few ms
    print(f"listening on {args.port} at {args.baud} 8N1 (receive only)", flush=True)
    t0 = time.monotonic()
    buf, last, prev_end = b"", 0.0, None
    try:
        while True:
            r, _, _ = select.select([fd], [], [], 0.002)
            now = time.monotonic()
            if r:
                d = os.read(fd, 256)
                if d:
                    if not buf:
                        start = now
                    buf += d
                    last = now
                    continue
            if buf and now - last >= gap:
                since = f"+{(start - prev_end) * 1000:6.1f} ms" if prev_end is not None else " " * 10
                for i, frame in enumerate(split(buf)):
                    stamp = f"{start - t0:9.3f} s {since}" if i == 0 else " " * 22
                    print(f"{stamp}  {frame.hex(' '):<40}  {describe(frame)}", flush=True)
                prev_end, buf = last, b""
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
