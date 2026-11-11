#!/usr/bin/env python3
"""Build a UEFI-DOS image from a staged directory tree (dirs -> dirs, files -> files).

Usage:  python make_tree_image.py OUT.IMG SRCDIR [--size-mb N] [--autoexec "..."]
"""
import argparse
import os
from fat32 import Fat32Maker


def walk(m, base, rel=''):
    for name in sorted(os.listdir(os.path.join(base, rel))):
        full = os.path.join(base, rel, name)
        tag = (rel + '/' + name) if rel else name
        if os.path.isdir(full):
            m.add_dir(tag)
            walk(m, base, tag)
        else:
            with open(full, 'rb') as f:
                m.add_file(tag, f.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('srcdir')
    ap.add_argument('--size-mb', type=int, default=64)
    ap.add_argument('--autoexec', default=None)
    args = ap.parse_args()

    m = Fat32Maker(args.out, size_mb=args.size_mb)
    m.add_dir('EFI')
    m.add_dir('EFI/BOOT')
    walk(m, args.srcdir)
    if args.autoexec:
        m.add_file('AUTOEXEC.BAT', args.autoexec.replace('\\n', '\n').encode('ascii'))
    m.write()
    print('image:', args.out, 'clusters:', m.total_clusters)


if __name__ == '__main__':
    main()
