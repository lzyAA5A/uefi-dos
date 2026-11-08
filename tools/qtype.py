"""Type text into the running QEMU guest via the monitor (sendkey), then report
how the serial log grows over time.

Usage: python tools/qtype.py --log build/hang-serial.log --text "dir a:"
"""
import argparse
import os
import socket
import time

KEYMAP = {
    ":": "shift-semicolon", "\\": "backslash", " ": "spc", "\r": "ret",
    "/": "slash", "-": "minus", ".": "dot", "_": "shift-minus",
    "=": "equal", ",": "comma", ";": "semicolon",
}


def keys_for(ch):
    if ch in KEYMAP:
        return KEYMAP[ch]
    if ch.isdigit():
        return ch
    if ch.isalpha():
        return ch if ch.islower() else "shift-" + ch.lower()
    raise ValueError(ch)


class Mon:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.s.settimeout(1.0)
        self.drain(0.5)

    def drain(self, tmo=0.5):
        buf = b""
        end = time.time() + tmo
        while time.time() < end:
            try:
                c = self.s.recv(65536)
                if not c:
                    break
                buf += c
            except socket.timeout:
                break
        return buf

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        return self.drain(0.4).decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default="build/hang-serial.log")
    ap.add_argument("--port", type=int, default=int(os.environ.get("QMON_PORT", "55556")))
    ap.add_argument("--text", required=True)
    ap.add_argument("--wait", type=float, default=20.0)
    a = ap.parse_args()

    start_size = os.path.getsize(a.log) if os.path.exists(a.log) else 0
    m = Mon(a.port)
    for ch in a.text:
        m.cmd("sendkey " + keys_for(ch))
        time.sleep(0.06)
    print("typed %r" % a.text)

    t0 = time.time()
    last = start_size
    while time.time() - t0 < a.wait:
        time.sleep(1.0)
        size = os.path.getsize(a.log) if os.path.exists(a.log) else 0
        if size != last:
            print("t=%4.1fs  log %d -> %d bytes" % (time.time() - t0, last, size))
            last = size
    if os.path.exists(a.log):
        with open(a.log, "r", errors="replace") as f:
            data = f.read()[start_size:]
        print("---- new serial output ----")
        print(data)


if __name__ == "__main__":
    main()
