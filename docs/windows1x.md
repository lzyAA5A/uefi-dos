# Running Microsoft Windows 1.x under UEFI-DOS

Windows 1.0/1.01 are *real-mode* DOS programs: they need a text screen, the CGA
graphics hardware, a `INT 33h` mouse driver, the timer and keyboard interrupts,
and a DOS version they consider "old enough".  This document records what had
to be added to UEFI-DOS for that, what already works, and where the runtime
still stops.

Versions tested (all original media, nothing redistributed):

| release | media | result |
| --- | --- | --- |
| Windows 1.0 Developer Release 5 (Nov 1984) | 3 x 360K | installs, but `KERNEL.EXE` wrecks its own memory before its first `INT 21h` — **identical hang on real Compaq MS-DOS 2.11** (see the DR5 section) |
| Windows 1.01 (1985) | 5 x 360K | **`SETUP` installs the whole release**; `WIN` starts the kernel, no window drawn yet |
| Windows 2.03 (1987) | 9 x 360K | not tested yet: it uses the same real-mode core as 1.01, so the same blockers apply |
| Windows/286 2.10 (1988) | 4 x 1.2M | not attempted: needs 286 protected mode + a real EMM |

## What was added for this (0.11.0)

### CGA graphics: INT 10h modes 4/5/6

The CGA bitmap lives at `B8000` in the even/odd **interleaved** layout (even
scan lines from `+0`, odd from `+0x2000`, 80 bytes per row pair) and packs 2 bit
(mode 4/5) or 1 bit (mode 6) per pixel.  `emu_video_flush()` expands it into a
linear 8bpp frame before handing it to the platform layer, so every colour path
in the firmware stays a plain palette lookup.

`INT 10h AH=0Bh` (and the `3D8h`/`3D9h` ports) select the CGA palette and the
background colour; `emu_cga_palette()` rebuilds DAC entries 0..3 from

    palette 0 = green / red / brown,  palette 1 = cyan / magenta / white

each with a low and a high intensity variant.  `AH=0Fh` now reports the real
column count (40 for 320-wide modes) and `0040:0049` holds the active mode, so
programs that sniff the BDA are not lied to.

`CGATEST.COM` (bundled) sets mode 4, selects palette 1 and paints four 80-pixel
stripes straight into VRAM through `ES=B800`/`rep stosb` — the probe the
regression suite asserts.

### INT 33h mouse driver

`src/emu/int33.c` implements the Microsoft-compatible call set: 0000h reset,
0001h/0002h show/hide, 0003h position + buttons, 0004h set position, 0005h/0006h
press/release counters, 0007h/0008h range, 000Bh motion counters, 000Ch event
handler + mask, 000Fh..0024h driver info.  The callback is a real-mode far
procedure, so the emulator pushes a return frame and lets the normal instruction
loop execute it; the loop closes the callback again when `CS:IP:SS:SP` come back
to the interrupted point.

Host side:

* UEFI: `EFI_SIMPLE_POINTER_PROTOCOL` (`Reset`/`GetState`), i.e. the PS/2 or USB
  mouse QEMU/OVMF exposes.  Two device counts are scaled into one DOS mickey, and
  a single count still moves the pointer by one so slow mice feel alive.
* Windows host harness: `UEFIDOS_MOUSE="dx,dy,buttons;..."` scripts the pointer
  so the mouse path is testable without hardware.

The pointer itself is drawn by the emulator into the frame (`g_arrow`, 12x12,
black outline / white body).  In a linear framebuffer the overwritten pixels are
saved and restored on the next flush, so the guest never sees the arrow in its
own video memory.

### Hardware interrupts: IRQ0 (timer) and IRQ1 (keyboard)

Real-mode Windows installs its **own** `INT 8` and `INT 9` handlers and expects
hardware-style delivery: the BIOS-level keyboard buffer is bypassed entirely.
`emu_run()` therefore injects an interrupt into whatever vector the guest put in
the IVT:

* `INT 8` every 55 ms (18.2 Hz) while `IF` is set and no handler is running;
* `INT 9` when a key arrives, with the scan code latched in the keyboard
  controller so `in al,60h` returns it (`0040:0017`-style flags and status port
  `64h` bit 0 follow the same latch).

`0040:006C` (the BIOS tick counter) is refreshed from the host clock every few
thousand instructions, because programs that poll the variable instead of
calling `INT 1Ah` — Windows' idle loop does exactly that — would otherwise wait
forever for time to move.

`HLT` is now an idle step while `IF` is set (`sti; hlt` is how real-mode Windows
sleeps between messages); only a `HLT` with interrupts disabled ends a program.

### SETVER: per-program DOS version

UEFI-DOS reports **MS-DOS 4.00** (0.9.0).  `WIN.COM` and `WIN100.BIN` refuse to
start on anything newer than DOS 3.x, and the Compaq DOS 2.11 `SYS.COM` wants the
old 2.10 quirk — a single global version cannot satisfy both, so the version is
now settable per program:

    SETVER WIN.COM 3.30          (shell)
    SETVER WIN100.BIN 3.30
    SETVER                       lists the entries
    SETVER -WIN.COM              removes one
    CONFIG.SYS:   SETVER=WIN.COM 3.30

`INT 21h AH=30h` compares the running program (`emu_current_program()`, set by
the shell and by `EXEC`) against the table and reports the override.  Lookup is
by file name only, so one entry covers `C:\WINDOWS\WIN.COM` and `WIN.COM`.

## Where Windows 1.01 stands

`SETUP` runs to completion through a scripted run of the five install disks
merged onto one `A:` volume (only "press Continue" keys are needed):

```
graphics adapter: 1          (CGA)
Printer: 1                   (Epson FX-80)
Setup is copying files from the Utilities disk to the C:\WINDOWS\ directory.
Microsoft Windows is set up to operate on your computer.
You are now in the Windows directory.
To start Microsoft Windows:
    - Type WIN
```

Installed: `WIN.COM`, `WIN100.BIN` (177 488), `WIN100.OVL` (214 080), `WIN.INI`,
spooler, WINOLDAP, the font set and the printer driver.

`WIN` then:

1. probes the largest free block (`AH=48h BX=FFFF` → error 8 with BX = max),
2. reads the DOS version (`AH=30h`),
3. `EXEC`s `WIN100.BIN` (an MZ executable) — the kernel loads and runs,
4. with the version check satisfied the kernel no longer prints
   `Incorrect DOS version`, but it never opens a window: it ends up in a polling
   loop (`40:001A`/`40:001C` keyboard head/tail and `40:006C` ticks) with the
   text screen untouched.

Open items, in the order they are likely to matter:

| item | why it blocks |
| --- | --- |
| `INT 21h AH=52h` returns `SFT = 0` | Windows reads the DOS list of lists; a null SFT pointer is not something real DOS would hand out |
| display driver is an installable module | 1.01 loads `CGA.DRV`/`SYSTEM.DRV` through its own loader, which needs the list of lists, `.GRB` grabber and `WIN.CNF` handling to line up |
| no `INT 15h`/XMS/EMS | Windows 1.x asks for expanded memory at startup and scales its heap from the answer |
| 40-column text mode | mode 0/1 are accepted but the mirror stays 80x25 |

None of these are architectural: they are the same class of "one more DOS
service" that made SimEarth (0.10.0) and the CardPuter86 tool set work.

## Reproducing

```
rem one A: volume holding all install disks, mounted as A:
python tools/make_tree_image.py build\win101_src.img <dir-with-all-disks> --size-mb 96
python tools/make_image.py build\w101boot.img --size-mb 128 --noboot ^
    --file build\win101_src.img=W101SRC.IMG

build\hosttest.exe --shell build\w101boot.img "MOUNT A: W101SRC.IMG" "A:SETUP"
build\hosttest.exe --shell build\w101boot.img "SETVER WIN.COM 3.30" ^
    "SETVER WIN100.BIN 3.30" "CD \WINDOWS" "WIN"
```

Set `UEFIDOS_KBD_GUARD=ENTER` so unanswered prompts continue, and
`UEFIDOS_DUMP_VRAM=1` to see the 80x25 screen the installer produced.

DR5 has its own one-shot path (`tools/install_win1x.py`, described below), and
the real-DOS control experiment is rebuilt with the shipped tools:

```
rem 360 KB DOS 2.11 boot floppy that also carries the DR5 runtime
python tools/mkfat12.py build\dr5flop360.img --360k --dir <dr5-flat-dir>
rem  ... or write the system files first so IBMBIO/IBMDOS stay the first
rem  two root entries, then copy DOS 2.11's boot sector over sector 0.

rem a plain MS-DOS 5 boot disk whose AUTOEXEC.BAT no longer starts SETUP
python tools/strip_autoexec.py <msdos5-disk01.img> build\dos5boot.img

rem type into either guest through the QEMU monitor
python tools/qtype.py --log build\serial.log --text "kernel"
```

## Windows 1.0 Developer Release 5 (4 Nov 1984)

DR5 is the release Microsoft handed to developers before Windows 1.0 shipped.
Its runtime is not a DOS program in the usual sense: `KERNEL.EXE` is an MZ stub
that builds its own MCB chain, relocates its stack and then loads the rest of the
system (`WINDOWS.EXE`, `GDI.EXE`, `MSDOS.EXE`, `TIMER.EXE`, `KEYBOARD.EXE`,
`MOUSE.EXE`, `DISPLAY.EXE` = `IBMCOLOR.DIN`, `CURSOR.EXE` = `IBMCOLOR.CIN`).
`MSWIN.BAT` is just `kernel %1 %2 %3`.

### Installing it (tools, no Microsoft files in this repository)

`tools/install_win1x.py` reproduces the layout `INSTALL.BAT` creates — runtime in
`\WINDOWS\TEST`, tools in `\WINDOWS\BIN`, docs in `\WINDOWS\DOC`, libraries in
`\LIB`, the `.DIN`/`.CIN` drivers copied to `DISPLAY.EXE`/`CURSOR.EXE` — on a
fresh UEFI-DOS image, then writes an `AUTOEXEC.BAT` that starts the kernel:

```
powershell -NoProfile -ExecutionPolicy Bypass -File run_win1x.ps1 `
    -Source "<folder with your extracted DR5 files>"
```

`start_win1x.bat` is the double-click version (it looks for `win1x-src` next to
itself, or takes a folder dragged onto it).  The image it builds,
`build\win1x.img`, is local: `build/` is git-ignored and no Microsoft file is
published.

Result: UEFI-DOS boots, `SETVER KERNEL.EXE 3.30` is applied, `KERNEL.EXE` starts
and the machine then sits on a blank screen.

### Why the kernel stops

`KERNEL.EXE` (28 880 bytes) has the header `e_ss = e_sp = 0`, `e_sp = 0x60`,
`e_minalloc = e_maxalloc = 0`, and its entry stub jumps to `0DC5:3904`
(`BOOTSTRAP`, straight from `KERNEL.MAP`).  BOOTSTRAP calls the memory setup at
`0DC5:55B3` with five arguments:

```
3925  mov es,[0x25e]      ; ES = PSP
3929  mov ax,es / add ax,0x10      ; arg3 = load segment
392e  mov bx,ss / sub bx,0x20      ; arg2 = SS - 0x20
3933  mov cx,es:[0x0002]           ; arg1 = PSP top of memory (0xA000)
3938  mov dx,0x80                  ; arg0 = command tail
3940  call 55b3
3943  or ax,ax / je 3963           ; AX = 0 -> skip the stack move
```

`55B3` rounds the two segments (`Align`, `0DC5:3C14`), then builds three MCBs:

| block | segment | size field |
| --- | --- | --- |
| free | `round4(arg3)` = `0x0D10` | `(end-0x100) - start - 1` |
| own (4 KB) | `end-0x100` | `0xFF` |
| last (`'Z'`) | `end = SS-0x20` | `top - end - 1` |

and clears each block (`zero_segments`, `0DC5:4974`).  With `SS = load segment`
(the MZ header says `e_ss = 0`, so that is what every DOS hands out — DOS and
UEFI-DOS alike) the first size comes out `0xFEDB`, i.e. **-293 paragraphs**: the
clear loop then runs 65 243 times, zeroing 16 bytes at the head of every segment
from `0x0D11` upward — including the interrupt vector table and, six iterations
in, the stack frame it is running on.  The step trace ends with

```
0DC5:4999 C2 06 00   ret 6      -> return IP was zeroed -> 0DC5:0000
0DC5:0000 00 00 00   (executing zeros forever)
```

**This is not an emulation artefact.**  The same files were installed on a real
Compaq MS-DOS 2.11 boot floppy (360 KB, built with `tools/mkfat12.py --360k` and
DOS 2.11's own boot sector) and run in QEMU: `A>kernel` hangs on a blank screen
exactly like UEFI-DOS, with no output on the screen or on COM1 — DR5's documented
"external console" — either.  Nothing reaches DOS: the kernel makes **no**
`INT 21h` call at all before it dies, so no DOS service can fix it.

In other words, this DR5 `KERNEL.EXE` destroys its own code and stack on any
standard DOS EXE load.  Windows 1.01 (1985), which is a fixed successor, gets
much further — see the section above.

### "DR5 needs DOS 2.x and an IBM PC" — both tested, neither is the blocker

DR5's own notes do say DOS 2.x on an IBM XT/AT, so both halves of that were
checked experimentally rather than argued:

* **DOS version.**  `SETVER KERNEL.EXE` was driven with **2.00, 2.10, 2.11,
  3.00 and 3.30**; all five runs ended in the same place, and so did a real
  Compaq **MS-DOS 2.11** boot (previous section).  The reason is structural:
  the kernel dies inside `BOOTSTRAP` before its **first** `INT 21h`, so no
  `AH=30h` answer can ever be observed (an `INT` trace of the run is empty,
  while the same trace of `HELLO.COM` shows its three calls).
* **"IBM PC or not".**  Two probes could have mattered and neither exists:
  `KERNEL.EXE` contains no `mov ax,0F000h` and no `1501512 COPR. IBM` string,
  so it never reads a ROM signature; the one ROM-adjacent call in `BOOTSTRAP`
  (`0DC5:4A8F`, made before the memory setup and with its result discarded)
  compares six bytes at `F000:0100` against `DGROUP:0x234`, which holds
  **`SEGDEB`** — the signature of DR5's own resident kernel debugger, not a
  machine test.  As a belt-and-braces check the emulator now publishes an IBM
  XT identity anyway (see below) and DR5 died identically; the crash *address*
  moves between runs only because the 55 ms timer interrupt lands in the
  already-corrupted code at different points.

Machine identity is now part of `install_ivt()` in `src/emu/emu.c`: the ROM
date `11/08/82` at `F000:FFF5` and the model byte `FEh` (IBM XT) at
`F000:FFFE`, alongside the BDA bytes filled in for SimEarth.  Programs of this
era read those two places to pick machine-specific patches, and an all-zero
ROM reads as "unknown machine".

The algebra also rules out any load-time rescue.  `BOOTSTRAP` passes `SS-0x20`
as the top of memory and the load segment as the bottom, and `55B3` computes

```
size = (SS - 0x20 - 0x100) - load_seg - 1 = e_ss - 0x121        (paragraphs)
```

which is independent of where the program is loaded — so `e_ss = 0` always
yields the observed `0xFEDB`.  A non-zero `e_ss` cannot help either: to make the
size positive it would have to be at least `0x122`, but the loader must place
`SS:SP` inside the relocated image (`e_ss*16 + 0x60 >= 0x7092` for the 28 802
byte file), which forces `e_ss >= 0x703` — and with any `e_ss` in that range the
4 KB block that `55B3` clears next (`[load+e_ss-0x100, load+e_ss)`) sits on top
of the code at `load+0xB5…load+0x7082`.  Free block positive and code intact
are mutually exclusive, so this `KERNEL.EXE` cannot start on any DOS.

Evidence kept in `build/` (not published): `f_kernel.png` (real DOS 2.11),
`g_dr5.png` (UEFI-DOS), `step_trace.txt`/`limit_dump.txt` (emulator trace).
