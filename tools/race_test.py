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

SCPI, RAW = 5025, 4001
MODEL_READ = bytes.fromhex("01 03 00 01 00 04 15 c9")     # the load answers 1840, which would read as 1.840
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'ok  ' if ok else 'FAIL'} {name}{'' if ok else '   ' + str(detail)}", flush=True)
    return ok


def connect(host, port):
    s = socket.create_connection((host, port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(3)
    return s


def query(s, cmd):
    s.sendall(cmd.encode() + b"\n")
    buf = b""
    try:
        while not buf.endswith(b"\n"):
            d = s.recv(256)
            if not d:
                return None
            buf += d
    except (socket.timeout, ConnectionError):
        return None
    return buf.decode().strip()


def scpi(host, *cmds):
    """Run commands on a fresh connection, retrying the connect for a few seconds."""
    for _ in range(30):
        try:
            s = connect(host, SCPI)
            break
        except OSError:
            time.sleep(0.2)
    else:
        return None
    out = [query(s, c) if "?" in c else s.sendall(c.encode() + b"\n") for c in cmds]
    s.close()
    time.sleep(0.05)
    return out


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "eth485"
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    first = scpi(host, "*TST?", "INP?", "CURR?", "MEAS:CURR?")
    if not first or first[0] != "0" or first[1] != "0":
        raise SystemExit(f"load not answering or input on: {first}")
    original = first[2]
    print(f"CC setpoint before: {original} A, input off, current {first[3]} A")

    print(f"A command sent just before closing, behind one still running ({rounds} rounds)")
    bad = []
    for i in range(rounds):
        want = f"{0.2 + i / 1000:.3f}"
        s = connect(host, SCPI)
        s.sendall(b"MEAS:VOLT?\n")
        time.sleep(0.005)                       # the voltage read is on the bus now
        s.sendall(f"CURR {want}\n".encode())
        s.close()
        got = scpi(host, "CURR?")
        if not got or got[0] != want:
            bad.append((want, got))
    check("each closing command was run and the gateway stayed up", not bad, bad[:3])

    print(f"A client leaves without its reply and is replaced at once ({rounds} rounds)")
    bad = []
    for i in range(rounds):
        a = connect(host, SCPI)
        a.sendall(b"MEAS:VOLT?\n")              # about 24 V
        a.close()
        b = None
        for _ in range(30):
            try:
                b = connect(host, SCPI)
                break
            except OSError:
                time.sleep(0.05)
        got = query(b, "MEAS:CURR?") if b else None     # 0.000 with the input off
        if b:
            b.close()
        if got != "0.000":
            bad.append(got)
        time.sleep(0.05)
    check("the new client never got the old client's reply", not bad, bad[:5])

    print(f"Raw client hands over to SCPI before the load has answered it ({rounds} rounds)")
    now = scpi(host, "CURR?")[0]                # the first section changed it
    bad = []
    for i in range(rounds):
        r = connect(host, RAW)
        r.sendall(MODEL_READ)
        r.close()
        got = scpi(host, "CURR?")
        if not got or got[0] != now:
            bad.append(got)
    check(f"SCPI read {now} A every time, never the raw reply (1.840)", not bad, bad[:5])

    print("Restore and check")
    end = scpi(host, f"CURR {original}", "CURR?", "INP?", "SYST:ERR?")
    check(f"CC setpoint restored ({end})", end and end[1] == original and end[2] == "0")
    print(f"\n{sum(results)} of {len(results)} checks passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
