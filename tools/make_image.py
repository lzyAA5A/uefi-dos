#!/usr/bin/env python3
"""Build UEFI-DOS FAT32 disk images (test tooling).

Usage:
  python make_image.py out.img [--size-mb 64] [--boot efi-file | --noboot]
                               [--files SPEC ...] [--no-autoexec]
  SPEC: "SRC=DEST" copies a local file, or "TEXT=CONTENT" (see --text prefix).
  --text "hello.txt=hello world" creates an ASCII text file in-memory.
  --autoexec "cmd1\\ncmd2" creates \\AUTOEXEC.BAT (unless --no-autoexec).
"""
import argparse
import sys
from fat32 import Fat32Maker


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--size-mb', type=int, default=64)
    ap.add_argument('--boot', help='file to place as \\EFI\\BOOT\\BOOTX64.EFI')
    ap.add_argument('--boot-ia32', help='file to place as \\EFI\\BOOT\\BOOTIA32.EFI')
    ap.add_argument('--noboot', action='store_true')
    ap.add_argument('--file', action='append', default=[], metavar='SRC=DEST')
    ap.add_argument('--text', action='append', default=[], metavar='NAME=CONTENT')
    ap.add_argument('--autoexec', help='AUTOEXEC.BAT content (commands separated by \\n)')
    ap.add_argument('--dir', action='append', default=[], metavar='DIR')
    args = ap.parse_args()

    m = Fat32Maker(args.out, size_mb=args.size_mb)
    m.add_dir('EFI')
    m.add_dir('EFI/BOOT')
    if args.boot and not args.noboot:
        with open(args.boot, 'rb') as f:
            m.add_file('EFI/BOOT/BOOTX64.EFI', f.read())
    if args.boot_ia32 and not args.noboot:
        with open(args.boot_ia32, 'rb') as f:
            m.add_file('EFI/BOOT/BOOTIA32.EFI', f.read())
    if not args.noboot and (args.boot or args.boot_ia32):
        # UEFI shell fallback: run the OS from FS0 if firmware fell back to the shell.
        if args.boot_ia32 and not args.boot:
            m.add_file('STARTUP.NSH', b'FS0:\\EFI\\BOOT\\BOOTIA32.EFI\r\n')
        else:
            m.add_file('STARTUP.NSH', b'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n')
    for d in args.dir:
        m.add_dir(d)
    for spec in args.file:
        src, dest = spec.split('=', 1)
        with open(src, 'rb') as f:
            data = f.read()
        parent = dest.rsplit('/', 1)[0] if '/' in dest else ''
        if parent:
            m.add_dir(parent)
        m.add_file(dest, data)
    for spec in args.text:
        name, content = spec.split('=', 1)
        content = content.replace('\\n', '\n').replace('\\0', '\0')
        parent = name.rsplit('/', 1)[0] if '/' in name else ''
        if parent:
            m.add_dir(parent)
        m.add_file(name, content.encode('ascii'))
    if args.autoexec is not None and not args.noboot:
        content = args.autoexec.replace('\\n', '\n')
        m.add_file('AUTOEXEC.BAT', content.encode('ascii'))
    m.write()
    print('image:', args.out, 'clusters:', m.total_clusters, 'entries:', m.read_root_names())


if __name__ == '__main__':
    main()
