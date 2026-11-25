#!/usr/bin/env python3
"""End-to-end test of the gateway against the KP184 simulator.

Usage: gateway_test.py HOST SERIAL_PORT [--soak N]

Starts tools/kp184_sim.py on SERIAL_PORT, which must be wired to the board's
TXD/RXD header in place of the load, then drives the SCPI port (5025) and the
raw port (4001) of the gateway at HOST.

Do not run this with a real load on the board: it switches the input on.
It refuses to start if something already answers on the RS485 side.
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SCPI_PORT, RAW_PORT = 5025, 4001
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'ok  ' if ok else 'FAIL'} {name}{'' if ok else '   ' + str(detail)}")


class Sim:
    def __init__(self, port):
        self.port, self.proc = port, None

    def start(self, *opts):
        self.stop()
        self.proc = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "kp184_sim.py"), self.port, *opts],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        line = self.proc.stdout.readline()
        if "simulator on" not in line:
            raise SystemExit(f"simulator did not start: {line!r}")

    def stop(self):
        if self.proc:
            self.proc.terminate()
            self.proc.wait()
            self.proc = None
            time.sleep(0.2)


class Scpi:
    def __init__(self, host):
        self.s = socket.create_connection((host, SCPI_PORT), timeout=5)
        self.s.settimeout(2.0)

    def write(self, cmd, end="\n"):
        self.s.sendall((cmd + end).encode())

    def query(self, cmd, end="\n"):
        """Reply without its newline, or None when nothing came back."""
        self.write(cmd, end)
        buf = b""
        try:
            while not buf.endswith(b"\n"):
                d = self.s.recv(256)
                if not d:
                    return None
                buf += d
        except (socket.timeout, ConnectionError):
            return None
        return buf.decode().rstrip("\n")

    def errors(self):
        """Drain the error queue; returns the list of error numbers."""
        out = []
        for _ in range(12):
            r = self.query("SYST:ERR?")
            if r is None or r.startswith("0,"):
                break
            out.append(int(r.split(",")[0]))
        return out

    def close(self):
        self.s.close()
        time.sleep(0.3)


def expect(scpi, cmd, want):
    got = scpi.query(cmd)
    check(f"{cmd} -> {want}", got == want, f"got {got!r}")


def expect_error(scpi, cmd, code):
    scpi.errors()
    if "?" in cmd:
        scpi.query(cmd)
    else:
        scpi.write(cmd)
        time.sleep(0.9 if code == -365 else 0.15)
    got = scpi.errors()
    check(f"{cmd} -> error {code}", got == [code], f"error queue {got}")


def rejected(host, port):
    """True if a connection to a port that already has a client is refused or closed at once."""
    try:
        s = socket.create_connection((host, port), timeout=3)
    except OSError:
        return True
    s.settimeout(3)
    try:
        closed = s.recv(16) == b""
    except ConnectionError:
        closed = True
    except socket.timeout:
        closed = False
    s.close()
    return closed


def rejected_after(s):
    """True if an open connection has been closed by the gateway."""
    s.settimeout(2)
    try:
        return s.recv(16) == b""
    except ConnectionError:
        return True
    except socket.timeout:
        return False


def main():
    soak = 0
    if "--soak" in sys.argv:
        i = sys.argv.index("--soak")
        soak = int(sys.argv[i + 1])
        del sys.argv[i:i + 2]
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    host, port = sys.argv[1], sys.argv[2]
    sim = Sim(port)

    # With the simulator not yet running, nothing may answer on the RS485 side.
    t = Scpi(host)
    alone = t.query("*TST?")
    t.close()
    if alone != "1":
        raise SystemExit(f"*TST? returned {alone!r} before the simulator started: a load seems to be "
                         "connected to the board. Refusing to run, this test switches the input on.")
    try:
        print("Simulated load, plain link")
        sim.start()
        t = Scpi(host)
        idn = t.query("*IDN?")
        check("*IDN? names the load", bool(idn) and idn.startswith("Kunkin,KP184,"), idn)
        expect(t, "*TST?", "0")
        for cmd, want in [("FUNC?", "CURRENT"), ("CURR?", "1.000"), ("VOLT?", "3.500"), ("RES?", "10.0"),
                          ("POW?", "3.00"), ("INP?", "0"), ("MEAS:VOLT?", "24.031"), ("MEAS:CURR?", "0.000"),
                          ("MEAS:POW?", "0.000"), ("MEAS:RES?", "9.9E37"), ("FETC:VOLT?", "24.031"),
                          ("CURR? MAX", "40.000"), ("VOLT? MAX", "150.000"), ("POW? MAX", "400.00")]:
            expect(t, cmd, want)
        for cmd, query, want in [("CURR 2.5", "CURR?", "2.500"), ("CURR 500mA", "CURR?", "0.500"),
                                 ("VOLT 12", "VOLT?", "12.000"), ("VOLT 150", "VOLT?", "150.000"),
                                 ("RES 47.5", "RES?", "47.5"), ("POW 30", "POW?", "30.00"),
                                 ("CURR MAX", "CURR?", "40.000"), ("CURR MIN", "CURR?", "0.000"),
                                 (":SOURce:CURRent:LEVel:IMMediate 1.25", ":SOUR:CURR?", "1.250"),
                                 ("FUNC RES", "FUNC?", "RESISTANCE"), ("FUNC POW", "FUNC?", "POWER"),
                                 ("FUNC VOLT", "FUNC?", "VOLTAGE"), ("FUNC CURR", "FUNC?", "CURRENT")]:
            t.write(cmd)
            expect(t, query, want)
        check("no errors so far", t.errors() == [])
        t.write("INP ON")
        expect(t, "INP?", "1")
        expect(t, "MEAS:CURR?", "1.250")
        expect(t, "MEAS:POW?", "30.039")        # 24.031 V x 1.250 A
        expect(t, "MEAS:RES?", "19.225")        # 24.031 V / 1.250 A
        expect_error(t, "FUNC RES", -221)       # the gateway refuses a mode change while the input is on
        t.write("*RST")
        expect(t, "INP?", "0")
        expect(t, "CURR 3;CURR?", "3.000")
        expect(t, "CURR?", "3.000")
        check("CR LF line ending", t.query("VOLT?", end="\r\n") == "150.000")
        t.write("CURR 1\nCURR?")                # two lines in one segment
        check("two commands in one packet", t.query("") == "1.000")
        for cmd, code in [("CURR 41", -222), ("RES 0", -222), ("CURR 1V", -131), ("CURR", -109),
                          ("BOGUS?", -113), ("FUNC BANANA", -224)]:
            expect_error(t, cmd, code)

        print("Second client on each port")
        check("second SCPI client is turned away", rejected(host, SCPI_PORT))
        expect(t, "CURR?", "1.000")

        print("Raw port")
        raw = socket.create_connection((host, RAW_PORT), timeout=5)
        raw.settimeout(2)
        time.sleep(0.3)
        raw.sendall(bytes.fromhex("01 03 00 01 00 04 15 c9"))
        got = b""
        try:
            while len(got) < 9:
                got += raw.recv(64)
        except socket.timeout:
            pass
        check("raw client reads the model register", got == bytes.fromhex("01 03 04 00 00 07 30 f8 17"), got.hex(" "))
        expect_error(t, "MEAS:VOLT?", 100)      # the raw client owns the RS485 port
        check("*IDN? still answers", bool(t.query("*IDN?")))
        # On the raw port the newest client takes over and the old one is closed.
        raw2 = socket.create_connection((host, RAW_PORT), timeout=5)
        raw2.settimeout(2)
        time.sleep(0.3)
        raw2.sendall(bytes.fromhex("01 03 00 01 00 04 15 c9"))
        got = b""
        try:
            while len(got) < 9:
                got += raw2.recv(64)
        except socket.timeout:
            pass
        check("a second raw client takes over", got == bytes.fromhex("01 03 04 00 00 07 30 f8 17"), got.hex(" "))
        check("... and the first is closed", rejected_after(raw))
        raw2.close()
        raw.close()
        time.sleep(0.5)
        expect(t, "MEAS:VOLT?", "24.031")
        t.close()

        print("RS485 module that echoes what the board sends")
        sim.start("--echo")
        t = Scpi(host)
        t.write("CURR 1.5")
        expect(t, "CURR?", "1.500")
        expect(t, "MEAS:VOLT?", "24.031")
        check("no errors", t.errors() == [])
        t.close()

        print("Echoing module and every third request unanswered")
        sim.start("--echo", "--drop", "3")
        t = Scpi(host)
        ok = True
        for i in range(12):
            want = f"{1 + i / 8:.3f}"
            t.write(f"CURR {want}")
            ok &= t.query("CURR?") == want
        check("12 writes each read back", ok)
        t.write("INP ON")
        expect(t, "INP?", "1")
        t.write("INP OFF")
        expect(t, "INP?", "0")
        check("no errors", t.errors() == [])
        t.close()

        print("Every third request unanswered, then every second reply corrupt")
        sim.start("--drop", "3")
        t = Scpi(host)
        check("12 queries all answered", all(t.query("MEAS:VOLT?") == "24.031" for _ in range(12)))
        check("no errors", t.errors() == [])
        t.close()
        sim.start("--corrupt", "2")
        t = Scpi(host)
        check("12 queries all answered", all(t.query("CURR?") == "1.000" for _ in range(12)))
        check("no errors", t.errors() == [])
        t.close()

        print("Load that acknowledges a locked mode change without obeying")
        sim.start("--locked-mode", "ack")
        t = Scpi(host)
        t.write("INP ON")
        expect_error(t, "FUNC RES", -221)
        t.write("INP OFF")
        expect(t, "INP?", "0")
        t.close()

        if soak:
            print(f"Soak: {soak} set/query pairs with an error case every 50")
            sim.start()
            t = Scpi(host)
            bad = 0
            for i in range(soak):
                want = f"{(i % 4000) / 100:.3f}"
                t.write(f"CURR {want}")
                bad += t.query("CURR?") != want
                if i % 50 == 49:
                    t.write("CURR 99")
                    bad += t.errors() != [-222]
            check("every pair matched", bad == 0, f"{bad} mismatches")
            check("*IDN? still answers", bool(t.query("*IDN?")))
            t.close()

        print("No load on the bus")
        sim.stop()
        t = Scpi(host)
        t0 = time.time()
        expect_error(t, "MEAS:VOLT?", -365)
        check("gives up within 2.5 s", time.time() - t0 < 2.5, f"{time.time() - t0:.1f} s")
        expect(t, "*TST?", "1")
        check("*TST? queues no error", t.errors() == [])
        check("*IDN? still answers", bool(t.query("*IDN?")))
        t.close()
    finally:
        sim.stop()
    print(f"\n{sum(results)} of {len(results)} checks passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
