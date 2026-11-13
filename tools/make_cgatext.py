"""Assemble CGATEXT.COM -- checks the INT 10h text services in CGA graphics mode.

The probe selects CGA mode 4 (320x200, 4 colours), positions the cursor with
AH=02h, writes two lines through AH=0Eh (teletype) and exits *without* restoring
the text mode, so the caller can dump the framebuffer and see the glyphs.  The
attribute in BL matters: in a graphics mode BL is the foreground colour, and 0
is black-on-black (invisible).

Nothing is printed through INT 21h, so a shell run of this program is silent;
the proof is the bitmap.  run_tests.ps1 runs it with UEFIDOS_GFX_DUMP and
asserts that the dumped frame contains pixels.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_tools import Asm

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build")


def build():
    a = Asm()

    def set_mode(mode):
        a.b(0xB8, mode & 0xFF, (mode >> 8) & 0xFF)   # mov ax,mode
        a.b(0xCD, 0x10)                              # int 10h

    def tele(ch):
        a.b(0xB4, 0x0E)                              # mov ah,0Eh
        a.b(0xB0, ord(ch) & 0xFF)                    # mov al,char
        a.b(0xBB, 0x0F, 0x00)                        # mov bx,000Fh (white)
        a.b(0xCD, 0x10)                              # int 10h

    set_mode(0x0004)                                 # CGA 320x200x4
    a.b(0xB4, 0x02)                                  # mov ah,2 (set cursor)
    a.b(0xB7, 0x00)                                  # mov bh,0
    a.b(0xBA, 0x02, 0x00)                            # mov dx,0002h (row 0, col 2)
    a.b(0xCD, 0x10)
    for ch in "CGA TEXT OK":
        tele(ch)
    tele("\r")
    tele("\n")
    for ch in "ASCII 8X8 FONT":
        tele(ch)
    a.b(0xB8, 0x00, 0x4C)                            # mov ax,4C00h
    a.b(0xCD, 0x21)                                  # int 21h
    return a.finish()


def main():
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "CGATEXT.COM")
    data = build()
    with open(path, "wb") as f:
        f.write(bytes(data))
    print("wrote %s (%d bytes)" % (path, len(data)))


if __name__ == "__main__":
    main()
