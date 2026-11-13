#!/usr/bin/env python3
"""Build FINDTEST.COM: exercise INT 21h AH=4Eh/4Fh exactly the way Windows 1.x
does, and print what the *caller* sees.

Windows 1.01's KERNEL wrapper calls DOS find-next in a loop, re-pointing the DTA
at the same buffer before every call and copying the 22-byte result area out.
With UEFI-DOS the loop never stops at the end of the directory: the kernel keeps
asking for the next entry and writing 22-byte records past the end of its buffer
until memory runs out ("Not enough memory to display entire directory").

This probe answers the question that decides whether the bug is on the DOS side:
after the last entry, what exactly does the caller get back?

  * how many entries the loop yields before the first failure
  * the AX the failing call returns
  * the 43-byte DTA content after the failure (does DOS leave the previous
    entry there, or clear it?)
  * whether a later find-next keeps failing

Run it under a real DOS and under UEFI-DOS and diff the two reports: any
difference in the number of entries, the error code or the DTA bytes is a
compatibility bug on our side.

Usage:  python make_find_test.py <output-dir> [pattern]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from make_tools import (Asm, call16, data, exit_dos, imm16, int21, jcc16,
                        jmp16, load_ax, newline_routine, prolog, putc,
                        putdec_routine, puthex_routine, puts, store_ax)  # noqa: E402

MAX_ENTRIES = 600


def set_dta(a):
    """mov dx, dta ; mov ah, 1Ah ; int 21h"""
    a.raw(b'\xBA'); a.addr('dta')
    a.raw(b'\xB4\x1A'); int21(a)


def find_call(a, ah, attr):
    """mov dx, spec ; mov cx, attr ; mov ah,<ah> ; int 21h"""
    a.raw(b'\xBA'); a.addr('spec')
    imm16(a, 0xB9, attr)                     # mov cx, attr
    a.raw(bytes([0xB4, ah])); int21(a)


_dump_n = [0]


def dump_dta(a):
    """print 43 bytes of the DTA as hex, space separated"""
    _dump_n[0] += 1
    lbl = 'dd_loop%d' % _dump_n[0]
    a.raw(b'\xBE'); a.addr('dta')            # mov si, dta
    imm16(a, 0xB9, 43)                       # mov cx, 43
    a.label(lbl)
    a.raw(b'\x8A\x04')                       # mov al, [si]
    call16(a, 'puthex')
    putc(a, 0x20)
    a.raw(b'\x46')                           # inc si
    a.raw(b'\xE2'); a.rel8(lbl)              # loop dd_loop
    call16(a, 'newline')


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
    pattern = sys.argv[2] if len(sys.argv) > 2 else 'C:\\*.*'
    os.makedirs(outdir, exist_ok=True)

    a = Asm()
    prolog(a)

    # ---- first entry ---------------------------------------------------
    set_dta(a)
    find_call(a, 0x4E, 0x30)
    jcc16(a, 0x3, 'ft_first_ok')             # jnc ft_first_ok
    puts(a, 's_firstfail')
    a.raw(b'\x89\xC1')                       # mov cx, ax
    call16(a, 'puthex16')                    # AX (error code in AL)
    call16(a, 'newline')
    puts(a, 's_dta')
    dump_dta(a)
    exit_dos(a, 1)

    a.label('ft_first_ok')
    imm16(a, 0xB8, 1)                        # mov ax, 1
    store_ax(a, 'count')                     # count = 1 (the first hit)

    # ---- walk to the end ------------------------------------------------
    a.label('ft_next')
    set_dta(a)                               # Windows re-points the DTA each time
    find_call(a, 0x4F, 0x0000)
    jcc16(a, 0x3, 'ft_got')                  # jnc ft_got
    store_ax(a, 'err1')                      # remember the failing AX
    jmp16(a, 'ft_end')
    a.label('ft_got')
    a.raw(b'\xFF\x06'); a.addr('count')      # inc word [count]
    a.raw(b'\x81\x3E'); a.addr('count')
    a.raw(b'\x58\x02')                       # cmp word [count], 600
    jcc16(a, 0x2, 'ft_next')                 # jb ft_next
    imm16(a, 0xB8, 0xFFFF)
    store_ax(a, 'err1')                      # never failed: marker

    a.label('ft_end')
    call16(a, 'newline')
    puts(a, 's_count')
    load_ax(a, 'count')
    call16(a, 'putdec')
    call16(a, 'newline')
    puts(a, 's_err')
    load_ax(a, 'err1')
    call16(a, 'puthex16')
    call16(a, 'newline')
    puts(a, 's_dta')
    dump_dta(a)

    # ---- two more find-next calls after the end ------------------------
    imm16(a, 0xB8, 1)
    store_ax(a, 'round')                     # round = 1
    a.label('ft_more')
    puts(a, 's_more')
    load_ax(a, 'round')
    call16(a, 'putdec')
    putc(a, 0x20)
    set_dta(a)
    find_call(a, 0x4F, 0x0000)
    store_ax(a, 'err2')                      # AX after the call (CF aside)
    load_ax(a, 'err2')
    call16(a, 'puthex16')
    call16(a, 'newline')
    puts(a, 's_dta')
    dump_dta(a)
    a.raw(b'\xFF\x06'); a.addr('round')      # inc word [round]
    a.raw(b'\x81\x3E'); a.addr('round')
    a.raw(b'\x03\x00')                       # cmp word [round], 3
    jcc16(a, 0x2, 'ft_more')                 # jb ft_more

    exit_dos(a)

    newline_routine(a)
    putdec_routine(a)
    puthex_routine(a)

    # puthex16 lives in make_tools but needs the local hexdigits table
    a.label('puthex16')
    a.raw(b'\x50')                           # push ax
    a.raw(b'\x88\xE0')                       # mov al, ah
    call16(a, 'puthex')
    a.raw(b'\x58')                           # pop ax
    call16(a, 'puthex')
    a.raw(b'\xC3')

    data(a, 'spec', pattern.encode() + b'\x00')
    data(a, 'dta', b'', 128)
    data(a, 'count', b'', 2)
    data(a, 'err1', b'', 2)
    data(a, 'err2', b'', 2)
    data(a, 'round', b'', 2)
    data(a, 's_firstfail', b'findfirst failed AX=$')
    data(a, 's_count', b'entries=$')
    data(a, 's_err', b'first failure AX=$')
    data(a, 's_dta', b'dta=$')
    data(a, 's_more', b'after-end call $')

    blob = a.finish()
    path = os.path.join(outdir, 'FINDTEST.COM')
    with open(path, 'wb') as f:
        f.write(blob)
    print('%-14s %5d bytes' % ('FINDTEST.COM', len(blob)))


if __name__ == '__main__':
    main()
