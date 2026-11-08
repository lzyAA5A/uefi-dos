#!/usr/bin/env python3
"""Screenshot helper: drive a running QEMU monitor (TCP) and take 'screendump'.

  python screenshot.py <monitor-port> <out.ppm>
"""
import os
import socket
import sys
import time


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 55555
    out = sys.argv[2] if len(sys.argv) > 2 else 'screen.ppm'
    s = socket.create_connection(('127.0.0.1', port), timeout=10)
    s.settimeout(2.0)
    try:
        s.recv(4096)  # banner
    except socket.timeout:
        pass
    time.sleep(0.2)
    s.sendall(('screendump ' + out.replace('\\', '/') + '\n').encode('ascii'))
    buf = b''
    t0 = time.time()
    while time.time() - t0 < 5.0:
        try:
            d = s.recv(65536)
        except socket.timeout:
            break
        if not d:
            break
        buf += d
        if b'(qemu)' in buf:
            break
    s.close()
    print(buf.decode('ascii', 'replace').strip())
    print('wrote:', out, os.path.getsize(out) if os.path.exists(out) else 'MISSING')


if __name__ == '__main__':
    main()
