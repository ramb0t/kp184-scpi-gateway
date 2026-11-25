#!/usr/bin/env python3
"""Send SCPI commands to the gateway and print the replies.

Usage: scpi.py HOST COMMAND [COMMAND ...]
       scpi.py eth485 '*IDN?' 'MEAS:VOLT?' 'MEAS:CURR?'

A command containing '?' waits for a reply. After the last command the error
queue is read, so a command that failed says why.

The gateway's fail-safe applies to this tool too: an input switched on here
goes off again when the tool exits, unless SYST:FAIL OFF is sent first:
       scpi.py eth485 'SYST:FAIL OFF' 'INP ON'
"""
import sys

from gateway import Scpi


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    gateway = Scpi(sys.argv[1])
    failed = False
    for cmd in sys.argv[2:]:
        if "?" in cmd:
            reply = gateway.query(cmd)
            failed |= reply is None
            print(f"{cmd} -> {reply if reply is not None else '(no reply)'}")
        else:
            gateway.write(cmd)
            print(cmd)
    while (error := gateway.query("SYST:ERR?")) and not error.startswith("0,"):
        failed = True
        print(f"error: {error}")
    gateway.close(settle=0)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
