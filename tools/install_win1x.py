#!/usr/bin/env python3
"""install_win1x.py - lay a Windows 1.x (DR5 / 1.0x) release out on a bootable
UEFI-DOS image and make it start automatically.

The Microsoft files are NOT part of this repository; point this tool at a
directory that holds your own extracted copy.

Windows 1.0 Developer Release 5 is the layout INSTALL.BAT produces:

    \\WINDOWS\\TEST   runtime (KERNEL.EXE, WINDOWS.EXE, GDI.EXE, ...)
    \\WINDOWS\\BIN    build tools (MC.EXE, RC.EXE, NE.EXE, ...)
    \\WINDOWS\\DOC    documentation
    \\LIB             libraries, WINDOWS.H, CMACROS.INC

layout:
  * .DIN/.CIN display drivers are copied to DISPLAY.EXE / CURSOR.EXE inside
  * .LIB/.H/.INC files go to \\LIB
  * .DOC files go to \\WINDOWS\\DOC
  * MC/RC/NE/MEDIT/ICONEDIT and *.BAT go to \\WINDOWS\\BIN
  * everything else stays in \\WINDOWS\\TEST

Usage:
    python tools/install_win1x.py SRC_DIR OUT.img
    python tools/install_win1x.py SRC_DIR OUT.img --exe KERNEL.EXE --setver WIN.COM
    python tools/install_win1x.py SRC_DIR OUT.img --dir WINDOWS --exe WIN.COM

--exe names the program the autoexec starts (default KERNEL.EXE); --setver adds
SETVER lines (DR5's kernel wants DOS 2.x/3.x, our shell reports 4.00).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fat32 import Fat32Maker  # noqa: E402

BOOT_FILES = (('BOOTX64.EFI', 'EFI/BOOT/BOOTX64.EFI'),
              ('BOOTIA32.EFI', 'EFI/BOOT/BOOTIA32.EFI'))

LIB_EXT = ('.LIB', '.H', '.INC')
DOC_EXT = ('.DOC', '.TXT')
BIN_NAMES = ('MC.EXE', 'RC.EXE', 'NE.EXE', 'MEDIT.EXE', 'ICONEDIT.EXE',
             'CVPACK.EXE', 'EXEHDR.EXE', 'IMPLIB.EXE', 'LIB.EXE', 'LINK.EXE',
             'MASM.EXE', 'CL.EXE')


def classify(name, target_dir):
    up = name.upper()
    ext = os.path.splitext(up)[1]
    if ext in LIB_EXT:
        return 'LIB'
    if ext in DOC_EXT:
        return os.path.join(target_dir, 'DOC')
    if up in BIN_NAMES or ext == '.BAT':
        return os.path.join(target_dir, 'BIN')
    return target_dir


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('srcdir')
    ap.add_argument('out')
    ap.add_argument('--dir', default='WINDOWS\\TEST',
                    help='directory the runtime files are installed into')
    ap.add_argument('--exe', default='KERNEL.EXE',
                    help='program the autoexec starts')
    ap.add_argument('--setver', action='append', default=[],
                    help='programs to give a DOS 3.30 entry (repeatable)')
    ap.add_argument('--size-mb', type=int, default=64)
    ap.add_argument('--boot-dir', default='build',
                    help='where BOOTX64.EFI / BOOTIA32.EFI live')
    ap.add_argument('--no-autoexec', action='store_true')
    args = ap.parse_args(argv[1:])

    target = args.dir.replace('/', '\\')
    m = Fat32Maker(args.out, size_mb=args.size_mb)
    m.add_dir('EFI')
    m.add_dir('EFI/BOOT')
    dirs = set()
    for src, dst in BOOT_FILES:
        path = os.path.join(args.boot_dir, src)
        if not os.path.isfile(path):
            raise SystemExit('missing boot file: ' + path + ' (run build.ps1 first)')
        with open(path, 'rb') as f:
            m.add_file(dst, f.read())

    def ensure(d):
        if d and d not in dirs:
            m.add_dir(d.replace('\\', '/'))
            dirs.add(d)

    ensure(target)
    count = 0
    for name in sorted(os.listdir(args.srcdir)):
        src = os.path.join(args.srcdir, name)
        if not os.path.isfile(src):
            continue
        dest_dir = classify(name, target)
        ensure(dest_dir)
        tag = dest_dir.replace('\\', '/') + '/' + name.upper()
        with open(src, 'rb') as f:
            m.add_file(tag, f.read())
        count += 1

    # DR5 ships IBMCOLOR.DIN / HERCULES.DIN and INSTALL.BAT renames a copy to
    # DISPLAY.EXE (the kernel loads its display driver by that name).
    for din, alias in (('IBMCOLOR.DIN', 'DISPLAY.EXE'),
                       ('IBMCOLOR.CIN', 'CURSOR.EXE'),
                       ('HERCULES.DIN', 'HERCULES.EXE'),
                       ('HERCULES.CIN', 'HERCURCU.EXE')):
        path = os.path.join(args.srcdir, din)
        if os.path.isfile(path) and not os.path.exists(os.path.join(args.srcdir, alias)):
            with open(path, 'rb') as f:
                m.add_file(target.replace('\\', '/') + '/' + alias, f.read())
            count += 1

    if not args.no_autoexec:
        lines = []
        for prog in args.setver:
            lines.append('SETVER %s 3.30' % prog.upper())
        lines.append('CD \\' + target)
        lines.append(args.exe.upper())
        m.add_file('AUTOEXEC.BAT', ('\r\n'.join(lines) + '\r\n').encode('ascii'))

    m.write()
    print('image %s: %d files installed into \\%s, autoexec starts %s'
          % (args.out, count, target, args.exe.upper()))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
