#!/usr/bin/env python3
"""Assert that a function in a built EFI binary still has a real body.

GCC happily deletes a whole firmware path when it can prove the pointer it uses
is NULL (that is exactly how the GOP graphics code turned into `ret`/`ud2` and
the guest got a #UD the moment a game set a video mode).  Symbol size is the
cheap tripwire: nm gives addresses, so the size is the distance to the next
symbol in the same section.

Usage:  python check_efi_symbols.py FILE.efi NAME=MINBYTES [NAME=MINBYTES ...]
"""
import subprocess
import sys

NM = r'C:\msys64\mingw64\bin\nm.exe'


def main():
    if len(sys.argv) < 3:
        print('usage: check_efi_symbols.py FILE.efi NAME=MINBYTES ...')
        return 2
    efi = sys.argv[1]
    nm = NM
    try:
        out = subprocess.run([nm, '-n', efi], capture_output=True, text=True, timeout=60).stdout
    except OSError:
        out = subprocess.run(['nm', '-n', efi], capture_output=True, text=True, timeout=60).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1].lower() in ('t', 'w'):
            try:
                syms.append((int(p[0], 16), p[2]))
            except ValueError:
                continue
    syms.sort()
    index = {}
    for i, (addr, name) in enumerate(syms):
        nxt = syms[i + 1][0] if i + 1 < len(syms) else addr + 0x1000
        index[name] = max(0, nxt - addr)

    fail = 0
    for spec in sys.argv[2:]:
        name, _, need = spec.partition('=')
        need = int(need or '64')
        size = index.get(name)
        if size is None:
            print('[FAIL] %s: symbol missing' % name)
            fail = 1
        elif size < need:
            print('[FAIL] %s: %d bytes of code (expected >= %d) - dead-code eliminated?'
                  % (name, size, need))
            fail = 1
        else:
            print('[ok]   %s: %d bytes of code' % (name, size))
    print('EFI SYMBOLS %s' % ('FAILED' if fail else 'OK'))
    return fail


if __name__ == '__main__':
    sys.exit(main())
