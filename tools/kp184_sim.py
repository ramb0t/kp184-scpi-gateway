#!/usr/bin/env python3
"""KP184 simulator: answers the load's serial protocol on a serial port.

Stands in for the electronic load when testing the gateway without one, e.g. on
the WCH-Link's serial port wired to the board's TXD/RXD header.

Frame formats and quirks follow replies captured from a real KP184 (firmware
2128, 57600 8N1, address 1) on 2026-11-25; `--selftest` replays that capture.
Write acknowledgements were not captured; the 8-byte form used here is what the
validated korjaa/kunkin-kp184 library expects.

Usage: kp184_sim.py PORT [--baud 57600] [--echo] [--drop N] [--corrupt N] [--locked-mode ack] [--verbose]
       kp184_sim.py --selftest
"""
import argparse
import os
import select
import sys
import termios
import time

MODES = ["CV", "CC", "CR", "CW"]
REG_MODEL, REG_OUTPUT, REG_MODE = 0x0001, 0x010E, 0x0110
REG_CV, REG_CC, REG_CR, REG_CW = 0x0112, 0x0116, 0x011A, 0x011E
REG_U, REG_I = 0x0122, 0x0126
REG_STATUS_SHORT, REG_STATUS_LONG = 0x0300, 0x0301


def crc16(data: bytes) -> bytes:
    r = 0xFFFF
    for x in data:
        r ^= x
        for _ in range(8):
            r = (r >> 1) ^ 0xA001 if r & 1 else r >> 1
    return r.to_bytes(2, "little")


class Load:
    """Register model of the load. Values are raw counts as sent on the wire."""

    def __init__(self, address=1, source_mv=24031, locked_mode="silent"):
        self.address = address
        self.locked_mode = locked_mode  # reply to a mode change while the input is on: "silent" or "ack"
        self.source_mv = source_mv      # open-circuit voltage of the supply under test
        self.output = 0
        self.mode = 1                   # CC
        self.cv_mv = 3500
        self.cc_ma = 1000
        self.cr_dohm = 100              # 0.1 ohm per count
        self.cw_cw = 300                # 0.01 W per count
        self.prev = b"\x00\x00"         # first two data bytes of the previous reply

    # --- electrical behaviour -------------------------------------------
    def current_ma(self) -> int:
        if not self.output or self.source_mv <= 0:
            return 0
        if self.mode == 1:
            return self.cc_ma
        if self.mode == 2:
            return round(self.source_mv * 10 / self.cr_dohm) if self.cr_dohm else 0
        if self.mode == 3:
            return round(self.cw_cw * 10 * 1000 / self.source_mv)
        return 0                        # CV: an ideal source above the setpoint is not modelled

    def setpoint(self) -> int:
        return (self.cv_mv, self.cc_ma, self.cr_dohm, self.cw_cw)[self.mode]

    # --- protocol -------------------------------------------------------
    def _reply(self, body: bytes, data: bytes) -> bytes:
        self.prev = data[:2]
        return body + crc16(body)

    def _status(self, long_form: bool) -> bytes:
        d = bytes([self.output | (self.mode << 1), 0x04])
        d += self.source_mv.to_bytes(3, "big") + self.current_ma().to_bytes(3, "big")
        d += self.setpoint().to_bytes(3, "big") + b"\x00\x0a\x00\x0a"
        if long_form:
            # Battery-test fields and counters, constant as captured.
            d += bytes.fromhex("000000010000000bb800001b3500029b4d")
            count = 0x1F
        else:
            d += b"\x00\x00\x00"
            count = 0x1E                # not the real length; the load sends it like this
        return self._reply(bytes([self.address, 3, count]) + d, d)

    def handle(self, frame: bytes):
        """Return the reply to one request frame, or None to stay silent."""
        if len(frame) < 8 or crc16(frame[:-2]) != frame[-2:]:
            return None
        if frame[0] != self.address:
            return None
        func, reg = frame[1], int.from_bytes(frame[2:4], "big")
        if func == 3 and len(frame) == 8:
            if reg == REG_STATUS_SHORT:
                return self._status(False)
            if reg == REG_STATUS_LONG:
                return self._status(True)
            wide = {REG_MODEL: 1840, REG_CV: self.cv_mv, REG_CC: self.cc_ma, REG_CR: self.cr_dohm,
                    REG_CW: self.cw_cw, REG_U: self.source_mv, REG_I: self.current_ma()}
            narrow = {REG_OUTPUT: self.output, REG_MODE: self.mode}
            if reg in wide:
                d = wide[reg].to_bytes(4, "big")
            elif reg in narrow:
                # Only the low 16 bits are written; the rest is left from the last reply.
                d = self.prev + narrow[reg].to_bytes(2, "big")
            else:
                return None
            return self._reply(bytes([self.address, 3, 4]) + d, d)
        if func == 6 and len(frame) == 13 and frame[4:7] == b"\x00\x01\x04":
            value = int.from_bytes(frame[7:11], "big")
            if reg == REG_OUTPUT:
                self.output = value & 1
            elif reg == REG_MODE:
                # The manual says the mode cannot change while loading. How the real
                # load answers such a request is not known, so both guesses are offered.
                if value > 3 or (self.output and self.locked_mode == "silent"):
                    return None
                if not self.output:
                    self.mode = value
            elif reg == REG_CV:
                self.cv_mv = value
            elif reg == REG_CC:
                self.cc_ma = value
            elif reg == REG_CR:
                self.cr_dohm = value
            elif reg == REG_CW:
                self.cw_cw = value
            else:
                return None
            body = bytes([self.address, 6]) + frame[2:4] + (value & 0xFFFF).to_bytes(2, "big")
            return self._reply(body, body[4:6])
        return None


CAPTURE = """
01 03 00 01 00 04 15 c9 | 01 03 04 00 00 07 30 f8 17
01 03 01 0e 00 04 24 36 | 01 03 04 00 00 00 00 fa 33
01 03 01 10 00 04 44 30 | 01 03 04 00 00 00 01 3b f3
01 03 01 12 00 04 e5 f0 | 01 03 04 00 00 0d ac fe de
01 03 01 16 00 04 a4 31 | 01 03 04 00 00 03 e8 fa 8d
01 03 01 1a 00 04 64 32 | 01 03 04 00 00 00 64 fb d8
01 03 01 1e 00 04 25 f3 | 01 03 04 00 00 01 2c fa 7e
01 03 01 22 00 04 e5 ff | 01 03 04 00 00 5d df 83 3b
01 03 01 26 00 04 a4 3e | 01 03 04 00 00 00 00 fa 33
01 03 03 00 00 00 45 8e | 01 03 1e 02 04 00 5d df 00 00 00 00 03 e8 00 0a 00 0a 00 00 00 59 e0
01 03 03 01 00 00 14 4e | 01 03 1f 02 04 00 5d df 00 00 00 00 03 e8 00 0a 00 0a 00 00 00 01 00 00 00 0b b8 00 00 1b 35 00 02 9b 4d ec f1
01 03 01 0e 00 04 24 36 | 01 03 04 02 04 00 00 ba 4a
01 03 01 10 00 04 44 30 | 01 03 04 02 04 00 01 7b 8a
"""


def selftest() -> int:
    load, bad = Load(), 0
    for line in CAPTURE.strip().splitlines():
        tx, rx = (bytes.fromhex(p) for p in line.split("|"))
        got = load.handle(tx)
        if got != rx:
            bad += 1
            print(f"MISMATCH for {tx.hex(' ')}\n  real {rx.hex(' ')}\n  sim  {got.hex(' ') if got else None}")
    print(f"selftest: {len(CAPTURE.strip().splitlines()) - bad} of {len(CAPTURE.strip().splitlines())} captured replies reproduced")
    return 1 if bad else 0


def open_port(path: str, baud: int) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[6][termios.VMIN] = a[6][termios.VTIME] = 0
    speed = getattr(termios, f"B{baud}")
    # The WCH-Link's serial port holds stale bytes until its line coding changes,
    # so set another rate first.
    other = termios.B9600 if baud != 9600 else termios.B19200
    for s in (other, speed):
        a[4] = a[5] = s
        termios.tcsetattr(fd, termios.TCSANOW, a)
        time.sleep(0.3)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", nargs="?")
    ap.add_argument("--baud", type=int, default=57600)
    ap.add_argument("--source-mv", type=int, default=24031, help="supply voltage seen by the load")
    ap.add_argument("--echo", action="store_true", help="send each request back first, like an RS485 module that hears itself")
    ap.add_argument("--drop", type=int, default=0, metavar="N", help="stay silent for every Nth request")
    ap.add_argument("--corrupt", type=int, default=0, metavar="N", help="break the CRC of every Nth reply")
    ap.add_argument("--delay-ms", type=float, default=5.5, help="reply delay, as measured on the real load")
    ap.add_argument("--locked-mode", choices=["silent", "ack"], default="silent",
                    help="how a mode change is answered while the input is on (unknown on the real load)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.port:
        ap.error("PORT is required")

    fd = open_port(args.port, args.baud)
    load = Load(source_mv=args.source_mv, locked_mode=args.locked_mode)
    gap = max(0.003, 40 / args.baud)        # 3.5 characters, floored for USB latency
    buf, last, n = b"", 0.0, 0
    print(f"KP184 simulator on {args.port} at {args.baud} 8N1, address {load.address}", flush=True)
    try:
        while True:
            r, _, _ = select.select([fd], [], [], 0.002)
            now = time.monotonic()
            if r:
                try:
                    d = os.read(fd, 256)
                except BlockingIOError:
                    d = b""
                if d:
                    buf += d
                    last = now
                    continue
            if buf and now - last >= gap:
                frame, buf = buf, b""
                n += 1
                reply = load.handle(frame)
                note = ""
                if reply and args.drop and n % args.drop == 0:
                    reply, note = None, " (dropped on purpose)"
                elif reply and args.corrupt and n % args.corrupt == 0:
                    reply, note = reply[:-1] + bytes([reply[-1] ^ 0xFF]), " (CRC broken on purpose)"
                if args.echo:
                    os.write(fd, frame)
                if reply:
                    time.sleep(args.delay_ms / 1000)
                    os.write(fd, reply)
                if args.verbose:
                    print(f"rx {frame.hex(' ')} -> {reply.hex(' ') if reply else 'silent'}{note}", flush=True)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
