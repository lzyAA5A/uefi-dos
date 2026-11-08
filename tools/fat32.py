#!/usr/bin/env python3
"""FAT32 volume builder for UEFI-DOS tests (dependency-free).

Creates a spec-conforming FAT32 volume, optionally inside an MBR partition
(type 0x0C, starting at LBA 2048), and populates it with 8.3 named
directories and files.  `verify()` re-reads the structures it wrote.

Layout produced (FAT32, one cluster chain per object):
  * sector 0            : MBR with one 0x0C partition (optional)
  * BPB + FSInfo + backup boot sector
  * two FATs
  * cluster 2           : root directory
"""
import struct
import sys
import time

SECTOR = 512
RESVD = 32
NUM_FATS = 2
ROOT_CLUSTER = 2
FSINFO_SECTOR = 1
BACKUP_BOOT_SECTOR = 6
BACKUP_FSINFO_SECTOR = 7

ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20

VOL_LABEL = b'UEFIDOS    '


def chs_bytes(lba, heads=255, spt=63):
    c = lba // (heads * spt)
    rem = lba % (heads * spt)
    h = rem // spt
    s = rem % spt + 1
    return bytes([(s & 0x3F) | ((c >> 8) << 6), h & 0xFF, c & 0xFF])


def write_mbr(f, part_lba, part_sectors):
    f.seek(0)
    f.write(bytes(0x1B8))
    f.write(struct.pack('<I', 0x55464944))     # 'DIFU' in little endian: UEFI-DOS disk id
    f.write(bytes(0x24))
    e = bytearray(16)
    e[0] = 0x00
    e[1:4] = chs_bytes(part_lba)
    e[4] = 0xEF
    e[5:8] = chs_bytes(part_lba + part_sectors - 1)
    struct.pack_into('<I', e, 8, part_lba)
    struct.pack_into('<I', e, 12, part_sectors)
    f.seek(446)
    f.write(e)
    f.seek(510)
    f.write(b'\x55\xAA')


class Fat32Maker:
    def __init__(self, path, size_mb=64, spc=1):
        self.path = path
        self.img_size = size_mb * 1024 * 1024
        self.spc = spc
        self.part_lba = 2048
        self.part_sectors = self.img_size // SECTOR - self.part_lba
        assert self.part_sectors * SECTOR >= 33 * 1024 * 1024, 'need >= 33 MiB for FAT32'
        self.fatsz = None
        self.total_clusters = None
        self.clusters = [None, None, None]   # index = cluster; 0..2 reserved (2 = root dir)
        self.chains = []          # (first_cluster, count)
        self.dirs = {'': []}      # path -> [[name11, attr, cluster, size], ...]
        self.root_clusters = [ROOT_CLUSTER]
        self.dir_chains = {}

    # ---------- allocation ----------
    def alloc(self, n):
        # cluster 2 is the root directory; allocations start at cluster 3
        start = len(self.clusters)
        for _ in range(n):
            self.clusters.append(bytearray(SECTOR * self.spc))
        self.chains.append((start, n))
        return start

    def _find_child_cluster(self, path):
        """first cluster of an existing directory (valid after _layout_dirs)"""
        return self.dir_chains[path][0]

    @staticmethod
    def name83(name):
        if name in ('.', '..'):
            return name.ljust(11).encode('ascii')
        base, ext = name, ''
        if '.' in name:
            base, ext = name.rsplit('.', 1)
        return (base.upper()[:8].ljust(8) + ext.upper()[:3].ljust(3)).encode('ascii')

    # ---------- content ----------
    def add_dir(self, path):
        parts = path.split('/')
        cur = ''
        for p in parts:
            full = (cur + '/' + p) if cur else p
            if full not in self.dirs:
                self.dirs[cur].append([self.name83(p), ATTR_DIRECTORY, 0, 0])
                self.dirs[full] = []
            cur = full

    def add_file(self, path, data, attr=ATTR_ARCHIVE):
        if '/' in path:
            parent, name = path.rsplit('/', 1)
        else:
            parent, name = '', path
        if parent and parent not in self.dirs:
            self.add_dir(parent)
        nc = max(1, (len(data) + SECTOR * self.spc - 1) // (SECTOR * self.spc))
        cl = self.alloc(nc)
        off = 0
        for k in range(nc):
            take = min(SECTOR * self.spc, len(data) - off)
            if take > 0:
                self.clusters[cl + k][:take] = data[off:off + take]
            off += take
        self.dirs[parent].append([self.name83(name), attr, cl if len(data) else 0, len(data)])

    # ---------- sizing ----------
    def _compute(self):
        total = self.part_sectors
        f = 1024
        for _ in range(32):
            c = (total - RESVD - NUM_FATS * f) // self.spc
            nf = (c + 2) * 4
            fs = (nf + SECTOR - 1) // SECTOR
            if fs == f:
                break
            f = fs
        self.fatsz = f
        self.total_clusters = (total - RESVD - NUM_FATS * f) // self.spc

    # ---------- serialization ----------
    @staticmethod
    def _entry(name11, attr, cluster, size):
        e = bytearray(32)
        e[0:11] = name11
        e[11] = attr
        lt = time.localtime()
        dos_date = ((lt.tm_year - 1980) << 9) | (lt.tm_mon << 5) | lt.tm_mday
        dos_time = (lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec // 2)
        struct.pack_into('<H', e, 14, dos_time)   # creation time
        struct.pack_into('<H', e, 16, dos_date)   # creation date
        struct.pack_into('<H', e, 18, dos_date)   # last access date
        struct.pack_into('<H', e, 22, dos_time)   # last write time
        struct.pack_into('<H', e, 24, dos_date)   # last write date
        struct.pack_into('<H', e, 20, (cluster >> 16) & 0xFFFF)
        struct.pack_into('<H', e, 26, cluster & 0xFFFF)
        struct.pack_into('<I', e, 28, size)
        return bytes(e)

    def _dir_blob(self, path):
        blob = bytearray()
        if path == '':
            e = bytearray(32)
            e[0:11] = VOL_LABEL
            e[11] = ATTR_VOLUME_ID
            blob += e
        else:
            cl = self._find_child_cluster(path)
            parent, _ = path.rsplit('/', 1) if '/' in path else ('', path)
            pcl = self._find_child_cluster(parent) if parent else 0
            blob += self._entry(self.name83('.'), ATTR_DIRECTORY, cl, 0)
            blob += self._entry(self.name83('..'), ATTR_DIRECTORY, pcl, 0)
        for name11, attr, cl, size in self.dirs[path]:
            blob += self._entry(name11, attr, cl, size)
        cap = SECTOR * self.spc * len(self.dir_chains[path])
        assert len(blob) <= cap, 'directory %r overflow: %d > %d' % (path, len(blob), cap)
        return bytes(blob)

    def _layout_dirs(self):
        """Assign cluster chains to every directory (grow as entries require)."""
        CL = SECTOR * self.spc
        nroot = max(1, (32 + 32 * len(self.dirs['']) + CL - 1) // CL)
        self.root_clusters = [ROOT_CLUSTER]
        if nroot > 1:
            start = len(self.clusters)
            for _ in range(nroot - 1):
                self.clusters.append(bytearray(CL))
            self.root_clusters += list(range(start, start + nroot - 1))
        self.dir_chains = {'': self.root_clusters}
        for path in [p for p in self.dirs if p != '']:
            need = max(1, (64 + 32 * len(self.dirs[path]) + CL - 1) // CL)
            cl = self.alloc(need)
            self.dir_chains[path] = list(range(cl, cl + need))
            parent, name = path.rsplit('/', 1) if '/' in path else ('', path)
            for e in self.dirs[parent]:
                if e[0] == self.name83(name) and (e[1] & ATTR_DIRECTORY):
                    e[2] = cl
                    break

    def write(self):
        self._compute()
        self._layout_dirs()
        # build FAT
        fat = bytearray(self.fatsz * SECTOR)
        struct.pack_into('<I', fat, 0, 0x0FFFFFF8)
        struct.pack_into('<I', fat, 4, 0x0FFFFFFF)
        for start, n in self.chains:
            for k in range(n):
                nxt = start + k + 1 if k + 1 < n else 0x0FFFFFFF
                struct.pack_into('<I', fat, 4 * (start + k), nxt)
        # root directory chain: 2 -> <continuation clusters> -> EOF
        rc = self.root_clusters
        for i in range(len(rc) - 1):
            struct.pack_into('<I', fat, 4 * rc[i], rc[i + 1])
        struct.pack_into('<I', fat, 4 * rc[-1], 0x0FFFFFFF)
        used = len(self.clusters) - 2
        # FSInfo
        fsinfo = bytearray(SECTOR)
        struct.pack_into('<I', fsinfo, 0, 0x41615252)
        struct.pack_into('<I', fsinfo, 484, 0x61417272)
        struct.pack_into('<I', fsinfo, 488, self.total_clusters - used)
        struct.pack_into('<I', fsinfo, 492, 0xFFFFFFFF)
        struct.pack_into('<I', fsinfo, 508, 0xAA550000)
        # boot sector
        boot = bytearray(SECTOR)
        boot[0:3] = b'\xEB\x58\x90'
        boot[3:11] = b'MSWIN4.1'
        struct.pack_into('<H', boot, 11, SECTOR)
        boot[13] = self.spc
        struct.pack_into('<H', boot, 14, RESVD)
        boot[16] = NUM_FATS
        struct.pack_into('<H', boot, 17, 0)
        struct.pack_into('<H', boot, 19, 0)
        boot[21] = 0xF8
        struct.pack_into('<H', boot, 22, 0)
        struct.pack_into('<H', boot, 24, 63)
        struct.pack_into('<H', boot, 26, 255)
        struct.pack_into('<I', boot, 28, self.part_lba)
        struct.pack_into('<I', boot, 32, self.part_sectors)
        struct.pack_into('<I', boot, 36, self.fatsz)
        struct.pack_into('<H', boot, 40, 0)     # ExtFlags
        struct.pack_into('<H', boot, 42, 0)     # FSVer
        struct.pack_into('<I', boot, 44, ROOT_CLUSTER)
        struct.pack_into('<H', boot, 48, FSINFO_SECTOR)
        struct.pack_into('<H', boot, 50, BACKUP_BOOT_SECTOR)
        boot[64] = 0x80
        boot[66] = 0x29
        struct.pack_into('<I', boot, 67, 0x55464944)
        boot[71:82] = VOL_LABEL
        boot[82:90] = b'FAT32   '
        boot[510:512] = b'\x55\xAA'
        # write everything
        data_lba = self.part_lba + RESVD + NUM_FATS * self.fatsz
        with open(self.path, 'wb') as f:
            f.truncate(self.img_size)
            write_mbr(f, self.part_lba, self.part_sectors)
            f.seek(self.part_lba * SECTOR)
            f.write(boot)
            f.write(fsinfo)
            f.seek(self.part_lba * SECTOR + RESVD * SECTOR)
            for _ in range(NUM_FATS):
                f.write(fat)
            # file data first, then directories (directories own their chains
            # but share the chain bookkeeping array).
            for start, n in self.chains:
                for k in range(n):
                    f.seek((data_lba + ((start - 2) + k) * self.spc) * SECTOR)
                    f.write(self.clusters[start + k])
            CL = SECTOR * self.spc
            for path in list(self.dirs):
                blob = bytearray(self._dir_blob(path))
                cls = self.dir_chains[path]
                blob += bytes(CL * len(cls) - len(blob))     # zero fill
                for i, c in enumerate(cls):
                    f.seek((data_lba + (c - 2) * self.spc) * SECTOR)
                    f.write(blob[i * CL:(i + 1) * CL])
            f.seek((self.part_lba + BACKUP_BOOT_SECTOR) * SECTOR)
            f.write(boot)
            f.seek((self.part_lba + BACKUP_FSINFO_SECTOR) * SECTOR)
            f.write(fsinfo)

    # ---------- verification ----------
    def read_root_names(self):
        data_lba = self.part_lba + RESVD + NUM_FATS * self.fatsz
        CL = SECTOR * self.spc
        data = bytearray()
        with open(self.path, 'rb') as f:
            for c in self.root_clusters:
                f.seek((data_lba + (c - 2) * self.spc) * SECTOR)
                data += f.read(CL)
        out = []
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] in (0x00, 0xE5):
                if e[0] == 0:
                    break
                continue
            attr = e[11]
            if attr & ATTR_VOLUME_ID:
                out.append(('VOL', e[0:11].decode('latin1').rstrip()))
            else:
                nm = e[0:11].decode('latin1')
                out.append((nm[:8].rstrip(), attr, e[26] | (e[20] << 8), struct.unpack('<I', e[28:32])[0]))
        return out


def main():
    img = sys.argv[1] if len(sys.argv) > 1 else 'fat32.img'
    mb = int(sys.argv[2]) if len(sys.argv) > 2 else 64
    m = Fat32Maker(img, size_mb=mb)
    m.add_dir('EFI')
    m.add_dir('EFI/BOOT')
    m.add_file('EFI/BOOT/BOOTX64.EFI', b'TEST')
    m.write()
    print('wrote', img, 'clusters:', m.total_clusters, 'fatsz:', m.fatsz, 'root:', m.read_root_names())


if __name__ == '__main__':
    main()
