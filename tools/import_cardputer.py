#!/usr/bin/env python3
"""Extract a curated set of games/tools from the CardPuter86 DOS images.

Both images are FAT12 floppies (cardputer86.img is the MS-DOS 5.0 release with
extra games; dos211.img is the Compaq DOS 2.11 disk).  The files land in
build/imported/ under their 8.3 destination names so build.ps1 can copy them
onto the UEFI-DOS test image.

Usage: python import_cardputer.py [--out DIR]
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fat_extract import FatImage

CARD = r'F:\1231\_tmp\CardPuter86\ESP32\CardPuter86\data\cardputer86.img'
DOS211 = r'F:\1231\_tmp\CardPuter86\ESP32\CardPuter86\data\dos211.img'

# (image, source 8.3 name, destination 8.3 name)
FILES = [
    # --- cardputer86.img: MS-DOS 5.0 release + games -------------------------
    (CARD, 'MINE.EXE', 'MINE.EXE'),           # minesweeper
    (CARD, 'SNAKE.EXE', 'SNAKE.EXE'),         # snake
    (CARD, 'G2048.EXE', 'G2048.EXE'),         # 2048
    (CARD, 'MINIGAME.EXE', 'MINIGAME.EXE'),   # game pack
    (CARD, 'DIAG.COM', 'DIAG.COM'),           # diagnostics
    (CARD, 'VID.COM', 'VID.COM'),             # video demo
    (CARD, 'SCRDEMO.EXE', 'SCRDEMO.EXE'),     # screen demo
    (CARD, 'CPPTEST.EXE', 'CPPTEST.EXE'),     # C++ test
    (CARD, 'HELLO.COM', 'C86HELLO.COM'),      # renamed: our own HELLO.COM exists
    (CARD, 'TE.EXE', 'TE.EXE'),               # text editor (DOS 5 build)
    (CARD, 'README.TXT', 'README.TXT'),
    # --- dos211.img: Compaq DOS 2.11 tools -----------------------------------
    (DOS211, 'MININASM.COM', 'MININASM.COM'),
    (DOS211, 'MININASM.LIC', 'MININASM.LIC'),
    (DOS211, 'TE.COM', 'TE.COM'),             # small 2.11 text editor
    (DOS211, 'TERM.COM', 'TERM.COM'),         # serial terminal
    (DOS211, 'CP86TEST.COM', 'CP86TEST.COM'), # self test
    (DOS211, 'ASM.EXE', 'ASM.EXE'),           # assembler
    (DOS211, 'HI.EXE', 'HI.EXE'),
    (DOS211, 'VID2.EXE', 'VID2.EXE'),
    (DOS211, 'DEMO.ASM', 'DEMO.ASM'),
    (DOS211, 'SB.COM', 'SB.COM'),             # sound demo
    (DOS211, 'SB.TXT', 'SB.TXT'),
    (DOS211, 'TOOLS.TXT', 'TOOLS.TXT'),
]


def main():
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       'build', 'imported')
    if '--out' in sys.argv:
        out = sys.argv[sys.argv.index('--out') + 1]
    os.makedirs(out, exist_ok=True)
    cache = {}
    total = 0
    for image, src, dest in FILES:
        if image not in cache:
            cache[image] = FatImage(image)
        data = cache[image].get(src)
        if data is None:
            print('MISSING %s in %s' % (src, os.path.basename(image)))
            continue
        with open(os.path.join(out, dest), 'wb') as f:
            f.write(data)
        total += 1
        print('%-13s %7d bytes  <- %s' % (dest, len(data), os.path.basename(image)))
    print('imported %d files into %s' % (total, out))


if __name__ == '__main__':
    main()
