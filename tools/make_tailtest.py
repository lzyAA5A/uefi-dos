#!/usr/bin/env python3
"""Build TAILCHK.COM: print the command-tail bytes DOS built for us.

A DOS EXEC does not merely copy an argument string: it fills the child's
PSP:0080h..00FFh area, where 0080h is the character count (the trailing CR is
NOT counted, so a bare command line has count 0) and the rest is the caller's
tail buffer copied verbatim -- pre-3.0 software hides data there (WIN.COM of
real-mode Windows 1.x puts the word 'XW' at PSP:00FEh).

    COUNT=00 C81=0D        TAILCHK           (no arguments)
    COUNT=04 C81=20        TAILCHK ABC       (count includes the leading space)

Used as a regression: the tests assert COUNT=00 for a bare command.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from make_tools import Asm, puthex_routine, call16, int21  # noqa: E402


def main():
    a = Asm()

    a.raw(b'\xBA'); a.addr('msg_count')          # mov dx, msg_count
    a.raw(b'\xB4\x09'); int21(a)                 # mov ah, 9 / int 21h
    a.raw(b'\xA0\x80\x00')                       # mov al, [0080h]  (count)
    call16(a, 'puthex')

    a.raw(b'\xBA'); a.addr('msg_c81')            # mov dx, msg_c81
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xA0\x81\x00')                       # mov al, [0081h]
    call16(a, 'puthex')

    a.raw(b'\xBA'); a.addr('msg_fe')             # mov dx, msg_fe
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xA0\xFE\x00')                       # mov al, [00FEh]
    call16(a, 'puthex')
    a.raw(b'\xA0\xFF\x00')                       # mov al, [00FFh]
    call16(a, 'puthex')

    a.raw(b'\xBA'); a.addr('msg_crlf')           # mov dx, msg_crlf
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xBA'); a.addr('msg_done')           # mov dx, msg_done
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xB8\x00\x4C'); int21(a)             # mov ax, 4C00h / int 21h

    puthex_routine(a)

    a.label('msg_count')
    a.raw(b'COUNT=$')
    a.label('msg_c81')
    a.raw(b' C81=$')
    a.label('msg_fe')
    a.raw(b' FE=$')
    a.label('msg_crlf')
    a.raw(b'\r\n$')
    a.label('msg_done')
    a.raw(b'TAILCHK-DONE\r\n$')

    code = a.finish()
    out = os.path.join(HERE, '..', 'build', 'TAILCHK.COM')
    with open(out, 'wb') as f:
        f.write(code)
    print('wrote %s (%d bytes)' % (os.path.normpath(out), len(code)))


if __name__ == '__main__':
    main()
