#!/usr/bin/env python3
"""Build CGATEST.COM and MOUSETST.COM.

CGATEST    INT 10h AH=00 mode 4 (320x200, 4 colours), AH=0Bh palette select, then
           paints four vertical stripes straight into the interleaved CGA bitmap
           at B8000.  Proves the packed-pixel path and the palette mapping.

MOUSETST   INT 33h reset / show / read position, waits for the pointer to move
           and prints the position, button state and motion counters.

Usage:  python make_video_test.py <output-dir>
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from make_tools import Asm, puthex_routine, puthex16_routine, call16, int21  # noqa: E402


# ---------------------------------------------------------------------------
# CGATEST.COM
# ---------------------------------------------------------------------------
def build_cga():
    a = Asm()

    a.raw(b'\xB8\x04\x00')                      # mov ax, 0004h
    a.raw(b'\xCD\x10')                          # int 10h        mode 4
    a.raw(b'\xB8\x00\x0B')                      # mov ax, 0B00h
    a.raw(b'\xBB\x00\x00')                      # mov bx, 0      background 0
    a.raw(b'\xCD\x10')                          # int 10h
    a.raw(b'\xB8\x00\x0B')                      # mov ax, 0B00h  (BH=0: background)
    a.raw(b'\xBB\x01\x01')                      # mov bx, 0101h  (BH=1: palette 1)
    a.raw(b'\xCD\x10')                          # int 10h

    a.raw(b'\xB8\x00\xB8')                      # mov ax, B800h
    a.raw(b'\x8E\xC0')                          # mov es, ax
    a.raw(b'\x31\xFF')                          # xor di, di     even field
    call16(a, 'paintfield')
    a.raw(b'\xBF\x00\x20')                      # mov di, 2000h  odd field
    call16(a, 'paintfield')

    a.raw(b'\xE9'); a.rel16('painted')          # jmp painted

    # 100 row pairs of 80 bytes: four 20-byte stripes (0,1,2,3)
    a.label('paintfield')
    a.raw(b'\xB9' + bytes([100, 0]))            # mov cx, 100
    a.label('row')
    a.raw(b'\x51')                              # push cx
    for colour in (0x00, 0x55, 0xAA, 0xFF):     # 20 bytes = 80 pixels per stripe
        a.raw(bytes([0xB0, colour]))            # mov al, colour
        a.raw(b'\x31\xC9')                      # xor cx, cx
        a.raw(b'\xB1\x14')                      # mov cl, 20
        a.raw(b'\xF3\xAA')                      # rep stosb
    a.raw(b'\x59')                              # pop cx
    a.raw(b'\xE2'); a.rel8('row')               # loop row
    a.raw(b'\xC3')                              # ret
    a.label('painted')

    a.raw(b'\xBA'); a.addr('msg_ok')            # mov dx, msg_ok
    a.raw(b'\xB4\x09'); int21(a)

    a.raw(b'\x31\xC0')                          # xor ax, ax
    a.raw(b'\xCD\x16')                          # int 16h        wait for a key

    a.raw(b'\xB8\x03\x00')                      # mov ax, 3
    a.raw(b'\xCD\x10')                          # int 10h        back to text
    a.raw(b'\xBA'); a.addr('msg_text')          # mov dx, msg_text
    a.raw(b'\xB4\x09'); int21(a)

    a.raw(b'\xB8\x00\x4C'); int21(a)            # mov ax, 4C00h / int 21h
    a.label('msg_ok')
    a.raw(b'CGA-OK 320x200x4\r\n$')
    a.label('msg_text')
    a.raw(b'CGA-TEXT-OK\r\n$')
    return a.finish()


# ---------------------------------------------------------------------------
# MOUSETEST.COM
# ---------------------------------------------------------------------------
def build_mouse():
    a = Asm()

    a.raw(b'\x31\xC0')                          # xor ax, ax
    a.raw(b'\xCD\x33')                          # int 33h        reset
    a.raw(b'\xA3'); a.addr('axv')               # mov [axv], ax
    a.raw(b'\x89\x1E'); a.addr('bxv')           # mov [bxv], bx  (button count)

    a.raw(b'\xB8\x01\x00')                      # mov ax, 1
    a.raw(b'\xCD\x33')                          # int 33h        show pointer

    a.raw(b'\xB8\x03\x00')                      # mov ax, 3
    a.raw(b'\xCD\x33')                          # int 33h        read position
    a.raw(b'\x89\x0E'); a.addr('x0')            # mov [x0], cx
    a.raw(b'\x89\x16'); a.addr('y0')            # mov [y0], dx
    a.raw(b'\xC7\x06'); a.addr('cnt'); a.raw(b'\x90\x01')  # mov word [cnt], 400

    a.label('poll')
    a.raw(b'\xB8\x03\x00')                      # mov ax, 3
    a.raw(b'\xCD\x33')                          # int 33h
    a.raw(b'\x3B\x0E'); a.addr('x0')            # cmp cx, [x0]
    a.raw(b'\x75'); a.rel8('moved')             # jnz moved
    a.raw(b'\xFF\x0E'); a.addr('cnt')           # dec word [cnt]
    a.raw(b'\x75'); a.rel8('poll')              # jnz poll
    a.raw(b'\xBA'); a.addr('msg_fail')          # mov dx, msg_fail
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xE9'); a.rel16('quit')             # jmp quit

    a.label('moved')
    a.raw(b'\x89\x0E'); a.addr('x1')            # mov [x1], cx
    a.raw(b'\x89\x16'); a.addr('y1')            # mov [y1], dx
    a.raw(b'\x89\x1E'); a.addr('b1')            # mov [b1], bx
    a.raw(b'\xB8\x0B\x00')                      # mov ax, 0Bh
    a.raw(b'\xCD\x33')                          # int 33h        motion counters
    a.raw(b'\x89\x0E'); a.addr('mdx')           # mov [mdx], cx
    a.raw(b'\x89\x16'); a.addr('mdy')           # mov [mdy], dx

    a.raw(b'\xBA'); a.addr('msg_ok')            # mov dx, msg_ok
    a.raw(b'\xB4\x09'); int21(a)

    a.raw(b'\xBA'); a.addr('msg_ax')            # mov dx, msg_ax
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xA1'); a.addr('axv')               # mov ax, [axv]
    call16(a, 'puthex16')

    a.raw(b'\xBA'); a.addr('msg_x')             # mov dx, msg_x
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xA1'); a.addr('x1')
    call16(a, 'puthex16')

    a.raw(b'\xBA'); a.addr('msg_y')             # mov dx, msg_y
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xA1'); a.addr('y1')
    call16(a, 'puthex16')

    a.raw(b'\xBA'); a.addr('msg_b')             # mov dx, msg_b
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xA1'); a.addr('b1')
    call16(a, 'puthex16')

    a.raw(b'\xBA'); a.addr('msg_m')             # mov dx, msg_m
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xA1'); a.addr('mdx')
    call16(a, 'puthex16')
    a.raw(b'\xBA'); a.addr('msg_comma')
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xA1'); a.addr('mdy')
    call16(a, 'puthex16')

    a.raw(b'\xBA'); a.addr('msg_crlf')
    a.raw(b'\xB4\x09'); int21(a)

    a.label('quit')
    a.raw(b'\xB8\x00\x4C'); int21(a)            # mov ax, 4C00h / int 21h

    puthex_routine(a)
    puthex16_routine(a)

    for name, size in (('axv', 2), ('bxv', 2), ('x0', 2), ('y0', 2), ('x1', 2),
                       ('y1', 2), ('b1', 2), ('mdx', 2), ('mdy', 2), ('cnt', 2)):
        a.label(name)
        a.raw(b'\x00' * size)

    a.label('msg_ok')
    a.raw(b'MOUSE-OK\r\n$')
    a.label('msg_fail')
    a.raw(b'MOUSE-FAIL no motion\r\n$')
    a.label('msg_ax')
    a.raw(b' reset=AX $')
    a.label('msg_x')
    a.raw(b' x=$')
    a.label('msg_y')
    a.raw(b' y=$')
    a.label('msg_b')
    a.raw(b' btn=$')
    a.label('msg_m')
    a.raw(b' motion=$')
    a.label('msg_comma')
    a.raw(b',$')
    a.label('msg_crlf')
    a.raw(b'\r\n$')
    return a.finish()


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    for name, builder in (('CGATEST.COM', build_cga), ('MOUSETST.COM', build_mouse)):
        blob = builder()
        path = os.path.join(out, name)
        with open(path, 'wb') as f:
            f.write(blob)
        print('%-14s %5d bytes' % (name, len(blob)))


if __name__ == '__main__':
    main()
