"""Make a bootable MS-DOS 5 floppy copy whose AUTOEXEC.BAT does nothing.

Usage: python strip_autoexec.py SRC.img DST.img
Reads the FAT12/16 BPB + root directory, finds AUTOEXEC.BAT, rewrites its
first cluster with a one-line comment and sets the directory size so the
DOS SETUP program no longer auto-starts.
"""
import shutil
import struct
import sys

PAYLOAD = b"rem\r\n\x1a"


def main(argv):
    src, dst = argv[1], argv[2]
    shutil.copyfile(src, dst)
    d = bytearray(open(dst, "rb").read())
    bps = struct.unpack("<H", d[0x0B:0x0D])[0]
    spc = d[0x0D]
    resv = struct.unpack("<H", d[0x0E:0x10])[0]
    nfats = d[0x10]
    rootent = struct.unpack("<H", d[0x11:0x13])[0]
    fatsec = struct.unpack("<H", d[0x16:0x18])[0]
    fat_lba = resv
    root_lba = resv + nfats * fatsec
    data_lba = root_lba + (rootent * 32 + bps - 1) // bps
    found = False
    for i in range(rootent):
        off = root_lba * bps + i * 32
        ent = d[off:off + 32]
        if ent[0] in (0x00, 0xE5):
            continue
        name = ent[0:8].decode("latin-1").rstrip()
        ext = ent[8:11].decode("latin-1").rstrip()
        if name + "." + ext != "AUTOEXEC.BAT":
            continue
        cl = struct.unpack("<H", ent[26:28])[0]
        clus_off = (data_lba + (cl - 2) * spc) * bps
        d[clus_off:clus_off + len(PAYLOAD)] = PAYLOAD
        struct.pack_into("<I", d, off + 28, len(PAYLOAD))
        found = True
        print("patched AUTOEXEC.BAT cluster", cl, "at", hex(clus_off))
    if not found:
        print("AUTOEXEC.BAT not found")
        return 1
    open(dst, "wb").write(d)
    print("wrote", dst, len(d), "bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
