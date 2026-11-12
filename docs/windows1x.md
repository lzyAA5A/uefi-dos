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
| Windows 1.01 (1985) | 5 x 360K | **`SETUP` installs the whole release**; `WIN100.BIN` now passes the entire DOS-internals validation (SFT chain, SDA, InDOS flag, PSP/JFT) and then exits with code 1 before loading a driver — see the 1.01 section |
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

`start_win1x.bat` is the double-click version and needs no arguments: it takes a
folder dragged onto it, else looks for `win1x-src` next to itself, else picks up
an already installed tree (see the Windows 1.01 section below).  `-Source` and
`-Installed` force either side; `UEFIDOS_WIN1X_SRC` / `UEFIDOS_WIN1X_INSTALLED`
set them from the environment.  The image it builds, `build\win1x.img`, is
local: `build/` is git-ignored and no Microsoft file is published — and the
script injects the freshly built `BOOTX64.EFI`/`BOOTIA32.EFI` into `EFI\BOOT`,
because an installed `C:\WINDOWS` tree has no boot loader of its own (without it
the firmware silently falls through to its built-in UEFI Shell).

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

### The patched arena crashes on real MS-DOS 5: the patch route is refuted

The two-byte experiment (`ne_heap` 0x0200 -> 0x9000 at file 0x410 plus the heap block's owner
immediate at `CS:7343`) makes the module database allocate inside UEFI-DOS, so it looked like
a candidate fix. The same patched `WIN100.BIN` was therefore run on the reference machine -
real MS-DOS 5.00, Windows tree exposed as a vvfat hard disk, `win` typed through the QEMU
monitor (`ref_dos5_hd.py`, screenshot `build/ref_own.png`) - and it dies during boot:

```
Microsoft Windows
Version 1.01
Copyright (c) Microsoft Corporation, 1985. All Rights Reserved.
Microsoft is a registered trademark of Microsoft Corp. Incorrect DOS version
...
FatalExit code = 8
stack overflow
```

Two independent conclusions:

1. **The patched values are wrong, not merely different.** A bigger `ne_heap` with a free-owned
   heap block makes the kernel overrun its own stack on the machine where it normally works, so
   `ne_heap = 0x0200` and `owner = FFFF` on the pool blocks are both correct.
2. **The arena is not where we diverge.** On real MS-DOS 5 the kernel's arena shows the module
   database at `0FCC M owner=0FCD size=001D` (29 paragraphs = 464 bytes, i.e. the 435-byte
   request) and a heap of `0FEA M owner=0FCD size=064B` (1611 paragraphs = 25 KB) owned by the
   same segment. A 25 KB heap cannot come from the NE header's 0x0200 field, so on the real
   machine `[instance+0x10]` was **overwritten with available memory before the heap was built**
   - which is exactly what the `CS:66F8..6705` block does:

   ```
   66f8 les %ss:0x2,%bx
   66fd mov %bp,%es:0x10(%bx)      ; available memory -> [instance+0x10]
   6702 mov %bp,%es:0x0e(%bx)      ; and -> [instance+0x0E]
   6705 ...                        ; then the 670F gate / 6745 -> 64B4
   ```

   Our run never executes `0x66E0..0x675B` (proved with `UEFIDOS_TRACE_WINDOW=66e0-6760`: only
   the exit path `0x675C mov $1,%al / call 0x38DD` is logged). We arrive at the module
   instantiation with the pristine `ne_heap`, the 428-byte heap, and the database request that
   cannot fit - and the walk that would have patched the field is exactly what the failed
   allocation prevents. It is a deadlock in the boot *path*, not a DOS service difference.

So the remaining question changed shape: not "how do we make the allocator accept the block",
but **why does the real kernel execute `CS:66F8` before the instantiation while we reach
`CS:6642` directly**. On real MS-DOS 5 the recorded translation-block order starts with
`670F -> 6713 -> 6745 -> 64B4 -> 64BB (lcall *CS:006Eh) -> 64C6 -> 64CA`, i.e. the gate runs
before the inner block; in our run the same code is entered from `CS:63AD` via a `ret` and the
gate is skipped entirely. That entry difference is the next thing to chase, with the
instrumented-kernel-on-real-DOS route (a COM1 logger patched into `CS:73AB`) as the way to see
the reference values at the exact instant.

### Ground truth from the real machine: the boot path is the same, the load address is not

Instead of guessing which branch the reference run takes, the kernel itself was instrumented:
each of ten boot-path instructions was replaced by a `jmp` to a stub in the image's zero
padding that ORs a bit into a word in the BIOS data area tail (`linear 0x04E2`) and increments
a hit counter, replays the relocated instruction and jumps back. One `pmemsave 0 0xa0000`
after `win` then says exactly which points ran.

Sites and the resulting mask (real MS-DOS 5.00, `build/refm_tr_*.bin`, mask `0x0237`):

| CS offset | instruction | bit | hit |
|---|---|---|---|
| 0x64E5 | NE entry (`mov cs:[0x10],es`) | 0x0001 | yes |
| 0x653E | `mov ax,es:[0xFE]` - the XW/GW command-tail check | 0x0002 | yes |
| 0x6551 | `incw cs:0x1c` (XW/GW path taken) | 0x0004 | yes |
| 0x655C | rebuild-command-line fallback | 0x0008 | **no** |
| 0x658B | heap prep (`mov cs:0x10 -> ES`, then the 73AB arguments) | 0x0010 | yes |
| 0x6642 | `call 0x2085` module instantiate | 0x0020 | yes |
| 0x6664 | `call 0x0BAA` | 0x0040 | **no** |
| 0x66FC | `les ss:2,bx` - the `[instance+0x10] = BP` patch | 0x0080 | **no** |
| 0x670F | the gate (`push ss` + `call 0x3398`) | 0x0100 | **no** |
| 0x73AB | the private arena/heap builder | 0x0200 | (mask bit set) |

Two conclusions that change the picture:

1. **The boot path is not the difference.** Real MS-DOS 5 executes exactly the sequence our
   emulator executes - entry, XW check, heap prep, module instantiate - and it too never
   reaches the `0x66FC` field patch or the `0x670F` gate. So our run is not taking a "wrong
   branch"; both take the same one, and both are refused by the module-database allocation.
2. **The load address is the difference.** In that reference run the kernel ran with
   `CS = 0x74B9`, which (the stub's own arithmetic `dest = CS - 0x20 + seg_sector`) makes the
   MZ image base `0x7480` and, with the image being `0x2B40` paragraphs, exactly
   `0x9FC0 = top of memory`. Our emulator runs the same kernel at `CS = 0x0D49`, i.e. image
   base `0x0D10`, right above `COMMAND.COM`. The *code offsets* are identical (CS:0x64E5 is
   image `0x6875` in both runs, since `image = CSoff + 0x390`), so everything up to and
   including the allocation is executing the same bytes at the same relative addresses.

What this buys: the `instance` computation `instance = SS + (SP>>4)` with `SS = load` and
`SP = 0x200` (set by the KERNSTUB at `0x13..0x1A`) yields `load + 0x20` in both runs, so
`[instance+0x10]` reads image `0x210` = `NE+0x10` = the `ne_heap` field (`0x0200`) in both.
Either the reference run's heap really is 428 bytes as well - and the database request must
then be smaller there - or the kernel rewrites that field somewhere not covered by these ten
sites. That is the next measurement, and it needs the builder's frame arguments rather than
the caller's addresses.

#### Instrumentation channels: what works and what does not

* **Works**: patching code with a 5-byte `jmp` into the image's zero padding (`CS:7454`;
  image `0x77E4..0x7A80` is unused), replaying the relocated bytes, and writing results
  either to BDA bits (`0x04E0`/`0x04E2`) or into image padding; read back with QEMU
  `pmemsave 0 0xa0000`. The channel was proven by instrumenting the NE entry, where the
  recorded `CS` came back as `0x74B9`.
* **Fails**: COM1 logging (the port ends up carrying binary fragments of the kernel image, so
  the guest was executing data - the write goes out while the display driver is live);
  `INT 10h AH=0Eh` logging (Windows installs its own INT 10h for graphics mode, and the
  teletype calls shred the splash screen into CP437 garbage); BDA cells at `0x04E0`/`0x04F0`
  are rewritten by the BIOS/Windows within seconds, so single-cell results are unreliable -
  prefer image padding plus a signature scan of the whole dump.

### Instrumenting the reference run: what the channels cost, and the one number still missing

Getting numbers out of a kernel that has already switched the display to graphics is the hard
part of this investigation, so the working recipe is worth recording:

* **Works**: `jmp` from the instrumented instruction into the image's zero padding
  (`CS:7454`, image `0x77E4..0x7A80`), replay the relocated bytes, `jmp` back; store results
  in the BIOS "inter-application communication area" `linear 0x04F0..0x04FF` with `DS = 0` or
  `ES = 0` (absolute addressing) and read them back with `pmemsave 0 0xa0000`.
  That area survived a complete boot (the marker signature `55 A5` written there at the NE
  entry was still intact 25 s later), and it proved the reference run's kernel executes with
  **`CS = 0x74B9`** - image base `0x7480`, flush against the top of memory.
* **Fails**: `CS`-relative stores (a stub that does `mov ds,cs` and writes `CS:0x04F0`
  actually writes image `0x880`, which the kernel's own arena initialisation zeroes later);
  the BIOS data area at `0x04E0..0x04EF` (the BIOS and Windows rewrite it, which is why the
  first marker run reported a mask of `0xE47F` with 18 thousand hits - noise);
  COM1 (the port ended up carrying fragments of the kernel image, i.e. the guest was
  executing data); `INT 10h AH=0Eh` (Windows hooks INT 10h for graphics mode and the
  teletype calls shred the splash).
* **Reached on the real machine**: the arena/heap builder at `CS:73AB` *is* called - a stub
  there logged its first call - which finally proves the reference kernel goes through the
  same private-arena path we do. The first call passes **all-zero arguments** (an early
  probe), so the logged frame must target a later call, not the first.

So the single number still missing is the argument set of the *real* builder call (top / load /
instance / heap term). Logging every call and reading the last one closes that gap, and it is
the next thing to run: with it we either learn that the reference heap term really is
`0x0200` - in which case the divergence is the *request* size, not the heap - or we get the
larger number the 25 KB reference heap implies.

### A trap worth writing down: relocating a relative `call` breaks it

The second marker run gave per-site counters in the BIOS inter-application area
(`0x04F0..0x04F7`, one byte per site). It reported

```
64E5 entry=1   658B heap-prep=0   65AC call73AB=1   6642 instantiate=0
66FC patch=0   670F gate=0        73AB builder=0    65AF after-builder=0
```

which is impossible - `0x65AC` is `call 0x73AB`, so reaching it must reach the callee's first
instruction and then the return point. The reason is a bug in the instrumentation, not in the
kernel: the stub **replays the relocated instruction**, and a relative `call`/`jmp` replayed
from a different address jumps somewhere else. `E8 FC 0D` executed at `CS:0x7454` instead of
`CS:0x65AC` calls `0x8253`, not `0x73AB` - so the callee and everything after it never ran.

Rule for the next round: any instrumented site whose first instruction is a relative branch
must be replayed by *emulating* it (push the original return address and jump to the absolute
target), or the site must be moved to a point whose first instruction is absolute (a `mov`,
`les`, `push`). All the sites that only test "did we get here" should point at non-branch
instructions.

The two other facts from that run are still valid and useful: the kernel reaches
`CS:0x64E5` exactly once, and the placement experiment (`UEFIDOS_EXEC_HIGH=1`) now really puts
the child flush against the top of memory - `psp=0x744B`, `CS=0x7494`, `DS(instance)=0x747B`,
`SS=0x7BDA` - which is what the reference machine looks like (`CS=0x74B9`), and the module
database allocation still returns `AX=1` there. So the load address is *not* the lever either.

### The heap term is identical on the real machine - so the difference is elsewhere

Reading the reference dump directly settles the question this round was chasing. With the
kernel running from its copy at image base `0x7480` (so `cs = 0x74B9`), the kernel's own cells
in `build/refm_c1_*.bin` hold:

```
cs:6498 (instance) = 0x74A0        == 0x7480 + 0x20, exactly the value our emulator computes
cs:6496            = 0x0000
cs:649A            = 0x0040
cs:64B2            = 0x0746
[instance+0x10]    = 0x0200        == the file's ne_heap field, unpatched
NE@load+0x200      = 'NE' 0x0004
```

and the file itself also says `0x0200`. So on the working reference machine:

* `instance = load + 0x20` - **the same** as ours (`0x0D30` with `load = 0x0D10`, `0x747B`/`0x749B`
  with the high placement),
* `[instance+0x10]` - **the same** `0x0200` heap term, which the builder turns into the same
  428-byte block (`'M' owner=0000 size=001B` at `load+2`).

The whole "the reference machine must be getting a bigger heap term" line is therefore dead:
the reference machine builds the same tiny heap we do, and the module-database request
(`0x19D + lenbyte + 1` = 435 bytes = 29 paragraphs) cannot fit it either. What differs is what
happens *next*: the 25 KB heap and the 29-paragraph database block that the reference arena
shows at `0x0FEA` / `0x0FCC` four seconds into the boot must be the product of a **second**
build - the `CS:66F8..0x6705` block that writes available memory into `[instance+0x10]` and
`[instance+0x0E]` - i.e. the reference kernel *recovers* from the failed allocation and rebuilds
its arena bigger, while ours falls into the `ES = 0001h` path and exits.

That reframes the remaining work: not "give the kernel a bigger heap up front" (that is what
the patched-image experiment did, and it crashes the real kernel with `FatalExit 8 / stack
overflow`), but "find what makes the reference kernel take the recovery path after the first
allocation fails". The 0x2085 failure branch at `0x2165..0x216B` and the loop at `0x6632` are
where to look, and the marker instrumentation for it must avoid relocating relative branches
(see the previous section).

### The allocation failure path, and the kernel heap bootstrap

Disassembly of the failed branch (file offset = CS offset + 0x590):

```
2123 b8 07 00     mov $0x7,%ax          ; flags
2126 33 db        xor %bx,%bx           ; size hint = 0
2128 50 57 53     push ax; push di; push bx
212b e8 5c e8     call 0x98a            ; GlobalAlloc(shift=0, size=DI, flags=7)
212e 0b c0        or  %ax,%ax
2130 74 33        je  0x2165            ; failure -> return 1, no retry
...
2156 8b d7        mov %di,%dx           ; SUCCESS path only: read the module header
2158 b4 3f        mov $0x3f,%ah
215a cd 21        int $0x21             ; DOS read of 0x40 bytes into the new block
215c 72 07        jb  0x2165
215e 8d 75 c0     lea -0x40(%di),%si
2161 3b c1        cmp %cx,%ax           ; must read the whole header
2163 74 0c        je  0x2171
2165 ff 76 fc     push -0x4(%bp)
2168 e8 88 e8     call 0x9f3            ; release the just-allocated block
216b 33 c0 40     xor %ax,%ax; inc %ax ; return 1  (bogus "segment")
216e e9 5f 02     jmp 0x23d0
```

Two consequences: (a) the failure path performs **no retry and no grow** — it frees the block and returns the bogus handle 1, which the caller (`CS:664D mov es,ax`) installs as a segment, so the kernel writes its module database into segment `0001` (the IVT); (b) although the module database is a *heap* allocation, its content is filled by a plain `INT 21h AH=3F` read of the file — which is why our INT trace never shows an `AH=3F` for `WIN100.BIN`: the read lives only on the success path.

The heap bootstrap (`CS:73AB`, `inc bp` prologue, args at `[bp+6..0xC]`):

```
73b2..73bb  ax=load  bx=instance  cx=top  dx=[instance+0x10]
73be        call 0x72d1           ; build the private MCB arena, returns the heap segment in BX
73c1        mov %ds,%cs:0x2       ; heap anchor cell (0x9FE1 in the stock layout)
73c8..73d9  heap+0x06=BX  heap+0x08=DX  heap+0x04=5  heap+0x18=0x1A=0x1E=0
73dc        heap+0x12 = 0x20     ; first block offset
73e1        heap+0x14 = 0x6163   ; grow callback (far pointer into the kernel)
73e6..73f5  bx = (0x23)&~3 = 0x20;  cx = ((cx-3)&~3) - 0x20;  cx >>= 1; cx >>= 1
73fb        mov %cx,(%bx)         ; first block header: unit count (4-byte units)
73ff        heap+0x10 = 0x22      ; free-list head
7404        call 0x53b6           ; initialise the free-list descriptor table
740e..7427  call 0x5389 (twice)   ; allocate size-class descriptors
741a        mov %bx,%cs:0x0       ; publish the heap header offset
```

Unit-count arithmetic confirms the usable-size formula measured earlier (a stock `ne_heap = 0x200` yields 428 bytes), and the arena block that the module database has to come out of is the one whose owner word is **0** — the block at `load+2`, whose paragraph count is `(ne_heap >> 4) - 5` (27 paragraphs for `ne_heap = 0x200`, 31 paragraphs for `0x240`).

That last relation explains both experiments at once: raising `ne_heap` to `0x240` alone makes the block 31 paragraphs and the 29-paragraph request fits; raising it to `0x1000` does **not** help (measured again with a clean patch at file offset `0x410`: `0x212E AX=0000`, `ES=0D12`, still 77 DOS calls and `AH=4C AL=1`), because the allocator's split path refuses to carve a block out of that arena entry once the heap sits elsewhere. So the kernel needs both a larger `ne_heap` **and** a heap/anchor layout that matches, which is exactly what the real machine had before it produced the 25 KB heap seen in the reference dump.

### The strip is the only allocatable block, and the request is 30 paragraphs

Allocator arithmetic, now measured with registers (`UEFIDOS_TRACE_WINDOW=5a80-5af0`,
image `build/w101h.img`, `ne_heap = 0x240`):

```
2079 0D49:5AA0 AX=0D12 ... DX=001E SI=0D12 DI=0000 ES=0D12   ; candidate block = the 27-paragraph strip
2080 0D49:5AA4 AX=001B ... DX=001E                          ; AX = block size (0x1B)
2081 0D49:5AA5 AX=001C ... DX=001E                          ; AX = size + 1
2083 0D49:5AE2 AX=001C ... DX=001E                          ; cmp %dx,%ax  ->  0x1C < 0x1E
2084 0D49:5AE4 ...                                          ; ret, caller takes `jb` -> block skipped
```

so the module database needs **DX = 0x1E = 30 paragraphs** (444 bytes rounded up plus the MCB
overhead) while the strip offers `size + 1 = 0x1C = 28`. The request is 30 for every plausible
name length (a 6-char name still asks for 29), so *which* module name is used cannot save it.

The reason the strip is the only candidate is in the arena builder itself (`CS:72D1`, complete):

```
72d1  lea 0x1(%bx),%si ; and $0xfffe,%si    ; si = load        (block 1)
72d8  lea 0x2(%si),%di ; and $0xfffe,%di    ; di = load + 2    (block 2 = the strip)
72ea  sub %dx,%cx ; neg %cx ; and $0xfe,%cl ; cx = top - heap paragraphs  (heap block)
72f1  dec %dx ; and $0xfe,%dl               ; dx = top - 1     ('Z' block)
72f5  dec %ax ; and $0xfe,%al               ; ax = instance - 1 (block 4, the big middle one)
...
7304  movw $0xffff,0x1(%bx)  ; block 1 owner = FFFF
7320  movw $0xffff,0x1(%bx)  ; 'Z' block owner = FFFF
7340  movw $0xffff,0x1(%bx)  ; heap block owner = FFFF
7365  movw $0xffff,0x1(%bx)  ; big middle block owner = FFFF
7384  mov    %bx,0x1(%bx)    ; strip owner = BX = 0   <- the only owner-0 block
```

and since the allocator's acceptance test is `cmp %di,%es:0x1(%di)` with `DI = 0`
(`CS:5A84`), **only the strip can ever satisfy an allocation**. Its size is
`instance - load - 5 = 0x1B` (27) for every `ne_heap`, because both ends of the strip
are fixed by `load` and `instance` — which is why raising `ne_heap` from `0x200` to `0x240`
grew the *middle* block (`0D2E`, size `0x926D`) and the heap block (`9F9C`, size `0x21`),
but left the strip at `0x1B` (measured in `build/a240.bin`).

### The reference dumps show the same strip, so the contradiction is now precise

Locating the kernel by its NE header (`4e45 0400 5200 3001 c707 ...`) in the real MS-DOS 5
dumps gives image base `0x74800` (= `0x9FC0 - 0x2B40`, flush against the top of memory) and

```
[instance+0x10] = 0x0200     [instance+0] = 'NE'     (build/refm_c1_0.bin)
```

so the reference machine runs with exactly our numbers: instance = `load + 0x20`, module
database request 30 paragraphs, strip 27 paragraphs. On our side the allocation therefore
fails, `CS:216B` returns 1, the caller installs `ES = 0001`, the walk reads `es:001C` out of
the IVT and the kernel exits with `AH=4C AL=1`.

Two candidate explanations remain, and they are now the only two:
1. the reference machine reaches `CS:212B` with a *different* `DX` (e.g. it re-entered through
   the module instantiate twice and the second call passes a size hint), or
2. the reference machine survives the failed allocation because on a real IVT the word at
   `0001:001C` (the INT 7 vector) makes `CS:0E8A cmp %si,%es:0x1C; jbe` take the *other*
   branch (`0EDC`) instead of the "return 0" path we take, so the kernel keeps going.

Both are decided by a single real-machine probe: store `DI` at `CS:2123`, store `AX`/`DX` at
`CS:5AE2`, and mark whether `CS:2130 je` is taken, all into the BIOS scratch area at
`linear 0x04F0` with `DS = 0` (the technique already proven for the NE-entry site). That probe
is the next step.

### Decisive real-machine probes: the allocation SUCCEEDS, and it is called with a different `load`

Five instrumented boots of real MS-DOS 5 / Windows 1.01 settled the questions left open above.
The technique: patch `WIN100.BIN` (file offset = CS offset + 0x590) so a 3-byte instruction site
jumps to a stub placed in the image's zero padding at `CS:5AE5`, the stub records registers into
`CS:5B48`, replays the replaced instruction and jumps back. Results are located in the 640 KB
`pmemsave` dump by searching for the marker words.

| probe | site | what it recorded | result |
|---|---|---|---|
| 6 | `CS:212B` (call to `0x98A`) | `DI` = requested size | **`DI = 0x1B3` = 435 bytes — identical to ours** |
| 7, 8 | `CS:2132` (after the call) | `AX` = returned segment | **`AX = 0x0FCD`, a real segment — the allocation SUCCEEDS** |
| 9 | `CS:667A` (call to `0x0E71`) | arg `SI`, returned `AX` | `SI = 1`, `AX = 0` — **the walk returns 0 on the real machine too** |
| 10 | `CS:73BE` (call to `0x72D1`) | `AX`,`BX`,`CX`,`DX` | **`instance = 0x74A0`, `load = 0x0FCA`, `top = 0x9FC0`, heap term = `0x0200`** |

Probe 10 is the answer to the whole puzzle. The arguments the kernel hands to its arena builder are

```
instance    = 0x74A0   (DGROUP of the copy that runs, flush against the top of memory)
load        = 0x0FCA   (PSP + 0x10 = 0x0FBA + 0x10 — the ORIGINAL load segment)
top         = 0x9FC0
heap term   = 0x0200   (the plain NE field, unpatched)
```

so the only allocatable block — the strip between `load + 2` and `instance - 2` — spans
`0x0FCC .. 0x749E` = **25 809 paragraphs**, which is why a 30-paragraph request succeeds there and
lands at `0x0FCD` (exactly the MCB seen in the earlier reference dumps: `0FCC M owner=0FCD size=001D`).
Our emulator instead runs the image in place, so `instance = load + 0x20` and the same strip is
27 paragraphs — one third of the request.

The kernel therefore relocates itself to the top of memory while keeping its PSP (and hence `load`)
at the low address DOS gave it. `WIN.COM` carries the primitive for exactly this at file `0x4A1`:

```
4a1 push %es ; push %ds
4a3 mov 0x27c,%bx        ; paragraph count
4a7 mov %bx,%cx
4a9 mov $0x48,%ah ; int 21h      ; allocate the destination block
4ad mov %ax,%es ; xchg %ax,0x27a ; mov %ax,%ds
4b5 xor %si,%si ; xor %di,%di ; shl cx x3 ; rep movsw
4c2 pop %ds ; pop %es ; ret
```

and the size gate just before the exec:

```
5e3 add $0x89f,%ax
5e6 mov $0x4,%cl ; shr %cl,%ax ; mov %ax,%bx
5ec push %ss ; pop %es
5ee mov $0x4a,%ah ; int 21h      ; shrink WIN.COM's own block to BX paragraphs
5f2 mov $0xffff,%bx
5f5 mov $0x48,%ah ; int 21h      ; ask for the largest free block (fails, returns it in BX)
5f9 cmp %cs:0x103,%bx
5fe jae 0x609                    ; enough memory -> continue
600 call 0x645 ; call 0x2de ; jmp 0x2bf   ; otherwise complain and quit
609 cmpw $0x2120,%cs:0x81
610 jne 0x644
612 sub %cs:0x103,%bx
...
```

So the next thing to establish is which of these two branches our run takes and what
`[cs:0x103]` holds — our emulator does answer `AH=48 BX=FFFFh` with a largest-block value, but the
comparison at `CS:5F9` and the `0x4A1` copy are the two places where the relocation can be skipped.

## Windows 1.01: what the kernel asks DOS for

`WIN.COM` is a small launcher: it saves `INT 21h`, checks the DOS version, works
out how much memory it has and `EXEC`s `WIN100.BIN` (177 488 bytes, MZ with
`e_cparhdr=0x20`, `e_ss=0x0B`, `e_sp=0x80`).  `WIN100.BIN` is the real-mode
kernel, and before it loads anything it validates the DOS internals it intends
to swap per task.  That validation is where it used to die, and every step of it
needed a structure UEFI-DOS had not published yet.

Disassembly anchor: for `WIN100.BIN`, `CS:0000` is file offset `0x590`.  That is
proved twice over — the string `"Incorrect DOS version"`, which the kernel
prints with `CS:6E7Ch` in DX, sits at file offset `0x740Ch`, and the `INT 22h`
stub the kernel installs at `CS:6E95h` matches file `0x7425h`.  The
"grab the DOS internals" routine spans `CS:6FF1h..72D0h`:

| kernel code | what it needs from DOS |
| --- | --- |
| `CS:7004h..7010h` | `AH=30h` must report a **2.x or 3.x** major version (`AL >= 2 && AL < 4`), so `SETVER WIN.COM 3.30` plus `SETVER WIN100.BIN 3.30` is required while UEFI-DOS reports 4.00 |
| `CS:7034h..704Ch` | `AH=52h` list of lists: `+4` must be a **dword far pointer** to the SFT chain, which the kernel walks by following each block's first dword until the first word reads `FFFFh` |
| `CS:7054h..708Fh` | `AH=25h`/`AH=35h` on `INT 22h`, then it **reads the vector back** through the slot it expects (`[CS:002Ah]` = `0x88`) |
| `CS:70AEh..70E4h` | `AH=5D06h` swappable data area: `+0Ch` current DTA, `+10h` current PSP, `+16h` current drive, `+17h` extended break flag |
| `CS:716Eh..7176h` | `AH=34h` pointer to the InDOS byte |
| `CS:727Bh..72A8h` | compares `SDA+10h` with its own PSP, `SDA+0` (critical error flag) with 0, and `SDA+16h` with the drive `AH=19h` reports |

Everything below was added to UEFI-DOS for that (`src/emu/int21.c`):

* `sft_build()` — one SFT block at `MAGIC_SEG:DOS_SFT_OFF` (`0xC800`) whose
  first dword is `FFFFh:0000h`, entry count at `+4`, then 59-byte entries
  (`+00` next entry, `+04` handle count, `+06` open mode, `+08` attribute,
  `+09` device info word, `+0B` starting cluster, `+0D` time, `+0F` date,
  `+11` position, `+2E` 8.3 name).  `AH=52h` publishes it at `+4` of the list of
  lists.  Windows keeps its private `INT 22h` inside the unused terminator
  entry, exactly as it does on real DOS, so that block has to stay writable RAM.
* `sda_build()` — swappable data area at `MAGIC_SEG:DOS_SDA_OFF` (`0xDC00`)
  following the documented DOS 3.10-3.30/4.x layout.  `AH=5D06h` returns
  `DS:SI`, `CX=0x100`, `DX=0x20`; `AH=5D0Bh` returns the DOS 4.0 style list of
  areas; `AH=1Ah` mirrors a new DTA into `SDA+0Ch`.
* `AH=34h` returns `ES:BX = MAGIC_SEG:(DOS_SDA_OFF+1)`, the InDOS byte.
* `AH=50h` sets the current PSP (it used to fall through to "success, do
  nothing"), `AH=51h` reads it; `g_dos.psp_seg` and the emulator copy move
  together and `load_com_at`/`load_exe_at` fill `g_dos.psp_seg` for the child.
* `dos_jft_sync()` — the job file table at `PSP:18h`: each slot holds the SFT
  index of the open handle (`0FFh` when free), refreshed on every `INT 21h` and
  right after each open, create and close.
* All of it writes by **linear address** (`mag_wb`/`mag_ww`/`mag_wd`/`psp_wb`)
  because `INT 21h` can be reflected from a DPMI client, where
  `emu_wb(e, MAGIC_SEG, ...)` would treat `0F000h` as a selector and latch a
  protection fault.  That mistake cost `DPMITEST` its 32-bit half until the
  regression suite caught it.

Current status: `WIN100.BIN` now walks the whole validation chain (no more
"Incorrect DOS version"), reads `WIN100.OVL`, sets the PSP to its own segment,
closes every handle and exits with code 1 — silently, with nothing painted and
no driver opened.  The last DOS call before the exit is `AH=50h`; the decision is
taken inside the instance walk at `CS:667Ah -> 0E71h`, which returns 0, and
`CS:6681h` is the `jmp 0x675Ch` exit path.  The next thing to work out is what
that instance walk expects (`ES:1Ch` count, `ES:22h` table) and whether the
kernel gives up because it never loaded a module.

Reproducing — quickest path, double-click `start_win1x.bat` (it auto-detects the
media / installed tree, writes the `SETVER` + `WIN` AUTOEXEC.BAT, drops the UEFI
loaders into the image and boots QEMU).  Or hand-run the same steps:

```powershell
python tools/fat_extract.py extract "<disk>.IMG" F:\1231\_tmp\win\w101
python F:\1231\_tmp\win\mk_w101run.py          # hand-built C:\WINDOWS (see below)
powershell -NoProfile -ExecutionPolicy Bypass -File run_win1x.ps1 -Installed F:\1231\_tmp\win\w101run
build\hosttest.exe --shell build\w101run.img "SETVER WIN.COM 3.30" "SETVER WIN100.BIN 3.30" "CD \WINDOWS" "WIN"
```

`run_win1x.ps1` looks for `-Source` (win1x-src, or a folder dragged onto the
.bat), then for an installed tree (`win1x-src\installed`, else the scratch
`_tmp\win\w101run`).  `UEFIDOS_WIN1X_SRC` / `UEFIDOS_WIN1X_INSTALLED` override
both.  The image it builds is `build\win1x.img`, and the ISO-free boot is the
firmware's own `EFI\BOOT` path — the tree gets the freshly built
`BOOTX64.EFI`/`BOOTIA32.EFI` injected because an installed `C:\WINDOWS` tree does
not carry them.

`SETUP` copies the startup files and the fonts but skips `WIN.CNF` and every
driver, so the image is laid out by hand: `WIN.COM`, `WIN100.BIN`,
`WIN100.OVL` and `WIN.INI` from the install; `MSDOS.EXE`, `WIN.CNF`,
`SYSTEM.DRV`, `CGA.DRV`, `KEYBUS.DRV`, `NOMOUSE.DRV`, `MOUSE.DRV`, `COMM.DRV`,
`EPSON.DRV`, `SPOOLER.EXE`, `WINOLDAP.MOD`, `CGA.GRB`, `CGA.LGO` and the font
files from the media.

## Reference run: Windows 1.01 on real MS-DOS 5.00 (and where UEFI-DOS still diverges)

To tell "our DOS is wrong" apart from "the installed tree is wrong", the same
files were booted under a real MS-DOS 5.00 in QEMU, with the tree exposed as a
hard disk by QEMU's `vvfat` driver:

    python _tmp\win\ref_dos5_hd.py 22 "c:;win" 25

(`-fda build\dos5boot.img -hda fat:rw:<dir>`; the tree directory must contain
the *whole* media set plus the installed runtime files, i.e. 153 files.)

**Result: Windows 1.01 runs.** `C:\> win` paints the MS-DOS Executive window
with its `File View Special` menu bar, the drive/document panel and the
directory listing, in CGA graphics.  Two things follow from that:

* the media, `WIN100.BIN`/`WIN100.OVL` (both produced by SETUP) and the whole
  file set are fine — the black screen seen earlier came from an *incomplete*
  tree (a `C:\WINDOWS` holding only the 24 files SETUP copied), not from bad
  binaries;
* UEFI-DOS is what diverges: on the very same input it never reaches `INT 10h`.

### Bug fixed: the EXEC command tail carried a stray CR (INT 21h AH=4Bh)

`INT 21h AH=4Bh` reads the count-prefixed command line from the parameter
block.  That string **ends with CR (0Dh)**, and the old handler copied the CR
through into the child's argument string, so `build_psp()` wrote the child's
tail as `" \r\r"` instead of `" \r"`.  Real-mode Windows 1.x derives the name of
its overlay from that tail — it rewrites the last three characters to `OVL` and
opens the result — so the stray CR made it open the wrong file, fall into the
"try the next name" path (`incb %es:(%si)` at `CS:661Dh` bumps a byte of the
name) and carry on with a garbage module segment.

`src/emu/int21.c` (case 0x4B) now stops at CR/NUL:

    u8 len = 0;
    for (u16 i = 0; i < n; i++) {
        u8 c = emu_rb(e, seg, (u16)(cmd_off + 1 + i));
        if (c == '\r' || c == 0) break;
        args[len++] = (char)c;
    }
    args[len] = 0;

Verified with the host probe `UEFIDOS_TRACE_PSP=1` (new, see the harness table):
the child PSP tail went from `len=3 " \r\r"` to `len=1 "\r"`, and the first open
in `WIN100.BIN`'s startup went from `AX=FFFFh` (failure, retry path) to
`AX=0005h` (handle 5, straight through `CS:6635h`).

### What still stops it inside UEFI-DOS

With the tail fixed, `WIN100.BIN` gets one step further and then aborts inside
its own memory setup — no DOS call is involved, so nothing in the trace looks
wrong:

1. `CS:65ACh` calls `73ABh`, which calls `72D1h`; that routine **rebuilds the
   MCB chain itself** from `(load_seg=2010h, instance=2030h, top=0A000h)` and
   returns the heap anchor segment, stored at `CS:0002h` (`= 9FE1h` in our run).
2. The anchor is fine, but `73ABh` sizes the first free heap block from
   `instance+10h`, and in our run `[2030h:0010h] = 0200h`, i.e. 512 bytes ⇒
   `((0x200+0Fh)>>4 - 3) & ~3 - 0x20` gives a **428-byte** heap.
3. The kernel's first `GlobalAlloc` asks for `1BCh` (444 bytes) — larger than
   428 — so `53 89h` finds no block, `54 FAh` returns 0, and `98 Ah` returns 0.
4. The loader's failure path (`CS:216Bh`: `xor ax,ax; inc ax`) then returns
   **1**, which the caller accepts as a segment (`CS:6649h` only rejects 0), so
   `ES` becomes `0001h` and the module walk at `0E71h` sees `es:001Ch = 0` and
   fails ⇒ `CS:675Ch` ⇒ `exit(1)`.

So the remaining question is precise: **what should `[instance+10h]` be?** On
real DOS the same instruction sequence ends up with a multi-hundred-kilobyte
arena (the reference dump of the low 640 KB shows dozens of MCBs owned by
Windows PSPs, i.e. Windows allocating its segments through `INT 21h AH=48h`;
our run makes *no* `AH=48h` call at all).  `AH=48h BX=FFFFh` answers correctly
here (`BX=9275h`), so the free-block reporting is not the difference — the
value is computed inside the kernel from its own instance header, which means
the divergence is either the address/size of the block our EXEC hands the child
(`emu_exec_program()` uses a fixed `PSP = 2000h + 1000h*depth` instead of the
first free block above the parent) or an instruction the kernel uses while
building that header.

New host probes used for this (both `#ifdef UEFIDOS_HOST`, env-gated):
`UEFIDOS_TRACE_PSP=1` prints each PSP's command tail plus the 16 bytes after it;
`UEFIDOS_TRACE_EXEC=1` prints every `INT 21h AH=4Bh` (path, env seg, command-line
far pointer, first bytes, caller PSP, caller DS) and every failing `AH=48h`
(requested and largest-free paragraphs).

### Also changed: the EXEC'd child gets the first free block above its caller

`emu_exec_program()` used to put every child at the fixed address
`PSP = 2000h + 1000h*depth`.  Real DOS hands the child the first free block
above the caller (that is why `WIN.COM` shrinks its own block with
`INT 21h AH=4Ah BX=0089h` before it EXECs `WIN100.BIN`), so the child PSP is now
`caller_psp + caller_block_paragraphs + 1`, rounded up to even, as long as the
image still fits below `A000h`; otherwise the old fixed address is used.

With this, `WIN.COM`'s child PSP moved from `2000h` to `0D8Ah` — exactly the
address DOS would pick, and the "bytes after the tail" the probe prints now come
from the program's own data instead of the space Windows had just zeroed.  It
did **not** by itself unblock the kernel (see the section above: the heap is
still sized from the same image word), but the arena layout a program sees now
matches a real DOS, and all 30 regression checks stay green.

## Reference dump analysis: what the real MS-DOS 5 run does that we do not

Method: boot real MS-DOS 5.00 with the Windows 1.01 tree as a `vvfat` hard disk,
type `c:` and `win`, and `pmemsave 0 655360` five times at one-second intervals
(`_tmp/win/ref_memdump2.py`, analysis in `_tmp/win/an_mem*.py`).  UEFI-DOS is
dumped with the new host knob `UEFIDOS_DUMP_MEM=<file>`, which writes the low
640 KB as `emu_run()` returns, i.e. exactly at the moment the program stops.

Facts established from the real dump (all reproducible):

| item | real MS-DOS 5.00 | UEFI-DOS |
| --- | --- | --- |
| `WIN100.BIN` load segment | `0FB2h` (CS base linear `0FEB0h`) | `0D10h` |
| PSP top-of-memory (`PSP+2`) | `9FC0h` (DOS 5 keeps 64 paragraphs) | `9FC0h` (since `DOS_MEM_TOP`) |
| first `mov ss` in the stub | `SS=load_seg, SP=0200h` | same |
| kernel's arena MCB signature | not at `load_seg:0` | `'M'` at `0D10h:0` |
| kernel's own "Z" block | not at `9FFEh` | `'Z'` at `9FFEh` |
| kernel's `CS:[6498]` (instance) | `749Fh` | `0D30h` = `load+20h` |
| `[instance+10h]` (module heap) | `F88Bh` (a large word) | `0200h` (`ne_heap` of the image) |
| module heap placed at | top of conventional memory | `9FE1h` (same place) |
| heap bytes available / wanted | enough / `1BCh` | `428` / `444` |

The chain that stops the kernel in UEFI-DOS (every step from
`UEFIDOS_TRACE_WINDOW`):

1. `CS:6506h..650Eh` computes the instance as `SS + (SP>>4)` and stores it in
   `CS:[6498h]`.  Both runs arrive here with `SS = load_seg`, `SP = 0200h`, so
   the instance is `load+20h` — which is **inside the loaded image**.
2. `CS:6590h..65ACh` calls the arena builder `CS:73ABh -> CS:72D1h` with
   (top=`A000h`, load_seg, instance, `ES:[instance+10h]`).  The last value is
   the module's heap size: `0200h` from the image, because in our run the
   instance points at the image's own `NE` header (`image+200h`).
3. `CS:72D1h` builds the kernel's private MCB chain and puts the module heap at
   the top of conventional memory (`9FE1h`, 464 bytes derived from `0200h`),
   leaving `428` usable bytes.
4. `CS:6642h -> CS:2085h` wants `1BCh` = 444 bytes for the first `GlobalAlloc`
   of the module database.  428 < 444 by 16 bytes, so `CS:212Bh -> 98Ah ->
   54FAh` returns 0, `CS:216Bh` turns that into `AX=1`, the caller only rejects
   zero, `ES` becomes `0001h`, the module walk `CS:0E71h` finds `es:001Ch = 0`
   and `CS:675Ch` calls the C runtime `exit(1)` — no `INT 10h` ever happens.

In the real run the instance is `749Fh`, a segment that is **not** inside the
loaded image: a relocated copy of the image lives at `7480h` (its first bytes
are the image's first bytes) and `7480h + 2B40h = 9FC0h` is exactly the top of
memory, i.e. the copy is placed flush against the top.  `SS` is `747Fh`, one
paragraph below that copy, so `SS + (SP>>4) = 749Fh` — the instance points into
free/relocated memory, and the word at `+10h` (`F88Bh`) is large, so the first
allocation succeeds.  In other words: **the kernel relocates itself before it
sizes its heap, and in our run that relocation does not happen.**

The relocation is not an `INT 21h AH=48h/4Ah` call — the code image contains no
`B4 48 CD 21` / `B4 4A CD 21` sequence at all.  The stub at `load_seg:0` that
runs before the far jump only sets `SS:SP = load:0200h`, scans for the `NE`
signature at `image+200h` and returns through `load:0080h`.  So the relocation
lives in the `CS` image and is skipped because of something it reads — the
remaining candidates are `PSP+2` (`9FC0h` vs `A000h`, the one value that is
known to differ) or a value derived from the DOS data area.

Next step: make the reported top-of-memory `9FC0h` and, if that alone does not
move the instance, trace the `CS` window `0x0000..0x64E5` with
`UEFIDOS_TRACE_WINDOW=0D49:0000-64E5` (a large but bounded trace) to find the
branch that decides whether to relocate.

## Startup path comparison and three targeted probes

Top-of-memory was the first suspect and is now matched (`PSP+2 = 9FC0h` in both
runs).  It changed nothing, so this round went after the exact divergence with
better references:

### The entry stub is identical on both DOSes

Disassembling `WIN100.BIN` at file `0x200..0x590` (that is `load_seg:0`, while
the `CS` code segment starts `0x390` bytes later at `load_seg+39h`) gives:

```
0000: push cs / pop ds                 ; DS = CS = load_seg
0002: mov si,0x130 / add si,0x1ff / and si,0xfe00    ; si = 0200h
000D: cmp word [si],0x454e             ; 'NE' at image+200h
0013: mov ax,ds / cli / mov ss,ax / mov sp,si / sti  ; SS = load_seg, SP = 0200h
```

So `SS:SP = load_seg:0200h` on **both** systems, and the instance formula
`CS:6506h: mov ax,ss / add ax,sp>>4` therefore yields `load+20h` on both — which
means the real run cannot have been at `CS:6506h` with the stub's stack either.
It got there after switching stacks, which is exactly the step we skip.

### Real run reaches `CS:670Fh` first; ours jumps straight to `CS:64E5h`

`-d exec -dfilter` (see below) over the real run shows the order
`670Fh → 6713h → 6745h → 64B4h → 64BBh (lcall *CS:006Eh) → 64C6h → 64CAh`.
`CS:670Fh` is the routine that

* saves the current `SS` into `CS:0018h` and `CS:0016h`,
* installs `INT 3Fh` (`CS:1369h`, old vector saved in `CS:0086h/0088h`),
* and then gates on the instance: `CS:673Dh: mov es,CS:[6498h] ; cmp word es:000Ah,0`
  — **non-zero exits with `mov ax,4C01h / int 21h`**, zero continues through
  `CS:6745h: mov ax,64B4h / push ax / jmp 69E8h`.

UEFI-DOS never executes `670Fh` at all: a window trace on `6730h-6765h` shows
only the *other* exit (`CS:675Ch: mov al,1 / push / call 38DDh`, i.e. the MS C
runtime `exit(1)`), reached from `CS:6681h`.  Our kernel reaches the
`CS:64E5h` block directly through a `ret` from `CS:63ADh`, i.e. before the
relocation/stack switch that the real run performs first.

### Probe 1 — enlarge `ne_heap` (scratch image, not shipped)

`WIN100.BIN` file `0x410` is the `NE` header's heap field (`0200h`).  Raising it
to `4000h` moved the run from 53 to **77** `INT 21h` calls and let it get past
the first allocation in some paths, but it still ended in `AH=4Ch AL=1`.

### Probe 2 — make the instance read the `NE` signature (the decisive one)

If the stack segment is one paragraph below the image, `SS + (SP>>4)` computes
`load+1Fh`, so `[instance+10h]` lands exactly on the `NE` signature
(`454Eh` = 17742) instead of `ne_heap`.  Done by patching the stub's `cli`
(file `0x215`) into `dec ax`:

```
instance  = 0D2Fh  (was 0D30h)
heap word = 454Eh  -> 17712 bytes
heap area = 9B6Bh .. 9FBE0h  (0x9FC0h - 0x9B6Bh = 455h paragraphs, verified in a
           memory dump: the initialiser at CS:53B6h writes 1143 free-list slots)
```

Result: **the first `GlobalAlloc(0x1BCh)` still returns 0** (`CS:212Eh: or ax,ax /
je 2165h`), and the run still ends with `AH=4Ch AL=1`.  So the heap *size* is not
the blocker: the kernel is handed 17 KB of free heap and its own allocator
(`CS:212Bh -> 98Ah -> 61EEh -> 54FAh -> 5389h` search) still finds nothing.

The three DOS-version branches were also tried through `SETVER` (major 2 ->
`CS:7677h`, major 3 minor<10 -> `CS:76B3h`, 3.1+ -> `AH=5D06h` SDA): all three
end in `AH=4Ch AL=1` (79 / 79 / 77 INT calls respectively).

### Host knob added: `UEFIDOS_EXEC_HIGH=1`

Places an `EXEC`'d child flush against the top of memory
(`psp = DOS_MEM_TOP - image_paras - 0x20`, the layout the real kernel ends up
running in).  Behaviour was bit-for-bit unchanged (77 INT calls, same exit), so
the kernel does not depend on where DOS put it — it does its own placement.

### Tooling notes from this round

* QEMU's gdb stub is usable in principle (`-gdb tcp::1234`, standard RSP), but a
  hand-written client that interrupts the target leaves the vCPU frozen while
  the "continue" packet is outstanding, and the run then looks like "Windows
  never started".  Treat `-d exec -dfilter` + `pmemsave` as the reliable path.
* `-d exec -dfilter 0x16300+0x300` prints translated-block starts with
  `[cs_base/linear_rip]`, which is enough to prove *whether* a kernel offset was
  executed; it cannot tell which program owns a linear address during boot, so
  never read early entries as "the kernel did this".
* `-d int` only logs hardware interrupts (no `INT 21h`), so it cannot replace the
  UEFI-DOS `UEFIDOS_TRACE_INTS` trace.

### Where the next attempt should look

The failure is inside the kernel's own heap after a successful arena build:
trace `CS:5389h` (the free-list search) on the Probe-2 image and compare the
list head it reads against what `CS:72D1h`/`CS:73ABh` wrote, plus the heap
anchor cell `CS:0002h`.  The arena the kernel hand-writes in our run is
`'M' owner=FFFFh size=0100h` at `0D10h:0`, another `'M'` at `0D12h:0` and
`'Z' owner=FFFFh size=0001h` at `9FBEh:0` — note that `9FBEh` is *not*
`9FFEh`, so the kernel's own `'Z'` block ends 32 paragraphs below the reported
top of memory, which is one structural difference from the real run worth
checking against the search's block walk.


## Windows 1.01 renders its splash screen (post-1.1.0)

Status: **`WIN` switches to CGA 640x200 and draws the "MICROSOFT" logo**, then the
kernel still exits with code 1.  Before the two fixes below it never reached a
single `INT 10h`.

### Bug 1 — EXEC must copy the whole 128-byte command tail

`INT 21h AH=4Bh` used to copy only the count-prefixed string and to stop at the
first CR (`src/emu/int21.c`, case `0x4B`).  Real DOS copies the caller's entire
`PSP:0080h..00FFh` tail buffer into the child PSP, and pre-3.0 software hides
data there: `WIN.COM` runs `mov word ptr [00FEh],'XW'` (`a3 fe 00`, file offset
`0x321`) before it EXECs `WIN100.BIN`, and the kernel checks that word at
`CS:653Eh`:

```
653E: mov ax,es:[0xFE]       ; ES = own PSP
6542: cmp ax,0x5758 ('XW') ; je 0x6551   -> mark "started by WIN.COM", skip the
6547: cmp ax,0x5747 ('GW') ; jne 0x655C     command-line rebuild at 0x655C
```

Without the marker the kernel rebuilt its own command line, and every later step
worked from that longer name.  `emu_exec_program_ex()` now takes the caller's
raw tail (`build_psp_ex(..., const u8 *tail128)`), so the 128 bytes go across
verbatim.

### Bug 2 — the tail count byte excludes the terminating CR

DOS puts the character count at `PSP:0080h` **without** the trailing CR, so a
bare command line is `count=0` with the CR at `0081h`; UEFI-DOS wrote `count=1`
(the CR counted).  `WIN.COM` appends the overlay path at `0081h+count` and never
updates the count:

```
WIN.COM 0x43E: mov di,0x80
        0x443: mov al,[di]        ; existing count
        0x445: add al,cl          ; + 13 (path length)
        0x44D: jle 0x451          ; clamp to keep count+13 <= 126
        0x453: mov al,[di]
        0x455: inc di
        0x456: add di,ax          ; <-- append point = 0081h + count
        0x458: mov al,0x0D / stosb
        0x45B: mov ax,cx   / stosb
        0x465: AH=19h, 'A'+drive, ':'     ; "C:"
        0x471: rep movsb                  ; "WIN100.BIN"
```

With `count=1` everything moved two bytes and the kernel's own reader
(`CS:0x65E5..0x6603`, which walks `count`, string, string and rewrites the last
three characters of the second string to `OVL`) picked up a 29-30 character
expanded path instead of `C:WIN100.BIN`.

Verification: `tools/make_tailtest.py` assembles `TAILCHK.COM`, which prints the
three interesting cells; `run_tests.ps1` asserts both forms:

```
C:\> TAILCHK
COUNT=00 C81=0D FE=0000
C:\> TAILCHK ABC
COUNT=04 C81=20 FE=0000
```

For Windows itself the dump of the child PSP now reads
`00 | 0D 0D "C:WIN100.OVL"` with `'XW'` at `00FEh` — the same layout a real
MS-DOS 5.00 child gets (see the reference dump below).

### What is still missing: 16 bytes of local heap

With the tail right, the kernel's module-database allocation
(`CS:2123h`, `call 0x98A` -> `GlobalAlloc`) asks for `DI = 0x1BB..0x1BC` (443-444)
bytes and fails.  The heap it carves for itself is `ne_heap` from the image's own
NE header (`[instance+0x10]` = `0x0200`), which leaves 428 usable bytes at
`0D12h:0` (`'M' owner=0000h size=001Bh`), followed by a 582 KB free block
(`'M' owner=FFFFh size=8E3Bh`) that the kernel's own allocator refuses because it
only re-uses blocks it owns (`CS:5A84h`: `cmp di,[es:di+1]`).

Under real MS-DOS 5.00 the same call asks for **425** bytes, because there the
module name is the short tail name `C:WIN100.BIN` (13 characters) instead of the
expanded path.  So the remaining gap is the kernel's name expansion, not the
arena: the arena, the SFT, the SDA and the EXEC contract are now all correct.

Reference arena under real MS-DOS 5.00 (`pmemsave` of a running Windows, MCB walk
of the dump; `rd4.bin`):

```
0B57 M owner=0008 size=02A4    COMMAND.COM
0DFC M owner=0DFD size=0116    WIN.COM
0F13 M owner=0F2A size=0003
0F17 M owner=0000 size=0000
0F18 M owner=0DFD size=0010
0F29 M owner=0F2A size=008B
0FB5 M owner=0FBA size=0003
0FB9 Z owner=0FBA size=9006    WIN100.BIN owns everything up to 9FC0h
```

and the reference child PSP (`0FBAh`): `0080h` = `00`, then `0D 0D "C:WIN100.OVL"`,
`00FEh` = `'XW'`.

### Reproducing

```powershell
# host harness, no firmware needed
$env:UEFIDOS_HARD_STEPS='80000000'
build\hosttest.exe --shell build\w101dbg.img 'SETVER WIN.COM 3.30' `
    'SETVER WIN100.BIN 3.30' 'CD \WINDOWS' 'WIN'
# -> "gfx: mode set 640x200x8", the logo, then back to text

# real firmware: boot the install and burst-screenshot the window
powershell -File run_win1x.ps1 -Installed F:\1231\_tmp\win\w101run -Rebuild -Headless -TimeoutSec 55
powershell -File tools\gfx_burst.ps1 -StartSec 22 -Count 50 -IntervalMs 350
python tools\ppm2png.py build\burst\shot002.ppm build\shot_w101_qemu.png
```

`tools/gfx_burst.ps1` + `tools/qburst.py` exist because the splash is on screen
for well under a second: a single timed `screendump` misses it, so the tools keep
one monitor connection open and fire 40-60 dumps.  `tools/gfx_burst.ps1` quotes
the *path* inside `-drive`/`-serial` values (`file:"C:\x.img"`); quoting the whole
`file=...` token makes QEMU read `file=C:\x` as a character-device driver name.

### Screen hand-back (1.1.2)

The splash is on screen for well under a second and then the kernel exits, so
`WIN.COM` resets the video mode.  That reset used to leave the logo behind: the
scaled frame only covers the middle of the screen, the console repaints just its
own grid, and the leftover pixels stayed until something overwrote them.  Two
calls now fix it — `gfx_clear()` wipes the whole linear framebuffer (called from
both `plat_video_gfx_begin()` and `plat_video_text()`), and `plat_video_text()`
also blanks the 25x80 console grid, since the text mirror only sends rows that
are not blank and the firmware would otherwise keep (and redraw) stale cells.

## The two INT 21h calls Windows 1.x uses to size its module database (1.1.2 .. 1.1.3)

The kernel now gets all the way past module instantiation in a *flat* install
layout, and the two bugs that got it there were both in calls whose answers we
had invented rather than measured.

### AH=47h "get current directory" was formatted wrong

The old handler prepended a backslash (`\WINDOWS`) and returned `AX=0000` for
the root.  RBIL 2147 says the path "does not include a drive or the initial
backslash", and a reference run of real MS-DOS 5.00 (`ref_cwd2.png`) confirms
`AX=0100h` on success — including in the root, where the string is empty.

Why it mattered: `WIN100.BIN` builds its overlay name at `CS:1D2Bh` by writing
`C:`, then `\`, then letting DOS drop the cwd straight into the same buffer
(`AH=47h` with `DS:SI` pointing just past the backslash).  With the leading
backslash we added, the name became `C:\\WINDOWS\WIN100.OVL` — 30 bytes with a
duplicated separator instead of the correct 20.

The name length then feeds the module-database `GlobalAlloc` request at
`CS:2123h`: `DI = 0x19D + lenbyte + 1`, where `lenbyte` is `strlen(path) + 8`
computed by the `repnz scas` at `CS:1C04h`.  The kernel's own local heap comes
from `[instance+0x10]` — the NE `ne_heap` field, 0x200 — which yields 428 usable
bytes.  A 30-byte name asked for 0x1BC (444) and failed, so `CS:212Bh` returned
0, `CS:216Bh` turned that into segment `0001h`, and the kernel wrote its module
header into the IVT and quit (`CS:675Ch` → `exit(1)`).

The fix (`src/emu/int21.c`, `case 0x47`) honours DL, writes
`g_dos.drives[d].cwd` with no leading backslash, NUL-terminates, and sets
`AX=0100h`.  Measured with the new `CWDCHK.COM` probe against real MS-DOS 5.00
and UEFI-DOS:

```
cd CF=0 rc=3B00
SUB    CF=0 AX=0100 [CWDTMP]      <- real DOS and UEFI-DOS agree
cd CF=0 rc=3B00
ROOT   CF=0 AX=0100 []
cd CF=0 rc=3B00
SUB-C:  CF=0 AX=0100 [CWDTMP]
```

### AH=57h "get file date and time" was a stub returning zeros

Windows opens `C:\WIN100.OVL`, calls `AH=57h` once, closes it again **without
reading a byte** (`docs`/serial traces show `AH=3D`, `AH=57`, `AH=3E` and no
`AH=3F` at all).  A zero timestamp is not a harmless placeholder for that
sequence — it is the value the staleness check compares against.

`dos_handle` now carries `u16 time, date` filled from the directory entry
(`raw[22..25]`) at both handle-creation sites in `src/kernel/dos.c`, `AH=57h`
returns them (and stores on `AL=1`), and `sft_entry_write()` publishes them at
`+0Dh`/`+0Fh` instead of zeros.  `src/host/dual_test.c` round-trips the values
through a real file (`[4.1b]`, part of `hosttest.exe --phase46`).

### What the two fixes changed, measured

| layout | module-name request | DOS calls before it quit |
| --- | --- | --- |
| `C:\WINDOWS\WIN` (installed tree) | `DI=0x1BB` (443 B) → fails | 53 |
| `C:\WINDOWS\WIN` after AH=47h | `DI=0x1BB` (443 B) → fails | 53 |
| flat root (`WIN` with everything in `C:\`) | `DI=0x1B3` (435 B) | 129, reaches graphics |

The flat layout mirrors what the real-DOS reference run does (its `C:` is a
flat copy of the media, so its cwd is empty and the joined name is short).  In
that layout UEFI-DOS now issues 129 DOS calls, sets a CGA mode and draws the
splash (`build/w101flat.png`, 640x200) before it still exits with code 1 right
after opening and timestamping `WIN100.OVL`.

Remaining divergence, in one line: the overlay is opened but never read, so the
kernel still bails between `AH=57h` and the module load; the heap it computes
(`[instance+0x10]` = `ne_heap` 0x200 → 428 usable) is also still smaller than
the 435 bytes it asks for in that layout.

## Where the kernel stops after 1.1.3: its own allocator, not DOS (1.1.4)

The splash now draws, and every DOS service the kernel asks for comes back with the
right answer. It still exits with code 1 before painting the first window frame.
This round pinned down exactly which instruction decides that, and ruled out three
plausible explanations. All numbers below are measured, not guessed.

### The decision point

`WIN100.BIN` at `CS:6642h` calls `CS:2085h` ("instantiate a module"). Inside, at
`CS:2123h`, it computes a byte count and calls `CS:98Ah` (GlobalAlloc):

| step | value | meaning |
| --- | --- | --- |
| `CS:209A` `mov al,[si]` | `0x15` = 21 | length byte of the module path = 13-char name + 8 |
| `CS:209C` `inc ax` | `0x16` = 22 | |
| `CS:2123` `DI` | `0x1B3` = 435 | bytes wanted: `0x19D + (lenbyte + 1)` |
| `CS:212B` `call 98Ah` | `AX = 0000` | **allocation fails**, after ~440 steps inside the allocator |
| `CS:2130` `je 2165` | taken | |
| `CS:216B` `xor ax,ax / inc ax` | `AX = 1` | "failure" marker returned to the caller |
| `CS:6649` `or ax,ax / je` | not taken | the caller only rejects 0, so it accepts the marker |
| `CS:664F` `mov es,ax` | `ES = 0001` | segment 0001 = the interrupt vector table |
| `CS:6653..665E` | | writes into segment 0001 |
| `CS:667A` `call 0E71h` | `AX = 0000` | module walk fails |
| `CS:6681` `jmp 675Ch` | | `mov al,1 / call 38DDh` = MS C `exit(1)` |
| `CS:394E` | | `mov ah,4Ch / int 21h` -> exit code 1, **zero `INT 10h` calls** |

So the whole abort hangs off one failed 435-byte allocation inside the kernel's own
memory manager. DOS is not consulted: the complete call trace contains **no
`AH=48h`/`AH=49h`/`AH=4Ah`** after the `EXEC`.

### Where the 428-byte wall came from, and why raising it did not help

`CS:65ACh` calls `CS:73ABh` with four arguments, visible in the register trace:

| arg | value | source |
| --- | --- | --- |
| `[bp+0Ah]` -> `AX` | `0D9Ch` | load segment (`PSP+0x10`) |
| `[bp+08h]` -> `BX` | `0DBCh` | instance = `SS + (SP>>4)` = load + `0x20` |
| `[bp+0Ch]` -> `CX` | `9FC0h` | top of memory from `ES:[0002h]` = `PSP+2` |
| `[bp+06h]` -> `DX` | `0200h` | `[instance+10h]` = the NE header's `heap` field |

`CS:72D1h` turns that heap field into an MCB chain, and `CS:73EFh`
(`sub 3,cx / and 0FCh,cl / sub bx,cx / shr 1,shr 1`) turns 0x200 bytes = 32
paragraphs into a **428-byte** user area — one byte less than the 435 the kernel
wants, so the very first allocation fails.

Two experiments were run to confirm that this is a symptom rather than the cause:

1. Patching the NE `heap` field at **file offset 0x410** (image `0x210`, `NE+0x10`)
   from `0x0200` to `0x1000` really does reach the kernel: the call trace then shows
   `DX = 1000h` at `CS:65ACh` and the heap is built much larger. **The 435-byte
   allocation still returns 0.**
2. Loading the kernel at the top of memory instead of below `COMMAND.COM`
   (`UEFIDOS_EXEC_HIGH=1`, host knob) puts `CS` at `7494h`, `DS` at `747Bh`,
   `SS` at `7BDAh` — the layout the real MS-DOS 5 run ends up with. **Identical
   failure at `CS:664Fh`.**
3. Raising the reported DOS version from 2.11 to 3.30 via `SETVER` only changes
   which validation branch the kernel takes (79 vs 77 DOS calls); the same abort.

### What the real MS-DOS 5 run shows

- The overlay name it builds is `C:WIN100.OVL` (12 characters, **no** leading
  backslash when the current directory is the root), and a second copy
  `C:\WIN100.OVL` exists at linear `0x00FE68h` — both found in the reference dumps
  `build/rd1.bin`..`build/rd4.bin`.
- The kernel image sits at segment `0F92h`..`0FB2h` (linear `0x0FB20h` for the
  segment that carries the code — established by matching a 32-byte needle from
  file offset `0x6990h`, which lands at linear `0x162B0h`), and by one second into
  the run the original MZ and NE headers are **no longer in memory at all**: the
  kernel has moved its module header into a private block.
- Its own MCB chain is built by the same `CS:72D1h` code and ends with
  `Z FF FF 01 00 00 9E 9F` at `0x9FBEh`, byte-for-byte what ours builds.

### What this leaves

The remaining difference is inside the segment-chain search at `CS:594Fh`, whose
accept condition is `cmp %bh,0x3(%si)` (`CS:597Fh`) followed by
`mov ax,es:[di+3] / inc ax / cmp ax,dx / jb` (`CS:598Ah`): a candidate block is only
usable when its type byte matches the high byte of the request *and* it is large
enough. Our chain visits `ES = 0D9Ch, 0D9Eh, 0DBAh, ...` (the blocks the kernel
itself just built from `load`, `instance`, `top`) and rejects every one, which means
the kernel expects a block layout that real DOS produces by *moving itself to the
top of memory first*. Reproducing that move - or finding the DOS call that makes the
real kernel do it - is the next concrete step; the probes needed are already in the
tree (`UEFIDOS_TRACE_WINDOW`, `UEFIDOS_WATCH`, `UEFIDOS_TRACE_PSP`,
`UEFIDOS_DUMP_MEM`, `UEFIDOS_EXEC_HIGH`).

Reproduction commands used for every number above:

```
python tools\make_tree_image.py build\w101flat.img F:\1231\_tmp\win\w101full --size-mb 64
$env:UEFIDOS_TRACE_WINDOW='6500-6770'; $env:UEFIDOS_TRACE_STEPS='6000000'
build\hosttest.exe --shell build\w101flat.img 'SETVER WIN.COM 3.30' 'SETVER WIN100.BIN 3.30' 'WIN'
```

### Second round on the same wall: the allocator's split/absorb logic (1.1.4)

Three more measurements, all with the arena snapshot probe from this release
(`UEFIDOS_DUMP_AT=0DD5:212B` -> `build/mem_at.bin`):

| experiment | result |
| --- | --- |
| NE `heap` swept through `0x4000`, `0x8000`, `0xC000`, `0xF000` | the allocation at `CS:212Bh` returns `0` for every value (52 DOS calls, `exit(1)`) |
| memory snapshot at the failing call | DOS-side MCB walk from `PSP-1` finds no MCB at all (`0D89` holds `0x4C`); the kernel's own chain is `0D9Ch size 0001` -> `0D9Eh size 001Bh` -> `0DBAh size 91E5h` (free) -> heap header `9FA1h` -> `Z FF FF 01 00 ...` at `9FBEh` |
| free-list inspection in the same snapshot | the bucket test *passes*: the node at `9FA1h:0026h` is `bb 0d 00 00 2e 00 ff ff` (block segment `0DBBh`, size `002Eh`, end marker `FFFFh`), and `[node+3]` is `00`, exactly the value `CS:597Fh` compares `BH` against |
| trace of the allocator's chain-end path (`CS:59D0h..5B40h`) | the search does reach the split for the 591 KB block (`DX = AX = 91E6h` at `CS:5A82h`), but `CS:5A82h` walks only 3 chain blocks (`0DBAh -> 9FA0h -> 9FBEh`), calls the grow helper `CS:5AA0h` on each, and when `CX` reaches 0 it does `xor ax,ax` -> return 0 |

The accept rule, read from the disassembly:

```
594f: push cx; push ax
5951: add $0x1,%bx                 ; requested paragraphs + 1
5954: call 0x542c
5957: mov 0x4(%di),%cx ; mov 0x6(%di),%es     ; head of the block chain
5972: mov 0xa(%di),%si                        ; free-list node
597f: cmp %bh,0x3(%si)              ; size bucket (high byte of the request) must match
598a: mov %es:0x3(%di),%ax ; inc %ax ; cmp %dx,%ax ; jb skip
5996: call 0x5a82                   ; split / absorb neighbours
```

`CS:5A82h` walks the chain and calls `CS:5AA0h` on each block, which refuses to extend
when the *next* block is not `Z` and does not carry flag bit 3
(`testb $0x8,0x5(%di)`), nor has `[block+0x1Eh] == 0`. In our run the block after the
591 KB free block is the kernel's own local-heap header at `9FA0h/9FA1h`, so every
extension attempt fails and the chain simply runs out.

That is the same shape of answer as before, one level lower: the kernel builds an arena
in which **the region above `instance-1` is advertised as free while its own 177 KB
image still lives there**. On real MS-DOS 5 the kernel is executing from a copy that sits
flush against the top of memory (segment `7480h`..`9FC0h`), i.e. the image is *outside*
the free region it advertises. Our runs never produce that copy, whether the image is
loaded low (`CS=0DD5h`) or flush against the top (`UEFIDOS_EXEC_HIGH=1`, `CS=7494h`) -
both fail at exactly `CS:664Fh`. Reproducing whatever makes the real kernel relocate
itself is now the single remaining unknown; the snapshot probe from this release is what
the real-DOS side needs so that the two arenas can be compared field by field.

## Kernel heap sizing: measured, and why it is not the blocker (1.1.4+)

Everything below was measured, not inferred. The point of the round was to stop
guessing which memory value the Windows 1.01 kernel wants.

### The child image layout (WIN100.BIN, 177,488 bytes)

* `e_cparhdr = 0x20` **paragraphs** = 0x200 bytes, so DOS loads the module starting
  at **file offset 0x200**. The in-memory image is therefore `file - 0x200`:
  * image `0x000` = file `0x200` = **KERNSTUB** (`push cs / pop ds / mov si,0x130 /
    add si,0x1ff / and si,0xfe00` -> `si = 0x200`; `cmpw [si],'NE'`; on failure it
    prints `KERNSTUB: Error during boot` from `CS:0x286` and `mov ax,4C01h / int 21h`).
  * image `0x200` = file `0x400` = **NE header** (rev 0x0052, `ne_cseg = 1`,
    `ne_csip = 0x64E5` -> segment index 1, `ne_heap` at NE+0x10 = **file 0x410**,
    segment table at NE+0x22, align shift at NE+0x32 = 4, entry 0 sector = 0x40).
  * image `0x590` = file `0x790` = the code of **segment 0**, i.e. `CS offset + 0x590
    = file offset` was wrong; the correct relation is `CS = load + 0x390 bytes`
    (`0x39` paragraphs) for this file, because `di = CS - 0x20 + (sector << (alignshift-4))`
    with `sector = 0x40`, `alignshift = 4` gives `di = load + 0x20` (the NE header,
    which is DGROUP) while the code segment stays where the loader put it.
* Real MS-DOS 5.00 loads it at `load = 0x0FB2`, code at `CS = 0x0FEB`
  (linear `0x0FEB0`), and that *is* `load + 0x39` - the same relation our emulator
  produces. The earlier "real DOS puts the code at +0x210" note was an arithmetic
  slip from assuming the child PSP was 0x0FBA.

### The heap-init call and the exact sizing formula

`CS:658B` (file `0x7B1B`) prepares the kernel's private heap:

```
658b  mov %cs:0x10,%es        ; ES = PSP
6590  mov %es,%ax  / add $0x10,%ax    ; AX = PSP+0x10 = load segment
6595  mov %cs:0x6498,%bx              ; BX = instance
659a  mov %es:0x2,%cx                 ; CX = [PSP+2] = top of memory
659f  mov %bx,%es / mov %es:0x10,%dx  ; DX = [instance+0x10]
65a6..65a9  push dx, bx, ax, cx ; push cs ; call 0x73ab
```

`CS:73AB` reads the four arguments back (`[bp+0xA]` = instance, `[bp+8]` = load,
`[bp+0xC]` = `[instance+0x10]`, `[bp+6]` = top of memory), calls `CS:72D1` to build
the kernel's MCB chain, and uses the value `CS:72D1` returns in `CX`:

```
73ef  sub $0x3,%cx ; and $0xfc,%cl ; sub %bx,%cx ; shr $1,%cx ; shr $1,%cx
      mov %cx,(%bx)          ; free units, 4 bytes each
```

So the free space of the kernel's local heap is `((CX-3) & ~3) - 0x20` bytes, and
`CS:72D1` returns `CX = [instance+0x10] - 0x30`.

Because the bootstrap left `SS = load_seg`, `SP = 0x0200`, the instance the kernel
computes at `CS:6506` (`mov %ss,%ax / add %bx,%ax`) is `load + 0x20` paragraphs =
exactly the NE header, so `[instance+0x10]` is `ne_heap` and the first heap is
`0x200 - 0x30 = 0x1D0` -> **428 usable bytes**. The module database wants
`DI = 0x19D + lenbyte + 1` = **435 bytes** (`lenbyte = 21` for the 13-character
`C:\WIN100.OVL` it builds), so the allocation fails by 7 bytes.

### The decisive experiment: a bigger heap does NOT help

`F:\1231\_tmp\win\mk_heap_img.py <hex> <img>` copies the tree, patches
`ne_heap` at **file 0x410** and rebuilds the image. With `ne_heap = 0x1000` the
kernel really does use it - `UEFIDOS_TRACE_WINDOW=73a8-7420` shows

```
0D49:73AB AX=0D10 BX=0D30 CX=9FC0 DX=1000 ...      ; load, instance, top, ne_heap
0D49:73BB AX=0D30 BX=0D10 CX=1000 DX=1000 ...      ; [bp+0xC] = heap arg
0D49:73C1 AX=0D2E BX=0D10 CX=0FD0 DX=9FBE ...      ; 0x72D1 returned CX = 0x1000-0x30
0D49:73EF AX=0D2E BX=0020 CX=0FD0 DX=9FBE ...      ; free = ((0xFD0-3)&~3)-0x20 = 4012 bytes
```

4012 bytes is nine times what the module database asks for, yet the program still
ends at `CS:675C -> exit(1)` with the same DOS call count. **The heap size is not
the blocker.** The rejection happens inside the allocator's split path
(`CS:5A82` -> `CS:5AA0`), which only accepts a block whose chain is structurally
what the kernel itself built:

```
5aa0  mov %es:0x3(%di),%ax   ; block size
5aa4  inc %ax
5aa5  cmp %di,0x1e(%di)      ; 0 = last block -> size is final
5aa8  je 0x5ae2
5aaa  testb $0x8,0xb(%di)    ; growable?
5aae  jne 0x5ae2
5ab0  mov %es:0x8(%di),%es   ; next block
5ab5  cmpb $0x5a,%es:(%di)   ; next block is 'Z'?
...
5ae2  cmp %dx,%ax ; ret      ; caller: jae success
```

So the remaining work is to make the kernel's own arena/block chain consistent -
the MCB chain our DOS hands back and the one the kernel rebuilds at `CS:72D1` have
to agree about which region is the kernel's image and which is free.

### Reference: real MS-DOS 5.00, measured with the monitor only

`F:\1231\_tmp\win\ref_snap.py <tag>` boots the DOS 5 floppy, answers the date
and time prompts, types `c:` and `win`, and then takes `info registers` plus
`pmemsave 0 0xa0000` snapshots (`build/refm_<tag>_<i>.bin`) one, two, four and
eight seconds in. Do not use the QEMU gdbstub for this: attaching stops the vCPU
and a `c` packet without a pending reply desynchronises the script.

What the reference shows:

* `CS = 0x0FEB` from one second on -> code at linear `0x0FEB0`, the code needle
  (`WIN100.BIN` file `0x590 + 0x212B`) found at linear `0x11FDB` confirms it.
* MCB chain around the child: `0FCA M owner=FFFF size=0001` (the kernel's own
  bootstrap MCB, exactly like ours), `0FCC M owner=0FCD size=001D`,
  `0FEA M owner=0FCD size=064B`, and the child's `Z` block ending at `0x9FC0`.
* `cs:6498` (the instance cell) reads `0x749F` and `[instance+0x10]` reads
  `0xF88B` - but both are *stale* by then (the kernel keeps running and reuses
  those cells), so post-hoc cell reads cannot be compared with our pre-crash
  state. That is why the numbers above come from the call site, not from memory.

## The module-database request, measured to the byte

Two numbers decide whether Windows 1.01 gets past its own bootstrap, and both are now
measured rather than guessed:

| quantity | value | where it comes from |
|---|---|---|
| module-database request | **0x1B3 = 435 bytes = 28 paragraphs** | `CS:211D..2123` sums `ne_rev(0x52) + ne_enttab(0x130) + 2*ne_cseg + 8 + 16 + lenbyte(0x15)` |
| arena block the kernel gives itself | **0x1B = 27 paragraphs = 464 bytes, 428 usable** | block at `load+0x2`, owner word 0, built by `CS:72D1` |

`lenbyte` (0x15 = 21) is `strlen("C:\\WIN100.OVL") + 8`; the path is assembled at
`CS:6560..6589` from the PSP command tail, `AH=19h` and the 11-byte literal at `CS:6751`,
so the name is *not* derived from the program's own file name: renaming `WIN100.BIN` to
`W.EXE` changes nothing (verified: same 77 DOS calls, same `AH=4C AL=1`).

The block size is `(instance-1)&~1 - (load+2) - 1` with `instance = SS + (SP>>4)`.
`CS:6500..650A` computes it from the live stack pointer, and the MZ stub forces
`SS = load, SP = 0200h`, so `instance = load+0x20` and the block is 27 paragraphs -
one short of the request's 28. `GlobalAlloc` therefore returns 0, `CS:216B` turns that
into 1, the caller only rejects 0 (`CS:6649 or ax,ax / je`), `mov es,ax` puts **ES = 0001h
(the IVT)** into play, and the module walk at `CS:0E71` then fails, ending in `exit(1)`.

### Diagnostic: shrink the request by 4 bytes

Patching one immediate - `CS:211D add $0x10,%di` -> `add $0x0C,%di` (file offset
`0x590+0x211F = 0x26AF` in `WIN100.BIN`) makes the request 431 bytes = 27 paragraphs,
which fits the 27-paragraph block. Result, from `UEFIDOS_TRACE_WINDOW=211d-2170` and
`6640-6700`:

```
0D49:2123 AX=0016 BX=0D30 CX=0000 DX=5D6C SI=0000 DI=01AF ...  ; request 431 bytes
0D49:212E AX=0D13 BX=0008 CX=0D13 DX=0D13 ...                  ; allocator SUCCEEDS
0D49:664F AX=0D13 ... ES=0D13                                  ; ES = the new DB segment
0D49:667D AX=0000 ...                                          ; but the module walk still returns 0
```

So the size wall is real and it is exactly two bytes of headroom (`434` needed vs `432`
available when the path keeps its drive prefix). It is *not* the only wall: with the
allocation satisfied the kernel still gets `0` back from `CS:0E71` (the module walk,
`mov es,[bp+0Ah] / mov si,[bp+8] / dec si / cmp %si,%es:0x1C / jbe`) - the freshly
created database at `0D13h` reports `[es:0x1C] = 0` modules - and exits with code 1 the
same way. Both blockers are therefore open; the second one has never been reached before
in our runs.

### Where the reference run differs (still unresolved)

On real MS-DOS 5.00 the equivalent block in the MCB chain is
`0FCC M owner=0FCD size=001D` - **29 paragraphs**, i.e. two more than ours, which is
exactly enough for a 28-paragraph request. A 29-paragraph block requires
`instance = load+0x21` or `load+0x22`, i.e. `SP` between `0210h` and `022Fh` at
`CS:6506`; our emulator (and, on paper, the stub) produce `SP = 0200h`. Reproducing the
real value means finding what puts 16..47 bytes more on the kernel's stack before that
instruction. Everything needed is in place to keep digging: `UEFIDOS_DUMP_AT=CS:IP`
writes the low 640 KB at an exact instruction, and the QEMU reference runs can be
dumped with `pmemsave 0 655360`.

### The instantiate path, and why "instance = load+0x21" is dead

Disassembly of `CS:6604..6684` (the block that reaches the module database) and a
window trace of `CS:2085` settle what the kernel is actually doing:

```
6609: mov %sp,%di                 ; 0x80-byte scratch buffer on the kernel stack
660b: mov $0x4000,%bx
6616: call 0x1A2C                 ; open the overlay (C:WIN100.OVL, handle 5)
661b: jne 0x6635                  ; open succeeded -> skip the retry path
663a: push cs:6498 / push 0xFFFF / push %ss / push %di
6642: call 0x2085                 ; instantiate the module -> AX = database segment
6645: add $0x80,%sp
664d: mov %ax,%es
667a: call 0x0E71                 ; module walk
667f: jne 0x6684                  ; *** only a NON-ZERO walk reaches the rest ***
6681: jmp 0x675C                  ; exit(1)
```

So `CS:6684` - and therefore the block at `CS:66F8..6705` that stores `BP` into
`[instance+0x10]` and `[instance+0x0E]`, and the `CS:670F` gate behind it - is reachable
**only after the first module instantiate and the module walk both succeed**. The heap
field is patched *after* the very allocation that needs it, so the first instantiate has
to fit the initial 428-byte heap.

`CS:2085` was traced from its entry: `CS:2097 lds si,[bp+4]` loads `DS:SI` from the
caller's `(ss, di)` pair, `CS:209A mov al,[si]` reads the length byte `0x15` there, and
`CS:20A0..20A3` compare `[bp+0Ah]` with `[bp+8]`; when they match (they do: both
`0x0D30`) `DS` stays at the **instance segment**, `SI` is zeroed and the code jumps to
the size computation at `CS:20F9`. The "module header" it sizes is therefore
`instance:0000` - the DGROUP/node base, which is the NE header inside the loaded image.
No `AH=3Fh` read happens anywhere in the run (the trace has `AH=3Dh`, `AH=57h`, `AH=3Eh`
and nothing else on that handle).

The overlay-name literal at `CS:6751` is `WIN100.BIN\0` (10 characters + NUL); the
assembler at `CS:6560..6589` prepends the drive (`AH=19h`) and a colon, so the path is
`C:WIN100.BIN` and the length byte becomes 13, with `lenbyte = 22` (13 + 9) feeding
`DI = 0x19D + 22 = 435`.

#### Experiment: force the next instance value

`instance = SS + (SP>>4)` with the stack the MZ stub sets, so an odd/extra value can be
forced by patching the stub itself: image `0x18` holds `8B E6 FB` (`mov %si,%sp; sti`);
replacing it with `8D 64 10` (`lea 0x10(%si),%sp`) makes `SP = 0x210`, hence
`instance = load+0x21`, hence a 29-paragraph block (29 >= 28) **and** a heap of
`0x05D8 = 1496` bytes - exactly the combination that "ought" to work.

It does not, and the reason is decisive: **`instance` is the kernel's DGROUP base.** Shifting
it by one paragraph moves every kernel variable by 16 bytes, so `DS` then points 16 bytes
into the NE header. The size computation at `CS:20F9` reads `[DS+4]`, `[DS+6]`, `[DS+0Ch]`…
from shifted bytes and asks for `DI = 0x66DB` = **26,331 bytes (1645 paragraphs)**, which
fails far harder than the original. The hypothesis "real DOS has `instance = load+0x21`"
is therefore refuted: with `instance = load+0x20` fixed, the arena block at `load+0x2` is
always 27 paragraphs, and the request is always 435 bytes.

Two smaller experiments close the neighbouring questions:

* an **odd child load segment** (`UEFIDOS_PSP_SEG=0x0D01`) changes nothing - the block size
  is invariant under the load segment's parity, because both `(load+1)&~1` and
  `(instance-1)&~1` shift together (`block = 0x1B` for even and odd loads alike);
* the kernel never asks DOS for memory (`AH=48h/49h/4Ah` do not appear after the `EXEC`),
  so a larger DOS block would not help either.

Where that leaves the bring-up: the first `GlobalAlloc` wants 435 bytes, the only block the
kernel's own allocator will consider is 27 paragraphs (432 bytes, ~428 usable), and every
lever outside `instance` (heap field, top of memory, load segment, stack size, DOS version)
has now been tested and rejected. The next question is internal to the kernel's allocator:
`CS:5A82` refuses to split/absorb the neighbouring FFFF-owned block because
`testb $0x8,0x5(%es:di)` is clear on it, and on a real MS-DOS 5 the same region ends up
owned by the module segment (`0FCC M owner=0FCD size=001D`). Reproducing *that* ownership
transfer - not the block size - is what remains.

### The allocator's predicate, and a second wall behind it

Tracing `CS:5A84..5AE4` with full registers during the failing request gives the exact
rule the kernel's own allocator applies, and it is not a size test first:

```
5a84: cmp %di,%es:0x1(%di)      ; is the block's owner word equal to DI ?
5a88: jne 0x5A96                ;   no  -> skip to the next block (mov %es:(%bx),%es)
5a8a: call 0x5AA0               ;   yes -> try to satisfy (size compare at 0x5AE2)
5ae2: cmp %dx,%ax / ret         ; AX = size+1, DX = request, caller uses jae
```

with `DI = 0000h` throughout, so a block is only even considered when its owner word is
**0**. The observed walk on our arena:

| block | owner | size (paras) | flags | verdict |
|---|---|---|---|---|
| `0D12` (instance block, `load+0x2`) | `0000` | `001B` = 27 | `D1` | considered, too small |
| `0D2E` (free tail) | `FFFF` | `9271` | `00` | skipped (owner) |
| `9FA0` (**the kernel's own heap**) | `FFFF` | `001D` = 29 | `00` | skipped (owner) |
| `9FBE` (`Z`) | `FFFF` | `0001` | `00` | skipped |

The request is converted as `DX = (bytes >> 4) + 2`: 435 bytes -> 29 paragraphs, and the
comparison at `0x5AE2` needs `size+1 >= 29`, i.e. a **29-paragraph** block. The instance
block offers 27, the heap block offers exactly the 29 that is needed - but its owner is
`FFFF`, so the walk never looks at it. That is the whole wall: one word.

Two-byte experiment that proves it: in a scratch image set `ne_heap` = `0x0240`
(file `0x410`) **and** change the heap block's owner immediate at `CS:7343`
(`movw $0xFFFF,0x1(%di)`) to `0x0000`. The allocation then succeeds:

```
0D49:2123 DI=01B3 ...                      ; request 435 bytes
0D49:212E AX=9F9D BX=0008 ES=9FBA ...      ; allocator returns the heap block
0D49:6645 AX=9F9D ES=9F9D DS=9F9C ...      ; instantiate returns a real segment
```

…and the run **still** exits with code 1, because the next check fails: `CS:667A call
0E71h` - the module walk - returns `AX=0000` after ~23,000 steps even with the database
segment present (`ES=9F9D`), and `CS:667F jne 0x6684` is not taken, so control falls into
`CS:6681 jmp 675Ch` (exit). So the allocation was never the only wall; the walk is a
second, independent one, and this is the first time our runs have been able to reach it.

#### What the real MS-DOS 5 arena shows

The reference MCB walk (`build/rd4.bin`) puts the same two blocks in a different state:

```
0FCC  M  owner=0FCD  size=001D     ; the module database, 29 paragraphs, owned by 0FCD
0FEA  M  owner=0FCD  size=064B     ; 1611 paragraphs (25 KB) owned by the same segment
```

`0FCD = load+3` for `load = 0FCAh`, i.e. the database landed exactly in the `load+0x2`
block and got the 29 paragraphs the request needs; the block behind it is a 25 KB heap,
not the 464-byte one our run builds. A 25 KB heap can only come from `[instance+0x10]`
having been written with the *available memory* before the heap was built - which is what
`CS:66F8..6705` does (`mov %bp,%es:0x10(%bx)`). So on real DOS that patch runs **before**
the heap init at `CS:658B`, while our run reaches `CS:658B` first and builds a heap from
the raw NE field `0x0200`.

That ordering, plus the owner word, is the remaining difference. Next: make the walk at
`CS:0E71` succeed (it is now reachable) and find which DOS-observable state it depends on.

#### The walk, once the database finally exists

With the module database allocated (scratch patch above) the walk gets past its first gate:
`0E8A cmp %si,%es:0x1C` compares the requested module index with the database's count field,
and the database is now properly filled - `[DB+0x1C] = 1`, `[DB+0x22] = 0x40`, and the first
0x40 bytes of the block are a copy of the module's NE header. The walk then:

```
0E91..0E9B  si = [DB+0x22] + (index-1)*10 = 0x40        ; the module record
0EA0        testw $0x400,%es:0x4(%si)                   ; [DB:0x44] = 0x0040 -> clear
0EA6        je 0x0EC8
0EC8        bx = %es:0x4(%si)  = 0x0040                 ; flags word
0ECC        test $0x2,%bl -> clear
0ED1        push bx / push es / push si / call 0x0B1F
0ED7        or ax,ax -> AX = 0                          ; module lookup returns 0
```

so it returns 0 and the bootstrap still exits with code 1. That is the third wall, and it is
the first one that the real DOS run and this one would also share if the arena matched - it
means the module lookup at `CS:0B1F` does not accept the record our instantiate produced
(flags `0x0040`, no segment loaded yet). The next round starts there.

#### The next wall: the first module segment needs 32 KB

With the database allocated, the walk calls `CS:0B1F` with the record at `DB:0x40`
(`flags = 0x0040`, and `+0x46` bumped to `0x0840` by the caller's `addw $0x800,es:0x6(si)`).
That routine is the module-segment loader:

```
b26  bx = %es:0x4(%si)        ; record flags
b2a  ax = %es:0x6(%si)        ; record size field (0x0840)
b2e  cmp %si,%es:0x8          ; is this the "self" record?
b5c  test $0x2,%bl / jne ...  ; already loaded?
b63..b67  push es / push bx / push ax / push cx=0 / call 0x98A   ; GlobalAlloc
b6a  or ax,ax / je  -> return 0
```

With a 36 KB heap it still returns 0. The request it passes to the allocator is

```
0D49:0B67 AX=7EE0 BX=0040 CX=0000 ... call 0x98A     ; 32,480 bytes
0D49:0B6A AX=0000 ...                                ; failed
```

32,480 bytes is more than the whole `ne_heap` window the arena gives the kernel even when
the NE field is raised to `0x9000`, so the module segment is meant to come out of the big
free pool - the one our arena marks `owner=FFFF`. The reference dump shows exactly that
shape: the loaded module segments sit in blocks owned by module segments
(`1636 M owner=9100`, `1732 M owner=2A75`), i.e. on real DOS the allocator did see the pool
as available. So the owner word on the pool blocks, not their size, is the whole
remaining difference - the same word that stops the module database one screen earlier.

Two more facts measured here and worth keeping:

* The kernel never calls `INT 15h`: adding a real `INT 15h` (AH=88h -> 0 KB, AH=86h wait,
  AH=84h no joystick, everything else `AH=86h`/CF=1, hooked like the other BIOS vectors)
  changes the INT trace not at all - 77 calls before and after. It is still the right thing
  to have (extenders and games probe it), it just is not what stops Windows.
* `CS:0x0B1F` and `CS:0x0E71` are the pair to watch: the walk decides *which* record, the
  loader is what actually asks for memory, and both end in the private allocator at
  `CS:0x98A -> CS:0x54FA -> CS:0x594F`.

### Windows 2.03 (1987/88): Setup runs, the copy stage stops on WIN.CNF

The 2.03 media (nine 360 KB disks) flattens to 172 files and its `SETUP.EXE` is a much
more demanding DOS client than 1.01's, so it is a useful compatibility probe on its own.
Under UEFI-DOS it starts, paints and drives its whole question-and-answer flow:

```
SETUP PROGRAM for Microsoft Windows Version 2
 ...
           WHEN YOU'RE READY TO                            PRESS
           Continue Setup                                  ENTER
           Exit without completing Setup               CONTROL+X
 ...
           Install on 1 high capacity disk (1.2MB or more)   1
           Install on 2 5.25" disks (360KB)                  2
           Install on 2 3.5" disks (720KB)                   3
           Install on a hard disk                            H
 ...
 Windows will now be set up on your hard disk in the directory shown below.
   C:\WINDOWS
```

That is three screens further than any earlier Windows build we tried, so text-mode
console, keyboard, `SETUP.INF` parsing and the directory prompts all work. The copy stage
then stops immediately:

```
[open-fail] C:\WINDOWS\WIN.OLD  (abs WINDOWS\WIN.OLD)
[open-fail] C:\WINDOWS\WIN.COM  (abs WINDOWS\WIN.COM)
Error: Can't open WIN.CNF (FILES= may be too small or disk full).
```

The error text is SETUP's own guess at the cause; the trace shows the two `WIN.OLD` /
`WIN.COM` opens failing with "file not found" as expected (they are the "does an old
install exist" probes), and then no `INT 21h` open at all for `WIN.CNF` - SETUP reaches
for it through a path that our DOS does not complete, which is the next thing to chase.
Raising `FILES=` to 60 and pre-copying the media into `C:\WINDOWS` both leave the message
unchanged, so it is neither the handle table nor a relative-path problem.

Reproducing:

```
python tools\fat_extract.py extract "D1_Setup.img"  w203     # ... D2..D9 as well
python tools\make_tree_image.py build\w203.img  w203 --size-mb 64
build\hosttest.exe --shell build\w203.img 'SETUP'          # keys: ENTER, H, ENTER...
```

#### Arena state at the 32 KB allocation, and where the walk actually goes

With the heap term raised and the heap block's owner byte cleared (`exp_own2.py`), the module
database lands and the arena at the moment `CS:0B1F` asks for 32,480 bytes looks like this
(`UEFIDOS_DUMP_AT=0D49:0B63`, walking the descriptor chain from `load`):

```
seg 0D10 M own=FFFF size=0001 fl=00 self=0D10 next=0D12
seg 0D12 M own=0000 size=001B fl=D1 self=0D10 next=0D2E    ; the instance/header block, free
seg 0D2E M own=FFFF size=8991 fl=00 self=0D12 next=96C0    ; the pool (system-owned, correct)
seg 96C0 M own=96C1 size=001D fl=00 self=0D2E next=96DE    ; heap head, now owned by the DB segment
seg 96DE M own=0000 size=08DF fl=FF self=96C0 next=9FBE    ; 2271 paragraphs = 36 KB, FREE
seg 9FBE Z own=FFFF size=0001 fl=00 self=96DE next=9FBE
```

So space is not the problem: a 2271-paragraph block with `owner = 0` exists and the request
needs 2032 paragraphs. Tracing the walk for exactly that request (`CS:5A84..5AE4`,
`DX=07F0` = 2032) shows what really happens:

```
... 0D49:5AA0 ... ES=FFE0  DX=07F0     ; the walk is following a "next" link into segment FFE0h
... 0D49:5AA0 ... ES=F000  DX=07F0     ; and then into F000h - outside the arena entirely
```

i.e. for requests that need the *big* block the allocator follows the heap's internal
free-list (the node pointer at `[heap+0x0A]`, and the merge arithmetic at `0x5AC2..0x5AD9`
subtracts the word at `[block+0x1E]`), and those links in our arena point at `FFE0h`/`F000h`
instead of at the free block. The free-list is installed by `CS:73AB` from the heap size we
compute, so a mismatch there sends the search off the end of the arena while the *small*
allocations (the database, 435 bytes) still succeed through the block chain.

That is a much sharper statement of the remaining problem than "the owner word":

* small allocations: block chain + `owner == 0` test -> works once the heap block is free-owned;
* large allocations: the heap free list -> our list head points outside the arena;
* the kernel marks its pool blocks `FFFF` on purpose (freeing them makes it allocate over its
  own image and crash into zeroed memory at `0DC5:1E80`, `SS=0D10 SP=000A`), so the only
  legal way forward is to make `CS:73AB`'s heap bookkeeping match what the kernel expects.

## The kernel's private arena, decoded instruction by instruction

This section records what `CS:670F` and `CS:72D1` actually do, because the
module-database allocation is rejected by *their* data structures, not by DOS.

### The gate at CS:670F

```
6709  push %ax / mov $0x670f,%ax / push %ax / lret   ; the jump trick (true lret)
670f  push %ss / call 0x3398
6713  mov %ss,%cs:0x18 / mov %ss,%cs:0x16            ; remember the stack segment
671d..6734  install INT 3Fh (handler 0x1369, old vector kept in cs:0x86/0x88)
6738  mov %cs:0x6498,%es                             ; ES = instance (set at 0x6506/0x650a)
673d  cmpw $0x0,%es:0x0a                             ; [instance+0x0A] is NE+0x0A = ne_crc
6743  jne 0x674c                                     ; non-zero -> mov ax,4C01h / int 21h
6745  mov $0x64b4,%ax / push %ax / jmp 0x69e8        ; the real startup continues
```

So the kernel refuses to start when the 32-bit checksum field of the in-memory NE
header is non-zero, and it decides that from `[instance+0x0A]` - i.e. it really does
believe `instance` points at the module header. This is the same test we can see
failing at `CS:675C` in our runs only if the values below are wrong.

### CS:72D1 rebuilds the arena from three numbers

Inputs (pushed by `CS:658B`): `AX = load segment`, `BX = instance`,
`CX = [instance+0x10]` (= `ne_heap`), `DX = [PSP+2]` (= top of memory).
With our measured values `load = 0x0D10`, `instance = 0x0D30`, `ne_heap = 0x0200`,
`top = 0x9FC0` the routine computes

```
72d1  si = (load + 1) & ~1              = 0x0D10     ; bootstrap block
72d8  di = (si + 2) & ~1                = 0x0D12     ; header/heap block
72df  cx = (ne_heap + 0x0F) >> 4        = 0x0020     ; heap paragraphs
72ea  cx = -(cx - top) & 0xFFFE         = 0x9FA0     ; heap segment = top - heap_paras
72f1  dx = (top - 1) & 0xFFFE           = 0x9FBE     ; last block
72f5  ax = (instance - 1) & 0xFFFE      = 0x0D2E     ; end of the header block
```

and then writes five blocks (`[0]` type, `[1]` owner, `[3]` size, `[6]`/`[8]` links):

| segment | type | owner | size | [+6] | [+8] next |
| --- | --- | --- | --- | --- | --- |
| 0x0D10 | `M` | FFFF | 0001 | 0x0D10 | 0x0D12 |
| 0x0D12 | `M` | **0000** | **001B** | 0x0D10 | 0x0D2E |
| 0x0D2E | `M` | FFFF | `(heap_seg - 0x0D2E - 1)` = 0x9271 | 0x0D2E | 0x9FA0 |
| 0x9FA0 | `M` | FFFF | `(0x9FBE - 0x9FA0 - 1)` = 0x1D | 0x9FA0 | 0x9FBE |
| 0x9FBE | `Z` | FFFF | 0001 | - | itself |

The `0x0D12` block is the one the kernel later uses as its **local heap**
(`DS = 0x9FA0` in the trace is the *top* block; the heap header ends up in the
`0x0D12` block when `ne_heap` is small, and in the top block when it is large).

Note the owner field of the `0x0D12` block is written by `7384 mov %bx,0x1(%bx)`
with `BX = 0`, so it is **0**. In the real MS-DOS reference dump the equivalent
block carries a *self segment* (`0FCC M owner=0FCD size=001D`), which means the
kernel (or a later pass of its allocator) rewrites that field with the block's own
data segment. That difference is the most likely reason the allocator walk below
never accepts a block under UEFI-DOS.

### Why the allocation is rejected (trace, not guesswork)

`UEFIDOS_TRACE_WINDOW=5300-5c00` on the flat installation with a 640 KB-visible
arena shows the module-database request and the whole search:

```
#1757 0D49:54FA BX=001C            ; 28 paragraphs = the 444-byte module database
#1759 0D49:551F                    ; or bx,bx -> search
#1760 0D49:594F                    ; walk: es=[di+6], then repeat es=[es+8]
#1779 0D49:5AA0 ES=0D12            ; grow attempt on the header block
#1810 0D49:5A82 ES=0D2E AX=DX=9272 ; split attempt on the 599 KB free block
#1857 0D49:5A82 ES=9FA0 AX=DX=001E ; split attempt on the top block (29 paragraphs!)
#1999 0D49:5AA0 ES=0D12            ; grow again
#2424 0D49:54FA BX=0000            ; second pass: BX=0 -> xchg ax,cx / test 2,cl -> fail
#2429 0D49:5519                    ; xor dx,dx / xor ax,ax  ==  allocation failed
```

The split helper only accepts a block whose header satisfies

```
5a84  cmp %di,%es:0x1(%di)     ; the block's owner word must equal DI (the heap base)
5a88  jne 0x5a96               ; otherwise: es = [es+8] and keep walking
5a8a  call 0x5aa0              ; owner matched -> size test / grow
5a8f  jae 0x5a9d               ; success only if size+1 >= request
```

With every block owned by `FFFF` or `0000` and `[es+8]` pointing at the *next
block segment*, the walk burns its `CX` counter and returns the failure path. The
29-paragraph top block would have been big enough (`0x1E >= 0x1C`), so the
rejection really is the owner/link convention, not the size.

### What is left to change

Make the arena the kernel builds agree with what its allocator walks: the block at
`load+2` has to carry its own data segment in the owner field (and the heap has to
be big enough for the 435-byte module database). Our DOS cannot write that field -
the kernel does - so the remaining question is which value the kernel *expects* and
why it computes `owner = 0` here while the reference run ends up with
`owner = block+1`. The next probe is `UEFIDOS_WATCH=0D130-0D16F` to catch every
write to that header between `CS:72D1` and the first `GlobalAlloc`.
