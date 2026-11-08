"""Send commands to a running QEMU monitor and print the responses.

Usage: python tools/qmon.py "cmd1" "cmd2" ...   (port via QMON_PORT, default 55556)
"""
import os
import socket
import sys
import time


def main():
    port = int(os.environ.get("QMON_PORT", "55556"))
    cmds = sys.argv[1:] or ["info status"]
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.settimeout(2.0)

    def drain(tmo=1.0):
        buf = b""
        end = time.time() + tmo
        while time.time() < end:
            try:
                chunk = s.recv(65536)
                if not chunk:
                    break
                buf += chunk
            except socket.timeout:
                break
        return buf.decode("utf-8", "replace")

    drain(1.0)
    for c in cmds:
        s.sendall((c + "\n").encode())
        out = drain(1.5)
        print("== %s ==" % c)
        print(out.strip())
    s.close()


if __name__ == "__main__":
    main()
