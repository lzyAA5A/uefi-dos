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
| PSP top-of-memory (`PSP+2`) | `9FC0h` (DOS 5 keeps 64 paragraphs) | `A000h` |
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
