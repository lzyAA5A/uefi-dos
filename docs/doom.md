# DOOM 0.99 shareware (12-10-1993): what it needs and what we fixed

The archive `Doom (0.99) Shareware (12-10-1993).7z` is the pre-release shareware
that shipped two months before DOOM 1.0. This page records the test, the two
real emulator bugs it exposed, and exactly where it stops today.

## What is in the archive

```
Doom (0.99) Shareware (12-10-1993).7z
└── doom1_0.zip
    ├── DEICE.EXE  FILE_ID.DIZ  SYSOP.DOC  INSTALL.BAT
    └── DOOM1_0.DAT + DOOM1_0.1 + DOOM1_0.2 + DOOM1_0.3      (BBS split parts)

concatenate the three parts  ->  DOOM1_0.EXE   (2,147,894 bytes, LHarc SFX)
extract that                 ->  DOOM.EXE   523,221
                                 DOOM1.WAD 4,207,819
                                 SETUP.EXE  README.EXE  LICENSE.DOC ...
```

`7z t` verifies the LHarc CRCs, and re-extracting gives a byte-identical
`DOOM.EXE`, so the file used below is exactly what the archive contains.

## Shape of DOOM.EXE

`DOOM.EXE` is a **bound** executable: an MZ loader followed by the 32-bit game.

| field | value | meaning |
| --- | --- | --- |
| `e_cparhdr` | `0x20` | 512-byte MZ header |
| `e_cs:e_ip` | `0x01BD:0x2372` | entry inside the loader |
| `e_cp` / `e_cblp` | `0x007A` / `0x0064` | DOS image = 121*512 + 100 = **62,052** bytes |
| loader | DOS/16M 1.5 + "DOS/4G Copyright (C) Rational Systems, Inc. 1987 - 1993" | |
| 32-bit runtime | "DOS/4GW Professional Protected Mode Run-time" | |

So the file is `[MZ header 512][loader 61,540][bound image 461,169]`, and the
game is `Oct 13 1993 17:15:23` plus a full set of DOOM strings — the image is
present and complete, only the hand-off from loader to image fails.

## Two emulator bugs it found (both fixed in 0.10.1)

### 1. `POPF` dropped IOPL and NT — the loader thought it was a 286

DOS/16M detects the CPU in real mode by writing `FLAGS=0F000h` and reading it
back:

```
0ECD:65C2  xor ax,ax
0ECD:65C4  push ax
0ECD:65C5  popf                  ; FLAGS = 0
0ECD:65C6  pushf / pop ax
0ECD:65C8  and ah,0F0h / cmp ah,0F0h / jz  -> 286 (bits 12-15 read back as 1)
0ECD:65D6  mov ax,0F000h
0ECD:65D9  push ax
0ECD:65DA  popf                  ; IOPL=3, NT=1
0ECD:65DB  pushf / pop bx
0ECD:65DD  and bx,ax / jz -> not a 386
```

A 386/486 keeps IOPL (bits 12-13) and NT (bit 14) writable and only bit 15 is
reserved, so the read-back is `7000h` and the code takes the "386" path. Our
`case 0x9D` masked the popped value with `0x0FD5`, which is 8086/286 behaviour,
so the read-back was `0002h`, `bx&ax` was zero, and the loader printed

```
DOS/16M error: [15]  protected mode available only with 386 or 486
```

Fix in `src/emu/emu.c`:

```c
if (op_is32(e)) e->eflags = (pop32(e) & 0x00FDDFFFu) | EFLAGS_FIXED;
else            e->flags  = (u16)(((pop16(e) & 0x7FD5) | 0x0002));
```

Regression vector: `popf keeps IOPL/NT (386)` in `src/emu/itest.c`.

### 2. `0F BA` (BT/BTS/BTR/BTC with an immediate bit) was not implemented

With the CPU probe passed, the next instruction the loader executed was `0F BA`
and the emulator stopped with `unsupported 386 opcode 0F BA`. Added in
`src/emu/emu.c` next to the `0F A3/AB/B3/BB` family: decode `r/m`, take an
`imm8` bit index, set `CF` from the old bit, then OR/AND-NOT/XOR for
`/5`,`/6`,`/7`. Regression vectors: `bt ax,12 sets CF`, `bts ax,3`,
`btr ax,1`, `btc ax,0`.

### 3. The PSP environment block had no program path

Right after the two fixes the loader stopped with

```
DOS/16M error: [2]  not a DOS/16M executable 'DOOM.EXE'
```

because it opens **itself** to load the 32-bit image, and the path it uses comes
from the environment block: DOS stores the program's full path at `env:2`
(word count at `env:0`). We only ever wrote `PATH=C:\`, so the loader read
`""` and later `DOOM.ETX` garbage. `build_psp()` now writes the real DOS layout:

```
+0  word   1                     (string count)
+2  ASCIIZ C:\DOOM.EXE           (program path, DOS 3+)
+.. ASCIZ  PATH=C:\  0 0
+.. word  1 + ASCIIZ C:\DOOM.EXE (DOS 5 "extra strings" trailer)
```

The path comes from a new `g_prog_path[]`, filled by `emu_shell_exec()` and
`emu_exec_program()`. Any DOS extender (DOS/16M, DOS/4GW, DOS/32A) depends on
this, so it was a real DOS-compatibility bug, not a DOOM quirk.

## Where it stops today: the bound header signature

After the loader opens itself it does:

```
INT 21h AH=3F  read 28 bytes at file position 0      -> MZ header
S = (e_cp-1)*512 + e_cblp = 62,052 = 0F264h
INT 21h AH=42  seek to S
INT 21h AH=3F  read 6 bytes
                     cmp byte [buf+0], 'M'
                     cmp byte [buf+1], 'F'
INT 21h AH=3F  read 1 byte, requires 'M'
```

Measured with the host harness (the bytes are dumped by
`UEFIDOS_TRACE_READ=<handle>`, a new host-only knob):

```
[read] h=05 pos=00000000 w=1C d= 4D 5A 64 00 7A 00 4E 00     <- real MZ header
[read] h=05 pos=0000F264 w=06 d= 42 57 30 01 AB 00           <- file[0F264h..]
```

* The file layer is faithful: `file[0F264h..0F269h]` really is `42 57 30 01 AB 00`
  (`"BW0" 01 AB 00`), and the same bytes come back from the emulated disk.
* The block at `0F264h` is a **DOS/4GW Professional binder header** — further
  down it contains the name `4GWPRO.EXP` (0F2E4h). Its signature is `BW`.
* The loader inside this particular `DOOM.EXE` requires `MF` and then `M`.
  The two-byte string `MF` **does not occur anywhere** in the 523,221-byte file
  (only inside the 32-bit image as unrelated symbol text: `MFABIND`, `VMFILEIO`).

So this dump's loader stub and its bound image come from different binders, and
the loader rejects its own image. Patching the two signature bytes makes the
loader accept the header and start reading the image, which confirms the
diagnosis but cannot produce a runnable game: the header fields it then parses
are the ones of a different format.

## What it would take to actually run DOOM

Even with a self-consistent image the loader needs things UEFI-DOS does not
have yet (tracked in `docs/roadmap-video-usb-lfn.md` and the status table):

1. `INT 15h` XMS (or the DPMI "switchmode" path) to get memory above 1 MB —
   the emulator has 16 MB of RAM and a DPMI 0.9 host, but no XMS driver API.
2. The extender's own protected-mode switch: `MOV CR0`, a GDT, and 32-bit code
   reload — supported (`step_386`), untested with a real extender.
3. Mode 13h — supported since 0.8.0 (`GFXTEST.COM`, `VESATEST.COM`).
4. For DOOM 1.9 and later (DOS/4GW bound, self-consistent) the same three
   steps apply; that is the realistic next target, not this 0.99 dump.

## Reproducing

```powershell
# extract
7z x "Doom (0.99) Shareware (12-10-1993).7z"
7z x doom1_0.zip
# concatenate the BBS parts, then unpack the LHarc SFX
# build a test image and run it with the host harness
python tools\make_image.py build\doom.img --size-mb 64 --noboot `
  --text "CONFIG.SYS=FILES=40`nLASTDRIVE=D`n" `
  --file DOOM.EXE=DOOM.EXE --file DOOM1.WAD=DOOM1.WAD
build\hosttest.exe --shell build\doom.img "DOOM"
```

Host-only debug knobs used above: `UEFIDOS_TRACE_INTS=1` (build/int_trace.txt),
`UEFIDOS_TRACE_STEPS=n` (build/step_trace.txt), `UEFIDOS_TRACE_READ=<handle>`
(read position + first bytes for that handle).
