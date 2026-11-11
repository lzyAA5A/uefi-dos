#!/usr/bin/env python3
"""Minimal read-only FAT12/16/32 extractor for raw disk images.

Used to pull tools/games out of the CardPuter86 test images into the
UEFI-DOS image.  Handles an MBR (partition table) or a superfloppy.

Usage:
  python fat_extract.py list    IMG [--long]
  python fat_extract.py extract IMG DESTDIR [--name 8.3NAME] [--max-mb N]
  python fat_extract.py get     IMG 8.3NAME OUTPATH
"""

import os
import struct
import sys

SEC = 512
ATTR_DIR = 0x10
ATTR_VOL = 0x08
ATTR_LFN = 0x0F


class FatImage:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        self.part_lba = self._find_part()
        self._parse_bpb()

    # ---------------- layout ----------------
    def _find_part(self):
        d = self.data
        if len(d) < 512 or d[510:512] != b'\x55\xAA':
            return 0
        # try partition entries; fall back to superfloppy
        best = 0
        for i in range(4):
            off = 446 + i * 16
            if off + 16 > len(d):
                break
            status, ptype = d[off], d[off + 4]
            lba = struct.unpack_from('<I', d, off + 8)[0]
            cnt = struct.unpack_from('<I', d, off + 12)[0]
            if status not in (0x00, 0x80):
                continue
            if ptype == 0 or lba == 0 or cnt == 0:
                continue
            if lba * SEC + SEC > len(d):
                continue
            if d[lba * SEC + 510: lba * SEC + 512] != b'\x55\xAA':
                continue
            if d[lba * SEC + 0x36] == 0x29 or d[lba * SEC + 0x26] == 0x29 or \
               d[lba * SEC:lba * SEC + 3] in (b'FAT',):
                best = lba
                break
        return best

    def _parse_bpb(self):
        b = self.data[self.part_lba * SEC:]
        self.bytes_sector = struct.unpack_from('<H', b, 11)[0]
        self.spc = b[13]
        self.reserved = struct.unpack_from('<H', b, 14)[0]
        self.nfats = b[16]
        self.root_entries = struct.unpack_from('<H', b, 17)[0]
        total16 = struct.unpack_from('<H', b, 19)[0]
        fat16 = struct.unpack_from('<H', b, 22)[0]
        total32 = struct.unpack_from('<I', b, 32)[0]
        self.fat_sectors = fat16 if fat16 else struct.unpack_from('<I', b, 36)[0]
        self.total_sectors = total16 if total16 else total32
        root_secs = (self.root_entries * 32 + SEC - 1) // SEC
        self.fat_lba = self.part_lba + self.reserved
        self.root_lba = self.fat_lba + self.nfats * self.fat_sectors
        self.data_lba = self.root_lba + root_secs + (struct.unpack_from('<I', b, 44)[0] if self.root_entries == 0 else 0)
        # include hidden sectors for the data start (FAT32 root dir follows boot+FSInfo+backup)
        self.root_cluster = struct.unpack_from('<I', b, 44)[0] if self.root_entries == 0 else 0
        self.cluster_count = (self.total_sectors - (self.reserved + self.nfats * self.fat_sectors + root_secs)) // self.spc
        if self.root_entries == 0 and fat16 == 0:
            self.fs_type = 32
        else:
            self.fs_type = 12 if self.cluster_count < 4085 else 16
        self.root_secs = root_secs

    def clamp(self, c):
        if self.fs_type == 32:
            return c & 0x0FFFFFFF
        if self.fs_type == 16:
            return c & 0xFFFF
        return c & 0xFFF

    def fat_get(self, cluster):
        if self.fs_type == 32:
            off = self.fat_lba * SEC + cluster * 4
            return self.clamp(struct.unpack_from('<I', self.data, off)[0])
        if self.fs_type == 16:
            off = self.fat_lba * SEC + cluster * 2
            return struct.unpack_from('<H', self.data, off)[0]
        off = self.fat_lba * SEC + cluster + cluster // 2
        v = struct.unpack_from('<H', self.data, off)[0]
        return (v >> 4) if (cluster & 1) else (v & 0x0FFF)

    def eoc(self):
        """First end-of-chain marker for this FAT width."""
        if self.fs_type == 32:
            return 0x0FFFFFF8
        if self.fs_type == 16:
            return 0xFFF8
        return 0xFF8

    def chain(self, start, max_clusters=4096):
        # Cluster numbering starts at 2, so the test must be `2 <= c`:
        # the first file on a freshly formatted volume lives at cluster 2.
        c = self.clamp(start)
        out = []
        while 2 <= c < self.eoc() and len(out) < max_clusters:
            out.append(c)
            nc = self.fat_get(c)
            if nc == c:
                break
            c = self.clamp(nc)
        return out

    def cluster_off(self, c):
        return self.data_lba * SEC + (c - 2) * self.spc * SEC

    def read_chain(self, start, size):
        out = bytearray()
        for c in self.chain(start):
            out += self.data[self.cluster_off(c): self.cluster_off(c) + self.spc * SEC]
            if len(out) >= size:
                break
        return bytes(out[:size])

    # ---------------- directory walk ----------------
    def name83(self, raw):
        base = raw[0:8].decode('latin-1').rstrip()
        ext = raw[8:11].decode('latin-1').rstrip()
        return (base + ('.' + ext if ext else '')).upper()

    def dir_entries(self, cluster):
        """Yield (name, attr, size, first_cluster, raw_name_bytes) for one dir."""
        if cluster == 0:
            base = self.root_lba * SEC
            size = self.root_secs * SEC
            blob = self.data[base:base + size]
        else:
            blob = b''.join(self.data[self.cluster_off(c): self.cluster_off(c) + self.spc * SEC]
                            for c in self.chain(cluster))
        for i in range(0, len(blob) - 31, 32):
            e = blob[i:i + 32]
            if e[0] == 0x00:
                break
            if e[0] == 0xE5:
                continue
            if e[11] == ATTR_LFN:
                continue
            if e[11] & ATTR_VOL:
                continue
            first = struct.unpack_from('<H', e, 26)[0] | (struct.unpack_from('<H', e, 20)[0] << 16)
            size = struct.unpack_from('<I', e, 28)[0]
            yield (self.name83(e[0:11]), e[11], size, first)

    def list(self, cluster=0, prefix=''):
        out = []
        for name, attr, size, first in self.dir_entries(cluster):
            path = prefix + name
            if attr & ATTR_DIR and name not in ('.', '..'):
                sub = self.list(first, path + '/')
                out.append((path, attr, 0, 0))
                out += sub
            else:
                out.append((path, attr, size, first))
        return out

    def find(self, name83, cluster=0, prefix=''):
        want = name83.upper()
        for name, attr, size, first in self.dir_entries(cluster):
            path = prefix + name
            if name == want:
                return (path, attr, size, first)
            if attr & ATTR_DIR and name not in ('.', '..'):
                r = self.find(name83, first, path + '/')
                if r:
                    return r
        return None

    def get(self, name83):
        r = self.find(name83)
        if not r:
            return None
        path, attr, size, first = r
        if attr & ATTR_DIR:
            return None
        return self.read_chain(first, size)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd = sys.argv[1]
    img = FatImage(sys.argv[2])
    if cmd == 'list':
        print('type=FAT%d spc=%d clusters=%d root_entries=%d part_lba=%d'
              % (img.fs_type, img.spc, img.cluster_count, img.root_entries, img.part_lba))
        for path, attr, size, first in img.list():
            if '--long' in sys.argv:
                print('  %-30s %s %8d cluster=%d' % (path, 'D' if attr & ATTR_DIR else 'F', size, first))
            else:
                print('  %-16s %8d' % (path, size))
    elif cmd == 'get':
        if len(sys.argv) < 5:
            raise SystemExit('usage: get IMG 8.3NAME OUTPATH')
        data = img.get(sys.argv[3])
        if data is None:
            raise SystemExit('not found: ' + sys.argv[3])
        with open(sys.argv[4], 'wb') as f:
            f.write(data)
        print('wrote', sys.argv[4], len(data), 'bytes')
    elif cmd == 'extract':
        dest = sys.argv[3]
        os.makedirs(dest, exist_ok=True)
        total = 0
        for path, attr, size, first in img.list():
            if attr & ATTR_DIR:
                continue
            data = img.read_chain(first, size)
            out = os.path.join(dest, path.replace('/', '_'))
            with open(out, 'wb') as f:
                f.write(data)
            total += 1
        print('extracted', total, 'files to', dest)
    else:
        raise SystemExit('unknown command ' + cmd)


if __name__ == '__main__':
    main()
