#!/usr/bin/env python3
"""Acceptance test of the gateway against a real KP184 with a DC supply on its input.

Usage: load_test.py HOST [--max-current 0.3]

Switches the load's input on at small currents (0.1 A up to --max-current) in
CC, CR and CW mode, so the supply must tolerate that. Never CV with the input
on: below the supply voltage a CV load sinks as much current as it can.

Safety: the input is switched off and checked after every step (the fail-safe
tests leave it on across a reconnect for a second or so), any current
reading above 1.5 x --max-current switches it off and aborts, the CV setpoint
is parked at 150 V before anything is switched on, and the load's original
mode and setpoints are restored at the end with the input off.
"""
import socket
import sys
import time

PORT = 5025
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'ok  ' if ok else 'FAIL'} {name}{'' if ok else '   ' + str(detail)}", flush=True)
    return ok


class Abort(Exception):
    pass


class Load:
    def __init__(self, host, trip_a):
        self.host, self.trip_a = host, trip_a
        self.connect()

    def connect(self):
        self.s = socket.create_connection((self.host, PORT), timeout=5)
        self.s.settimeout(2.0)

    def close(self):
        self.s.close()
        time.sleep(0.3)

    def write(self, cmd):
        self.s.sendall(cmd.encode() + b"\n")
        time.sleep(0.08)

    def query(self, cmd):
        self.s.sendall(cmd.encode() + b"\n")
        buf = b""
        try:
            while not buf.endswith(b"\n"):
                d = self.s.recv(256)
                if not d:
                    return None
                buf += d
        except socket.timeout:
            return None
        return buf.decode().strip()

    def num(self, cmd):
        r = self.query(cmd)
        try:
            return float(r)
        except (TypeError, ValueError):
            return None

    def errors(self):
        out = []
        for _ in range(12):
            r = self.query("SYST:ERR?")
            if r is None or r.startswith("0,"):
                break
            out.append(int(r.split(",")[0]))
        return out

    def current(self):
        i = self.num("MEAS:CURR?")
        if i is not None and i > self.trip_a:
            self.write("INP OFF")
            raise Abort(f"current {i} A above the trip level {self.trip_a} A")
        return i

    def off(self):
        self.write("INP OFF")
        time.sleep(0.4)
        return self.query("INP?") == "0"


def near(value, want, tol):
    return value is not None and abs(value - want) <= tol


def main():
    args = sys.argv[1:]
    max_a = 0.3
    if "--max-current" in args:
        i = args.index("--max-current")
        max_a = float(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        raise SystemExit(__doc__)
    host = args[0]
    ld = Load(host, trip_a=1.5 * max_a)
    original = None
    try:
        print("Preconditions")
        if not check("*TST? -> 0 (load answers)", ld.query("*TST?") == "0"):
            raise Abort("the load does not answer")
        if ld.query("INP?") != "0":
            check("input was on; switching it off", ld.off())
        supply = ld.num("MEAS:VOLT?")
        if not check("supply between 5 and 30 V", supply is not None and 5 <= supply <= 30, supply):
            raise Abort("unexpected supply voltage")
        original = {c: ld.query(c + "?") for c in ("FUNC", "CURR", "VOLT", "RES", "POW")}
        print(f"  saved {original}")

        print("Setpoints, input off")
        r_ohm = round(supply / 0.1, 1)                  # about 0.1 A in CR
        p_w = round(supply * 0.1, 2)                    # about 0.1 A in CW
        for cmd, query, want in [("VOLT MAX", "VOLT?", "150.000"), ("CURR 0.1", "CURR?", "0.100"),
                                 (f"RES {r_ohm}", "RES?", f"{r_ohm:.1f}"), (f"POW {p_w}", "POW?", f"{p_w:.2f}"),
                                 ("CURR 250mA", "CURR?", "0.250"), ("CURR 0.1", "CURR?", "0.100"),
                                 ("FUNC RES", "FUNC?", "RESISTANCE"), ("FUNC POW", "FUNC?", "POWER"),
                                 ("FUNC CURR", "FUNC?", "CURRENT")]:
            ld.write(cmd)
            got = ld.query(query)
            check(f"{cmd} -> {query} {want}", got == want, f"got {got!r}")
        check("no errors", ld.errors() == [])
        # Firmware 2128 acknowledges a switch to CV over the bus but stays in its mode.
        ld.write("FUNC VOLT")
        errs = ld.errors()
        mode = ld.query("FUNC?")
        check("FUNC VOLT is refused with -221 (the load ignores it)", errs == [-221] and mode == "CURRENT",
              f"errors {errs}, FUNC? {mode}")

        print("Inputs that must not change the setpoint")
        for cmd, code in [("CURR 1 E-1", -120), ("CURR 1E -1", -120), ("CURR 0,2", -108),
                          ("CURR 41", -222), ("CURR 0.2V", -131), ("CURR -0.1", -222)]:
            ld.write(cmd)
            errs = ld.errors()
            got = ld.query("CURR?")
            check(f"{cmd} -> error {code}, CURR? still 0.100", errs == [code] and got == "0.100", f"errors {errs}, CURR? {got}")
        ld.write("INP 1E-3")
        check("INP 1E-3 rounds to off", ld.query("INP?") == "0" and ld.errors() == [])
        ld.s.sendall(b"X" * 300 + b"CURR 0.2\n")
        time.sleep(0.3)
        errs = ld.errors()
        got = ld.query("CURR?")
        check("overlong line is dropped whole", got == "0.100" and -363 in errs, f"CURR? {got}, errors {errs}")
        ld.s.sendall(b"CURR 0.15\n")
        ld.close()
        ld.connect()
        got = ld.query("CURR?")
        check("a command sent just before closing is run", got == "0.150", f"CURR? {got}")
        ld.write("CURR 0.1")

        def on_and_measure(label, want_a, want_mode):
            ld.write("INP ON")
            time.sleep(1.5)                             # the load takes about 0.5 s to start sinking
            check(f"{label}: INP? -> 1", ld.query("INP?") == "1")
            check(f"{label}: FUNC? -> {want_mode}", ld.query("FUNC?") == want_mode)
            i = ld.current()
            v = ld.num("MEAS:VOLT?")
            p = ld.num("MEAS:POW?")
            res = ld.num("MEAS:RES?")
            check(f"{label}: MEAS:CURR? {i} A, want {want_a} A", near(i, want_a, 0.02 + 0.03 * want_a))
            check(f"{label}: MEAS:POW? {p} W = V x I", p is not None and v is not None and i is not None
                  and near(p, v * i, 0.01), f"V {v} I {i}")
            check(f"{label}: MEAS:RES? {res} ohm = V / I", res is not None and i and near(res, v / i, 0.5))
            return i, v

        print("Input on, CC")
        on_and_measure("CC 0.1 A", 0.1, "CURRENT")
        ld.write(f"CURR {max_a}")
        time.sleep(1.0)
        check(f"setpoint change while on: MEAS:CURR? near {max_a} A", near(ld.current(), max_a, 0.02 + 0.03 * max_a))
        check("input off", ld.off())
        time.sleep(0.5)
        i = ld.current()
        check(f"no current with the input off ({i} A)", i is not None and i < 0.01)
        ld.write("CURR 0.1")

        print("Input on, CR")
        ld.write("FUNC RES")
        on_and_measure(f"CR {r_ohm} ohm", supply / r_ohm, "RESISTANCE")
        check("input off", ld.off())

        print("Input on, CW")
        ld.write("FUNC POW")
        on_and_measure(f"CW {p_w} W", p_w / supply, "POWER")
        check("input off", ld.off())

        print("Mode change while the input is on")
        # The load itself would switch at once (firmware 2128); the gateway refuses,
        # as the manual says the load should. The other setpoints are parked at about
        # 0.1 A in case it does not.
        ld.write("FUNC CURR")
        ld.write("INP ON")
        time.sleep(1.0)
        ld.write("FUNC RES")
        time.sleep(0.5)
        errs = ld.errors()
        mode = ld.query("FUNC?")
        i = ld.current()
        check(f"refused with -221, still CC at 0.1 A ({i} A)",
              errs == [-221] and mode == "CURRENT" and near(i, 0.1, 0.02), f"errors {errs}, FUNC? {mode}")
        check("input off", ld.off())

        print("*RST and client disconnect")
        ld.write("INP ON")
        time.sleep(0.5)
        ld.write("*RST")
        time.sleep(0.4)
        check("*RST switches the input off", ld.query("INP?") == "0")
        check("no errors", ld.errors() == [])

        print("Fail-safe")
        check("SYST:FAIL? -> 1 by default", ld.query("SYST:FAIL?") == "1")
        ld.write("INP ON")
        time.sleep(0.5)
        ld.close()                                      # dropped without INP OFF
        ld.connect()
        check("the input goes off when the client that switched it on leaves", ld.query("INP?") == "0")
        ld.write("SYST:FAIL OFF")
        ld.write("INP ON")
        time.sleep(0.5)
        ld.close()
        ld.connect()
        check("with SYST:FAIL OFF the input stays on after the client leaves", ld.query("INP?") == "1")
        check("SYST:FAIL? -> 1 again for the new client", ld.query("SYST:FAIL?") == "1")
        ld.close()                                      # this client never switched it on
        ld.connect()
        check("a client that did not switch the input on leaves it alone", ld.query("INP?") == "1")
        ld.write("INP ON")                              # now this client owns it
        raw = socket.create_connection((host, 4001), timeout=5)
        time.sleep(0.3)
        ld.close()                                      # the raw client holds the bus: fail-safe must wait
        raw.sendall(bytes.fromhex("01 03 03 00 00 00 45 8e"))     # status block, read by the raw client
        raw.settimeout(1)
        status = b""
        try:
            while len(status) < 23:
                status += raw.recv(64)
        except socket.timeout:
            pass
        check("fail-safe waits while a raw client holds the bus", len(status) == 23 and status[3] & 1 == 1,
              status.hex(" "))
        raw.close()
        time.sleep(0.6)
        ld.connect()
        check("... and switches the input off once the raw client has gone", ld.query("INP?") == "0")
        check("no errors", ld.errors() == [])
        print("  diagnostics: reset cause", ld.query("DIAG:RES:CAUS?"), "| uptime", ld.query("DIAG:UPT?"), "s")
    except Abort as e:
        check(f"ABORTED: {e}", False)
    finally:
        try:
            ld.close()
        except OSError:
            pass
        ld = Load(host, trip_a=99)
        ok = ld.off()
        if original:
            ld.write(f"FUNC {original['FUNC'][:4]}")
            for c in ("CURR", "RES", "POW", "VOLT"):
                ld.write(f"{c} {original[c]}")
            restored = {c: ld.query(c + "?") for c in ("FUNC", "CURR", "VOLT", "RES", "POW")}
            check(f"original settings restored {restored}", restored == original)
        check("input off at the end", ok and ld.query("INP?") == "0")
        ld.close()
    print(f"\n{sum(results)} of {len(results)} checks passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
