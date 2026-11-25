"""What the gateway's test scripts share: an SCPI connection and a tally of checks."""
import socket
import time

SCPI_PORT, RAW_PORT = 5025, 4001


class Scpi:
    """One client on the gateway's SCPI port."""

    def __init__(self, host, timeout=2.0):
        self.host, self.timeout = host, timeout
        self.connect()

    def connect(self):
        self.s = socket.create_connection((self.host, SCPI_PORT), timeout=5)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s.settimeout(self.timeout)

    def connect_when_free(self, tries=30, pause=0.2):
        """Connect once the port will have a new client; False if it never would.

        The gateway accepts a second client and resets it at once, so a
        connection proves itself only by answering.
        """
        for _ in range(tries):
            try:
                self.connect()
                if self.query("*OPC?") == "1":
                    return True
                self.close(settle=0)
            except OSError:
                pass
            time.sleep(pause)
        return False

    def close(self, settle=0.3):
        """Close, and give the gateway a moment to notice before the next client."""
        self.s.close()
        time.sleep(settle)

    def write(self, cmd, end="\n"):
        self.s.sendall((cmd + end).encode())

    def query(self, cmd, end="\n"):
        """The reply without its newline, or None when nothing came back."""
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
        return buf.decode().rstrip("\r\n")

    def number(self, cmd):
        try:
            return float(self.query(cmd))
        except (TypeError, ValueError):
            return None

    def errors(self):
        """Empty the error queue; returns its error numbers."""
        out = []
        for _ in range(12):
            r = self.query("SYST:ERR?")
            if r is None or r.startswith("0,"):
                break
            out.append(int(r.split(",")[0]))
        return out


def raw_exchange(sock, request, reply_len, timeout=2.0):
    """Send a frame of the load's own protocol on a raw-port socket; returns what came back."""
    sock.settimeout(timeout)
    sock.sendall(request)
    reply = b""
    try:
        while len(reply) < reply_len:
            d = sock.recv(64)
            if not d:
                break
            reply += d
    except (socket.timeout, ConnectionError):
        pass
    return reply


class Checks:
    """Prints each check as it is made and keeps the score."""

    def __init__(self):
        self.results = []

    def __call__(self, name, ok, detail=""):
        ok = bool(ok)
        self.results.append(ok)
        print(f"  {'ok  ' if ok else 'FAIL'} {name}{'' if ok else '   ' + str(detail)}", flush=True)
        return ok

    def summary(self):
        """Print the score; returns the process exit code."""
        print(f"\n{sum(self.results)} of {len(self.results)} checks passed")
        return 0 if all(self.results) else 1
