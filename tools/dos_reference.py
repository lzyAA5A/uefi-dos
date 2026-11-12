#!/usr/bin/env python3
"""Reference run: boot the CardPuter86 MS-DOS 5 floppy in QEMU, type a command,
and capture a screenshot, so UEFI-DOS rendering can be compared with real DOS.

  python tools/dos_reference.py "mine" --out build/ref_mine --keys "ENTER,SPACE"

Writes <out>.ppm and <out>.png (ppm2png), and a log of what was typed.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
IMG = r'F:\1231\_tmp\CardPuter86\ESP32\CardPuter86\data\cardputer86.img'

KEYSPECIAL = {
    ':': 'shift-semicolon', '\\': 'backslash', ' ': 'spc', '\r': 'ret',
    '\n': 'ret', '/': 'slash', '-': 'minus', '.': 'dot', '=': 'equal',
}
KEYS = {
    'ENTER': 'ret', 'ESC': 'esc', 'SPACE': 'spc', 'TAB': 'tab',
    'UP': 'up', 'DOWN': 'down', 'LEFT': 'left', 'RIGHT': 'right',
    'BACKSPACE': 'backspace',
}


def key_name(ch):
    ch = ch.strip()
    if ch.upper() in KEYS:
        return KEYS[ch.upper()]
    if ch in KEYSPECIAL:
        return KEYSPECIAL[ch]
    if ch.isascii() and ch.isalpha():
        return ch.lower()
    return ch.lower()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('command', help='DOS command typed at the C> prompt, e.g. "mine"')
    ap.add_argument('--out', default='build/ref')
    ap.add_argument('--mbytes', type=int, default=640)
    ap.add_argument('--boot-wait', type=float, default=14.0)
    ap.add_argument('--after-command', type=float, default=3.0)
    ap.add_argument('--keys', default='', help='comma separated keys pressed after the command')
    ap.add_argument('--after-keys', type=float, default=3.0)
    ap.add_argument('--port', type=int, default=55600)
    ap.add_argument('--image', default=IMG)
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_ppm = os.path.join(root, args.out + '.ppm')
    log = []

    qargs = [
        QEMU, '-accel', 'tcg', '-m', str(args.mbytes), '-smp', '1',
        '-machine', 'pc', '-boot', 'a',
        '-drive', 'if=floppy,index=0,format=raw,file=' + args.image,
        '-monitor', 'tcp:127.0.0.1:%d,server,nowait' % args.port,
        '-display', 'gtk', '-no-reboot', '-net', 'none',
    ]
    print('starting qemu ...')
    proc = subprocess.Popen(qargs)
    sock = None
    try:
        for _ in range(50):
            try:
                sock = socket.create_connection(('127.0.0.1', args.port), timeout=1)
                break
            except OSError:
                time.sleep(0.2)
        if sock is None:
            raise SystemExit('monitor port never opened')

        def mon(cmd, wait=0.4):
            sock.sendall((cmd + '\r\n').encode())
            log.append('mon: ' + cmd)
            time.sleep(wait)

        print('waiting %0.1fs for DOS boot ...' % args.boot_wait)
        time.sleep(args.boot_wait)

        for ch in args.command:
            mon('sendkey ' + key_name(ch), 0.06)
        mon('sendkey ret', 0.2)
        time.sleep(args.after_command)

        for k in [x for x in args.keys.split(',') if x.strip()]:
            mon('sendkey ' + key_name(k), 0.3)
        if args.keys:
            time.sleep(args.after_keys)

        mon('screendump ' + out_ppm.replace('\\', '/'), 1.5)
        print('screendump ->', out_ppm)
    finally:
        try:
            if sock:
                sock.sendall(b'quit\r\n')
                time.sleep(0.5)
                sock.close()
        except OSError:
            pass
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if not os.path.exists(out_ppm):
        raise SystemExit('screendump failed (no file)')
    png = os.path.join(root, args.out + '.png')
    subprocess.check_call([sys.executable, os.path.join(root, 'tools', 'ppm2png.py'), out_ppm, png])
    print('png ->', png)
    print('\n'.join(log))


if __name__ == '__main__':
    main()
