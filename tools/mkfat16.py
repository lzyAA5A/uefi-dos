#!/usr/bin/env python3
"""mkfat16.py -- create a FAT16 disk image (MBR + one FAT16 partition).

Writes only the boot sector, both FATs, the root directory and the FSInfo-like
metadata; the data area stays a sparse hole so multi-gigabyte test volumes cost
almost no disk space on NTFS.

usage: python tools/mkfat16.py OUT.img [--size-mb 32] [--spc 4] [--label NAME]
"""
import struct, sys, os, time

SEC = 512


def build(path, size_mb, spc, label, part_lba=2048):
    total_sec = size_mb * 1024 * 1024 // SEC
    part_sec = total_sec - part_lba
    reserved = 32
    nfats = 2
    root_entries = 512

    # FAT size: clusters = (part_sec - reserved - nfats*fatsz - root_sec)/spc
    # solve iteratively for fatsz (approximate then adjust)
    fatsz = 1
    for _ in range(20):
        data_sec = part_sec - reserved - nfats * fatsz - (root_entries * 32 + SEC - 1) // SEC
        clusters = data_sec // spc
        need = ((clusters + 2) * 2 + SEC - 1) // SEC
        if need == fatsz:
            break
        fatsz = need
    data_sec = part_sec - reserved - nfats * fatsz - (root_entries * 32 + SEC - 1) // SEC
    clusters = data_sec // spc
    if clusters < 4085:
        raise SystemExit('too few clusters for FAT16 (%d); raise --size-mb or lower --spc' % clusters)
    if clusters > 65524:
        raise SystemExit('too many clusters for FAT16 (%d); lower --size-mb or raise --spc' % clusters)

    bs = bytearray(SEC)
    bs[0:3] = b'\xEB\x3C\x90'
    bs[3:11] = b'MSDOS5.0'
    struct.pack_into('<HBHBHHBHHHII', bs, 11, SEC, spc, reserved, nfats,
                     root_entries, part_sec if part_sec < 65536 else 0, 0xF8,
                     fatsz, 16, 16, part_lba, part_sec if part_sec >= 65536 else 0)
    struct.pack_into('<BBBI', bs, 36, 0x80, 0, 0x29, 0x55464944)
    bs[43:54] = label.upper().ljust(11)[:11].encode('ascii')
    bs[54:62] = b'FAT16   '
    bs[510:512] = b'\x55\xAA'

    fat = bytearray(fatsz * SEC)
    struct.pack_into('<HH', fat, 0, 0xFFF8, 0xFFFF)

    with open(path, 'wb') as f:
        # MBR
        mbr = bytearray(SEC)
        e = 446
        mbr[e + 0] = 0x80
        mbr[e + 4] = 0x0E                      # FAT16 LBA
        struct.pack_into('<II', mbr, e + 8, part_lba, part_sec)
        mbr[510:512] = b'\x55\xAA'
        f.write(mbr)
        f.seek(part_lba * SEC)
        f.write(bs)
        for i in range(nfats):
            f.seek((part_lba + reserved + i * fatsz) * SEC)
            f.write(fat)
        # root directory (zeroed entries) already sparse; extend the file
        f.truncate(total_sec * SEC)
    return dict(part_sec=part_sec, fatsz=fatsz, clusters=clusters, spc=spc,
                data_lba=part_lba + reserved + nfats * fatsz +
                         (root_entries * 32 + SEC - 1) // SEC)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    size_mb, spc, label = 32, 4, 'FAT16TEST'
    args = sys.argv[2:]
    i = 0
    while i < len(args):
        if args[i] == '--size-mb':
            size_mb = int(args[i + 1]); i += 2
        elif args[i] == '--spc':
            spc = int(args[i + 1]); i += 2
        elif args[i] == '--label':
            label = args[i + 1]; i += 2
        else:
            raise SystemExit('unknown option ' + args[i])
    t = time.time()
    info = build(out, size_mb, spc, label)
    print('%s: %d MB, spc=%d, clusters=%d, FAT %d sectors, data at LBA %d (%.2fs)'
          % (out, size_mb, info['spc'], info['clusters'], info['fatsz'],
             info['data_lba'], time.time() - t))
    return 0


if __name__ == '__main__':
    sys.exit(main())
