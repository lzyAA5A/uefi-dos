#!/usr/bin/env python3
"""Generate DOS4TEST.COM -- a DOS 4.0 real-mode conformance probe.

The program exercises the kernel surfaces that MS-DOS 4.0 added or made
mandatory and prints one line per check:

  version      DOS version word (AH=30h) is 4.00
  dpb-*        AH=32h returns a Drive Parameter Block for the default drive
  lol-*        AH=52h returns a list-of-lists whose DPB chain is non-empty
  exterr-*     AH=59h reports the class of the previous failure
  6c-* / 68h   AH=6Ch extended open (create/open/replace/refuse) and AH=68h
  5a-*         AH=5Ah creates a unique temporary file (and we delete it again)
  60h-*        AH=60h canonicalises a name into a full path
  63h-*        AH=63h reports "no DBCS" (AL=0)

Every check prints "<tag>: OK" or "<tag>: FAIL"; the last line is
"DOS4-OK" (exit 0) or "DOS4-FAIL" (exit 1).

Two rules make this program reliable and are worth keeping if you extend it:

  * a check must never depend on a register that was live across another
    check -- the reporter prints through INT 21h and uses BX/SI, so results
    are stored to memory cells and re-read for each comparison;
  * CF is only meaningful immediately after the interrupt, so "check_cf"
    is emitted directly after the INT and before anything else.

Byte encodings are spelled out on purpose -- there is no assembler in the
tool chain, and a wrong encoding shows up as a stray line instead of a build
error.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from make_tools import (Asm, prolog, puts, putc, int21, imm16, exit_dos,
                        call16, jmp16, jcc16, data, newline_routine,
                        putdec_routine, put2_routine)

# ---------------------------------------------------------------------------
# tiny helpers shared by the checks
# ---------------------------------------------------------------------------
_ctr = [0]


def uniq(prefix):
    _ctr[0] += 1
    return '%s%d' % (prefix, _ctr[0])


def push_ds(a):
    """DS = CS again (INT 21h services are allowed to clobber DS)."""
    a.raw(b'\x0E\x1F')                      # push cs ; pop ds


def mov_mem_ax(a, label):
    a.raw(b'\xA3'); a.addr(label)           # mov [label], ax


def mov_mem_cx(a, label):
    a.raw(b'\x89\x0E'); a.addr(label)       # mov [label], cx


def ld_ax_mem(a, label):
    a.raw(b'\xA1'); a.addr(label)           # mov ax, [label]


def ld_al_mem(a, label):
    a.raw(b'\xA0'); a.addr(label)           # mov al, [label]


def ld_bx_mem(a, label):
    a.raw(b'\x8B\x1E'); a.addr(label)       # mov bx, [label]


def cmp_ax(a, value):
    a.raw(b'\x3D'); a.raw(struct.pack('<H', value & 0xFFFF))


def cmp_al(a, value):
    a.raw(b'\x3C'); a.raw(bytes([value & 0xFF]))


def flag_if(a, cc):
    """BL = 1 when the condition `cc` holds, else 0.  Nothing else changes."""
    ok = uniq('f')
    a.raw(b'\xB3\x01')                      # mov bl, 1
    jcc16(a, cc, ok)
    a.raw(b'\xB3\x00')                      # mov bl, 0
    a.label(ok)


def report(a, tag):
    a.raw(b'\xBE')
    a.addr('t_' + tag)                      # mov si, t_<tag>
    call16(a, 'report')


def check_cc(a, tag, cc):
    """Report the tag with pass = "condition cc holds"."""
    flag_if(a, cc)
    report(a, tag)


def check_cf(a, tag):
    """Pass when CF is clear -- emit directly after the interrupt."""
    check_cc(a, tag, 0x3)                   # jae


def check_ax_eq(a, tag, label, value):
    ld_ax_mem(a, label)
    cmp_ax(a, value)
    check_cc(a, tag, 0x4)                   # jz


def check_al_eq(a, tag, label, value):
    ld_al_mem(a, label)
    cmp_al(a, value)
    check_cc(a, tag, 0x4)                   # jz


def check_al_nz(a, tag, label):
    ld_al_mem(a, label)
    a.raw(b'\x84\xC0')                      # test al, al
    check_cc(a, tag, 0x5)                   # jnz


def check_ax_nz(a, tag, label):
    ld_ax_mem(a, label)
    a.raw(b'\x85\xC0')                      # test ax, ax
    check_cc(a, tag, 0x5)                   # jnz


def putz_routine(a):
    """PUTZ: print the NUL terminated string at SI (AH=02 loop, SI advances)."""
    a.label('putz')
    a.label('pz_loop')
    a.raw(b'\x8A\x04')                      # mov al, [si]
    a.raw(b'\x84\xC0')                      # test al, al
    jcc16(a, 0x4, 'pz_done')
    a.raw(b'\x88\xC2')                      # mov dl, al
    a.raw(b'\xB4\x02')                      # mov ah, 2
    int21(a)
    a.raw(b'\x46')                          # inc si
    jmp16(a, 'pz_loop')
    a.label('pz_done')
    a.raw(b'\xC3')                          # ret


def report_routine(a):
    """REPORT: BL = 1 pass / 0 fail, SI = tag.  Prints "<tag>: OK|FAIL"."""
    a.label('report')
    a.raw(b'\x53')                          # push bx
    a.raw(b'\x56')                          # push si
    call16(a, 'putz')
    a.raw(b'\xBA')
    a.addr('s_colon')
    a.raw(b'\xB4\x09')
    int21(a)                                # ": "
    a.raw(b'\x5E')                          # pop si
    a.raw(b'\x5B')                          # pop bx
    a.raw(b'\x84\xDB')                      # test bl, bl
    jcc16(a, 0x4, 'rp_bad')
    a.raw(b'\xBA')
    a.addr('s_ok')
    a.raw(b'\xB4\x09')
    int21(a)
    jmp16(a, 'rp_end')
    a.label('rp_bad')
    a.raw(b'\xC6\x06')
    a.addr('bad')
    a.raw(b'\x01')                          # mov byte [bad], 1
    a.raw(b'\xBA')
    a.addr('s_fail')
    a.raw(b'\xB4\x09')
    int21(a)
    a.label('rp_end')
    a.raw(b'\xBA')
    a.addr('s_crlf')
    a.raw(b'\xB4\x09')
    int21(a)
    a.raw(b'\xC3')


# ---------------------------------------------------------------------------
# DOS4TEST.COM
# ---------------------------------------------------------------------------
def build_dos4test():
    a = Asm()
    prolog(a)

    # ---- AH=30h: version word -----------------------------------------
    puts(a, 'l_dosver')
    imm16(a, 0xB8, 0x3000)                  # mov ax, 0x3000
    int21(a)
    mov_mem_ax(a, 'ver')                    # AL = major, AH = minor
    ld_al_mem(a, 'ver')
    a.raw(b'\xB4\x00')                      # mov ah, 0
    call16(a, 'putdec')
    putc(a, 0x2E)                           # '.'
    ld_al_mem(a, 'ver1')
    call16(a, 'put2')
    call16(a, 'newline')
    check_ax_eq(a, 'version', 'ver', 0x0004)

    # ---- AH=32h: drive parameter block --------------------------------
    puts(a, 'l_dpb')
    a.raw(b'\x31\xD2')                      # xor dx, dx  (DL=0: default drive)
    a.raw(b'\xB4\x32')                      # mov ah, 0x32
    int21(a)                                # DS:BX -> DPB
    a.raw(b'\x89\xDE')                      # mov si, bx
    a.raw(b'\x8C\xD8')                      # mov ax, ds
    a.raw(b'\x8E\xC0')                      # mov es, ax
    push_ds(a)
    a.raw(b'\x26\x8B\x44\x02')              # mov ax, es:[si+2]    bytes/sector
    mov_mem_ax(a, 'w1')
    a.raw(b'\x26\x8A\x44\x08')              # mov al, es:[si+8]    number of FATs
    a.raw(b'\xA2'); a.addr('b1')            # mov [b1], al
    a.raw(b'\x26\x8B\x44\x0F')              # mov ax, es:[si+0x0F] sectors/FAT
    mov_mem_ax(a, 'w2')
    check_ax_eq(a, 'dpb-bps', 'w1', 512)
    check_al_nz(a, 'dpb-nfats', 'b1')
    check_ax_nz(a, 'dpb-spf', 'w2')

    # ---- AH=52h: list of lists ----------------------------------------
    puts(a, 'l_lol')
    imm16(a, 0xB8, 0x5200)
    int21(a)                                # ES:BX -> list of lists
    a.raw(b'\x89\xD8')                      # mov ax, bx
    mov_mem_ax(a, 'lolb')
    a.raw(b'\x8C\xC0')                      # mov ax, es
    mov_mem_ax(a, 'lolseg')
    a.raw(b'\x26\x8B\x47\x02')              # mov ax, es:[bx+2]  first DPB
    mov_mem_ax(a, 'w1')
    push_ds(a)
    check_ax_nz(a, 'lol-es', 'lolseg')
    check_ax_nz(a, 'lol-dpb', 'w1')

    # ---- AH=59h: extended error after a failed open -------------------
    puts(a, 'l_exterr')
    a.raw(b'\xBA')
    a.addr('f_missing')
    imm16(a, 0xB8, 0x3D00)                  # open "NOSUCH.TXT" read only
    int21(a)                                # must fail with AX = 2
    imm16(a, 0xB8, 0x5900)                  # AH=59h
    int21(a)
    mov_mem_ax(a, 'err')                    # AL = error code
    a.raw(b'\x88\xF8')                      # mov al, bh   (error class)
    a.raw(b'\xA2'); a.addr('cls')           # mov [cls], al
    a.raw(b'\x88\xD8')                      # mov al, bl   (suggested action)
    a.raw(b'\xA2'); a.addr('act')           # mov [act], al
    check_al_eq(a, 'exterr-code', 'err', 0x02)
    check_al_eq(a, 'exterr-class', 'cls', 0x08)

    # ---- AH=6Ch: extended open ---------------------------------------
    puts(a, 'l_6c')
    # (a) create a file that does not exist (action bit 1)
    a.raw(b'\xBE')
    a.addr('f_new')                         # mov si, "NEW64.TXT"
    imm16(a, 0xBB, 0x0002)                  # mov bx, 2   read/write, compat
    imm16(a, 0xB9, 0x0020)                  # mov cx, 0x20 archive
    imm16(a, 0xBA, 0x0002)                  # mov dx, 2   create if missing
    imm16(a, 0xB8, 0x6C00)
    int21(a)
    mov_mem_ax(a, 'hnd')
    mov_mem_cx(a, 'actn')
    check_cf(a, '6c-create-cf')
    check_ax_eq(a, '6c-create', 'actn', 2)
    # write 5 bytes through the handle
    ld_bx_mem(a, 'hnd')
    a.raw(b'\xBA')
    a.addr('m_hello')
    imm16(a, 0xB9, 0x0005)
    a.raw(b'\xB4\x40')                      # mov ah, 0x40
    int21(a)
    mov_mem_ax(a, 'res')
    check_cf(a, '6c-write-cf')
    check_ax_eq(a, '6c-write', 'res', 5)
    # AH=68h commit
    ld_bx_mem(a, 'hnd')
    imm16(a, 0xB8, 0x6800)
    int21(a)
    check_cf(a, '68h')
    # close
    ld_bx_mem(a, 'hnd')
    a.raw(b'\xB4\x3E')                      # mov ah, 0x3E
    int21(a)
    # (b) open the existing file (action bit 0)
    a.raw(b'\xBE')
    a.addr('f_new')
    imm16(a, 0xBB, 0x0002)
    imm16(a, 0xB9, 0x0020)
    imm16(a, 0xBA, 0x0001)                  # mov dx, 1   open if it exists
    imm16(a, 0xB8, 0x6C00)
    int21(a)
    mov_mem_ax(a, 'hnd')
    mov_mem_cx(a, 'actn')
    check_cf(a, '6c-open-cf')
    check_ax_eq(a, '6c-open', 'actn', 1)
    # read the 5 bytes back
    ld_bx_mem(a, 'hnd')
    a.raw(b'\xBA')
    a.addr('rbuf')
    imm16(a, 0xB9, 0x0005)
    a.raw(b'\xB4\x3F')                      # mov ah, 0x3F
    int21(a)
    mov_mem_ax(a, 'res')
    check_cf(a, '6c-read-cf')
    check_ax_eq(a, '6c-read', 'res', 5)
    check_al_eq(a, '6c-data', 'rbuf', 0x48)  # 'H'
    ld_bx_mem(a, 'hnd')
    a.raw(b'\xB4\x3E')
    int21(a)                                # close
    # (c) replace the existing file (action bit 2): must truncate
    a.raw(b'\xBE')
    a.addr('f_new')
    imm16(a, 0xBB, 0x0002)
    imm16(a, 0xB9, 0x0020)
    imm16(a, 0xBA, 0x0004)                  # mov dx, 4   replace
    imm16(a, 0xB8, 0x6C00)
    int21(a)
    mov_mem_ax(a, 'hnd')
    mov_mem_cx(a, 'actn')
    check_cf(a, '6c-trunc-cf')
    check_ax_eq(a, '6c-trunc', 'actn', 3)
    ld_bx_mem(a, 'hnd')
    a.raw(b'\xB4\x3E')
    int21(a)
    # (d) refuse an existing file (action 0)
    a.raw(b'\xBE')
    a.addr('f_new')
    imm16(a, 0xBB, 0x0002)
    imm16(a, 0xB9, 0x0020)
    a.raw(b'\x31\xD2')                      # xor dx, dx
    imm16(a, 0xB8, 0x6C00)
    int21(a)
    mov_mem_ax(a, 'res')
    check_al_eq(a, '6c-exists', 'res', 0x50)
    # (e) refuse a missing file (action 0)
    a.raw(b'\xBE')
    a.addr('f_missing')
    imm16(a, 0xBB, 0x0002)
    imm16(a, 0xB9, 0x0020)
    a.raw(b'\x31\xD2')                      # xor dx, dx
    imm16(a, 0xB8, 0x6C00)
    int21(a)
    mov_mem_ax(a, 'res')
    check_al_eq(a, '6c-nofile', 'res', 0x02)
    # delete the scratch file
    a.raw(b'\xBA')
    a.addr('f_new')
    a.raw(b'\xB4\x41')                      # mov ah, 0x41
    int21(a)
    check_cf(a, '6c-del')

    # ---- AH=5Ah: unique temporary file -------------------------------
    puts(a, 'l_5a')
    a.raw(b'\xBA')
    a.addr('p_temp')                        # "\\" -> the current directory
    imm16(a, 0xB8, 0x5A00)
    int21(a)
    mov_mem_ax(a, 'hnd')
    check_cf(a, '5a-create-cf')
    ld_ax_mem(a, 'hnd')
    cmp_ax(a, 0xFFFF)
    check_cc(a, '5a-create', 0x5)           # jnz (not 0xFFFF)
    ld_bx_mem(a, 'hnd')
    a.raw(b'\xB4\x3E')
    int21(a)                                # close the temp file
    # find it again (the DTA carries the generated name) and delete it
    a.raw(b'\xBA')
    a.addr('p_tmp')                         # "TMP*.TMP"
    imm16(a, 0xB8, 0x4E00)
    int21(a)
    check_cf(a, '5a-find')
    imm16(a, 0xB8, 0x2F00)                  # AH=2Fh: get the DTA
    int21(a)                                # ES:BX -> DTA
    a.raw(b'\x89\xDA')                      # mov dx, bx
    a.raw(b'\x83\xC2\x1E')                  # add dx, 0x1E  (name field)
    a.raw(b'\x06\x1F')                      # push es ; pop ds
    a.raw(b'\xB4\x41')                      # mov ah, 0x41
    int21(a)
    push_ds(a)
    check_cf(a, '5a-del')

    # ---- AH=60h: truename --------------------------------------------
    puts(a, 'l_60h')
    a.raw(b'\xBE')
    a.addr('f_true')                        # mov si, "hello.txt"
    a.raw(b'\xBF')
    a.addr('out')                           # mov di, out
    a.raw(b'\x0E\x07')                      # push cs ; pop es
    imm16(a, 0xB8, 0x6000)
    int21(a)
    check_al_eq(a, '60h-drive', 'out1', 0x3A)   # ':'
    check_al_eq(a, '60h-root', 'out2', 0x5C)    # '\'
    a.raw(b'\xBE')
    a.addr('out')
    call16(a, 'putz')                       # echo the canonical name
    call16(a, 'newline')

    # ---- AH=63h: DBCS lead byte table --------------------------------
    imm16(a, 0xB8, 0x6300)
    int21(a)
    mov_mem_ax(a, 'res')
    check_al_eq(a, '63h-dbcs', 'res', 0x00)

    # ---- verdict ------------------------------------------------------
    a.raw(b'\x80\x3E')
    a.addr('bad')
    a.raw(b'\x00')                          # cmp byte [bad], 0
    jcc16(a, 0x5, 'fail_end')               # jnz
    puts(a, 's_pass')
    exit_dos(a, 0)
    a.label('fail_end')
    puts(a, 's_failall')
    exit_dos(a, 1)

    # ---- routines -----------------------------------------------------
    newline_routine(a)
    putdec_routine(a)
    put2_routine(a)
    putz_routine(a)
    report_routine(a)

    # ---- data ---------------------------------------------------------
    data(a, 'l_dosver', b'DOS version $')
    data(a, 'l_dpb', b'-- AH=32h drive parameter block\r\n$')
    data(a, 'l_lol', b'-- AH=52h list of lists\r\n$')
    data(a, 'l_exterr', b'-- AH=59h extended error\r\n$')
    data(a, 'l_6c', b'-- AH=6Ch extended open\r\n$')
    data(a, 'l_5a', b'-- AH=5Ah temporary file\r\n$')
    data(a, 'l_60h', b'-- AH=60h truename\r\n$')
    data(a, 's_colon', b': $')
    data(a, 's_ok', b'OK$')
    data(a, 's_fail', b'FAIL$')
    data(a, 's_crlf', b'\r\n$')
    data(a, 's_pass', b'DOS4-OK\r\n$')
    data(a, 's_failall', b'DOS4-FAIL\r\n$')
    data(a, 'f_new', b'NEW64.TXT\x00')
    data(a, 'f_missing', b'NOSUCH.TXT\x00')
    data(a, 'f_true', b'hello.txt\x00')
    data(a, 'p_temp', b'\\\x00')
    data(a, 'p_tmp', b'TMP*.TMP\x00')
    data(a, 'm_hello', b'HELLO')
    data(a, 'rbuf', b'', 8)
    data(a, 'out', b'', 1)
    data(a, 'out1', b'', 1)
    data(a, 'out2', b'', 1)
    data(a, 'outrest', b'', 69)
    data(a, 'ver', b'', 1)
    data(a, 'ver1', b'', 1)
    data(a, 'err', b'', 1)
    data(a, 'cls', b'', 1)
    data(a, 'act', b'', 1)
    data(a, 'b1', b'', 1)
    data(a, 'res', b'', 2)
    data(a, 'w1', b'', 2)
    data(a, 'w2', b'', 2)
    data(a, 'hnd', b'', 2)
    data(a, 'actn', b'', 2)
    data(a, 'lolb', b'', 2)
    data(a, 'lolseg', b'', 2)
    data(a, 'dpbseg', b'', 2)
    data(a, 'bad', b'', 1)
    for tag in ('version', 'dpb-bps', 'dpb-nfats', 'dpb-spf', 'lol-es',
                'lol-dpb', 'exterr-code', 'exterr-class',
                '6c-create-cf', '6c-create', '6c-write-cf', '6c-write', '68h',
                '6c-open-cf', '6c-open', '6c-read-cf', '6c-read', '6c-data',
                '6c-trunc-cf', '6c-trunc', '6c-exists', '6c-nofile', '6c-del',
                '5a-create-cf', '5a-create', '5a-find', '5a-del',
                '60h-drive', '60h-root', '63h-dbcs'):
        data(a, 't_' + tag, tag.encode() + b'\x00')

    return a.finish()


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(outdir, exist_ok=True)
    blob = build_dos4test()
    path = os.path.join(outdir, 'DOS4TEST.COM')
    with open(path, 'wb') as f:
        f.write(blob)
    print('%-10s %5d bytes' % ('DOS4TEST.COM', len(blob)))


if __name__ == '__main__':
    main()
