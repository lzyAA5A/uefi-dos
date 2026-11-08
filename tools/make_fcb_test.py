#!/usr/bin/env python3
"""make_fcb_test.py -- hand assemble FCBTEST.COM (DOS 4.0 FCB call smoke test).

Creates FCBDATA.DAT with the FCB interface, writes one 128-byte record, closes,
reopens, random-reads the record back and compares it.  Prints FCB-OK / FCB-FAIL
so the QEMU smoke test can assert on it.

usage: python tools/make_fcb_test.py <builddir>
"""
import os, sys


class Asm:
    def __init__(self):
        self.buf = bytearray()
        self.labels = {}
        self.fix = []      # (pos, label, kind)   kind: 'w8' = rel8, 'a16' = abs16

    def here(self):
        return len(self.buf)

    def label(self, name):
        self.labels[name] = self.here()

    def b(self, *vals):
        for v in vals:
            self.buf.append(v & 0xFF)

    def wabs(self, label):
        self.fix.append((len(self.buf), label, 'a16'))
        self.b(0, 0)

    def rel8(self, label):
        self.fix.append((len(self.buf), label, 'w8'))
        self.b(0)

    def finish(self):
        base = 0x100
        for pos, label, kind in self.fix:
            target = self.labels[label]
            if kind == 'a16':
                v = base + target
                self.buf[pos] = v & 0xFF
                self.buf[pos + 1] = (v >> 8) & 0xFF
            else:
                v = target - (pos + 1)
                assert -128 <= v <= 127, (label, v)
                self.buf[pos] = v & 0xFF
        return bytes(self.buf)


def build():
    a = Asm()

    # ---- code ----
    a.b(0xBE)                    # mov si, fcbname   (address patched later)
    a.wabs('fcb')
    # DS:DX = FCB for every FCB call
    def fcb_call(ah, extra=None):
        a.b(0xBA); a.wabs('fcb')          # mov dx, fcb
        a.b(0xB4, ah)                     # mov ah, imm8
        a.b(0xCD, 0x21)                   # int 21h

    # DTA := buf128 (FCB record I/O transfers through the DTA, AH=1Ah)
    a.b(0xBA); a.wabs('buf128')           # mov dx, buf128
    a.b(0xB4, 0x1A); a.b(0xCD, 0x21)      # mov ah,1Ah ; int 21h

    # create (AH=16h)
    fcb_call(0x16)
    a.b(0x3C, 0x00)                       # cmp al, 0
    a.b(0x75); a.rel8('fail')             # jne fail

    # sequential write one record (case: AH=15h) - the record comes from the DTA
    fcb_call(0x15)
    a.b(0x3C, 0x00)
    a.b(0x75); a.rel8('fail')

    # close (AH=10h) then open (AH=0Fh)
    fcb_call(0x10)
    fcb_call(0x0F)
    a.b(0x3C, 0x00)
    a.b(0x75); a.rel8('fail')

    # random read record 0 (AH=21h) - buffer comes from the DTA
    fcb_call(0x21)
    a.b(0x84, 0xC0)                       # test al, al
    a.b(0x75); a.rel8('fail')

    # close (AH=10h)
    fcb_call(0x10)

    # verify buf128[0] == 'A'
    a.b(0x80, 0x3E); a.wabs('buf128'); a.b(0x41)
    a.b(0x75); a.rel8('fail')

    # print FCB-OK
    a.b(0xBA); a.wabs('okmsg')
    a.b(0xB4, 0x09); a.b(0xCD, 0x21)
    # delete the test file through the FCB (AH=13h)
    fcb_call(0x13)
    a.b(0xB8, 0x00, 0x4C); a.b(0xCD, 0x21)     # mov ax, 4C00h ; int 21h

    a.label('fail')
    a.b(0xBA); a.wabs('failmsg')
    a.b(0xB4, 0x09); a.b(0xCD, 0x21)
    a.b(0xB8, 0x01, 0x4C); a.b(0xCD, 0x21)

    # ---- data ----
    a.label('fcb')                         # 37-byte FCB
    a.b(0x00)                              # drive 0 = default
    a.b(*b'FCBDATA ')                      # name (8)
    a.b(*b'DAT')                           # ext  (3)
    a.b(0x00, 0x00)                        # current block
    a.b(0x80, 0x00)                        # record size 128
    a.b(*([0] * 4))                        # file size
    a.b(*([0] * 4))                        # date / time
    a.b(*([0] * 8))                        # reserved (0x18..0x1F)
    a.b(0x00)                              # current record (0x20)
    a.b(*([0] * 4))                        # random record (0x21..0x24)
    assert a.here() - a.labels['fcb'] == 37

    a.label('buf128')
    a.b(*([0x41] * 128))                   # the record the DTA points at: 'A' x 128

    a.label('okmsg')
    a.b(*b'FCB-OK\r\n$')
    a.label('failmsg')
    a.b(*b'FCB-FAIL\r\n$')

    # fix: the first instruction loads si (unused) -- patch it to a nop pair
    code = bytearray(a.finish())
    code[0] = 0x90
    code[1] = 0x90
    code[2] = 0x90
    return bytes(code)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else 'build'
    os.makedirs(outdir, exist_ok=True)
    data = build()
    path = os.path.join(outdir, 'FCBTEST.COM')
    with open(path, 'wb') as f:
        f.write(data)
    print('%s (%d bytes)' % (path, len(data)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
