#!/usr/bin/env python3
"""Burst screenshots from a running QEMU monitor (TCP).

Some guests only keep a frame on screen for a fraction of a second (Windows 1.x
paints its splash and then leaves graphics mode), so a single timed screendump
misses it.  This keeps the connection open and fires `screendump` repeatedly.

  python qburst.py <monitor-port> <outdir> <count> <interval-ms> [prefix]
"""
import os
import socket
import sys
import time


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 55555
    outdir = sys.argv[2] if len(sys.argv) > 2 else 'shots'
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    interval = int(sys.argv[4]) if len(sys.argv) > 4 else 400
    prefix = sys.argv[5] if len(sys.argv) > 5 else 'burst'
    if not os.path.isdir(outdir):
        os.makedirs(outdir)
    s = socket.create_connection(('127.0.0.1', port), timeout=10)
    s.settimeout(2.0)
    try:
        s.recv(65536)                       # banner
    except socket.timeout:
        pass
    time.sleep(0.2)
    made = []
    for i in range(count):
        path = os.path.join(outdir, '%s%03d.ppm' % (prefix, i))
        p = path.replace('\\', '/')
        s.sendall(('screendump ' + p + '\n').encode('ascii'))
        time.sleep(0.15)                    # let the file land
        try:
            while True:
                s.recv(65536)               # drain the (echoed) response
        except socket.timeout:
            pass
        if os.path.exists(path):
            made.append((i, os.path.getsize(path)))
        time.sleep(max(0, interval - 150) / 1000.0)
    s.close()
    for i, size in made:
        print('%s%03d.ppm  %d bytes' % (prefix, i, size))


if __name__ == '__main__':
    main()
