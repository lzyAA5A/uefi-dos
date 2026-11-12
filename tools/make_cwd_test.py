#!/usr/bin/env python3
"""Build CWDCHK.COM: report INT 21h AH=3Bh (chdir) and AH=47h (get cwd).

Per case it prints ``cd CF=n rc=xxxx`` and then ``SUB    CF=0 AX=0100 [CWDTMP]``.

Cases:
    chdir \\CWDTMP ; AH=47h DL=0   -> SUB
    chdir \\        ; AH=47h DL=0   -> ROOT
    chdir \\CWDTMP ; AH=47h DL=3   -> SUB-C:

RBIL 2147 says the returned path "does not include a drive or the initial
backslash" and that AX is 0100h on success.  Windows 1.x depends on the first
half: its path builder appends a '\\' and then has DOS write the cwd right
there, so a leading backslash gives it ``C:\\CWDTMP`` and a longer module
name (which then does not fit in its 428-byte local heap).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from make_tools import Asm, puthex_routine, call16, int21  # noqa: E402


def main():
    a = Asm()

    # the probe makes its own subdirectory so it works on any volume
    a.raw(b'\xBA'); a.addr('dir_win')            # mov dx, dir_win
    a.raw(b'\xB4\x39'); int21(a)                 # mov ah, 39h / int 21h (mkdir)

    # ---- case 1: subdirectory, default drive -----------------------------
    a.raw(b'\xBA'); a.addr('dir_win')            # mov dx, dir_win
    a.raw(b'\xB4\x3B'); int21(a)                 # mov ah, 3Bh / int 21h
    call16(a, 'show_cd')
    a.raw(b'\xBA'); a.addr('lbl_sub')            # mov dx, lbl_sub
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xB3\x00')                           # mov bl, 0   (DL=0: current drive)
    call16(a, 'probe')

    # ---- case 2: root, default drive -------------------------------------
    a.raw(b'\xBA'); a.addr('dir_root')           # mov dx, dir_root
    a.raw(b'\xB4\x3B'); int21(a)
    call16(a, 'show_cd')
    a.raw(b'\xBA'); a.addr('lbl_root')           # mov dx, lbl_root
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xB3\x00')
    call16(a, 'probe')

    # ---- case 3: subdirectory, explicit drive C: -------------------------
    a.raw(b'\xBA'); a.addr('dir_win')
    a.raw(b'\xB4\x3B'); int21(a)
    call16(a, 'show_cd')
    a.raw(b'\xBA'); a.addr('lbl_drv')            # mov dx, lbl_drv
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xB3\x03')                           # mov bl, 3   (DL=3: C:)
    call16(a, 'probe')

    # clean up: back to the root, then remove the directory again
    a.raw(b'\xBA'); a.addr('dir_root')           # mov dx, dir_root
    a.raw(b'\xB4\x3B'); int21(a)                 # mov ah, 3Bh / int 21h
    a.raw(b'\xBA'); a.addr('dir_win')            # mov dx, dir_win
    a.raw(b'\xB4\x3A'); int21(a)                 # mov ah, 3Ah / int 21h (rmdir)

    a.raw(b'\xBA'); a.addr('msg_done')           # mov dx, msg_done
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xB8\x00\x4C'); int21(a)             # mov ax, 4C00h / int 21h

    # ---- show_cd: report the AH=3Bh result -------------------------------
    a.label('show_cd')
    a.raw(b'\xA3'); a.addr('cdax')               # mov [cdax], ax
    a.raw(b'\x88\xE0')                           # mov al, ah
    a.raw(b'\xA2'); a.addr('cdah')               # mov [cdah], al
    a.raw(b'\x9C')                               # pushf
    a.raw(b'\x58')                               # pop ax
    a.raw(b'\x24\x01')                           # and al, 1   (CF)
    a.raw(b'\x04\x30')                           # add al, '0'
    a.raw(b'\xA2'); a.addr('cdcf')               # mov [cdcf], al

    a.raw(b'\xBA'); a.addr('msg_chdir')          # "cd CF="
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\x8A\x16'); a.addr('cdcf')           # mov dl, [cdcf]
    a.raw(b'\xB4\x02'); int21(a)
    a.raw(b'\xBA'); a.addr('msg_rc')             # " rc="
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xA0'); a.addr('cdah')               # mov al, [cdah]
    call16(a, 'puthex')
    a.raw(b'\xA0'); a.addr('cdax')               # mov al, [cdax]
    call16(a, 'puthex')
    a.raw(b'\xBA'); a.addr('msg_crlf')           # "\r\n"
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xC3')                               # ret

    # ---- probe: AH=47h into buf, print CF/AX/string ----------------------
    a.label('probe')
    a.raw(b'\x88\xDA')                           # mov dl, bl
    a.raw(b'\xBE'); a.addr('buf')                # mov si, buf
    a.raw(b'\xB4\x47'); int21(a)                 # mov ah, 47h / int 21h
    a.raw(b'\xA3'); a.addr('axv')                # mov [axv], ax
    a.raw(b'\x88\xE0')                           # mov al, ah
    a.raw(b'\xA2'); a.addr('ahv')                # mov [ahv], al
    a.raw(b'\x9C')                               # pushf
    a.raw(b'\x58')                               # pop ax
    a.raw(b'\x24\x01')                           # and al, 1
    a.raw(b'\x04\x30')                           # add al, '0'
    a.raw(b'\xA2'); a.addr('cfc')                # mov [cfc], al

    a.raw(b'\xBA'); a.addr('msg_cf')             # " CF="
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\x8A\x16'); a.addr('cfc')            # mov dl, [cfc]
    a.raw(b'\xB4\x02'); int21(a)

    a.raw(b'\xBA'); a.addr('msg_ax')             # " AX="
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xA0'); a.addr('ahv')                # mov al, [ahv]
    call16(a, 'puthex')
    a.raw(b'\xA0'); a.addr('axv')                # mov al, [axv]
    call16(a, 'puthex')

    a.raw(b'\xBA'); a.addr('msg_open')           # " ["
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xBE'); a.addr('buf')                # mov si, buf
    a.label('str_top')
    a.raw(b'\xAC')                               # lodsb
    a.raw(b'\x84\xC0')                           # test al, al
    a.raw(b'\x74'); a.rel8('str_end')            # jz str_end
    a.raw(b'\x88\xC2')                           # mov dl, al
    a.raw(b'\xB4\x02'); int21(a)                 # mov ah, 2 / int 21h
    a.raw(b'\xEB'); a.rel8('str_top')            # jmp str_top
    a.label('str_end')
    a.raw(b'\xBA'); a.addr('msg_close')          # "]\r\n"
    a.raw(b'\xB4\x09'); int21(a)
    a.raw(b'\xC3')                               # ret

    puthex_routine(a)

    a.label('dir_win')
    a.raw(b'\\CWDTMP\x00')
    a.label('dir_root')
    a.raw(b'\\\x00')
    a.label('lbl_sub')
    a.raw(b'SUB   $')
    a.label('lbl_root')
    a.raw(b'ROOT  $')
    a.label('lbl_drv')
    a.raw(b'SUB-C: $')
    a.label('msg_chdir')
    a.raw(b'cd CF=$')
    a.label('msg_rc')
    a.raw(b' rc=$')
    a.label('msg_cf')
    a.raw(b' CF=$')
    a.label('msg_ax')
    a.raw(b' AX=$')
    a.label('msg_open')
    a.raw(b' [$')
    a.label('msg_close')
    a.raw(b']\r\n$')
    a.label('msg_crlf')
    a.raw(b'\r\n$')
    a.label('msg_done')
    a.raw(b'CWDCHK-DONE\r\n$')
    a.label('cdax')
    a.raw(b'\x00\x00')
    a.label('cdah')
    a.raw(b'\x00')
    a.label('cdcf')
    a.raw(b'\x00')
    a.label('axv')
    a.raw(b'\x00\x00')
    a.label('ahv')
    a.raw(b'\x00')
    a.label('cfc')
    a.raw(b'\x00')
    a.label('buf')
    a.raw(b'\x00' * 68)

    code = a.finish()
    out = os.path.join(HERE, '..', 'build', 'CWDCHK.COM')
    with open(out, 'wb') as f:
        f.write(code)
    print('wrote %s (%d bytes)' % (os.path.normpath(out), len(code)))


if __name__ == '__main__':
    main()
