#!/usr/bin/env python3
"""Boot build/UEFIDOS.img under OVMF in QEMU, type a shell command, screenshot.

  python tools/uefidos_shot.py SNAKE --out build/shot_snake
  python tools/uefidos_shot.py "DIR" --wait 20 --keys "ENTER"

Writes <out>.ppm and <out>.png.  Needs a display backend because QEMU cannot
screendump the "none" backend.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
FW64 = r'C:\Program Files\qemu\share\edk2-x86_64-code.fd'
FW32 = r'C:\Program Files\qemu\share\edk2-i386-code.fd'
VARS = r'C:\Program Files\qemu\share\edk2-i386-vars.fd'

KEYSPECIAL = {':': 'shift-semicolon', '\\': 'backslash', ' ': 'spc', '\r': 'ret',
              '/': 'slash', '-': 'minus', '.': 'dot', '=': 'equal', '_': 'shift-minus'}
KEYS = {'ENTER': 'ret', 'ESC': 'esc', 'SPACE': 'spc', 'TAB': 'tab', 'UP': 'up',
        'DOWN': 'down', 'LEFT': 'left', 'RIGHT': 'right', 'BACKSPACE': 'backspace'}


def key_name(ch):
    # a single space is a key of its own: check it before stripping, otherwise
    # "TE HELLO.TXT" turns into "tehello.txt" (stripped to an empty key name)
    if ch == ' ':
        return 'spc'
    if ch in KEYSPECIAL:
        return KEYSPECIAL[ch]
    ch = ch.strip()
    if ch.upper() in KEYS:
        return KEYS[ch.upper()]
    return ch.lower()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('command', help='command typed at the UEFI-DOS prompt')
    ap.add_argument('--out', default='build/shot')
    ap.add_argument('--image', default='build/UEFIDOS.img')
    ap.add_argument('--boot-wait', type=float, default=22.0)
    ap.add_argument('--after-command', type=float, default=6.0)
    ap.add_argument('--keys', default='')
    ap.add_argument('--after-keys', type=float, default=2.0)
    ap.add_argument('--firmware', choices=['x64', 'ia32'], default='x64')
    ap.add_argument('--port', type=int, default=55700)
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    img = os.path.join(root, args.image)
    vars_path = os.path.join(root, 'build', 'vars-shot.fd')
    with open(VARS, 'rb') as src, open(vars_path, 'wb') as dst:
        dst.write(src.read())
    out_ppm = os.path.join(root, args.out + '.ppm')
    if os.path.exists(out_ppm):
        os.remove(out_ppm)

    fw = FW64 if args.firmware == 'x64' else FW32
    qargs = [
        QEMU, '-accel', 'tcg', '-m', '256', '-smp', '1', '-machine', 'pc',
        '-drive', 'if=pflash,format=raw,readonly=on,file=' + fw,
        '-drive', 'if=pflash,format=raw,file=' + vars_path,
        '-drive', 'file=%s,if=ide,index=0,media=disk,format=raw' % img,
        '-monitor', 'tcp:127.0.0.1:%d,server,nowait' % args.port,
        '-serial', 'file:' + os.path.join(root, 'build', 'serial-shot.log'),
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

        def mon(cmd, wait=0.3):
            sock.sendall((cmd + '\r\n').encode())
            time.sleep(wait)

        print('waiting %0.0fs for UEFI-DOS ...' % args.boot_wait)
        time.sleep(args.boot_wait)
        for ch in args.command:
            mon('sendkey ' + key_name(ch), 0.08)
        mon('sendkey ret', 0.2)
        time.sleep(args.after_command)
        for k in [x for x in args.keys.split(',') if x.strip()]:
            mon('sendkey ' + key_name(k), 0.35)
        if args.keys:
            time.sleep(args.after_keys)
        if proc.poll() is not None:
            print('qemu exited early, code', proc.returncode, '-- see build/serial-shot.log')
        mon('screendump ' + out_ppm.replace('\\', '/'), 2.0)
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


if __name__ == '__main__':
    main()
