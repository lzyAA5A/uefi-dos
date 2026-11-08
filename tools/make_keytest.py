#!/usr/bin/env python3
"""Build KEYTEST.COM: read five keys with INT 16h AH=00 and print scan/ascii.

    S=48 A=00      arrow up   (extended key: scan code, ASCII 0)
    S=1C A=0D      enter

Used to prove that the platform layer really delivers extended (arrow/F-key)
keys into the BIOS keyboard buffer, which is what menus and games poll.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from make_tools import Asm, puthex_routine, call16, int21  # noqa: E402

KEYS = 5


def main():
    a = Asm()

    a.raw(b'\xB9' + bytes([KEYS, 0]))            # mov cx, KEYS
    a.label('loop_top')
    a.raw(b'\x31\xC0')                           # xor ax, ax
    a.raw(b'\xCD\x16')                           # int 16h      -> AX = scan:ascii
    a.raw(b'\xA3'); a.addr('keyv')               # mov [keyv], ax
    a.raw(b'\x88\xE0')                           # mov al, ah   (scan code)
    a.raw(b'\xA2'); a.addr('scanv')              # mov [scanv], al
    a.raw(b'\xBA'); a.addr('msg_s')              # mov dx, msg_s
    a.raw(b'\xB4\x09'); int21(a)                 # mov ah, 9 / int 21h
    a.raw(b'\xA0'); a.addr('scanv')              # mov al, [scanv]
    call16(a, 'puthex')
    a.raw(b'\xBA'); a.addr('msg_a')              # mov dx, msg_a
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xA0'); a.addr('keyv')               # mov al, [keyv]   (ascii)
    call16(a, 'puthex')
    a.raw(b'\xBA'); a.addr('msg_crlf')           # mov dx, msg_crlf
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\x49')                               # dec cx
    a.raw(b'\x75'); a.rel8('loop_top')           # jnz loop_top

    a.raw(b'\xBA'); a.addr('msg_done')           # mov dx, msg_done
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xB8\x00\x4C'); int21(a)             # mov ax, 4C00h / int 21h

    puthex_routine(a)

    a.label('keyv')
    a.raw(b'\x00\x00')
    a.label('scanv')
    a.raw(b'\x00')
    a.label('msg_s')
    a.raw(b'S=$')
    a.label('msg_a')
    a.raw(b' A=$')
    a.label('msg_crlf')
    a.raw(b'\r\n$')
    a.label('msg_done')
    a.raw(b'KEYTEST-DONE\r\n$')

    code = a.finish()
    out = os.path.join(HERE, '..', 'build', 'KEYTEST.COM')
    with open(out, 'wb') as f:
        f.write(code)
    print('wrote %s (%d bytes)' % (os.path.normpath(out), len(code)))


if __name__ == '__main__':
    main()
