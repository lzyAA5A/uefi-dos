#!/usr/bin/env python3
"""Generate the bundled UEFI-DOS utility set (16-bit DOS .COM programs).

Everything here is written from scratch for this project and carries the same
MIT license as the rest of the repository -- no third-party binaries and no
Microsoft code are involved.  The programs double as compatibility tests for
the INT 21h / INT 2Fh services UEFI-DOS implements.

  INFO.COM   system panel: DOS version, date, time, drive, directory, native API
  CLOCK.COM  live HH:MM:SS clock, press any key to stop
  HEX.COM    hex + ASCII dump of a file named on the command line
  SUM.COM    byte count and 16-bit checksum of a file (exercises lseek)
  CAL.COM    calendar for the current month (exercises the DOS date services)

Usage:  python make_tools.py <output-dir>
"""
import os
import struct
import sys

ORG = 0x100


class Asm:
    """Minimal 16-bit COM assembler: raw bytes plus label fixups."""

    def __init__(self):
        self.code = bytearray()
        self.labels = {}
        self.fixups = []           # (kind, offset, name)

    # ---- emission -----------------------------------------------------
    def b(self, *vals):
        for v in vals:
            self.code.append(v & 0xFF)
        return self

    def raw(self, data):
        self.code += data
        return self

    def addr(self, name):
        """Write a placeholder word fixed up to the label's address."""
        self.fixups.append(('abs16', len(self.code), name))
        self.code += b'\x00\x00'
        return self

    def rel8(self, name):
        self.fixups.append(('rel8', len(self.code), name))
        self.code.append(0x00)
        return self

    def rel16(self, name):
        self.fixups.append(('rel16', len(self.code), name))
        self.code += b'\x00\x00'
        return self

    def label(self, name):
        assert name not in self.labels, 'duplicate label ' + name
        self.labels[name] = ORG + len(self.code)
        return self

    def finish(self):
        for kind, off, name in self.fixups:
            if name not in self.labels:
                raise KeyError('undefined label: ' + name)
            target = self.labels[name]
            if kind == 'abs16':
                self.code[off:off + 2] = struct.pack('<H', target)
            elif kind == 'rel16':
                delta = target - (ORG + off + 2)
                if not -32768 <= delta <= 32767:
                    raise ValueError('rel16 out of range for %s (%d)' % (name, delta))
                self.code[off:off + 2] = struct.pack('<h', delta)
            else:
                delta = target - (ORG + off + 1)
                if not -128 <= delta <= 127:
                    raise ValueError('rel8 out of range for %s (%d)' % (name, delta))
                self.code[off] = delta & 0xFF
        if len(self.code) % 2:
            self.code.append(0x90)
        if len(self.code) > 0xFF00:
            raise ValueError('COM program too large')
        return bytes(self.code)


# ---------------------------------------------------------------------------
# Common instruction helpers (all encodings spelled out on purpose)
# ---------------------------------------------------------------------------
def call16(a, name):
    """call near (E8 rel16) to a label."""
    a.raw(b'\xE8'); return a.rel16(name)


def jmp16(a, name):
    """jmp near (E9 rel16) to a label."""
    a.raw(b'\xE9'); return a.rel16(name)


def jcc16(a, cc, name):
    """Emit a conditional jump that can reach any label.

    `jcc short +3 ; jmp near name` -- the inverse condition skips the near
    jump, so the reach is 32 KB instead of 128 bytes.
    """
    a.raw(bytes([0x70 | ((cc ^ 1) & 0x0F), 0x03]))
    return jmp16(a, name)


def prolog(a):
    a.raw(b'\x0E\x1F')                          # push cs ; pop ds


def int21(a):
    return a.raw(b'\xCD\x21')


def puts(a, label):
    """mov dx, label ; mov ah,9 ; int 21h"""
    a.raw(b'\xBA'); a.addr(label)
    a.raw(b'\xB4\x09'); int21(a)


def putc(a, ch):
    """mov dl, ch ; mov ah,2 ; int 21h"""
    a.raw(bytes([0xB2, ch & 0xFF, 0xB4, 0x02])); int21(a)


def mov_abs8(a, name):
    a.raw(b'\xA0'); return a.addr(name)


def store_al(a, name):
    a.raw(b'\xA2'); return a.addr(name)


def store_ax(a, name):
    a.raw(b'\xA3'); return a.addr(name)


def load_ax(a, name):
    a.raw(b'\xA1'); return a.addr(name)


def load_bx(a, name):
    a.raw(b'\x8B\x1E'); return a.addr(name)


def load_si(a, name):
    a.raw(b'\x8B\x36'); return a.addr(name)


def store_si(a, name):
    a.raw(b'\x89\x36'); return a.addr(name)


def imm16(a, opcode, value):
    a.raw(bytes([opcode, value & 0xFF, (value >> 8) & 0xFF]))


def exit_dos(a, code=0):
    imm16(a, 0xB8, 0x4C00 | (code & 0xFF))
    int21(a)


def newline_routine(a):
    a.label('newline')
    putc(a, 0x0D)
    putc(a, 0x0A)
    a.raw(b'\xC3')                              # ret


def putdec_routine(a):
    """PUTDEC: print AX as unsigned decimal.  Clobbers AX, BX, CX, DX."""
    a.label('putdec')
    imm16(a, 0xBB, 10)                          # mov bx, 10
    a.raw(b'\x31\xC9')                          # xor cx, cx
    a.label('pd_div')
    a.raw(b'\x31\xD2')                          # xor dx, dx
    a.raw(b'\xF7\xF3')                          # div bx
    a.raw(b'\x52')                              # push dx
    a.raw(b'\x41')                              # inc cx
    a.raw(b'\x85\xC0')                          # test ax, ax
    jcc16(a, 0x5, 'pd_div')            # jnz pd_div
    a.label('pd_out')
    a.raw(b'\x5A')                              # pop dx
    a.raw(b'\x80\xC2\x30')                      # add dl, '0'
    a.raw(b'\xB4\x02'); int21(a)                # mov ah,2 ; int 21h
    a.raw(b'\xE2'); a.rel8('pd_out')            # loop pd_out
    a.raw(b'\xC3')                              # ret


def put2_routine(a):
    """PUT2: print AL as two zero-padded decimal digits.  Clobbers AX, BX, DX."""
    a.label('put2')
    a.raw(b'\xB4\x00')                          # mov ah, 0
    a.raw(b'\xB3\x0A')                          # mov bl, 10
    a.raw(b'\xF6\xF3')                          # div bl  (AL=tens, AH=ones)
    a.raw(b'\x50')                              # push ax
    a.raw(b'\x88\xC2')                          # mov dl, al
    a.raw(b'\x80\xC2\x30')                      # add dl, '0'
    a.raw(b'\xB4\x02'); int21(a)
    a.raw(b'\x58')                              # pop ax
    a.raw(b'\x88\xE2')                          # mov dl, ah
    a.raw(b'\x80\xC2\x30')                      # add dl, '0'
    a.raw(b'\xB4\x02'); int21(a)
    a.raw(b'\xC3')                              # ret


def puthex_routine(a):
    """PUTHEX: print AL as two uppercase hex digits.  Clobbers AX, BX, DX."""
    a.label('puthex')
    a.raw(b'\x50')                              # push ax
    for _ in range(4):
        a.raw(b'\xD0\xE8')                      # shr al, 1  (x4 -> high nibble)
    call16(a, 'puthex_nib')        # call puthex_nib
    a.raw(b'\x58')                              # pop ax
    a.raw(b'\x24\x0F')                          # and al, 0x0F
    call16(a, 'puthex_nib')        # call puthex_nib
    a.raw(b'\xC3')                              # ret
    a.label('puthex_nib')
    a.raw(b'\xBB'); a.addr('hexdigits')         # mov bx, hexdigits
    a.raw(b'\xD7')                              # xlat
    a.raw(b'\x88\xC2')                          # mov dl, al
    a.raw(b'\xB4\x02'); int21(a)
    a.raw(b'\xC3')                              # ret
    a.label('hexdigits')
    a.raw(b'0123456789ABCDEF')


def puthex16_routine(a):
    """PUTHEX16: print AX as four uppercase hex digits.  Clobbers AX, BX, DX."""
    a.label('puthex16')
    a.raw(b'\x50')                              # push ax
    a.raw(b'\x88\xE0')                          # mov al, ah
    call16(a, 'puthex')
    a.raw(b'\x58')                              # pop ax
    call16(a, 'puthex')
    a.raw(b'\xC3')


def getname_routine(a, namebuf):
    """COPYNAME: first token of the command tail -> NAMEBUF (NUL terminated).

    On return BL=1 when a name was found, BL=0 when the tail was empty.
    Clobbers AX, CX, SI, DI, BL.
    """
    a.label('copyname')
    a.raw(b'\xBE\x81\x00')                      # mov si, 0x81
    a.raw(b'\xA0\x80\x00')                      # mov al, [0x0080]
    a.raw(b'\x88\xC1')                          # mov cl, al   (tail length)
    a.raw(b'\xBF'); a.addr(namebuf)             # mov di, namebuf
    a.label('cn_skip')
    a.raw(b'\x84\xC9')                          # test cl, cl
    jcc16(a, 0x4, 'cn_empty')          # jz cn_empty
    a.raw(b'\x8A\x04')                          # mov al, [si]
    a.raw(b'\x3C\x20')                          # cmp al, ' '
    jcc16(a, 0x4, 'cn_more')           # jz cn_more
    a.raw(b'\x3C\x09')                          # cmp al, TAB
    jcc16(a, 0x5, 'cn_copy')           # jne cn_copy
    a.label('cn_more')
    a.raw(b'\x46')                              # inc si
    a.raw(b'\xFE\xC9')                          # dec cl
    jmp16(a, 'cn_skip')           # jmp cn_skip
    a.label('cn_copy')
    a.raw(b'\x8A\x04')                          # mov al, [si]
    a.raw(b'\x3C\x0D')                          # cmp al, CR
    jcc16(a, 0x4, 'cn_done')           # jz cn_done
    a.raw(b'\x3C\x00')                          # cmp al, 0
    jcc16(a, 0x4, 'cn_done')           # jz cn_done
    a.raw(b'\x88\x05')                          # mov [di], al
    a.raw(b'\x46')                              # inc si
    a.raw(b'\x47')                              # inc di
    a.raw(b'\xFE\xC9')                          # dec cl
    jcc16(a, 0x5, 'cn_copy')           # jnz cn_copy
    a.label('cn_done')
    a.raw(b'\xC6\x05\x00')                      # mov byte [di], 0
    a.raw(b'\xC6\x45\x01\x24')                  # mov byte [di+1], '$'
    a.raw(b'\xB3\x01')                          # mov bl, 1
    a.raw(b'\xC3')                              # ret
    a.label('cn_empty')
    a.raw(b'\xC6\x05\x00')                      # mov byte [di], 0
    a.raw(b'\xC6\x45\x01\x24')                  # mov byte [di+1], '$'
    a.raw(b'\x30\xDB')                          # xor bl, bl
    a.raw(b'\xC3')                              # ret


def putname_routine(a, namebuf):
    """PUTNAME: print the NUL terminated file name in NAMEBUF (AH=02 loop)."""
    a.label('putname')
    a.raw(b'\xBE'); a.addr(namebuf)             # mov si, namebuf
    a.label('pn_loop')
    a.raw(b'\x8A\x04')                          # mov al, [si]
    a.raw(b'\x84\xC0')                          # test al, al
    jcc16(a, 0x4, 'pn_done')                    # jz pn_done
    a.raw(b'\x88\xC2')                          # mov dl, al
    a.raw(b'\xB4\x02'); int21(a)
    a.raw(b'\x46')                              # inc si
    jmp16(a, 'pn_loop')
    a.label('pn_done')
    a.raw(b'\xC3')                              # ret


def data(a, label, payload, size=None):
    a.label(label)
    a.raw(payload if size is None else payload.ljust(size, b'\x00'))
    return a


# ---------------------------------------------------------------------------
# INFO.COM
# ---------------------------------------------------------------------------
def build_info():
    a = Asm()
    prolog(a)
    puts(a, 'title')

    # --- DOS version ---------------------------------------------------
    puts(a, 'l_version')
    imm16(a, 0xB8, 0x3000); int21(a)            # AL=major, AH=minor
    a.raw(b'\x88\xE3')                          # mov bl, ah  (minor)
    a.raw(b'\x88\xC2')                          # mov dl, al  (major)
    a.raw(b'\x80\xC2\x30')                      # add dl, '0'
    a.raw(b'\xB4\x02'); int21(a)
    putc(a, 0x2E)                               # '.'
    a.raw(b'\x88\xD8')                          # mov al, bl
    call16(a, 'put2')
    call16(a, 'newline')

    # --- date ----------------------------------------------------------
    puts(a, 'l_date')
    a.raw(b'\xB4\x2A'); int21(a)                # CX=year DH=month DL=day
    a.raw(b'\x89\x0E'); a.addr('g_year')        # mov [g_year], cx
    a.raw(b'\x88\xF0'); store_al(a, 'g_mon')    # mov al, dh
    a.raw(b'\x88\xD0'); store_al(a, 'g_day')    # mov al, dl
    load_ax(a, 'g_year')
    call16(a, 'putdec')
    putc(a, 0x2D)
    mov_abs8(a, 'g_mon')
    call16(a, 'put2')
    putc(a, 0x2D)
    mov_abs8(a, 'g_day')
    call16(a, 'put2')
    call16(a, 'newline')

    # --- time ----------------------------------------------------------
    puts(a, 'l_time')
    a.raw(b'\xB4\x2C'); int21(a)                # CH=hour CL=min DH=sec
    a.raw(b'\x88\xC8'); store_al(a, 'g_min')    # mov al, cl
    a.raw(b'\x88\xF0'); store_al(a, 'g_sec')    # mov al, dh
    a.raw(b'\x88\xE8')                          # mov al, ch
    call16(a, 'put2')
    putc(a, 0x3A)
    mov_abs8(a, 'g_min')
    call16(a, 'put2')
    putc(a, 0x3A)
    mov_abs8(a, 'g_sec')
    call16(a, 'put2')
    call16(a, 'newline')

    # --- current drive -------------------------------------------------
    puts(a, 'l_drive')
    a.raw(b'\xB4\x19'); int21(a)                # AL = current drive
    a.raw(b'\x04\x41')                          # add al, 'A'
    a.raw(b'\x88\xC2')                          # mov dl, al
    a.raw(b'\xB4\x02'); int21(a)
    putc(a, 0x3A)
    call16(a, 'newline')

    # --- current directory ---------------------------------------------
    puts(a, 'l_cwd')
    a.raw(b'\xB2\x00')                          # mov dl, 0 (current drive)
    a.raw(b'\xBE'); a.addr('dirbuf')            # mov si, dirbuf
    a.raw(b'\xB4\x47'); int21(a)                # get current directory
    # AH=47h returns a NUL terminated string; AH=09 wants '$', so re-terminate
    a.raw(b'\xA0'); a.addr('dirbuf')            # mov al, [dirbuf]  (keep DS)
    a.raw(b'\xBF'); a.addr('dirbuf')            # mov di, dirbuf
    a.label('dir_scan')
    a.raw(b'\x8A\x05')                          # mov al, [di]
    a.raw(b'\x84\xC0')                          # test al, al
    jcc16(a, 0x4, 'dir_done')                     # jz dir_done
    a.raw(b'\x47')                               # inc di
    jmp16(a, 'dir_scan')
    a.label('dir_done')
    a.raw(b'\xC6\x05\x24')                      # mov byte [di], '$'
    puts(a, 'dirbuf')
    call16(a, 'newline')

    # --- native API probe ---------------------------------------------
    puts(a, 'l_api')
    imm16(a, 0xB8, 0x5544)                      # mov ax, 5544h
    imm16(a, 0xBA, 0x0001)                      # mov dx, 1
    a.raw(b'\xCD\x2F')                          # int 2Fh
    # AX = 0FFFFh means "no native multiplex"; any other value is a version
    imm16(a, 0xB9, 0xFFFF)                      # mov cx, 0FFFFh
    a.raw(b'\x39\xC8')                          # cmp ax, cx
    jcc16(a, 0x5, 'api_yes')           # jne api_yes
    puts(a, 's_apino')
    call16(a, 'newline')
    jmp16(a, 'info_end')
    a.label('api_yes')
    a.raw(b'\x53')                              # push bx (puthex clobbers BX)
    a.raw(b'\x88\xE0')                          # mov al, ah  (version high byte)
    call16(a, 'puthex')
    a.raw(b'\x5B')                              # pop bx
    putc(a, 0x2F)                               # '/'
    a.raw(b'\xB4\x00')                          # mov ah, 0
    a.raw(b'\x88\xD8')                          # mov al, bl (pointer size)
    a.raw(b'\xB3\x08')                          # mov bl, 8
    a.raw(b'\xF6\xE3')                          # mul bl -> AX = bits (64/32)
    call16(a, 'putdec')
    puts(a, 's_bits')
    call16(a, 'newline')
    a.label('info_end')
    call16(a, 'newline')
    exit_dos(a)

    newline_routine(a)
    putdec_routine(a)
    put2_routine(a)
    puthex_routine(a)

    data(a, 'title', b'\r\n=== UEFI-DOS system info ===\r\n$')
    data(a, 'l_version', b'DOS version : $')
    data(a, 'l_date', b'Date        : $')
    data(a, 'l_time', b'Time        : $')
    data(a, 'l_drive', b'Drive       : $')
    data(a, 'l_cwd', b'Directory   : $')
    data(a, 'l_api', b'Native API  : $')
    data(a, 's_apino', b'not present (plain DOS)$')
    data(a, 's_bits', b'-bit pointers$')
    data(a, 'dirbuf', b'', 68)
    for lbl in ('g_mon', 'g_day', 'g_min', 'g_sec'):
        data(a, lbl, b'', 1)
    data(a, 'g_year', b'', 2)
    return a.finish()


# ---------------------------------------------------------------------------
# CLOCK.COM
# ---------------------------------------------------------------------------
def build_clock():
    a = Asm()
    prolog(a)
    puts(a, 'head')
    a.label('clk_loop')
    putc(a, 0x0D)                               # CR: redraw on the same line
    a.raw(b'\xB4\x2C'); int21(a)                # CH=hour CL=min DH=sec
    a.raw(b'\x88\xC8'); store_al(a, 'g_min')
    a.raw(b'\x88\xF0'); store_al(a, 'g_sec')
    a.raw(b'\x88\xE8')                          # mov al, ch
    call16(a, 'put2')
    putc(a, 0x3A)
    mov_abs8(a, 'g_min')
    call16(a, 'put2')
    putc(a, 0x3A)
    mov_abs8(a, 'g_sec')
    call16(a, 'put2')
    a.raw(b'\xB4\x0B'); int21(a)                # check keyboard status
    a.raw(b'\x3C\x00')                          # cmp al, 0
    jcc16(a, 0x4, 'clk_loop')          # jz clk_loop
    a.raw(b'\xB4\x08'); int21(a)                # consume the key
    puts(a, 'bye')
    exit_dos(a)

    put2_routine(a)
    data(a, 'head', b'UEFI-DOS clock -- press any key to stop\r\n\r\n$')
    data(a, 'bye', b'\r\nclock stopped.\r\n$')
    for lbl in ('g_min', 'g_sec'):
        data(a, lbl, b'', 1)
    return a.finish()


# ---------------------------------------------------------------------------
# HEX.COM <file>
# ---------------------------------------------------------------------------
def build_hex():
    a = Asm()
    prolog(a)
    call16(a, 'copyname')
    a.raw(b'\x84\xDB')                          # test bl, bl
    jcc16(a, 0x5, 'hx_open')           # jnz hx_open
    puts(a, 'usage')
    exit_dos(a, 1)

    a.label('hx_open')
    a.raw(b'\xBA'); a.addr('namebuf')           # mov dx, namebuf
    imm16(a, 0xB8, 0x3D00); int21(a)            # open read-only
    jcc16(a, 0x3, 'hx_ok')             # jnc hx_ok
    puts(a, 'nofile')
    exit_dos(a, 2)
    a.label('hx_ok')
    store_ax(a, 'handle')
    imm16(a, 0xB8, 0x0000); store_ax(a, 'offset')  # offset = 0

    a.label('hx_read')
    load_bx(a, 'handle')
    imm16(a, 0xB9, 16)                          # mov cx, 16
    a.raw(b'\xBA'); a.addr('buf')               # mov dx, buf
    a.raw(b'\xB4\x3F'); int21(a)                # read
    a.raw(b'\x3D\x00\x00')                      # cmp ax, 0
    jcc16(a, 0x4, 'hx_done')           # jz hx_done
    store_ax(a, 'rem')                          # bytes in this block
    a.raw(b'\xC6\x06'); a.addr('idx'); a.raw(b'\x00')   # mov byte [idx], 0

    # offset column
    load_ax(a, 'offset')
    call16(a, 'puthex16')
    putc(a, 0x3A)
    putc(a, 0x20)

    # hex bytes
    a.raw(b'\xBE'); a.addr('buf')               # mov si, buf
    store_si(a, 'ptr')
    a.label('hx_hexloop')
    a.raw(b'\xA0'); a.addr('idx')               # mov al, [idx]
    a.raw(b'\x3A\x06'); a.addr('rem')           # cmp al, [rem]
    jcc16(a, 0x3, 'hx_hexdone')        # jae hx_hexdone
    load_si(a, 'ptr')
    a.raw(b'\x8A\x04')                          # mov al, [si]
    call16(a, 'puthex')
    putc(a, 0x20)
    a.raw(b'\x46')                              # inc si
    store_si(a, 'ptr')
    a.raw(b'\xFE\x06'); a.addr('idx')           # inc byte [idx]
    jmp16(a, 'hx_hexloop')

    # pad the text column so short blocks still line up
    a.label('hx_hexdone')
    a.raw(b'\xB3\x10')                          # mov bl, 16
    a.raw(b'\xA0'); a.addr('rem')               # mov al, [rem]
    a.raw(b'\x28\xC3')                          # sub bl, al
    a.label('hx_pad')
    a.raw(b'\x84\xDB')                          # test bl, bl
    jcc16(a, 0x4, 'hx_ascii')
    puts(a, 's_blank')
    a.raw(b'\xFE\xCB')                          # dec bl
    jmp16(a, 'hx_pad')

    # ASCII column
    a.label('hx_ascii')
    a.raw(b'\xBE'); a.addr('buf')               # mov si, buf
    store_si(a, 'ptr')
    a.raw(b'\xC6\x06'); a.addr('idx'); a.raw(b'\x00')
    a.label('hx_aloop')
    a.raw(b'\xA0'); a.addr('idx')
    a.raw(b'\x3A\x06'); a.addr('rem')
    jcc16(a, 0x3, 'hx_lineend')        # jae hx_lineend
    load_si(a, 'ptr')
    a.raw(b'\x8A\x04')                          # mov al, [si]
    a.raw(b'\x3C\x20')                          # cmp al, ' '
    jcc16(a, 0x2, 'hx_dot')            # jb hx_dot
    a.raw(b'\x3C\x7F')                          # cmp al, 0x7F
    jcc16(a, 0x2, 'hx_emit')           # jb hx_emit
    a.label('hx_dot')
    a.raw(b'\xB0\x2E')                          # mov al, '.'
    a.label('hx_emit')
    a.raw(b'\x88\xC2')                          # mov dl, al
    a.raw(b'\xB4\x02'); int21(a)
    a.raw(b'\x46')                              # inc si
    store_si(a, 'ptr')
    a.raw(b'\xFE\x06'); a.addr('idx')
    jmp16(a, 'hx_aloop')

    a.label('hx_lineend')
    call16(a, 'newline')
    load_ax(a, 'offset')
    a.raw(b'\x05\x10\x00')                      # add ax, 16
    store_ax(a, 'offset')
    a.raw(b'\xA1'); a.addr('rem')               # mov ax, [rem]
    a.raw(b'\x3D\x10\x00')                      # cmp ax, 16
    jcc16(a, 0x4, 'hx_read')           # full block -> read more
    a.label('hx_done')
    load_bx(a, 'handle')
    a.raw(b'\xB4\x3E'); int21(a)                # close
    exit_dos(a)

    newline_routine(a)
    puthex_routine(a)
    puthex16_routine(a)
    getname_routine(a, 'namebuf')

    data(a, 'usage', b'usage: HEX <file>\r\n$')
    data(a, 'nofile', b'HEX: cannot open file\r\n$')
    data(a, 's_blank', b'   $')
    data(a, 'namebuf', b'', 68)
    data(a, 'buf', b'', 16)
    data(a, 'handle', b'', 2)
    data(a, 'offset', b'', 2)
    data(a, 'rem', b'', 2)
    data(a, 'ptr', b'', 2)
    data(a, 'idx', b'', 1)
    return a.finish()


# ---------------------------------------------------------------------------
# SUM.COM <file>
# ---------------------------------------------------------------------------
def build_sum():
    a = Asm()
    prolog(a)
    call16(a, 'copyname')
    a.raw(b'\x84\xDB')                          # test bl, bl
    jcc16(a, 0x5, 'sm_open')
    puts(a, 'usage')
    exit_dos(a, 1)

    a.label('sm_open')
    a.raw(b'\xBA'); a.addr('namebuf')
    imm16(a, 0xB8, 0x3D00); int21(a)
    jcc16(a, 0x3, 'sm_ok')
    puts(a, 'nofile')
    exit_dos(a, 2)
    a.label('sm_ok')
    store_ax(a, 'handle')

    # size = lseek(handle, 0, SEEK_END)
    load_bx(a, 'handle')
    a.raw(b'\x31\xC9')                          # xor cx, cx
    a.raw(b'\x31\xD2')                          # xor dx, dx
    imm16(a, 0xB8, 0x4202); int21(a)
    store_ax(a, 'size')
    # rewind to the start
    load_bx(a, 'handle')
    a.raw(b'\x31\xC9')
    a.raw(b'\x31\xD2')
    imm16(a, 0xB8, 0x4200); int21(a)

    a.raw(b'\x31\xC0'); store_ax(a, 'cksum')    # checksum = 0
    a.label('sm_read')
    load_bx(a, 'handle')
    imm16(a, 0xB9, 256)
    a.raw(b'\xBA'); a.addr('buf')
    a.raw(b'\xB4\x3F'); int21(a)
    a.raw(b'\x3D\x00\x00')                      # cmp ax, 0
    jcc16(a, 0x4, 'sm_done')
    store_ax(a, 'rem')
    a.raw(b'\xC6\x06'); a.addr('idx'); a.raw(b'\x00')
    a.raw(b'\xBE'); a.addr('buf')
    store_si(a, 'ptr')
    a.label('sm_sum')
    a.raw(b'\xA0'); a.addr('idx')               # mov al, [idx]
    a.raw(b'\x3A\x06'); a.addr('rem')           # cmp al, [rem]
    jcc16(a, 0x3, 'sm_read')           # jae sm_read
    load_si(a, 'ptr')
    a.raw(b'\x8A\x04')                          # mov al, [si]
    a.raw(b'\x30\xE4')                          # xor ah, ah
    a.raw(b'\x03\x06'); a.addr('cksum')         # add ax, [cksum]
    store_ax(a, 'cksum')
    a.raw(b'\x46')                              # inc si
    store_si(a, 'ptr')
    a.raw(b'\xFE\x06'); a.addr('idx')
    jmp16(a, 'sm_sum')

    a.label('sm_done')
    load_bx(a, 'handle')
    a.raw(b'\xB4\x3E'); int21(a)                # close
    call16(a, 'putname')                        # file name (NUL terminated)
    putc(a, 0x3A)                               # ':'
    putc(a, 0x20)
    load_ax(a, 'size')
    call16(a, 'putdec')
    puts(a, 's_bytes')
    load_ax(a, 'cksum')
    call16(a, 'puthex16')
    call16(a, 'newline')
    exit_dos(a)

    newline_routine(a)
    putdec_routine(a)
    puthex_routine(a)
    puthex16_routine(a)
    getname_routine(a, 'namebuf')
    putname_routine(a, 'namebuf')

    data(a, 'usage', b'usage: SUM <file>\r\n$')
    data(a, 'nofile', b'SUM: cannot open file\r\n$')
    data(a, 's_bytes', b' bytes, checksum $')
    data(a, 'namebuf', b'', 68)
    data(a, 'buf', b'', 256)
    data(a, 'handle', b'', 2)
    data(a, 'size', b'', 2)
    data(a, 'cksum', b'', 2)
    data(a, 'rem', b'', 2)
    data(a, 'ptr', b'', 2)
    data(a, 'idx', b'', 1)
    return a.finish()


# ---------------------------------------------------------------------------
# CAL.COM
# ---------------------------------------------------------------------------
MONTHS = [b'January  $', b'February $', b'March    $', b'April    $',
          b'May      $', b'June     $', b'July     $', b'August   $',
          b'September$', b'October  $', b'November $', b'December $']
MONTH_STRIDE = 10          # every entry above is exactly 10 bytes, '$' included
assert all(len(m) == MONTH_STRIDE for m in MONTHS), 'month table stride mismatch'


def build_cal():
    a = Asm()
    prolog(a)
    a.raw(b'\xB4\x2A'); int21(a)                # AL=dow CX=year DH=month DL=day
    store_al(a, 'dow')
    a.raw(b'\x88\xF0'); store_al(a, 'mon')      # mov al, dh
    a.raw(b'\x88\xD0'); store_al(a, 'day')      # mov al, dl
    a.raw(b'\x89\x0E'); a.addr('year')          # mov [year], cx

    # --- days in month -------------------------------------------------
    mov_abs8(a, 'mon')
    a.raw(b'\x2C\x01')                          # sub al, 1
    a.raw(b'\xBB'); a.addr('mdays')             # mov bx, mdays
    a.raw(b'\xD7')                              # xlat
    store_al(a, 'dcount')
    mov_abs8(a, 'mon')
    a.raw(b'\x3C\x02')                          # cmp al, 2
    jcc16(a, 0x5, 'cal_days')          # jne cal_days
    # February: leap if year%4==0 and (year%100!=0 or year%400==0)
    load_ax(a, 'year')
    a.raw(b'\x31\xD2')                          # xor dx, dx
    imm16(a, 0xBB, 4)                           # mov bx, 4
    a.raw(b'\xF7\xF3')                          # div bx
    a.raw(b'\x85\xD2')                          # test dx, dx
    jcc16(a, 0x5, 'cal_days')          # jne cal_days  (not /4)
    load_ax(a, 'year')
    a.raw(b'\x31\xD2')
    imm16(a, 0xBB, 100)
    a.raw(b'\xF7\xF3')
    a.raw(b'\x85\xD2')                          # test dx, dx
    jcc16(a, 0x5, 'cal_leap')          # jne cal_leap  (not a century)
    load_ax(a, 'year')
    a.raw(b'\x31\xD2')
    imm16(a, 0xBB, 400)
    a.raw(b'\xF7\xF3')
    a.raw(b'\x85\xD2')
    jcc16(a, 0x5, 'cal_days')          # not /400 -> 28 days
    a.label('cal_leap')
    a.raw(b'\xB0\x1D'); store_al(a, 'dcount')   # 29
    a.label('cal_days')

    # --- first weekday = (dow + 1 - day) mod 7 -------------------------
    mov_abs8(a, 'dow')
    a.raw(b'\x04\x01')                          # add al, 1
    a.raw(b'\x2A\x06'); a.addr('day')           # sub al, [day]
    a.label('cal_norm')
    a.raw(b'\x3C\x80')                          # cmp al, 0x80
    jcc16(a, 0x2, 'cal_norm_ok')       # jb cal_norm_ok (non-negative)
    a.raw(b'\x04\x07')                          # add al, 7
    jmp16(a, 'cal_norm')
    a.label('cal_norm_ok')
    store_al(a, 'first')

    # --- header --------------------------------------------------------
    call16(a, 'newline')
    puts(a, 's_indent')
    mov_abs8(a, 'mon')
    a.raw(b'\x2C\x01')                          # sub al, 1
    a.raw(bytes([0xB3, MONTH_STRIDE]))          # mov bl, 10 (table stride)
    a.raw(b'\xF6\xE3')                          # mul bl -> AX = idx*stride
    a.raw(b'\xBB'); a.addr('mnames')            # mov bx, mnames
    a.raw(b'\x01\xC3')                          # add bx, ax
    a.raw(b'\x89\xDA')                          # mov dx, bx
    a.raw(b'\xB4\x09'); int21(a)
    puts(a, 's_year')
    load_ax(a, 'year')
    call16(a, 'putdec')
    call16(a, 'newline')
    puts(a, 's_dow')

    # --- leading blanks ------------------------------------------------
    mov_abs8(a, 'first')
    store_al(a, 'n')
    a.label('cal_pad')
    mov_abs8(a, 'n')
    a.raw(b'\x84\xC0')                          # test al, al
    jcc16(a, 0x4, 'cal_days_start')    # jz
    puts(a, 's_blank')
    a.raw(b'\xA0'); a.addr('n')                 # mov al, [n]
    a.raw(b'\x2C\x01')                          # sub al, 1
    store_al(a, 'n')
    jmp16(a, 'cal_pad')

    # --- day numbers ---------------------------------------------------
    a.label('cal_days_start')
    a.raw(b'\xB0\x01'); store_al(a, 'd')        # day = 1
    a.label('cal_dayloop')
    mov_abs8(a, 'd')
    a.raw(b'\x3A\x06'); a.addr('dcount')        # cmp al, [dcount]
    jcc16(a, 0x7, 'cal_end')           # ja cal_end
    mov_abs8(a, 'd')
    call16(a, 'putday')
    puts(a, 's_gap')
    # advance the weekday, wrapping to a new line
    a.raw(b'\xA0'); a.addr('first')
    a.raw(b'\x04\x01')                          # add al, 1
    store_al(a, 'first')
    a.raw(b'\x3C\x07')                          # cmp al, 7
    jcc16(a, 0x5, 'cal_nodwrap')       # jne cal_nodwrap
    a.raw(b'\x30\xC0'); store_al(a, 'first')    # xor al, al
    call16(a, 'newline')
    a.label('cal_nodwrap')
    a.raw(b'\xA0'); a.addr('d')
    a.raw(b'\x04\x01')                          # add al, 1
    store_al(a, 'd')
    jmp16(a, 'cal_dayloop')
    a.label('cal_end')
    call16(a, 'newline')
    call16(a, 'newline')
    exit_dos(a)

    newline_routine(a)
    putdec_routine(a)

    # PUTDAY: AL = 1..31, print it right aligned in two columns as DIGITS
    a.label('putday')
    a.raw(b'\xB4\x00')                          # mov ah, 0
    a.raw(b'\xB3\x0A')                          # mov bl, 10
    a.raw(b'\xF6\xF3')                          # div bl -> AL=tens, AH=ones
    a.raw(b'\x50')                              # push ax
    a.raw(b'\x88\xC2')                          # mov dl, al
    a.raw(b'\x84\xD2')                          # test dl, dl
    jcc16(a, 0x5, 'pd_tens')           # jnz pd_tens
    putc(a, 0x20)                               # leading space for 1..9
    jmp16(a, 'pd_first')
    a.label('pd_tens')
    a.raw(b'\x80\xC2\x30')                      # add dl, '0'
    a.label('pd_first')
    a.raw(b'\xB4\x02'); int21(a)                # print tens
    a.raw(b'\x58')                              # pop ax
    a.raw(b'\x88\xE2')                          # mov dl, ah (ones)
    a.raw(b'\x80\xC2\x30')                      # add dl, '0'
    a.raw(b'\xB4\x02'); int21(a)
    a.raw(b'\xC3')

    data(a, 'mnames', b''.join(MONTHS))
    data(a, 'mdays', bytes([31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]))
    data(a, 's_indent', b'      $')
    data(a, 's_year', b' $')
    data(a, 's_dow', b'Su Mo Tu We Th Fr Sa\r\n$')
    data(a, 's_gap', b' $')                     # one space between day cells
    data(a, 's_blank', b'   $')
    for lbl in ('dow', 'mon', 'day', 'dcount', 'first', 'n', 'd'):
        data(a, lbl, b'', 1)
    data(a, 'year', b'', 2)
    return a.finish()


# ---------------------------------------------------------------------------
def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(outdir, exist_ok=True)
    programs = [
        ('INFO.COM', build_info),
        ('CLOCK.COM', build_clock),
        ('HEX.COM', build_hex),
        ('SUM.COM', build_sum),
        ('CAL.COM', build_cal),
    ]
    for name, fn in programs:
        blob = fn()
        with open(os.path.join(outdir, name), 'wb') as f:
            f.write(blob)
        print('%-10s %5d bytes' % (name, len(blob)))


if __name__ == '__main__':
    main()
