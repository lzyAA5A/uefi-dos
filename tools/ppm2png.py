#!/usr/bin/env python3
"""Convert a QEMU screendump PPM (P6) to PNG using only stdlib (zlib).

  python ppm2png.py in.ppm out.png
"""
import re
import struct
import sys
import zlib


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, 'rb').read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', data)
    if not m:
        raise SystemExit('not a P6 PPM: %s' % src)
    w, h, maxv = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if maxv != 255:
        raise SystemExit('unsupported maxval %d' % maxv)
    off = m.end()
    px = data[off:off + w * h * 3]
    if len(px) != w * h * 3:
        raise SystemExit('truncated pixel data')
    raw = bytearray()
    for y in range(h):
        raw += b'\x00' + px[y * w * 3:(y + 1) * w * 3]

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
           + chunk(b'IEND', b''))
    with open(dst, 'wb') as f:
        f.write(png)
    print('png:', dst, w, 'x', h)


if __name__ == '__main__':
    main()
