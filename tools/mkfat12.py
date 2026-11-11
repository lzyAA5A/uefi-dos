"""mkfat12.py - build a 1.44 MB FAT12 floppy image from a directory of files.

Usage:
    python tools/mkfat12.py OUT.img FILE [FILE ...]
    python tools/mkfat12.py OUT.img --dir SOMEDIR

The image is a plain 3.5" 1.44 MB superfloppy (no partition table):
    bytes/sector 512, sectors/cluster 1, 1 reserved, 2 FATs (9 sectors each),
    224 root entries, 2880 sectors total, media 0xF0.

Files land in the root directory with upper-case 8.3 names. Everything is
written contiguously, which is exactly what a freshly formatted DOS disk
looks like -- handy for driving vintage installers that expect a floppy.
"""

import os
import struct
import sys

SEC = 512
TOTAL_SEC = 2880
SPC = 1
RESV = 1
NFATS = 2
ROOT_ENT = 224
FAT_SEC = 9
MEDIA = 0xF0
SPT = 18
HEADS = 2

ROOT_LBA = RESV + NFATS * FAT_SEC          # 19
ROOT_SEC = (ROOT_ENT * 32 + SEC - 1) // SEC  # 14
DATA_LBA = ROOT_LBA + ROOT_SEC             # 33
MAX_CLUSTERS = (TOTAL_SEC - DATA_LBA) // SPC


def boot_sector(label=b'UEFIDOSFLPY'):
    bs = bytearray(SEC)
    bs[0:3] = b'\xEB\x3C\x90'
    bs[3:11] = b'MSDOS5.0'
    struct.pack_into('<H', bs, 11, SEC)
    bs[13] = SPC
    struct.pack_into('<H', bs, 14, RESV)
    bs[16] = NFATS
    struct.pack_into('<H', bs, 17, ROOT_ENT)
    struct.pack_into('<H', bs, 19, TOTAL_SEC)
    bs[21] = MEDIA
    struct.pack_into('<H', bs, 22, FAT_SEC)
    struct.pack_into('<H', bs, 24, SPT)
    struct.pack_into('<H', bs, 26, HEADS)
    struct.pack_into('<I', bs, 28, 0)
    struct.pack_into('<I', bs, 32, 0)
    struct.pack_into('<H', bs, 36, 0)
    bs[38] = 0x29                      # boot signature present
    struct.pack_into('<I', bs, 39, 0x55454449)
    bs[43:54] = label[:11].ljust(11, b' ')  # volume label
    bs[54:62] = b'FAT12   '
    bs[510] = 0x55
    bs[511] = 0xAA
    return bytes(bs)


def name83(path):
    base, ext = os.path.splitext(os.path.basename(path))
    base = base.upper()[:8]
    ext = ext.lstrip('.').upper()[:3]
    return base.ljust(8) + ext.ljust(3)


def build(out_path, files):
    img = bytearray(TOTAL_SEC * SEC)
    img[0:SEC] = boot_sector()

    fat = bytearray(FAT_SEC * SEC)
    # entry 0: media descriptor + 0xFFF, entry 1: EOC
    fat[0] = MEDIA
    fat[1] = 0xFF
    fat[2] = 0xFF

    def set_fat(n, val):
        off = n + n // 2
        v = struct.unpack_from('<H', fat, off)[0]
        if n & 1:
            v = (v & 0x000F) | ((val & 0xFFF) << 4)
        else:
            v = (v & 0xF000) | (val & 0xFFF)
        struct.pack_into('<H', fat, off, v)

    cluster = 2
    root = bytearray(ROOT_SEC * SEC)
    slot = 0
    used = 0
    for path in files:
        data = open(path, 'rb').read()
        ncl = max(1, (len(data) + SEC * SPC - 1) // (SEC * SPC))
        if len(data) == 0:
            ncl = 0
        if cluster + ncl > MAX_CLUSTERS + 2 or slot >= ROOT_ENT:
            raise SystemExit('image full at ' + path)
        for i in range(ncl):
            set_fat(cluster + i, cluster + i + 1 if i + 1 < ncl else 0xFFF)
            lba = DATA_LBA + (cluster - 2 + i) * SPC
            chunk = data[i * SEC * SPC:(i + 1) * SEC * SPC]
            img[lba * SEC:lba * SEC + len(chunk)] = chunk
        e = bytearray(32)
        e[0:11] = name83(path).encode('latin-1')
        e[11] = 0x20
        struct.pack_into('<H', e, 26, cluster if ncl else 0)
        struct.pack_into('<I', e, 28, len(data))
        # fixed date/time so images are reproducible
        struct.pack_into('<H', e, 22, 0x6000)   # 12:00
        struct.pack_into('<H', e, 24, 0x5A21)   # 2026-01-01
        root[slot * 32:(slot + 1) * 32] = e
        slot += 1
        used += ncl
        cluster += ncl

    for i in range(NFATS):
        lba = RESV + i * FAT_SEC
        img[lba * SEC:(lba + FAT_SEC) * SEC] = fat
    img[ROOT_LBA * SEC:(ROOT_LBA + ROOT_SEC) * SEC] = root

    with open(out_path, 'wb') as f:
        f.write(img)
    print('wrote %s: %d bytes, %d files, %d/%d clusters used'
          % (out_path, len(img), slot, used, MAX_CLUSTERS))


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    out = argv[1]
    args = argv[2:]
    files = []
    if args and args[0] == '--dir':
        d = args[1]
        for n in sorted(os.listdir(d)):
            p = os.path.join(d, n)
            if os.path.isfile(p):
                files.append(p)
    else:
        files = args
    build(out, files)


if __name__ == '__main__':
    main(sys.argv)
