#!/usr/bin/env python3
"""Probe UEFI-DOS boot interactively: drive QEMU's monitor and UEFI shell.

Steps:
  1. start qemu with monitor on tcp 127.0.0.1:PORT, serial to file
  2. wait for the shell prompt, type the load command via monitor sendkey
  3. capture serial output, report whether UEFI-DOS started

Run: python tools/qemu_probe.py build/UEFIDOS.img [--typing "fs0:\\efi\\boot\\bootx64.efi"]
"""
import argparse
import os
import subprocess
import socket
import time
import shutil
import sys

QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
FW = r'C:\Program Files\qemu\share\edk2-x86_64-code.fd'
VARS_SRC = r'C:\Program Files\qemu\share\edk2-i386-vars.fd'

KEYSPECIAL = {
    ':': 'shift-semicolon',
    '\\': 'backslash',
    ' ': 'spc',
    '\r': 'ret',
    '\n': 'ret',
    '/': 'slash',
    '-': 'minus',
    '.': 'dot',
}


def send_key(sock, ch):
    key = KEYSPECIAL.get(ch, ch.lower() if ch.isascii() else ch)
    sock.sendall(('sendkey %s\r\n' % key).encode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--typing', default=r'fs0:\efi\boot\bootx64.efi')
    ap.add_argument('--cmds', default='', help='pipe separated commands to type into the shell')
    ap.add_argument('--initial-wait', type=float, default=10.0)
    ap.add_argument('--serial-wait', type=float, default=25.0)
    ap.add_argument('--port', type=int, default=55555)
    args = ap.parse_args()

    img = os.path.abspath(args.image)
    build = os.path.dirname(img)
    vars_path = os.path.join(build, 'vars.fd')
    serial = os.path.join(build, 'serial.log')
    shutil.copyfile(VARS_SRC, vars_path)
    if os.path.exists(serial):
        os.remove(serial)

    qargs = [
        '-m', '256', '-smp', '1', '-machine', 'pc',
        '-drive', 'if=pflash,format=raw,readonly=on,file=' + FW,
        '-drive', 'if=pflash,format=raw,file=' + vars_path,
        '-drive', 'file=%s,format=raw,if=ide,index=0,media=disk' % img,
        '-display', 'none', '-serial', 'file:' + serial,
        '-monitor', 'tcp:127.0.0.1:%d,server,nowait' % args.port,
        '-no-reboot', '-net', 'none', '-no-shutdown',
    ]
    p = subprocess.Popen([QEMU] + qargs)
    try:
        time.sleep(args.initial_wait)
        s = socket.create_connection(('127.0.0.1', args.port), timeout=5)
        s.settimeout(2)
        # read monitor banner
        try:
            s.recv(4096)
        except socket.timeout:
            pass
        cmds = [c for c in args.cmds.split('|') if c] or [args.typing]
        for cmd in cmds:
            for ch in cmd:
                send_key(s, ch)
                time.sleep(0.05)
            time.sleep(0.3)
            s.sendall(b'sendkey ret\r\n')
            time.sleep(2.0)
        # wait for serial output
        end = time.time() + args.serial_wait
        while time.time() < end and p.poll() is None:
            time.sleep(0.5)
        print('qemu rc:', p.poll())
        if os.path.exists(serial):
            data = open(serial, 'rb').read()
            # strip ANSI escapes for readability
            import re
            txt = data.decode('latin1', 'replace')
            txt = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', txt)
            print('---- serial tail ----')
            print(txt[-4000:])
    finally:
        if p.poll() is None:
            p.kill()


if __name__ == '__main__':
    main()
