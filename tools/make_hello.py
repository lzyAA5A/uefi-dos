"""Generate tiny test programs for UEFI-DOS.

HELLO.COM   -- INT 21h AH=09 '$' string output, then AH=4C terminate.
TELETYPE.COM-- INT 10h AH=0Eh teletype loop, then INT 20h terminate.
MZEXE.EXE   -- minimal MZ executable (32-byte header, CS:IP=0) printing a line.
RUNNER.COM  -- INT 21h AH=4Bh EXEC of HELLO.COM, then prints "RUNNER DONE!".
VRAM.COM    -- writes text straight into 0xB800 video RAM.
UPCALL.COM  -- calls the native 32/64-bit side through INT 2Fh AX=5544h.
"""
import os
import struct
import sys


def hello():
    msg = b"Hello from 8086 code!\r\n$"
    # mov dx, msg ; mov ah,9 ; int 21h ; mov ax,4C00h ; int 21h
    code = bytes([0xBA, 0x0C, 0x01, 0xB4, 0x09, 0xCD, 0x21,
                  0xB8, 0x00, 0x4C, 0xCD, 0x21])
    assert len(code) == 12
    return code + msg


def teletype():
    # mov si,msg ; lodsb ; test al,al ; jz done ; mov ah,0Eh ; mov bx,7
    # int 10h ; jmp loop ; done: int 20h ; msg
    code = bytes([0xBE, 0x13, 0x01,  # mov si, 0x0113
                  0xAC,              # lodsb
                  0x84, 0xC0,        # test al, al
                  0x74, 0x09,        # jz +9  (to int 20h)
                  0xB4, 0x0E,        # mov ah, 0Eh
                  0xBB, 0x07, 0x00,  # mov bx, 0007h
                  0xCD, 0x10,        # int 10h
                  0xEB, 0xF2,        # jmp loop
                  0xCD, 0x20])       # int 20h
    assert len(code) == 19, len(code)
    return code + b"TELETYPE OK!\r\n\x00"


def mzexe():
    """Minimal MZ: 32-byte header; code starts at CS:IP=0 and does push cs/pop ds."""
    msg = b"Hello from MZ EXE!\r\n$"
    code = bytearray()
    code += b"\x0E\x1F"                        # push cs ; pop ds
    code += b"\xBA" + struct.pack("<H", 14)    # mov dx, msg (offset 0x0E)
    code += b"\xB4\x09\xCD\x21"                # mov ah,9 ; int 21h
    code += b"\xB8\x00\x4C\xCD\x21"            # mov ax,4C00h ; int 21h
    code += msg
    hdr = bytearray(32)
    hdr[0:2] = b"MZ"
    struct.pack_into("<H", hdr, 0x02, 0)        # e_cblp
    struct.pack_into("<H", hdr, 0x04, 1)        # e_cp
    struct.pack_into("<H", hdr, 0x06, 0)        # e_crlc
    struct.pack_into("<H", hdr, 0x08, 2)        # e_cparhdr (32-byte header)
    struct.pack_into("<H", hdr, 0x0A, 0)        # e_minalloc
    struct.pack_into("<H", hdr, 0x0C, 0xFFFF)   # e_maxalloc
    struct.pack_into("<H", hdr, 0x0E, 0)        # e_ss
    struct.pack_into("<H", hdr, 0x10, 0x0100)   # e_sp
    struct.pack_into("<H", hdr, 0x12, 0)        # e_csum
    struct.pack_into("<H", hdr, 0x14, 0)        # e_ip
    struct.pack_into("<H", hdr, 0x16, 0)        # e_cs
    struct.pack_into("<H", hdr, 0x18, 0x1C)     # e_lfarlc
    struct.pack_into("<H", hdr, 0x1A, 0)        # e_ovno
    return bytes(hdr) + bytes(code)


def runner():
    """COM that EXECs HELLO.COM via INT 21h AH=4Bh, then prints RUNNER DONE!."""
    code = bytearray()
    patches = {}
    labels = {}
    code += b"\xB8\x00\x4B"                                   # mov ax,4B00h
    code += b"\xBA"
    patches["name"] = len(code)
    code += b"\x00\x00"
    code += b"\xBB"
    patches["pb"] = len(code)
    code += b"\x00\x00"
    code += b"\xCD\x21"                                       # int 21h
    code += b"\xBA"
    patches["msg"] = len(code)
    code += b"\x00\x00"
    code += b"\xB4\x09\xCD\x21"                               # mov ah,9 ; int 21h
    code += b"\xB8\x00\x4C\xCD\x21"                           # mov ax,4C00h ; int 21h
    labels["name"] = len(code)
    code += b"HELLO.COM\x00"
    labels["pb"] = len(code)
    code += b"\x00\x00"                                       # env seg = 0
    patches["cmd"] = len(code)
    code += b"\x00\x00"                                       # cmdline off
    code += b"\x00\x00"                                       # cmdline seg = 0 (use DS)
    code += b"\x00\x00\x00\x00"                               # fcb1
    code += b"\x00\x00\x00\x00"                               # fcb2
    labels["cmd"] = len(code)
    code += b"\x00"                                           # command-tail count 0
    labels["msg"] = len(code)
    code += b"RUNNER DONE!\r\n$"
    for key, pos in patches.items():
        addr = 0x100 + labels[key]
        code[pos] = addr & 0xFF
        code[pos + 1] = (addr >> 8) & 0xFF
    return bytes(code)


def vram():
    """COM that writes text straight into video RAM at 0xB800:0A00 (row 10)."""
    code = bytearray()
    code += b"\xB8\x00\xB8"                    # mov ax,0B800h
    code += b"\x8E\xC0"                        # mov es,ax
    code += b"\xBF\x00\x0A"                    # mov di,0A00h (row 10)
    code += b"\xBE\x12\x01"                    # mov si,0112h (msg)
    code += b"\xB9\x0A\x00"                    # mov cx,10
    code += b"\xF3\xA5"                        # rep movsw
    code += b"\xCD\x20"                        # int 20h
    assert len(code) == 18, len(code)
    msg = b"VIDEO-OK! "
    pairs = b"".join(bytes([c, 0x1F]) for c in msg)
    return bytes(code) + pairs


def upcall():
    """COM that calls the native 32/64-bit side through INT 2Fh AX=5544h.

    DX=1 returns AX=API version (or 0FFFFh when the multiplex is absent); DX=2
    makes the native side print its banner.  Prints UPCALL-OK when the probe
    succeeds.
    """
    code = bytearray()
    patches = {}
    code += b"\xB8\x44\x55"        # mov ax,5544h
    code += b"\xBA\x01\x00"        # mov dx,1
    code += b"\xCD\x2F"            # int 2Fh -> AX = native API version
    # the native side answers AX=0FFFFh when the multiplex is absent, so accept
    # anything else (the version itself is printed by the DX=2 banner call)
    code += b"\x83\xF8\xFF"        # cmp ax,-1
    code += b"\x74\x00"            # je fail (rel8 patched below)
    code += b"\xB8\x44\x55"        # mov ax,5544h
    code += b"\xBA\x02\x00"        # mov dx,2 (native side prints the banner)
    code += b"\xCD\x2F"            # int 2Fh
    code += b"\xBA"
    patches["ok"] = len(code)
    code += b"\x00\x00"
    code += b"\xB4\x09\xCD\x21"    # mov ah,9 ; int 21h
    code += b"\xB8\x00\x4C\xCD\x21"  # mov ax,4C00h ; int 21h
    fail_handler = len(code)
    code += b"\xBA"
    patches["fail"] = len(code)
    code += b"\x00\x00"
    code += b"\xB4\x09\xCD\x21"
    code += b"\xB8\x01\x4C\xCD\x21"  # mov ax,4C01h ; int 21h
    ok_msg = len(code)
    fail_msg = ok_msg + len(b"UPCALL-OK\r\n$")
    # jne rel8: from the byte after the jne (offset 13) to the fail handler
    rel = fail_handler - 13
    assert 0 <= rel < 128, rel
    code[12] = rel
    for key, pos in patches.items():
        addr = 0x100 + (ok_msg if key == "ok" else fail_msg)
        code[pos] = addr & 0xFF
        code[pos + 1] = (addr >> 8) & 0xFF
    return bytes(code) + b"UPCALL-OK\r\n$UPCALL-FAIL\r\n$"


class Asm:
    """Tiny assembler helper for the hand-built test COMs below."""

    def __init__(self):
        self.code = bytearray()
        self.fix = []
        self.labels = {}

    def label(self, name):
        self.labels[name] = len(self.code)

    def db(self, b):
        self.code += b

    def word(self, label):
        self.fix.append((len(self.code), label))
        self.code += b"\x00\x00"

    def rel8(self, label):
        self.fix.append((len(self.code), ("rel8", label)))
        self.code += b"\x00"

    def done(self):
        for pos, spec in self.fix:
            if isinstance(spec, tuple):
                label = spec[1]
                rel = (0x100 + self.labels[label]) - (0x100 + pos + 1)
                assert -128 <= rel < 128, (label, rel)
                self.code[pos] = rel & 0xFF
            else:
                addr = 0x100 + self.labels[spec]
                self.code[pos] = addr & 0xFF
                self.code[pos + 1] = (addr >> 8) & 0xFF
        return bytes(self.code)


def dtatest():
    """Phase 0 / 0.3: a program that never sets a DTA must still get its
    find-first result written to PSP:0080h (the DOS default DTA)."""
    a = Asm()
    a.db(b"\xBA"); a.word("pattern")        # mov dx, pattern
    a.db(b"\xB9\x00\x00")                   # mov cx, 0 (normal files)
    a.db(b"\xB4\x4E")                       # mov ah, 4Eh  find first
    a.db(b"\xCD\x21")                       # int 21h
    a.db(b"\x72"); a.rel8("fail")           # jc fail
    a.db(b"\xA0\x9E\x00")                   # mov al,[009Eh]  PSP:0080+1Eh
    a.db(b"\x3C\x00"); a.db(b"\x74"); a.rel8("fail")   # cmp al,0  ; je fail
    a.db(b"\x3C\x21"); a.db(b"\x72"); a.rel8("fail")   # cmp al,'!' ; jb fail
    a.db(b"\xBA"); a.word("okmsg")
    a.db(b"\xB4\x09\xCD\x21")               # mov ah,9 ; int 21h
    a.db(b"\xB8\x00\x4C\xCD\x21")           # mov ax,4C00h ; int 21h
    a.label("fail")
    a.db(b"\xBA"); a.word("failmsg")
    a.db(b"\xB4\x09\xCD\x21")
    a.db(b"\xB8\x03\x4C\xCD\x21")           # mov ax,4C03h ; int 21h
    a.label("pattern"); a.db(b"*.*\x00")
    a.label("okmsg"); a.db(b"DTA-OK\r\n$")
    a.label("failmsg"); a.db(b"DTA-FAIL\r\n$")
    return a.done()


def int2ftest():
    """Phase 0 / 0.4: an INT 2Fh call that is not ours must leave every
    register untouched."""
    a = Asm()
    a.db(b"\xB8\xCD\xAB")                   # mov ax,0ABCDh
    a.db(b"\xBB\x34\x12")                   # mov bx,1234h
    a.db(b"\xB9\x78\x56")                   # mov cx,5678h
    a.db(b"\xCD\x2F")                       # int 2Fh (unhandled)
    a.db(b"\x3D\xCD\xAB"); a.db(b"\x75"); a.rel8("fail")   # cmp ax,0ABCDh ; jne
    a.db(b"\x81\xFB\x34\x12"); a.db(b"\x75"); a.rel8("fail")  # cmp bx,1234h
    a.db(b"\x81\xF9\x78\x56"); a.db(b"\x75"); a.rel8("fail")  # cmp cx,5678h
    a.db(b"\xBA"); a.word("okmsg")
    a.db(b"\xB4\x09\xCD\x21")
    a.db(b"\xB8\x00\x4C\xCD\x21")
    a.label("fail")
    a.db(b"\xBA"); a.word("failmsg")
    a.db(b"\xB4\x09\xCD\x21")
    a.db(b"\xB8\x04\x4C\xCD\x21")
    a.label("okmsg"); a.db(b"INT2F-OK\r\n$")
    a.label("failmsg"); a.db(b"INT2F-FAIL\r\n$")
    return a.done()


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "build"
    os.makedirs(outdir, exist_ok=True)
    outputs = {
        "HELLO.COM": hello(),
        "TELETYPE.COM": teletype(),
        "MZEXE.EXE": mzexe(),
        "RUNNER.COM": runner(),
        "VRAM.COM": vram(),
        "UPCALL.COM": upcall(),
        "DTATEST.COM": dtatest(),
        "I2FTEST.COM": int2ftest(),
    }
    for name, data in outputs.items():
        with open(os.path.join(outdir, name), "wb") as f:
            f.write(data)
    print("wrote", ", ".join(sorted(outputs)), "to", outdir)


if __name__ == "__main__":
    main()
