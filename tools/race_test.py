#!/usr/bin/env python3
"""Connection-timing test of the gateway against the real KP184. The input stays off.

Usage: race_test.py HOST [ROUNDS]

Exercises what the gateway must survive when clients come and go while it is
talking to the load: a command sent just before closing, a client that leaves
without reading its reply and is replaced at once, and a raw-port client
handing over to SCPI before the load has answered it. Changes only the CC
setpoint, and puts it back.
"""
import socket
import sys
import time

from gateway import RAW_PORT, Checks, Scpi

MODEL_READ = bytes.fromhex("01 03 00 01 00 04 15 c9")     # the load answers 1840, which would read as 1.840
check = Checks()


def connect(host, pause=0.2):
    """A new SCPI client, retrying for a few seconds while the gateway is still busy with the last one."""
    for _ in range(30):
        try:
            return Scpi(host, timeout=3)
        except OSError:
            time.sleep(pause)
    return None


def scpi(host, *cmds):
    """Run commands on a fresh connection; returns the replies, None for a command without one."""
    s = connect(host)
    if not s:
        return None
    out = [s.query(c) if "?" in c else s.write(c) for c in cmds]
    s.close(settle=0.05)
    return out


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "eth485"
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    first = scpi(host, "*TST?", "INP?", "CURR?", "MEAS:CURR?")
    if not first or first[0] != "0" or first[1] != "0":
        raise SystemExit(f"load not answering or input on: {first}")
    original = first[2]
    print(f"CC setpoint before: {original} A, input off, current {first[3]} A")
    try:
        races(host, rounds)
    finally:
        print("Restore and check")
        end = scpi(host, f"CURR {original}", "CURR?", "INP?", "SYST:ERR?")
        check(f"CC setpoint restored, input off, no errors ({end})",
              end and end[1] == original and end[2] == "0" and (end[3] or "").startswith("0,"))
    return check.summary()


def races(host, rounds):
    print(f"A command sent just before closing, behind one still running ({rounds} rounds)")
    bad = []
    for i in range(rounds):
        want = f"{0.2 + i / 1000:.3f}"
        s = Scpi(host)
        s.write("MEAS:VOLT?")
        time.sleep(0.005)                       # the voltage read is on the bus now
        s.write(f"CURR {want}")
        s.close(settle=0)
        got = scpi(host, "CURR?")
        if not got or got[0] != want:
            bad.append((want, got))
    check("each closing command was run and the gateway stayed up", not bad, bad[:3])

    print(f"A client leaves without its reply and is replaced at once ({rounds} rounds)")
    bad = []
    for i in range(rounds):
        a = Scpi(host)
        a.write("MEAS:VOLT?")                   # about 24 V
        a.close(settle=0)
        b = connect(host, pause=0.05)
        got = b.query("MEAS:CURR?") if b else None      # 0.000 with the input off
        if b:
            b.close(settle=0.05)
        if got != "0.000":
            bad.append(got)
    check("the new client never got the old client's reply", not bad, bad[:5])

    print(f"Raw client hands over to SCPI before the load has answered it ({rounds} rounds)")
    now = (scpi(host, "CURR?") or [None])[0]    # the first section changed it
    bad = []
    for i in range(rounds):
        r = socket.create_connection((host, RAW_PORT), timeout=5)
        r.sendall(MODEL_READ)
        r.close()
        got = scpi(host, "CURR?")
        if not got or got[0] != now:
            bad.append(got)
    check(f"SCPI read {now} A every time, never the raw reply (1.840)", not bad, bad[:5])


if __name__ == "__main__":
    sys.exit(main())
