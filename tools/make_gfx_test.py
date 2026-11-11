"""Graphics test programs for UEFI-DOS.

GFXTEST.COM    sets VGA mode 13h (320x200x256) through INT 10h AH=00, programs
               the whole DAC palette (INT 10h AX=1012h), paints a test pattern
               straight into A0000, reports GFX-OK, waits for a key, then goes
               back to text mode and reports GFX-TEXT-OK.

VESATEST.COM   calls the VESA BIOS extensions (INT 10h AX=4F00h/4F01h/4F02h),
               verifies the "VESA" signature and the 640x480x256 mode, paints a
               gradient, reports VESA-OK / VESA-MODE-OK and restores text mode.

Both are hand-assembled with the helper routines in tools/make_tools.py.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_tools import Asm, ORG, imm16, puts, putc, call16, jmp16, jcc16, int21, exit_dos  # noqa: E402

BUILD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build')


def int10(a):
    a.raw(b'\xCD\x10')
    return a


def build_palette_table():
    """256 DAC entries: 6x6x6 colour cube then a grey ramp (6-bit values)."""
    tbl = bytearray()
    for i in range(216):
        r = i // 36
        g = (i // 6) % 6
        b = i % 6
        tbl += bytes([r * 12 + 3, g * 12 + 3, b * 12 + 3])
    while len(tbl) < 256 * 3:
        v = (len(tbl) // 3 - 216) * 63 // 39
        tbl += bytes([v, v, v])
    return bytes(tbl[:256 * 3])


def make_gfxtest():
    a = Asm()
    # --- 320x200x256 ---
    a.raw(b'\xB8\x13\x00')                  # mov ax,0013h
    int10(a)                                # int 10h (set mode)
    # palette: INT 10h AX=1012h, BX=0, CX=256, ES:DX = table
    # ES must point at our own segment: a .COM starts with ES = PSP, so without
    # this the table pointer read the PSP (i.e. a black palette)
    a.raw(b'\x0E')                          # push cs
    a.raw(b'\x07')                          # pop es
    a.raw(b'\xB8\x12\x10')                  # mov ax,1012h
    a.raw(b'\xBB\x00\x00')                  # mov bx,0
    a.raw(b'\xB9\x00\x01')                  # mov cx,256
    a.raw(b'\xBA')                          # mov dx, offset palette
    a.addr('pal')
    int10(a)
    # --- paint: 200 rows, each a horizontal gradient + colour bars ---
    a.raw(b'\x1E')                          # push ds
    a.raw(b'\xB8\x00\xA0')                  # mov ax,0A000h
    a.raw(b'\x8E\xD8')                      # mov ds,ax
    a.raw(b'\x31\xFF')                      # xor di,di
    a.raw(b'\x31\xD2')                      # xor dx,dx  (dx = y)
    a.label('row')
    a.raw(b'\x31\xC9')                      # xor cx,cx  (cx = x)
    a.label('col')
    a.raw(b'\x88\xD0')                      # mov al,dl  (y)
    a.raw(b'\x02\xC1')                      # add al,cl  (y + x)
    a.raw(b'\x88\xC3')                      # mov bl,al
    a.raw(b'\x80\xE3\x0F')                  # and bl,0Fh (16 colour bands)
    a.raw(b'\x80\xC3\x10')                  # add bl,10h
    a.raw(b'\x88\xD8')                      # mov al,bl
    a.raw(b'\x88\x05')                      # mov [di],al
    a.raw(b'\x47')                          # inc di
    a.raw(b'\x41')                          # inc cx
    a.raw(b'\x81\xF9\x40\x01')              # cmp cx,320
    a.raw(b'\x75')                          # jne col
    a.rel8('col')
    a.raw(b'\x42')                          # inc dx
    a.raw(b'\x81\xFA\xC8\x00')              # cmp dx,200
    a.raw(b'\x75')                          # jne row
    a.rel8('row')
    # a solid white 32x32 block in the middle (index 15)
    a.raw(b'\xBE\xE0\x00')                  # mov si,00E0h  (y = 96)
    a.label('blk_y')
    a.raw(b'\xBF\x40\x01')                  # mov di,0140h  (x = 64 + 256?)
    # di = y*320 + x: recompute from si each row
    a.raw(b'\x8B\xC6')                      # mov ax,si
    a.raw(b'\xB9\x40\x01')                  # mov cx,320
    a.raw(b'\xF7\xE1')                      # mul cx  -> dx:ax = y*320
    a.raw(b'\x05\x40\x00')                  # add ax,64
    a.raw(b'\x89\xC7')                      # mov di,ax
    a.raw(b'\xB9\x20\x00')                  # mov cx,32
    a.label('blk_x')
    a.raw(b'\xB0\x0F')                      # mov al,15
    a.raw(b'\x88\x05')                      # mov [di],al
    a.raw(b'\x47')                          # inc di
    a.raw(b'\x49')                          # dec cx
    a.raw(b'\x75')                          # jne blk_x
    a.rel8('blk_x')
    a.raw(b'\x46')                          # inc si
    a.raw(b'\x81\xFE\x00\x01')              # cmp si,0100h (96+32)
    a.raw(b'\x75')                          # jne blk_y
    a.rel8('blk_y')
    a.raw(b'\x1F')                          # pop ds
    puts(a, 's_ok')
    # wait for a key so the screen stays visible (INT 16h AH=00)
    a.raw(b'\xB4\x00')
    a.raw(b'\xCD\x16')
    # back to text mode
    a.raw(b'\xB8\x03\x00')
    int10(a)
    puts(a, 's_text')
    exit_dos(a, 0)
    # --- data: strings first (from the top of the code image) ---
    a.label('pal')
    pal = build_palette_table()
    a.raw(pal)
    a.label('s_ok')
    a.raw(b'GFX-OK 320x200x256\r\n$')
    a.label('s_text')
    a.raw(b'GFX-TEXT-OK\r\n$')
    return a.finish()


def make_vesatest():
    a = Asm()
    # ES:DI -> 512-byte scratch buffer for the VBE structures (ES = CS), and the
    # same segment for the palette table handed to INT 10h AX=1012h
    a.raw(b'\x0E')                          # push cs
    a.raw(b'\x07')                          # pop es
    a.raw(b'\xBE')                          # mov si, offset buf
    a.addr('buf')
    a.raw(b'\x89\xF7')                     # mov di,si
    a.raw(b'\xB8\x00\x4F')                # mov ax,4F00h (get controller info)
    int10(a)
    a.raw(b'\x3D\x4F\x00')                # cmp ax,004Fh
    a.raw(b'\x74')                          # je got_info
    a.rel8('got_info')
    puts(a, 's_no_vesa')
    jmp16(a, 'vesa_end')
    a.label('got_info')
    # the structure must start with the "VESA" signature
    a.raw(b'\x89\xF7')                     # mov di,si
    a.raw(b'\x26\x81\x3D\x56\x45')      # cmp word es:[di],"VE"
    a.raw(b'\x75')                          # jne bad_sig
    a.rel8('bad_sig')
    a.raw(b'\x26\x81\x7D\x02\x53\x41')  # cmp word es:[di+2],"SA"
    a.raw(b'\x74')                          # je sig_ok
    a.rel8('sig_ok')
    a.label('bad_sig')
    puts(a, 's_no_sig')
    jmp16(a, 'vesa_end')
    a.label('sig_ok')
    puts(a, 's_vesa_ok')
    # mode information for 101h (640x480x256)
    a.raw(b'\xB9\x01\x01')                # mov cx,0101h
    a.raw(b'\x89\xF7')                     # mov di,si
    a.raw(b'\xB8\x01\x4F')                # mov ax,4F01h
    int10(a)
    a.raw(b'\x3D\x4F\x00')
    a.raw(b'\x74')
    a.rel8('mode_info_ok')
    puts(a, 's_no_mode')
    jmp16(a, 'vesa_end')
    a.label('mode_info_ok')
    # set 640x480x256 with the linear framebuffer bit
    a.raw(b'\xBB\x01\x41')                # mov bx,4101h (0101 | LFB)
    a.raw(b'\xB8\x02\x4F')                # mov ax,4F02h
    int10(a)
    a.raw(b'\x3D\x4F\x00')
    a.raw(b'\x74')
    a.rel8('set_ok')
    puts(a, 's_no_set')
    jmp16(a, 'vesa_end')
    a.label('set_ok')
    puts(a, 's_mode_ok')
    # palette (ES:DX = table) so the gradient has colour
    a.raw(b'\xB8\x12\x10')                # mov ax,1012h
    a.raw(b'\xBB\x00\x00')                # mov bx,0
    a.raw(b'\xB9\x00\x01')                # mov cx,256
    a.raw(b'\xBA')                          # mov dx, offset palette
    a.addr('pal')
    int10(a)
    # paint all 480 rows in five 64 KB windows: a 16-bit offset cannot reach
    # past 64 KB, so each pass moves DS by 96 rows (3840 paragraphs)
    # the pass counter lives in BP: DS points at the framebuffer inside the
    # loop, so a memory variable would be read from the wrong segment
    a.raw(b'\x31\xED')                     # xor bp,bp
    a.label('pass_loop')
    a.raw(b'\x89\xE8')                     # mov ax,bp
    a.raw(b'\x83\xF8\x05')                # cmp ax,5
    a.raw(b'\x74')                          # je paint_done
    a.rel8('paint_done')
    a.raw(b'\xBB\x0F\x00')                # mov bx,15
    a.raw(b'\xF7\xE3')                     # mul bx      -> dx:ax = pass*15
    a.raw(b'\xB1\x08')                     # mov cl,8
    a.raw(b'\xD3\xE0')                     # shl ax,cl   -> pass*3840
    a.raw(b'\x05\x00\xA0')                # add ax,0A000h
    a.raw(b'\x8E\xD8')                     # mov ds,ax
    a.raw(b'\x31\xFF')                     # xor di,di
    a.raw(b'\xBA\x60\x00')                # mov dx,96   (rows in this pass)
    a.label('vrow')
    a.raw(b'\x31\xC9')                     # xor cx,cx
    a.label('vcol')
    a.raw(b'\x88\xC8')                     # mov al,cl
    a.raw(b'\xC0\xE8\x02')                # shr al,2
    a.raw(b'\x88\x05')                     # mov [di],al
    a.raw(b'\x47')                          # inc di
    a.raw(b'\x41')                          # inc cx
    a.raw(b'\x81\xF9\x80\x02')           # cmp cx,640
    a.raw(b'\x75')
    a.rel8('vcol')
    a.raw(b'\x4A')                          # dec dx
    a.raw(b'\x75')
    a.rel8('vrow')
    a.raw(b'\x45')                          # inc bp
    jmp16(a, 'pass_loop')
    a.label('paint_done')
    # DS was repointed at the framebuffer: get our own data segment back
    a.raw(b'\x0E')                          # push cs
    a.raw(b'\x1F')                          # pop ds
    a.raw(b'\xB4\x00')                     # mov ah,0
    a.raw(b'\xCD\x16')                     # int 16h: wait for a key
    a.raw(b'\xB8\x03\x00')                # mov ax,0003h (back to text)
    int10(a)
    puts(a, 's_done')
    a.label('vesa_end')
    exit_dos(a, 0)
    a.label('buf')
    a.raw(bytes(512))
    a.label('pal')
    a.raw(build_palette_table())
    a.label('s_vesa_ok')
    a.raw(b'VESA-OK\r\n$')
    a.label('s_mode_ok')
    a.raw(b'VESA-MODE-OK 640x480x256\r\n$')
    a.label('s_no_vesa')
    a.raw(b'VESA-FAIL info\r\n$')
    a.label('s_no_sig')
    a.raw(b'VESA-FAIL signature\r\n$')
    a.label('s_no_mode')
    a.raw(b'VESA-FAIL modeinfo\r\n$')
    a.label('s_no_set')
    a.raw(b'VESA-FAIL setmode\r\n$')
    a.label('s_done')
    a.raw(b'VESA-TEXT-OK\r\n$')
    return a.finish()


def main():
    os.makedirs(BUILD, exist_ok=True)
    for name, data in (('GFXTEST.COM', make_gfxtest()), ('VESATEST.COM', make_vesatest())):
        path = os.path.join(BUILD, name)
        with open(path, 'wb') as f:
            f.write(data)
        print('wrote %s (%d bytes)' % (path, len(data)))


if __name__ == '__main__':
    main()
