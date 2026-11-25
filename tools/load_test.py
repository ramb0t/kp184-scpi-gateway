#!/usr/bin/env python3
"""Acceptance test of the gateway against a real KP184 with a DC supply on its input.

Usage: load_test.py HOST [--max-current 0.3]

Switches the load's input on at small currents (0.1 A up to --max-current) in
CC, CR and CW mode, so the supply must tolerate that. Never CV with the input
on: below the supply voltage a CV load sinks as much current as it can.

Safety: the input is switched on only after the mode and the setpoint it is
about to use have been read back, the current is watched while it settles and
any reading above 1.5 x --max-current switches the input off and aborts, and
the input is switched off and checked after every step. The fail-safe tests
leave it on across a reconnect for a second or so; one of them does that
with the gateway's fail-safe off, so a test killed right there leaves the
input on. The load's original setpoints and mode are restored at the end with
the input off, except a mode of CV, which this load does not take remotely.
"""
import argparse
import socket
import sys
import time

from gateway import RAW_PORT, Checks, Scpi, raw_exchange

SETTINGS = ("FUNC", "CURR", "VOLT", "RES", "POW")
MODE_COMMAND = {"CURRENT": "CURR", "RESISTANCE": "RES", "POWER": "POW"}     # FUNC? reply -> FUNC parameter
STATUS_READ = bytes.fromhex("01 03 03 00 00 00 45 8e")      # the load's status block, 23 bytes back
check = Checks()


class Abort(Exception):
    pass


class Load(Scpi):
    """The load through the gateway, with a current trip."""

    def __init__(self, host, trip_a):
        self.trip_a = trip_a
        super().__init__(host)

    def write(self, cmd, end="\n"):
        super().write(cmd, end)
        time.sleep(0.08)

    def current(self):
        i = self.number("MEAS:CURR?")
        if i is not None and i > self.trip_a:
            self.write("INP OFF")
            raise Abort(f"current {i} A above the trip level {self.trip_a} A")
        return i

    def on(self, mode, setpoint, settle):
        """Switch the input on, but only into the mode and setpoint the step expects."""
        query, want = setpoint
        found = (self.query("FUNC?"), self.query(query))
        if found != (mode, want):
            raise Abort(f"not switching on: expected {mode} with {query} {want}, the load has {found}")
        self.write("INP ON")
        until = time.monotonic() + settle
        while time.monotonic() < until:
            self.current()
            time.sleep(0.1)

    def off(self):
        self.write("INP OFF")
        time.sleep(0.4)
        return self.query("INP?") == "0"

    def reconnect(self):
        self.close()
        self.connect()

    def settings(self):
        return {c: self.query(c + "?") for c in SETTINGS}


def near(value, want, tol):
    return value is not None and abs(value - want) <= tol


def preconditions(ld):
    """Returns the supply voltage."""
    print("Preconditions")
    if not check("*TST? -> 0 (load answers)", ld.query("*TST?") == "0"):
        raise Abort("the load does not answer")
    if ld.query("INP?") != "0":
        check("input was on; switching it off", ld.off())
    supply = ld.number("MEAS:VOLT?")
    if not check("supply between 5 and 30 V", supply is not None and 5 <= supply <= 30, supply):
        raise Abort("unexpected supply voltage")
    return supply


def setpoints(ld, r_ohm, p_w):
    print("Setpoints, input off")
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


def refused_inputs(ld):
    print("Inputs that must not change the setpoint")
    for cmd, code in [("CURR 1 E-1", -120), ("CURR 1E -1", -120), ("CURR 0,2", -108),
                      ("CURR 41", -222), ("CURR 0.2V", -131), ("CURR -0.1", -222)]:
        ld.write(cmd)
        errs = ld.errors()
        got = ld.query("CURR?")
        check(f"{cmd} -> error {code}, CURR? still 0.100", errs == [code] and got == "0.100", f"errors {errs}, CURR? {got}")
    ld.write("INP 1E-3")
    state, errs = ld.query("INP?"), ld.errors()
    check("INP 1E-3 rounds to off", state == "0" and errs == [], f"INP? {state}, errors {errs}")
    ld.s.sendall(b"X" * 300 + b"CURR 0.2\n")
    time.sleep(0.3)
    errs = ld.errors()
    got = ld.query("CURR?")
    check("overlong line is dropped whole", got == "0.100" and -363 in errs, f"CURR? {got}, errors {errs}")
    ld.s.sendall(b"CURR 0.15\n")
    ld.reconnect()
    got = ld.query("CURR?")
    check("a command sent just before closing is run", got == "0.150", f"CURR? {got}")
    ld.write("CURR 0.1")


CC_100MA = ("CURRENT", ("CURR?", "0.100"))


def on_and_measure(ld, label, want_a, want_mode, setpoint):
    ld.on(want_mode, setpoint, settle=1.5)      # the load takes about 0.5 s to start sinking
    check(f"{label}: INP? -> 1", ld.query("INP?") == "1")
    check(f"{label}: FUNC? -> {want_mode}", ld.query("FUNC?") == want_mode)
    i = ld.current()
    v = ld.number("MEAS:VOLT?")
    p = ld.number("MEAS:POW?")
    res = ld.number("MEAS:RES?")
    check(f"{label}: MEAS:CURR? {i} A, want {want_a} A", near(i, want_a, 0.02 + 0.03 * want_a))
    check(f"{label}: MEAS:POW? {p} W = V x I", p is not None and v is not None and i is not None
          and near(p, v * i, 0.01), f"V {v} I {i}")
    check(f"{label}: MEAS:RES? {res} ohm = V / I", res is not None and v is not None and i
          and near(res, v / i, 0.5), f"V {v} I {i}")


def input_on(ld, supply, r_ohm, p_w, max_a):
    print("Input on, CC")
    on_and_measure(ld, "CC 0.1 A", 0.1, *CC_100MA)
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
    on_and_measure(ld, f"CR {r_ohm} ohm", supply / r_ohm, "RESISTANCE", ("RES?", f"{r_ohm:.1f}"))
    check("input off", ld.off())

    print("Input on, CW")
    ld.write("FUNC POW")
    on_and_measure(ld, f"CW {p_w} W", p_w / supply, "POWER", ("POW?", f"{p_w:.2f}"))
    check("input off", ld.off())


def mode_change_while_on(ld):
    print("Mode change while the input is on")
    # The load itself would switch at once (firmware 2128); the gateway refuses,
    # as the manual says the load should. The other setpoints are parked at about
    # 0.1 A in case it does not.
    ld.write("FUNC CURR")
    ld.on(*CC_100MA, settle=1.0)
    ld.write("FUNC RES")
    time.sleep(0.5)
    errs = ld.errors()
    mode = ld.query("FUNC?")
    i = ld.current()
    check(f"refused with -221, still CC at 0.1 A ({i} A)",
          errs == [-221] and mode == "CURRENT" and near(i, 0.1, 0.02), f"errors {errs}, FUNC? {mode}")
    check("input off", ld.off())


def reset(ld):
    print("*RST")
    ld.on(*CC_100MA, settle=0.5)
    ld.write("*RST")
    time.sleep(0.4)
    check("*RST switches the input off", ld.query("INP?") == "0")
    check("no errors", ld.errors() == [])


def failsafe(ld):
    print("Fail-safe")
    check("SYST:FAIL? -> 1 by default", ld.query("SYST:FAIL?") == "1")
    ld.on(*CC_100MA, settle=0.5)
    ld.reconnect()                                  # dropped without INP OFF
    check("the input goes off when the client that switched it on leaves", ld.query("INP?") == "0")
    ld.write("SYST:FAIL OFF")
    ld.on(*CC_100MA, settle=0.5)
    ld.reconnect()
    check("with SYST:FAIL OFF the input stays on after the client leaves", ld.query("INP?") == "1")
    check("SYST:FAIL? -> 1 again for the new client", ld.query("SYST:FAIL?") == "1")
    ld.reconnect()                                  # this client never switched it on
    check("a client that did not switch the input on leaves it alone", ld.query("INP?") == "1")
    ld.write("INP ON")                              # already on; now this client owns it
    raw = socket.create_connection((ld.host, RAW_PORT), timeout=5)
    try:
        time.sleep(0.3)
        ld.close()                                  # the raw client holds the bus: fail-safe must wait
        status = raw_exchange(raw, STATUS_READ, 23, timeout=1)
        check("fail-safe waits while a raw client holds the bus", len(status) == 23 and status[3] & 1 == 1,
              status.hex(" "))
    finally:
        raw.close()
    time.sleep(0.6)
    ld.connect()
    check("... and switches the input off once the raw client has gone", ld.query("INP?") == "0")
    check("no errors", ld.errors() == [])
    print("  diagnostics: reset cause", ld.query("DIAG:RES:CAUS?"), "| uptime", ld.query("DIAG:UPT?"), "s")


def restore(ld, original):
    """Input off and the original settings back, on a fresh connection, whatever state the test stopped in."""
    try:
        ld.close()
    except OSError:
        pass
    if not ld.connect_when_free():
        check("clean-up: the gateway takes no client, so the input and settings were NOT put back", False)
        return
    off = ld.off()
    if original:
        if original["FUNC"] in MODE_COMMAND:
            ld.write(f"FUNC {MODE_COMMAND[original['FUNC']]}")
        else:
            print(f"  the load was in {original['FUNC']} mode, which it only takes at its front panel")
        for c in ("CURR", "RES", "POW", "VOLT"):
            ld.write(f"{c} {original[c]}")
        restored = ld.settings()
        check(f"original settings restored {restored}", restored == original)
    check("input off at the end", off and ld.query("INP?") == "0")
    ld.close()


def arguments():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host")
    ap.add_argument("--max-current", type=float, default=0.3, metavar="A",
                    help="highest current the test draws, 0.1 to 5 A (default 0.3)")
    args = ap.parse_args()
    if not 0.1 <= args.max_current <= 5:
        ap.error("--max-current must be between 0.1 and 5 A")
    return args.host, args.max_current


def main():
    host, max_a = arguments()
    ld = Load(host, trip_a=1.5 * max_a)
    original = None
    try:
        supply = preconditions(ld)
        original = ld.settings()
        print(f"  saved {original}")
        r_ohm = round(supply / 0.1, 1)                  # about 0.1 A in CR
        p_w = round(supply * 0.1, 2)                    # about 0.1 A in CW
        setpoints(ld, r_ohm, p_w)
        refused_inputs(ld)
        input_on(ld, supply, r_ohm, p_w, max_a)
        mode_change_while_on(ld)
        reset(ld)
        failsafe(ld)
    except Abort as e:
        check(f"ABORTED: {e}", False)
    finally:
        restore(ld, original)
    return check.summary()


if __name__ == "__main__":
    sys.exit(main())
