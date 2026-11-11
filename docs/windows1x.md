# Running Microsoft Windows 1.x under UEFI-DOS

Windows 1.0/1.01 are *real-mode* DOS programs: they need a text screen, the CGA
graphics hardware, a `INT 33h` mouse driver, the timer and keyboard interrupts,
and a DOS version they consider "old enough".  This document records what had
to be added to UEFI-DOS for that, what already works, and where the runtime
still stops.

Versions tested (all original media, nothing redistributed):

| release | media | result |
| --- | --- | --- |
| Windows 1.0 Developer Release 5 (Nov 1984) | 3 x 360K | kernel boots and reaches its message loop, screen stays blank |
| Windows 1.01 (1985) | 5 x 360K | **`SETUP` installs the whole release**; `WIN` starts the kernel, no window drawn yet |
| Windows 2.03 (1987) | 9 x 360K | `SETUP` starts, needs the same work as 1.01 |
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
