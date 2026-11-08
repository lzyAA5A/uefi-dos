"""Generate the protected-mode / DPMI test programs.

DPMITEST.COM -- full DPMI client bootstrap: INT 2Fh AX=1687h, switch to PM
                through the host entry point, 16-bit INT 21h, far jump to a
                32-bit flat selector, 32-bit INT 21h, INT 31h AX=0300h
                real-mode interrupt reflection, AX=0501h DOS memory
                allocation, then exit.

Both are plain .COM files (CS=DS=SS=ES=PSP, code starts at 0x100).
"""
import os
import sys

# Selectors published by the DPMI host GDT (see src/emu/dpmi.c).
SEL_CODE32 = 0x30
SEL_DATA32 = 0x38


class Asm:
    def __init__(self):
        self.code = bytearray()
        self.fix = []
        self.labels = {}

    def label(self, name):
        self.labels[name] = len(self.code)

    def db(self, b):
        self.code += b

    def w(self, label):
        """16-bit segment offset of a label (COM: 0x100 + offset)."""
        self.fix.append((len(self.code), ("abs16", label)))
        self.code += b"\x00\x00"

    def d32(self, label):
        """32-bit segment offset of a label (add it to (cs<<4) at run time)."""
        self.fix.append((len(self.code), ("abs32", label)))
        self.code += b"\x00\x00\x00\x00"

    def rel8(self, label):
        self.fix.append((len(self.code), ("rel8", label)))
        self.code += b"\x00"

    def done(self):
        for pos, spec in self.fix:
            kind, label = spec
            addr = 0x100 + self.labels[label]
            if kind == "rel8":
                rel = addr - (0x100 + pos + 1)
                assert -128 <= rel < 128, (label, rel)
                self.code[pos] = rel & 0xFF
            elif kind == "abs16":
                self.code[pos] = addr & 0xFF
                self.code[pos + 1] = (addr >> 8) & 0xFF
            elif kind == "abs32":
                self.code[pos] = addr & 0xFF
                self.code[pos + 1] = (addr >> 8) & 0xFF
                self.code[pos + 2] = (addr >> 16) & 0xFF
                self.code[pos + 3] = (addr >> 24) & 0xFF
            else:
                raise AssertionError(kind)
        return bytes(self.code)


# flat 4 GiB descriptors: base 0, limit 4K*1M, G=1
DESC_CODE32 = bytes([0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xCF, 0x00])
DESC_DATA32 = bytes([0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0xCF, 0x00])


# ---------------------------------------------------------------------------
# DPMITEST.COM: full DPMI client
# ---------------------------------------------------------------------------
def dpmitest():
    a = Asm()
    a.db(b"\xFC")                               # cld
    # save the linear base of our segment while still in real mode (after the
    # switch CS holds a selector, so cs<<4 no longer means anything)
    a.db(b"\x66\x31\xDB")                       # xor ebx,ebx
    a.db(b"\x8C\xCB")                           # mov bx,cs
    a.db(b"\x66\xC1\xE3\x04")                   # shl ebx,4
    a.db(b"\x66\x89\x1E"); a.w("segbase")        # mov [segbase],ebx
    # INT 2Fh AX=1687h : DPMI installation check
    a.db(b"\xB8\x87\x16")                       # mov ax,1687h
    a.db(b"\xBB\x00\x00")                       # mov bx,0
    a.db(b"\xCD\x2F")                           # int 2Fh
    a.db(b"\x85\xC0")                           # test ax,ax
    a.db(b"\x75"); a.rel8("fail")               # jnz fail
    a.db(b"\x89\x3E"); a.w("entry")             # mov [entry],di
    a.db(b"\x8C\x06"); a.w("entry2")            # mov [entry+2],es
    a.db(b"\xBA"); a.w("msg_host")              # mov dx,msg_host
    a.db(b"\xB4\x09\xCD\x21")                   # mov ah,9 ; int 21h
    # switch to protected mode (16-bit client)
    a.db(b"\xB8\x01\x00")                       # mov ax,1
    a.db(b"\x31\xDB")                           # xor bx,bx
    a.db(b"\xFF\x1E"); a.w("entry")             # call far [entry]
    # ---- 16-bit protected mode ----
    a.db(b"\xBA"); a.w("msg_pm16")
    a.db(b"\xB4\x09\xCD\x21")                   # mov ah,9 ; int 21h
    # ebx = linear base of this segment (saved in real mode, survives the jump)
    a.db(b"\x66\x8B\x2E"); a.w("segbase")        # mov ebp,[segbase]
    a.db(b"\x66\x89\xE8")                       # mov eax,ebp
    a.db(b"\x66\x05"); a.d32("pm32")            # add eax,<pm32>
    a.db(b"\x66\x89\x06"); a.w("farptr")        # mov [farptr],eax
    a.db(b"\xC7\x06"); a.w("farptr_pad")        # mov word [farptr+4],SEL_CODE32
    a.db(b"\x30\x00")
    a.db(b"\x66\xFF\x2E"); a.w("farptr")        # jmp far dword [farptr]
    a.db(b"\xB8\x01\x4C\xCD\x21")               # (unreachable) exit 1
    a.label("fail")
    a.db(b"\xBA"); a.w("msg_fail")
    a.db(b"\xB4\x09\xCD\x21")
    a.db(b"\xB8\x01\x4C\xCD\x21")
    # ---- 32-bit protected mode, CS = flat 32-bit code ----
    a.label("pm32")
    a.db(b"\x66\xB8\x38\x00")                   # mov ax,SEL_DATA32
    a.db(b"\x8E\xD8")                           # mov ds,ax
    a.db(b"\x8E\xC0")                           # mov es,ax
    a.db(b"\x8E\xD0")                           # mov ss,ax
    a.db(b"\xBC\xF0\xFF\x07\x00")               # mov esp,0x0007FFF0
    # print msg_pm32 through INT 21h AH=9 (EDX = linear address)
    a.db(b"\x89\xEA")                           # mov edx,ebp
    a.db(b"\x81\xC2"); a.d32("msg_pm32")        # add edx,<msg_pm32>
    a.db(b"\xB4\x09\xCD\x21")                   # mov ah,9 ; int 21h
    # --- INT 31h AX=0300h: reflect a real-mode INT 21h AH=9 ---
    a.db(b"\x66\xB8\x38\x00")                   # mov eax,SEL_DATA32
    a.db(b"\x8E\xC0")                           # mov es,ax
    a.db(b"\x89\xEF")                           # mov edi,ebp
    a.db(b"\x81\xC7"); a.d32("rmregs")          # add edi,<rmregs>
    a.db(b"\xC7\x47\x1C\x00\x09\x00\x00")       # mov dword [edi+1Ch],00000900h
    a.db(b"\xC7\x47\x14"); a.d32("msg_reflect") # mov dword [edi+14h],<msg_reflect>
    a.db(b"\xC7\x47\x2E\xFE\xFF\x00\x00")       # mov dword [edi+2Eh],0000FFFEh
    a.db(b"\x89\xE8")                           # mov eax,ebp
    a.db(b"\xC1\xE8\x04")                       # shr eax,4 -> real-mode segment
    a.db(b"\x89\x47\x24")                       # mov [edi+24h],eax  (DS)
    a.db(b"\x89\x47\x30")                       # mov [edi+30h],eax  (SS)
    a.db(b"\x66\xB8\x00\x03")                   # mov eax,0300h
    a.db(b"\xB3\x21")                           # mov bl,21h
    a.db(b"\xCD\x31")                           # int 31h
    a.db(b"\x72"); a.rel8("refl_fail")          # jc refl_fail
    a.db(b"\x89\xEA")                           # mov edx,ebp
    a.db(b"\x81\xC2"); a.d32("msg_refl_ok")
    a.db(b"\xB4\x09\xCD\x21")
    # --- INT 31h AX=0501h: allocate 16 paragraphs of DOS memory ---
    a.db(b"\x66\xB8\x01\x05")                   # mov eax,0501h
    a.db(b"\x66\xBB\x10\x00\x00\x00")           # mov ebx,10h paragraphs
    a.db(b"\xCD\x31")                           # int 31h
    a.db(b"\x72"); a.rel8("alloc_fail")         # jc alloc_fail
    a.db(b"\x89\xEA")                           # mov edx,ebp
    a.db(b"\x81\xC2"); a.d32("msg_alloc_ok")
    a.db(b"\xB4\x09\xCD\x21")
    a.db(b"\x66\xB8\x00\x4C\x00\x00")           # mov eax,4C00h
    a.db(b"\xCD\x21")
    a.label("refl_fail")
    a.db(b"\x89\xDA")
    a.db(b"\x81\xC2"); a.d32("msg_refl_fail")
    a.db(b"\xB4\x09\xCD\x21")
    a.db(b"\x66\xB8\x01\x4C\x00\x00")
    a.db(b"\xCD\x21")
    a.label("alloc_fail")
    a.db(b"\x89\xDA")
    a.db(b"\x81\xC2"); a.d32("msg_alloc_fail")
    a.db(b"\xB4\x09\xCD\x21")
    a.db(b"\x66\xB8\x02\x4C\x00\x00")
    a.db(b"\xCD\x21")
    # data: keep everything below 64 KiB so 16-bit DX/EDX keep working
    a.label("entry")
    a.db(b"\x00\x00")
    a.label("entry2")
    a.db(b"\x00\x00")
    a.label("farptr")
    a.db(b"\x00\x00\x00\x00\x30\x00")
    a.label("farptr_pad")
    a.db(b"\x00\x00")
    a.label("segbase")
    a.db(b"\x00\x00\x00\x00")
    a.label("rmregs")
    a.db(b"\x00" * 0x32)
    a.label("msg_host")
    a.db(b"DPMI-HOST-OK\r\n$")
    a.label("msg_pm16")
    a.db(b"PM16-OK\r\n$")
    a.label("msg_pm32")
    a.db(b"PM32-DOS-OK\r\n$")
    a.label("msg_refl_ok")
    a.db(b"RMREFLECT-OK\r\n$")
    a.label("msg_refl_fail")
    a.db(b"RMREFLECT-FAIL\r\n$")
    a.label("msg_alloc_ok")
    a.db(b"DOSMEM-OK\r\n$")
    a.label("msg_alloc_fail")
    a.db(b"DOSMEM-FAIL\r\n$")
    a.label("msg_reflect")
    a.db(b"REFLECTED-21H-OK\r\n$")
    a.label("msg_fail")
    a.db(b"DPMI-FAIL\r\n$")
    return a


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "build"
    os.makedirs(outdir, exist_ok=True)
    for name, builder in (("DPMITEST.COM", dpmitest),):
        a = builder()
        code = a.done()
        with open(os.path.join(outdir, name), "wb") as f:
            f.write(code)
        print("wrote %s (%d bytes) to %s" % (name, len(code), outdir))
        print("   labels:", {k: hex(0x100 + v) for k, v in sorted(a.labels.items())})


if __name__ == "__main__":
    main()
