#!/usr/bin/env python3
"""Send SCPI commands to the gateway and print the replies.

Usage: scpi.py HOST COMMAND [COMMAND ...]
       scpi.py eth485 '*IDN?' 'MEAS:VOLT?' 'MEAS:CURR?'

A command containing '?' waits for a reply. After the last command the error
queue is read, so a command that failed says why.
"""
import socket
import sys
import time

PORT = 5025


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    s = socket.create_connection((sys.argv[1], PORT), timeout=5)
    s.settimeout(2.0)

    def query(cmd):
        s.sendall(cmd.encode() + b"\n")
        buf = b""
        try:
            while not buf.endswith(b"\n"):
                d = s.recv(256)
                if not d:
                    break
                buf += d
        except socket.timeout:
            return None
        return buf.decode().strip()

    for cmd in sys.argv[2:]:
        if "?" in cmd:
            reply = query(cmd)
            print(f"{cmd} -> {reply if reply is not None else '(no reply)'}")
        else:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(0.1)
            print(f"{cmd}")
    failed = False
    for _ in range(10):
        err = query("SYST:ERR?")
        if err is None or err.startswith("0,"):
            break
        failed = True
        print(f"error: {err}")
    s.close()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
