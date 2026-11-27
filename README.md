# UEFI-DOS

**English** | [简体中文](README.zh-CN.md)

**DOS, rewritten for UEFI.** A small DOS-compatible operating environment that
boots as a UEFI application on x86_64 and IA32 firmware, mounts FAT32 itself,
runs a `COMMAND.COM`-like shell, and executes 16-bit real-mode DOS programs
through an internal 8086 interpreter — a "V86-like" software compatibility layer
with no BIOS and no virtualization support required.

It boots as one `.EFI` file, and what a DOS program sees is **MS-DOS 4.0**
(`INT 21h AH=30h` returns `AX=0x0004`), on top of which it adds the things DOS
never had: FAT32, 64-bit UEFI boot, FAT16 large volumes, a native 32/64-bit DOS
API, and 386 protected mode with a DPMI 0.9 host.

1.10.1 is a one-line bug in a structure definition, and it explains three
reports in a row.  On the laptop the pointer stopped flying around (1.10.0) but
could then only move **along one line**, and **clicking did nothing**:
`EFI_SIMPLE_POINTER_STATE` was declared with `INT64` fields while every firmware
fills EDK2's 32-bit definition, so our `X` read `X | Y<<32` (the original
flailing), our `Y` read EDK2's Z field, which is always 0 (one-line travel) and
our button flags sat two fields past the 14 bytes the firmware had written
(clicks lost).  The fields are now `INT32` + two flags, and two compile-time
asserts fail the build if the layout ever drifts from 16/32 bytes again.  The
winner-take-one rule from 1.10.0 now applies **per axis**: a device that reports
only X can no longer freeze Y.  `MOUSE` grew the diagnostics that would have
found this in one run -- a `raw=` column (what the firmware itself reported,
before our halving and clamping), the device table printed both before and after
the ten-second probe, a `ps/2 aux:` line, and a verdict that says "counts arrive
on one axis only" out loud.

1.10.0 is the benchmarking round, and it starts with the pointer.  On the real
laptop the mouse was recognised but **flew across the screen** the moment it
moved: 1.9.3 polled every pointer handle the firmware published and added their
movements together, so a firmware that publishes the same physical mouse -- or a
touchpad handle repeating a stale count -- as two or three handles handed the
guest two or three times the motion the hand made.  The rule is now
**winner-take-one**: every device is asked, only the one that moved most in this
poll moves the pointer, buttons still OR together; a handle that repeats the
same counts for 128 polls is treated as stuck and ignored, one wild sample is
clamped to 96 counts, an absolute `Mode` range that is empty or absurd is not
believed, and `MOUSE` now prints a per-device table (`moves=`, `last=`) that
says which handle is moving.  The pointer is also polled on wall-clock time
(once every 2 ms) instead of every 1024 instructions, because on a fast host
that was 40 000 firmware calls a second and a third of the machine.

The benchmark is `BENCH`.  **AUTOBANCH 2.0** (`tools/autobanch.asm`, 16-bit,
8086-only) runs sixteen one-second windows -- the original ALU/MUL/DIV/memory/
INT 21h/disk set plus an **x87 chain, FSQRT, a 640x200x256 frame written into
A000 by hand, a CGA text fill, a sieve of Eratosthenes, an insertion sort, CRC16
and an 8x8 matrix multiply** -- and **writes its own report file**,
`B<yymmdd>.TXT` (`B261128.TXT` for 28 Nov 2026), created from the DOS date and
appended to, so the native probes land in the same file.  `TOOLS\BENCH64.EXE`
(`sdk/examples/bench.c`, started with `RUN64`) does the same natively in 64 bits
with RDTSC timing: ALU, MUL, DIV, copy, fill, a double FPU chain, Newton sqrt,
sieve to 65536, insertion sort 1024, CRC32 and a 32x32 matrix multiply.
`TOOLS\BENCH32.EXE` is the 32-bit twin -- a 64-bit kernel runs only 64-bit
images, so it needs `BOOTIA32.EFI`.  `TYPE B*.TXT` prints the result (TYPE now
accepts wildcards, because a batch file cannot know today's date).

Version **1.10.1**.  Verified here by: the 8086 + 186/286 + **8087** instruction
suite (96 of 96), the host harness shell, FAT16/32 and dual-case checks, a 1.5 GB
single-file write/read on a 1.9 GB FAT16 volume, split-file joining, the QEMU +
OVMF boot smoke tests for x86_64 and IA32, the release image booting to a live
`C:\>` prompt, Windows 1.01 and 1.04 reaching their desktops **with the
coprocessor announced**, and Windows 1.01 booting from a **USB stick** under
QEMU + OVMF with a working pointer ([`docs/windows1x.md`](docs/windows1x.md),
Rounds 38 and 39).  Two checks still skip unless real MS-DOS 2.11 / MS-DOS 5
floppy images are supplied (`UEFIDOS_DOS211`, `UEFIDOS_DOS5`).

1.9.3 is the round that makes a real machine **explain its own pointer**.
The first laptop this stick booted on (Secure Boot off, Windows 1.01 up and
running) had no working pointer with either its touchpad or a wired USB mouse,
and from inside the application both cases look the same: no protocol.  So the
firmware layer now (a) runs driver binding over every handle once, via
`ConnectController(..., TRUE)` -- what `connect -r` does in the UEFI shell --
when the pointer enumeration comes back empty, which is what makes a mouse
plugged in after POST appear on a Fast-Boot firmware; (b) **polls every**
pointer handle the firmware published instead of only the first, because a
laptop usually exposes its touchpad as a handle that never reports a state
right next to the USB mouse that does, and holding only the first one is a
pointer that does not move while a perfectly good device sits one handle
further along; (c) reports **button releases**, not just presses, so a guest
cannot leave a button stuck; and (d) adds a built-in `MOUSE` command that
enumerates the protocols and then prints one line per second of what the
firmware really reports, with the emulated machine out of the way.  Ten
seconds of that output names the failing side.  Honest limit: an I2C touchpad
that the firmware never publishes is not reachable from a UEFI application,
and the PS/2 fallback cannot see it either.

1.9.1 makes the machine the ROM describes match the machine the emulator runs.
Three bytes told every old program it was on an **8088 XT** (`F000:FFFE` = `FEh`,
ROM date `11/08/82`, equipment-word bits 14-15 = `00b`) while the emulator was
executing 80286 protected mode, the 386 instruction set and DPMI.  They are now
generated from one place, `machine_kind()`: a **286 AT** by default (`FCh`,
`11/15/85`, bits 14-15 = `01b`, equipment word `4023h`), `UEFIDOS_MACHINE=xt`
or `=386` to pick another identity.  Windows 1.01 (desktop **and pointer**) and
1.04 still reach their desktops under the AT identity.

The same round pins down Windows 2.03's first fault exactly: at `0E68:8733` the
guest executes `mov es,ax` with `AX=0` and then `rep movsb` copies 64 bytes of
WIN200.BIN's NE header over the interrupt vector table at `0000:0000`
(`[lowmem-hit] IVT[0]=4E450501 after step 2831`).  Reproducible on every run,
independent of the timer period, and now visible in one line because `cs_trap`
dumps the IVT.

1.8.1 is the stick round.  The USB-stick tree now carries **all three**
Windows installations side by side (`W101`, `W104`, `W203`) plus `TOOLS\` and a
`README.TXT` that also answers "the firmware will not offer the stick" --
Secure Boot, CSM-only mode, the legacy boot entry, Fast Boot, a front-panel
USB3 port, and the firmware that insists on partition type `EFh`.  The batch
files on the stick are straight-line because the shell's batch runner has no
`IF` and skips `GOTO`.  For Windows **2.03** this round corrected its own
diagnosis: the stop is not "DOS returned a zero segment" but a module traversal
through `cs:[7CC4]`, a segment the kernel computes from `SS:SP` itself, and it
is **nondeterministic** (2824 / 3938 / 3950 steps, and it does not stop at all
when tracing is on), so the remaining lead is the wall-clock-paced timer, not an
instruction.  Details: [`docs/windows1x.md`](docs/windows1x.md), Round 40.

1.8.0 is the floating-point round: `D8h..DFh` used to be decoded and discarded
("an 8086 without a coprocessor"); the emulator now carries a complete software
8087/80287/80387 -- eight 80-bit registers kept in the hardware's own raw image,
arithmetic through the host `long double` (the same format), a real tag word,
the environment and save areas, BCD, the 387 extras (FSIN/FCOS/FPATAN/F2XM1/
FXTRACT/FCMOVcc/FUCOMI), and -- because the EFI build has no libm -- sqrt, log2,
2^x, atan and sin/cos written as series with explicit range reduction.  The FPU
is announced in the INT 11h equipment word, CPUID leaf 1 and CR0.ET, and
`UEFIDOS_NOFPU=1` hides it again.  Details: [`docs/itest.md`](docs/itest.md),
section "1.8.0 -- the 8087 (x87) sweep".
1.7.0 is the large-file and instruction-completeness round.  **The 32 MB "DOS 4.0
file limit" was a testing boundary, not a limit in the code**: a single file can
be written up to the FAT32 directory-entry ceiling of 4 GB - 1 through the
ordinary DOS calls, verified here with a 1.5 GB file on a 1.9 GB FAT16 volume.
For 16-bit programs that need to address past 2 GB, or streams bigger than one
file can hold, there is a second, documented surface: **`INT E0h`** ("UEFI-DOS
extended services") with 64-bit offsets, plus the split-file convention
(`NAME.001`, `NAME.002`, ...) that the same API joins into one stream -- a
single part is still capped at 4 GB - 1 by the filesystem, so N parts span
N x (4 GB - 1).  `SPLIT` and `JOIN` in the shell make and unmake those parts;
`SPLIT` defaults to 32 MB a part, the historical boundary, so every part stays
readable by any DOS.  Building it surfaced a real filesystem bug: `dos_unlink`
and `dos_rename` re-derived an 8.3 name from the caller's spelling, so deleting
a **long-named** file (exactly what `NAME.001` is) silently failed and leaked
its whole cluster chain -- eighty clusters per run on the test volume.  They now
delete by the 8.3 name the directory entry actually carries.  On the instruction
side, **the 186 and 286 sets are complete and correct**: `PUSH SP` now answers
"186 or later" to the classic CPU probe, shift counts are taken modulo 32 and
leave flags alone at zero, `ENTER`'s display chain is the one the SDM specifies,
`BOUND` actually raises INT 5, `SMSW`/`LMSW` are no longer swapped, `VERR`/
`VERW`/`LAR`/`LSL` consult the descriptor tables instead of answering stubs, and
`LSS`/`LFS`/`LGS`, `BSF`/`BSR`, `CMPXCHG`, `XADD`, `CMPXCHG8B`, `BSWAP` and the
`0F 18-1F` NOP hints no longer halt the machine.  `--itest` went from 68 to 90
checks; `--ifuzz` reports every remaining `0F` hole as an SSE/MMX or reserved
encoding ([`docs/itest.md`](docs/itest.md),
[`docs/largefiles.md`](docs/largefiles.md)).

1.6.0 is the real-hardware round.  **UEFI-DOS and Windows 1.01 fit on a plain
FAT32 USB stick** -- no install, no BIOS, no bootloader: the firmware runs
`EFI\BOOT\BOOTX64.EFI` and the rest of the stick becomes `C:`.  **`AUTOBANC.COM`
(autobanch) measures the machine it runs on**: eight workloads timed over exactly
one second each (ALU, `mul`, `div`, block copy/fill/read, `INT 21h` dispatch, disk
read), plus DOS version, video mode, memory and the emulated BIOS tick rate.  To
time anything at all a guest needs a clock, so this round also **emulates the
readable half of the 8253 timer**: channel 0's 1.193182 MHz counter latches on
`out 43h,0` and reads back on `in 40h` -- the BIOS tick alone has 55 ms of
resolution and a rate the wall-clock pacer keeps re-negotiating, which made the
first version of the benchmark swing by 20x between runs.  Three fixes fell out
of building it: the BIOS data area no longer reports **128 KB of conventional
memory** (a stray write zeroed the high byte of `0040:0013`, so a program reading
the BDA directly saw a quarter of the machine), `INT 11h`/`INT 12h` are actually
dispatched instead of halting the machine, and the Windows host harness uses
`QueryPerformanceCounter` instead of the 15.6 ms `GetTickCount64` -- whose
coarseness had the pacer running guest time about 30% fast.  The shell also runs
`NAME.BAT` from the prompt now, so a stick can offer `WIN1` and `BENCH` instead of
command lines.  Mouse, colour and resolution on real hardware: Round 38.

1.5.1 fixes the one thing Windows 1.04 still lacked: **its mouse works**.
1.04's `MOUSE.DRV` does not stop at checking the bus-mouse signature -- it then
reads the card's control register (port `23Eh`) 10,000 times and drops the card
when no two reads differ.  Our control port echoed the byte the driver had just
written, so it never changed and 1.04 loaded no IRQ handler: the desktop came up
without a pointer.  The port now answers like the real card (the register itself,
whose low four bits the card sets as it answers, bit 0 being the interrupt
latch), so reads run `0Eh, 0Fh, 0Fh, ...`, the probe passes, 1.04 hooks IRQ 5 and
pulls position packets out of the nibble registers -- **the pointer appears and
moves on the 1.04 desktop**, and a scripted double-click on the `CALENDAR.EXE`
entry starts the application (Calendar comes up with its menu bar and hour
list).  Windows 1.01 runs the same probe and is unchanged (it passes it too and
its pointer keeps working).  Frame dumps, port trace and the disassembly of the
probe are in `docs/windows1x.md`, Round 37.

1.5.0 was the Windows 1.04 / 2.03 round.  **Windows 1.04 now runs its desktop**,
full screen, with the complete file listing (QEMU + OVMF screenshot in
`docs/windows1x.md`, Round 36).  The last missing piece was not a DOS defect but
the DOS version: 1.04 refuses to start under a DOS that reports 4.00, and the
tree has to carry `SETVER WIN.COM 3.30` / `SETVER WIN100.BIN 3.30` /
`SETVER MSDOS.EXE 3.30` in `AUTOEXEC.BAT` -- exactly what `run_win1x.ps1` writes
for Windows 1.x.  **Device names are now DOS devices**: `CON`, `NUL`, `PRN`,
`AUX`, `COM1..4`, `LPT1..3` and `CLOCK$` are resolved before the directory and in
every directory, which is what Windows 2.03 needs while it initialises (it opens
`CON` five times and gives up when those opens fail).  **Windows 2.03 installs**:
with its `SETUP.INF` machine profile pointed at `cga.drv` instead of
`egahires.drv`, SETUP links a CGA kernel (`WIN200.BIN` 220,160 bytes,
`WIN200.OVL` 267,856 bytes), copies every file and writes `WIN.COM`.  Running it
still stops right after the logo: the kernel's DOS-structure scan returns failure
and it prints "Incorrect DOS version: 3.30" and exits (the 3.30 is what DOS
answered, so the probe is a memory scan, not the version query).  The trace and
the two remaining leads are in `docs/windows1x.md`, Round 36.

1.4.0 was the Windows 1.04 round.  **Windows 1.04 now reaches its desktop**: the
divide overflow that killed it after the title bar was DOS telling it that `CON`
is a block device (`INT 21h AH=44h AL=08h`), so its console setup produced a
zero-width window and the text renderer divided by zero.  With `AH=44h` answering
like DOS, 1.04 enumerates its directory, reads its files and paints the MS-DOS
Executive.  **Windows 2.03's SETUP** gets through its copy stage as well now: a
missing directory component reported "file not found" instead of DOS error 3, so
SETUP's attempt to create `WIN.COM` landed in the root of the drive and failed as
"disk full".  Details in [docs/windows1x.md](docs/windows1x.md) (Round 35).

1.3.0 fixes the last Windows 1.01 defect of the session: **closing an
application window used to end the whole Windows session** (the kernel printed
nothing, closed its DOS handles and you were back at `C:\WINDOWS>`).  The window
manager was fine -- DOS had told the kernel the whole process was terminating.
Three DOS answers had to become real implementations: `INT 21h AH=55h` (create a
child PSP) was a no-op, so every Windows task record was uninitialised memory;
`AH=50h` did not update the current PSP inside the swappable data area, which is
exactly where Windows 1.x reads it from; and `AH=4Ch` halted the machine instead
of continuing at the process's terminate address.  Closing the Calculator now
returns to the MS-DOS Executive with its full listing.  See
[docs/windows1x.md](docs/windows1x.md) (Round 34) for the traces, and the new
host knobs `UEFIDOS_TRACE_TERM`, `UEFIDOS_TRACE_PSP55` and `UEFIDOS_DUMP_HALT`.

1.2.3 adds a **benchmark** that runs the same eight workloads in the 16-bit
DOS VM, in a native 32-bit image and in a native 64-bit image, with identical
byte counts and checksums, so the cost of each execution world can be compared
directly: CPU-bound DOS code is 180x-1,200x slower than native, a DOS API call
costs about 2 us against 1 ns, while console output, file I/O and directory
scans are already at native speed because the platform, not the guest, is the
bottleneck. See [docs/benchmark.md](docs/benchmark.md) and `bench/`. It also
made the per-instruction step ring opt-in on the host as well (`RINGLOG=ON` in
`CONFIG.SYS`, or `UEFIDOS_RINGLOG=1`), and `INT 21h AH=2Ch` now reports real
hundredths instead of zero.

1.2.2 is a **performance** release for Windows 1.x, which felt stiff for
three separate reasons:

* the video path repainted the whole 640x200 screen into the GOP framebuffer
  on every frame, so one moved mouse pixel cost 4 MB of framebuffer writes.
  It now records the guest row and the column span of every VRAM write and
  repaints only those (`plat_video_gfx_show_dirty`); `UEFIDOS_GFX_VERIFY=1`
  compares every incremental frame against a full expansion to prove it;
* frames were paced on an instruction count (one flush per 256K steps), which
  is about one frame per 140 ms at the speed the guest actually runs. Pacing is
  now wall-clock (about 33 fps, skipped when nothing changed), so the pointer
  tracks the hand instead of trailing it;
* an interrupt whose vector still points at the emulator's own `IRET` stub is a
  no-op, so it is no longer executed instruction by instruction. Windows 1.x's
  idle loop calls `INT 28h` hundreds of thousands of times per second.

Measured in QEMU + OVMF on the same image: **1.88M → 2.29M guest
instructions/s** (+22%) and, while the mouse moves, **19 presented frames/s
instead of the 7.2 fps ceiling the old step-gated flush imposed**
(one frame per 262144 instructions at 1.88M instructions/s was one frame
every 140 ms). Real-mode address translation also lost a function call per memory
access, and the firmware event pump now runs every 16 ms instead of 8 ms.

1.2.1 fixed **`LAHF` and `SAHF` being swapped** in the 8086 core — `LAHF`
wrote AH *into* the flags instead of reading the flags into AH (and `SAHF` did
the opposite), so every program that tested a flag after a `LAHF` saw garbage.
Turbo Link 3.01 starts its command-line scanner with `call get_char; lahf; je`
and consequently rejected its own argument list; with the two instructions the
right way round **`TLINK HELLO.OBJ,HELLO.EXE,,..\LIB\C0S.OBJ+..\LIB\CS.LIB`
links a 6426-byte `HELLO.EXE` that runs** (`Hello from Turbo C++ 1.01 on
UEFI-DOS`, `Sum of squares 1..100 = 338350`, `malloc(4096) = ok`). Turbo C++
1.01 is therefore fully verified: compile, link, run.

1.2.0 is a compatibility release: three more real DOS packages were taken
from their original floppy images and driven under the emulator end to end —
**Forth-83 2.1.0** (block files, `F83 SAMPLE.BLK`, `2 3 + . 5  ok`),
the **Windows 1.01 Demo Slideshow** (the `PRESENT DEMO` player, CGA 320×200
slides with dithered windows) and **Borland Turbo C++ 1.01**, whose compiler
builds a real object file (`TCC -c HELLO.C` → `HELLO.OBJ 633`). Getting there
fixed three DOS contracts: the **PSP default FCBs** at `005Ch`/`006Ch` are now
parsed from the command tail (CP/M-era programs read their file name from
there — Forth-83 printed `Open error in .` without it), **extended keys**
(F-keys, arrows) are handed to a program the way MS-DOS 5.00 does it
(`AL=0` first, then the scan code in `AL`, measured against real DOS 5.00 with
a probe program), and `INT 21h AH=37h` (get/set switch character) answers
`AL=0`, `DL='/'` instead of falling through to a stale register. Turbo Link
still refuses its own command line, so linking is the one open item from that
package (see [`docs/compat.md`](docs/compat.md)).

1.1.1 fixes the EXEC command-tail contract DOS has and UEFI-DOS did not:
the whole 128-byte `PSP:0080h..00FFh` buffer is copied verbatim (that is
where `WIN.COM` hides its `'XW'` marker) and the count byte excludes the
terminating CR. With those two right, **Windows 1.01 draws its splash
screen** in CGA 640x200 before it stops.

1.1.0 is about the hardware a real-mode GUI needs. `INT 10h` now speaks the
**CGA modes 4/5/6** (the even/odd-interleaved bitmap at `B800`, palette
selection through `AH=0Bh` and the `3D8h`/`3D9h` ports), there is a full
**`INT 33h` mouse driver** backed by the firmware's `EFI_SIMPLE_POINTER_PROTOCOL` (with a direct i8042 PS/2 fallback, and a Microsoft bus-mouse card at ports 23Ch-23Fh for Windows 1.x, whose driver never touches INT 33h)
(plus a software pointer drawn into the frame), and the emulator finally
**injects hardware interrupts** — `INT 8` at 18.2 Hz and `INT 9` with a latched
scan code on port `60h` — because real-mode Windows installs its own keyboard and
timer handlers and never touches the BIOS buffer. `HLT` became an idle step, so
`sti; hlt` message loops work, and the BIOS tick counter at `0040:006C` runs.

One DOS-level feature made that possible: **`SETVER`**, a per-program DOS
version. UEFI-DOS reports MS-DOS 4.00, and Windows 1.x refuses to start on
anything newer than 3.x, so `SETVER WIN.COM 3.30` (shell or `CONFIG.SYS`) is the
escape hatch instead of lowering the version for everybody. With it, **the whole
Windows 1.01 release installs and the MS-DOS Executive desktop comes up** —
[`docs/windows1x-guide.md`](docs/windows1x-guide.md) is the **usage guide** for
working with it (mouse, double-click, menus, the bundled apps, quitting), and
[`docs/windows1x.md`](docs/windows1x.md) has the traces and what is still
missing, and [`docs/performance.md`](docs/performance.md) says what makes it
fast or slow (the `[perf]` line, the knobs, and the rules the video path
follows).

## Run it now (no compiler needed)

[**Download `UEFI-DOS-1.5.1.zip`**](https://github.com/lzyAA5A/uefi-dos/releases/latest/download/UEFI-DOS-1.5.1.zip)
(240 KB) — it contains a ready-to-boot 64 MB image plus a launcher:

1. install QEMU for Windows (it also brings the OVMF firmware):
   `winget install SoftwareFreedomConservancy.QEMU`
2. unzip, double-click **`start.bat`** — UEFI-DOS boots and stops at a live
   `C:\>` prompt with the demos already run.

The same image can be written to a USB stick (Rufus in *DD image* mode, or `dd`)
and booted on real UEFI hardware; `EFI/BOOT/BOOTX64.EFI` and `BOOTIA32.EFI` are
both inside. `make_release.ps1` rebuilds that package from source.

## Status

| Area | Status |
| --- | --- |
| UEFI boot, x86_64 | works — PE32+ `BOOTX64.EFI`, boots from the disk boot entry |
| UEFI boot, IA32 | works — PE32 `BOOTIA32.EFI`, same image holds both files |
| Boot volume discovery | LoadedImage → `DeviceHandle` → `BlockIo` → MBR partition (superfloppy fallback) |
| FAT32 | read + write (create/delete/rename/mkdir/rmdir, cluster alloc/free) |
| FAT12 / FAT16 | read + write |
| Floppy/disk image mount | `MOUNT A: IMAGE.IMG` exposes a file on `C:` as a read-write drive |
| Shell | `DIR CD MD RD DEL REN TYPE COPY CLS ECHO VER VOL DATE TIME PROMPT PATH SET MEM HELP APITEST MOUNT UNMOUNT CONFIG FCB DRIVES EXIT SHUTDOWN`, batch files, `AUTOEXEC.BAT` |
| DOS kernel | 8.3 names (+ VFAT long names), per-drive current directory, DOS error codes, **DOS 4.0 semantics** (see below) |
| DOS internals | `AH=52h` list of lists with a **walkable SFT chain**, `AH=5D06h`/`5D0Bh` **swappable data area**, `AH=34h` InDOS flag pointer, `AH=50h`/`51h` current PSP, and a real **job file table** at `PSP:18h` — the structures multitaskers (Windows 1.x/2.x, DESQview) swap per task — see [`docs/windows1x.md`](docs/windows1x.md) |
| 8086 emulator | 8086/80186 real-mode interpreter, 16 MiB emulated RAM, PSP, IVT, `.COM` + MZ `.EXE` loader with relocations, 8087 escapes treated as 8086 no-ops |
| INT 21h | console, file I/O, find first/next + DTA, dirs, date/time, version (`4.00`), vector get/set, IOCTL, dup, PSP, memory stubs, **EXEC (AH=4Bh)**, **AH=67h**, **AH=5Ch** byte locks, **SHARE modes**, **DOS 4.0 surface: 32h 52h 59h 5Ah 60h 63h 68h 6Ch** |
| FCB family | AH=0Fh 10h 11h 12h 13h 14h 15h 16h 17h 21h 22h 23h 27h 28h (`src/emu/fcb.c`) |
| INT 10h / 16h / 1Ah / 13h | text services (mode set, cursor, read/write char+attr, teletype, window scroll) on the emulated screen, blocking keyboard with extended keys, BIOS tick counter, disk read/write/geometry |
| Video (text) | an 80×25 colour-text screen at `B800:0000` inside emulated RAM; both direct VRAM writes and `INT 10h` output land there and are mirrored to the console (only changed rows are redrawn) |
| Video (graphics) | `INT 10h AL=13h` (320×200×256) and VESA VBE `4F00/4F01/4F02/4F03/4F05` (640×400, 640×480, 800×600, 8 bpp); DAC palette through `INT 10h AX=1000h/1012h/1015h` and ports `3C7/3C8/3C9`; frames are converted to the GOP format and written straight into the firmware framebuffer with integer upscaling — see [`docs/graphics.md`](docs/graphics.md) |
| Drives | every volume the firmware publishes is mounted: boot volume → `C:`, then `D:`, `E:`… (`DRIVES` lists them, read-only media are flagged) |
| Long file names | VFAT LFN chains are parsed for lookup, `DIR` and the native API, and files created with a long name get an LFN chain plus a generated 8.3 alias; DOS programs still see only the alias |
| Native 32/64-bit API | `g_dosapi` table — same operations and error codes as INT 21h, native pointers, one source for both firmware widths |
| 16-bit bridge | `exec16()` downcall (native → real mode) and `INT 2Fh AX=5544h` upcall (real mode → native) |
| CGA text | `INT 10h` `AH=02/03/06/07/09/0A/0E/13` render 8x8 glyphs into the CGA bitmap (40/80 columns, public-domain font) |
| CGA graphics | `INT 10h` modes 4/5/6 (320×200×4, 640×200×2), interleaved bitmap at `B800`, palette/background via `AH=0Bh` and the `3D8h`/`3D9h` ports |
| Mouse | `INT 33h` functions 0000h–0024h, event handler + mask (000Ch), firmware pointer polling and a software arrow drawn into the frame |
| Hardware interrupts | `INT 8` at 18.2 Hz and `INT 9` with a scan code latched for port `60h`, delivered only to handlers the guest installed; BIOS tick counter `0040:006C` runs |
| Program version override | `SETVER prog x.yy` (shell) and `SETVER=` (`CONFIG.SYS`) change what `INT 21h AH=30h` reports to one program |
| 386 protected mode | real → protected transition, GDT/LDT parsing, hidden segment caches with limit checks, `MOV CRn`/`LGDT`/`LIDT`/`LLDT`/`CLTS`, 32-bit addressing (SIB), `0F xx` opcode set, protected-mode IDT delivery |
| DPMI 0.9 host | `INT 2Fh AX=1687h` bootstrap, `INT 31h` descriptors/DoS memory/real-mode reflection — see [`docs/protected-mode.md`](docs/protected-mode.md) |
| Bundled utilities | `INFO CAL SUM HEX CLOCK` — 16-bit `.COM` programs built from `tools/make_tools.py` (see below) |
| Native programs (SDK) | `RUN64 file.exe` maps a freestanding PE32+/PE32 image, applies base relocations and calls it on the real CPU — see [`docs/sdk-guide.md`](docs/sdk-guide.md) |
| Emulator tests | `hosttest.exe --itest` — 64 byte-stream instruction cases — and `--ifuzz`, a brute-force audit of every opcode — see [`docs/itest.md`](docs/itest.md) |
| Build | PowerShell scripts (Windows), `make` (Linux/macOS/WSL) and a `Dockerfile` |

## Bundled utilities

Five small DOS programs are generated by [`tools/make_tools.py`](tools/make_tools.py)
and shipped inside `build/UEFIDOS.img`. They are written from scratch for this
project (MIT, same as the rest of the tree), so the image is self-contained and
free of third-party binaries, and every one of them exercises the real `INT 21h`
services rather than the shell's own code:

| Program | What it does | DOS services exercised |
| --- | --- | --- |
| `INFO` | system panel: DOS version, date, time, default drive, current directory, native API version and pointer width | `AH=30h, 2Ah, 2Ch, 19h, 47h`, `INT 2Fh AX=5544h` |
| `CAL` | calendar grid for the current month, correct first weekday and leap years | `AH=2Ah` (weekday), console output |
| `SUM` | byte count + 16-bit checksum of a file named on the command line | `AH=3Dh 42h 3Fh 3Eh` (open/lseek/read/close) |
| `HEX` | hex + ASCII dump of a file, 16 bytes per line | same file handle path, command-tail parsing |
| `CLOCK` | live `HH:MM:SS` clock, any key stops it | `AH=2Ch, 0Bh, 08h` (time, key status, key input) |

```
C:\> INFO
=== UEFI-DOS system info ===
DOS version : 4.00
Date        : 2026-11-08
Time        : 13:58:09
Drive       : C:
Directory   : C:\
Native API  : 01/64-bit pointers

C:\> CAL

      November  2026
Su Mo Tu We Th Fr Sa
  1   2   3   4   5   6   7
  8   9  10  11  12  13  14
 15  16  17  18  19  20  21
 22  23  24  25  26  27  28
 29  30

C:\> SUM HELLO.TXT
HELLO.TXT: 15 bytes, checksum 0471
C:\> HEX HELLO.TXT
0000: 48 65 6C 6C 6F 20 55 45 46 49 2D 44 4F 53 21    Hello UEFI-DOS!
```

`CLOCK` is interactive, so it is not part of `AUTOEXEC.BAT`: type it at the
prompt and press a key to return to the shell.


## Native 32/64-bit API and the 16-bit bridge

The same DOS services are reachable three ways with identical semantics:

| Caller | Entry point | Notes |
| --- | --- | --- |
| 16-bit DOS program | `INT 21h` | `src/emu/int21.c`, DOS-visible error codes |
| native 64-bit code | `g_dosapi` (`src/api/dosapi.h`) | same functions, native pointers and handles |
| native 32-bit code | `g_dosapi`, same table | `BOOTIA32.EFI` links the same source |

`APITEST` (a shell built-in, i.e. native code calling its own API) exercises the
path end to end:

```
C:\> APITEST
APITEST: DOS version 4.00, native pointer size 8
APITEST: size 16, read 16 bytes: Hello UEFI-DOS!
APITEST: wrote 13 bytes to TMPAPI.TXT
APITEST: find '*.TXT' -> 1 file(s)
APITEST: cwd C:\
Hello from 8086 code!
APITEST: exec16 HELLO.COM -> 0 (16-bit program ran)
API-OK
```

The table covers INT 21h AH=30h/3Ch/3Dh/3Eh/3Fh/40h/42h/41h/39h/3Ah/43h/3Bh/
47h/19h/0Eh/4Eh/4Fh/2Ah/2Ch plus INT 1Ah AH=00h, and returns DOS error codes
(0 success, 2 file not found, 3 path not found, 5 access denied, 0x0B bad
format, 0x27 disk full, …). Handles are `u16`; open modes and seek whences match
INT 21h; `find_first`/`find_next` return the fields the DTA would carry.

- **Falling from 64/32-bit into 16-bit** (`exec16`): loads a real-mode
  `.COM`/`.EXE`, runs it to completion on the emulated 8086, returns its exit
  code. The caller's state is snapshotted and restored, so native code can use
  DOS programs as subroutines.
- **Falling up from 16-bit into native code**: the compatibility layer routes
  `INT 2Fh AX=5544h` into `src/emu/int2f.c` — `DX=1` returns `AX=0101h` and
  `BX` = native pointer size (4 or 8), `DX=2` prints the native banner, `DX=3`
  fetches the RTC into `ES:BX`, anything else returns `AX=FFFFh`.
- External EFI applications can obtain the table at runtime: the image installs
  a UEFI configuration table entry under `UEFIDOS_API_GUID`
  (`55454449-2D44-4F53-4150-4900-000000000001`) whose value is a
  `dosapi_exports { version, pointer_size, services }` record.

## DOS 4.0 real-mode semantics

Details in [`docs/dos4-semantics.md`](docs/dos4-semantics.md); migration pitfalls
for existing 16-bit code in [`docs/migration.md`](docs/migration.md).

| Feature | Implementation |
| --- | --- |
| SHARE compatibility modes | `share_mode` / `open_mode` / `psp` / `locks[8]` per handle; DENY_RW/WRITE/READ/NONE checked in both directions, `compat` conflicts only across PSPs |
| `FILES=` | default 8 (handles 0..4 are devices, so 3 free); `CONFIG.SYS` `FILES=`/`FCBS=`/`LASTDRIVE=` honoured; exceeding the limit reports DOS error 4; changeable with `AH=67h` |
| FCB call family | stateless FCB layer (8.3 name re-resolved per call), 14 AH entry points, DTA is the record buffer |
| Byte locks | `AH=5Ch AL=0/1`, overlap detection inside the handle table, conflicts report error 5 |
| FAT16 large volumes | allocation cursor + free-cluster counter; `tools/mkfat16.py` builds 32 MB / 1.9 GB sparse test volumes |
| File size | `u32` size/offset in the DOS calls (FAT32 format ceiling 4 GB-1, verified with a 1.5 GB file); `INT E0h` gives 64-bit offsets, split files (`NAME.001`...) past that |
| Reported version | `AH=30h` returns `AX=0x0004` (DOS 4.00); `VER` prints the same level. Set `UEFIDOS_DOSVER_*` in `src/uefidos_config.h` to claim another one |
| Data structures | `AH=32h` synthesises a DPB per mounted drive in the reserved ROM window; `AH=52h` returns a list-of-lists whose `+2` points at that chain |
| EXE image placement | `CONFIG.SYS` `EXECHIGH=ON` loads an EXE image flush against the top of the program's block instead of right after the PSP (what DOS itself does). `CONFIG` shows the current mode; the host harness has `UEFIDOS_EXEC_HIGH_IMAGE` |
| Instruction-rate log | `CONFIG.SYS` `PERFLOG=ON` prints `[perf] steps=.. ms=.. ips=.. frames=..` to the serial port every ~2M guest instructions. It splits "this program is slow" into emulator cost (`ips`) and the program's own drawing (`frames`) |
| Fatal-trace ring | `CONFIG.SYS` `RINGLOG=ON` keeps the per-instruction register ring a fatal trace prints (13 registers per instruction). Off by default in both the UEFI build and the host harness; on the host, `UEFIDOS_RINGLOG=1` turns it on without touching the image |
| Mouse speed | `CONFIG.SYS` `MOUSESPEED=n` sets how many host counts the emulated Microsoft bus mouse (the card Windows 1.x drives) needs per count it reports (default 2 = one reported count per two host counts, i.e. 1 CGA pixel of travel; 1 is twice as fast, 4 gives fine control) |
| Error reporting | `AH=59h` maps the last failure to a class / suggested action / locus triple (`2`→class 8, `5`/`21h`→class 3, `27h`→class 11, `50h`→class 12, …) |
| Extended open | `AH=6Ch` with the DOS action bits: open-existing / create-if-missing / replace-existing, returning handle in `AX` and `1`/`2`/`3` in `CX`; `AH=68h` commits |
| Unique temp files | `AH=5Ah` appends `TMPxxxxx.TMP` to the given directory and retries on collision, so two programs never collide |
| Truename / DBCS | `AH=60h` canonicalises to `C:\DIR\FILE.EXT`; `AH=63h` reports `AL=0` (single-byte code page) |

Dual-path consistency (`--phase46`) runs the same operation through `INT 21h`
and through `g_dosapi` and asserts: valid handle, identical byte counts,
identical seek position, byte-identical read-back, identical error codes
(missing file → 2, delete read-only → 5).

## Protected mode and DPMI

A 16-bit program can drop into 386 protected mode and run flat 32-bit code, and
32-bit clients can bootstrap through the built-in DPMI host. `DPMITEST.COM`
asserted by the regression suite walks the whole path:

```
DPMI-HOST-OK       INT 2Fh AX=1687h found the DPMI host
PM16-OK            the host entry point switched us to 16-bit protected mode
PM32-DOS-OK        far jump to a flat 32-bit selector, INT 21h AH=9 from 32-bit code
REFLECTED-21H-OK   printed by a real-mode INT 21h reached via INT 31h AX=0300h
RMREFLECT-OK       the reflection returned to protected mode
DOSMEM-OK          INT 31h AX=0501h allocated DOS memory
```

The remaining gaps (ring 3, paging, full DPMI) and the staged plan for them are
in [`docs/protected-mode.md`](docs/protected-mode.md).

## Build and run

### Linux / macOS / WSL: `make`

The whole OS builds with any PE-targeting GCC -- mingw-w64 on Linux or macOS --
and the same `make` works under WSL:

```sh
sudo apt install build-essential gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 python3 make

make            # build/BOOTX64.EFI
make all        # + build/BOOTIA32.EFI
make image      # + build/UEFIDOS.img (bootable, AUTOEXEC ends at a live C:\> prompt)
make sdk        # + build/HELLO64/FILET64.EXE (64-bit), HELLO32/FILET32.EXE (32-bit)
make run        # boot the image in QEMU (set OVMF_DIR=/usr/share/OVMF)
make clean
```

`make` needs no Visual Studio, no PowerShell and no Python packages -- only a
cross compiler, `make` and `python3` (stdlib). `make host` / `make test` (the host
harness and the instruction test vectors) are Windows-only: the harness uses
Win32 console and file APIs.

### Or entirely inside Docker

```sh
docker build -t uefi-dos .
docker run --rm -v "$PWD/out:/src/build" uefi-dos        # BOOTX64/IA32 + image + SDK
docker run --rm -v "$PWD/out:/src/build" uefi-dos make sdk
```

The container installs `gcc-mingw-w64` and builds everything from source.

### Windows: PowerShell

Requirements:

- Windows with MSYS2 `mingw64` gcc -- one x86_64 toolchain builds both the
  64-bit and the 32-bit UEFI binaries (the project avoids libc; 64-bit division
  helpers live in `src/common/divdi3.c`)
- Python 3 (image builder and hand-assembled test programs; stdlib only)
- QEMU plus OVMF/edk2 firmware for the boot tests

```powershell
# build BOOTX64.EFI, BOOTIA32.EFI, hosttest.exe and build/UEFIDOS.img
powershell -ExecutionPolicy Bypass -File build.ps1

# boot it under QEMU + OVMF x86_64 (serial log -> build\serial.log)
powershell -ExecutionPolicy Bypass -File run_uefi.ps1

# same image with 32-bit OVMF firmware
powershell -ExecutionPolicy Bypass -File run_uefi.ps1 -Firmware ia32

# full regression suite (host harness + both firmware boots)
powershell -ExecutionPolicy Bypass -File run_tests.ps1
```

Override anything through the environment instead of editing the scripts:

| Variable | Meaning | Default |
| --- | --- | --- |
| `UEFIDOS_GCC_DIR` | directory holding `gcc.exe` / `objdump.exe` | `C:\msys64\mingw64\bin`, else `PATH` |
| `UEFIDOS_PYTHON` | python interpreter | `python` |
| `UEFIDOS_QEMU` | `qemu-system-x86_64.exe` | `C:\Program Files\qemu\qemu-system-x86_64.exe` |
| `UEFIDOS_OVMF_DIR` | firmware directory (`edk2-*-code.fd`, `edk2-i386-vars.fd`) | `C:\Program Files\qemu\share` |
| `UEFIDOS_DOS211` | your copy of an MS-DOS 2.11 floppy image | unset → floppy checks skip |
| `UEFIDOS_DOS5` | your copy of an MS-DOS 5 floppy image | unset → that check skips |

## Interactive QEMU launcher

Double-click `start_qemu.bat` (or run `run_interactive.ps1`) to boot UEFI-DOS in
a QEMU window. It builds on demand, runs a short demo `AUTOEXEC.BAT`
(VER/DIR/HELLO/TELETYPE/MZEXE/RUNNER/VRAM/APITEST/UPCALL) and then **stops at the
`C:\>` prompt** so you can type commands yourself:

```
start_qemu.bat                 # window mode, boot to C:\>
start_qemu.bat -Headless       # no window, serial log only
start_qemu.bat -Firmware ia32  # 32-bit payload
```

At the prompt, `APITEST` shows the native 32/64-bit API at work, `UPCALL` shows
a 16-bit program calling into it, `HELP` lists built-ins, `SHUTDOWN` powers off.

If `UEFIDOS_DOS211` is set, the demo image also carries `DOS211.IMG` and mounts
it as `A:`. Screenshot helpers (stdlib only): start QEMU with
`-monitor tcp:127.0.0.1:55555,server,nowait`, then
`python tools/screenshot.py 55555 build/screen.ppm` and
`python tools/ppm2png.py build/screen.ppm build/screen.png`.

## Native 32/64-bit programs (SDK)

DOS programs are 16-bit. UEFI-DOS also runs **native** programs: freestanding PE
images loaded into executable memory, relocated, and called on the real CPU --
no interpreter, no mode switch -- using the same DOS services, the same file
handles and the same current directory as the 16-bit world.

```sh
make sdk                       # HELLO64/FILET64.EXE (PE32+) and HELLO32/FILET32.EXE (PE32)
```

```powershell
powershell -ExecutionPolicy Bypass -File sdk\build_sdk.ps1
```

A native program is a normal C file with one entry point:

```c
#include "uefidos/dosapi.h"
#include "uefidos/runtime.h"

int uefidos_main(const uefidos_exports *api, const char *args)
{
    uefidos_init(api);
    rt_puts("hello from a 64-bit program\n");

    u16 h;
    if (api->svc->open("HELLO.TXT", DOS_OPEN_READ, &h) == 0) {
        char buf[80]; u32 got;
        api->svc->read(h, buf, sizeof(buf) - 1, &got);
        api->svc->write(1, buf, got, &got);       /* handle 1 == console */
        api->svc->close(h);
    }
    i16 code;                                      /* 16-bit downcall */
    api->svc->exec16("HELLO.COM", "", &code);
    return 0;
}
```

```
C:\> RUN64 HELLO64.EXE
RUN64: loading HELLO64.EXE as a native 64-bit image
=== UEFI-DOS native SDK demo ===
pointer size : 64 bit
DOS version  : 4.00
reading      : HELLO.TXT (16 bytes)
Hello UEFI-DOS!
wrote        : NATIVE.TXT (44 bytes)
Hello from 8086 code!
exec16       : HELLO.COM -> exit 0 (16-bit program ran)
NATIVE-OK
RUN64: exit code 0 -> NATIVE-OK
```

The `Hello from 8086 code!` line is a 16-bit `.COM` started by the 64-bit program
-- the downcall path end to end. Bitness is enforced by the loader (a 64-bit
kernel refuses 32-bit images: that code cannot execute in a 64-bit address
space). The full service list, the linker flags and the limitations are in
[`docs/sdk-guide.md`](docs/sdk-guide.md).

## Instruction-level emulator tests

`src/emu/emu.c` is one large `switch`, so regressions there used to surface as
"a game renders garbage". `src/emu/itest.c` turns each opcode into a byte stream
plus a register/memory expectation, executed through the real decoder:

```powershell
build\hosttest.exe --itest          # 64 cases
build\hosttest.exe --ifuzz          # brute-force audit of every opcode
build\hosttest.exe --itest shift    # only cases whose name contains "shift"
```

```
  [PASS] add ax,bx (reg dst)
  [PASS] shl ax,1
  [PASS] rep stosw (3 words, 2 store units)
  [PASS] jmp far (EA)
itest: 64 passed, 0 failed
ITEST PASSED
```

Adding a case is a five-line table edit; see [`docs/itest.md`](docs/itest.md) for
the format, the traps (macro commas, `lodsb` preserving `AH`) and what is still
uncovered.

## Real hardware: a USB stick and autobanch

**The stick.**  Nothing has to be installed: a FAT32 volume holding
`EFI\BOOT\BOOTX64.EFI` (or `BOOTIA32.EFI` on 32-bit firmware) is the whole
operating system, and every file on that same volume is `C:`.  Windows 1.01 can
live next to it, which is what the USB kit does: `WINDOWS\` holds the tree from
the 1.01 disks, `AUTOEXEC.BAT` sets the three `SETVER ... 3.30` entries 1.01 and
1.04 need (both refuse to start under the DOS 4.00 that UEFI-DOS reports),
`CONFIG.SYS` carries `FILES=40 / LASTDRIVE=D / EXECHIGH=ON / MOUSESPEED=2`, and
`WIN1.BAT` / `BENCH.BAT` give one-word commands -- `WIN1`, `BENCH` -- because the
shell now runs `NAME.BAT` from the prompt instead of handing it to the program
loader.  Boot verified on the real firmware path by attaching the image as a
**USB stick** to QEMU + OVMF (`-device qemu-xhci` plus `usb-storage`, no other
disk, so the firmware's only boot option is the stick), then moving a virtual USB
mouse with a QMP event: the pointer follows it into the 1.01 desktop
(`docs/windows1x.md`, Round 38).

**autobanch.**  `AUTOBANC.COM` (5,324 bytes, `tools/autobanch.asm`) is an
8086-only .COM that measures the machine it runs on: ALU chain, `mul`, `div`,
`rep movsw` / `stosw` / `lodsw` block rates, `INT 21h` dispatch cost and disk read
throughput, each timed over exactly one second, plus DOS version, video mode,
conventional memory and the BIOS tick rate measured against the timer chip.
Since 1.6.0 the emulator answers reads of the 8253 channel-0 counter (`out 43h,0`
to latch, `in 40h` twice for low and high byte), so a guest has a real 1.193182
MHz clock; the earlier version had to time with the 55 ms BIOS tick, whose rate
the wall-clock pacer keeps adjusting, and the same binary then reported `MUL
2,080,000 /s` in one run and `MUL 90,240 /s` in the next.  Same binary, two
machines: on the Windows host harness `ALU 14.9M /s`, `copy 120 MB/s`,
`INT 21h 486k /s`, `disk 145 MB/s`; inside QEMU + OVMF (nested emulation)
`1.6M /s`, `5.7 MB/s`, `221k /s`, `4 MB/s`.

## Graphics, extra drives, long names

**Graphics.** `GFXTEST.COM` switches to `INT 10h` mode 13h, loads the whole DAC
through `AX=1012h`, paints 320x200 pixels and reports `GFX-OK 320x200x256`;
`VESATEST.COM` walks the VESA calls (`4F00`, `4F01`, `4F02` with the linear
framebuffer bit) into 640x480x256 and paints the screen in five 64 KB passes.
Under UEFI the emulated framebuffer is converted to the GOP pixel format and
written straight into `Mode->FrameBufferBase`: the horizontal step is an
integer repeat of each source column, the vertical step is `out_h / h` source
rows per destination row, and the picture is sized to 4:3 rather than to the
framebuffer's own ratio. That is what a real card did — a 200-line mode's pixels
are about 1:2.4 tall, not square — so a 320x200 game still fills the width at 2x
while a CGA 640x200 desktop (Windows 1.01) fills a 1280x800 panel edge to edge
instead of sitting in a letterbox with the text squeezed to two thirds of its
height. While a graphics mode owns the
screen, text output is redirected to the serial port so the console cannot
repaint its text grid over the picture. Details, including the two firmware
`Blt` paths that had to be abandoned, are in [`docs/graphics.md`](docs/graphics.md).

**Every volume is a drive.** `plat_enum_blockdevs()` asks the firmware for all
`BlockIo` handles, keeps the boot volume first (`C:`), skips whole disks when
logical partitions exist (so `D:` never aliases `C:`), labels each device from
its device path (`USB`, `SATA`, `NVME`, `SD`) and mounts them as `D:`, `E:`…
Read-only media mount read-only. `DRIVES` prints the table:

```
C: >FAT32 UEFIDOS
D:  FAT32 USBSTICK
```

**Long file names.** LFN chains are validated against the 8.3 checksum, resolved
case-insensitively for `open`, listed by `DIR` and reported by the native API.
Creating a name that 8.3 cannot express (`Long Name Report.txt`) writes the LFN
chain plus a generated alias (`LONGNA~1.TXT`), and the native API resolves an
alias back with `longname()`. DOS programs keep seeing only 8.3 names, which is
the point: the old side stays old, the new side gets the real name.

```
C:\> RUN64 HELLO64.EXE
long name    : created "Long Name Report.txt"
long read    : this file has a VFAT long name
8.3 alias    : LONGNA~1.TXT -> Long Name Report.txt
...
C:\> DIR *.TXT
Long Name Report.txt         31 13:12  11-11-26
```

## Regression suite

`run_tests.ps1` builds everything and runs 34 checks: FAT32 read/write selftest;
host shell DIR/TYPE/VER/COM/EXE/EXEC/video plus native-API and upcall demos;
host shell write path; floppy-image `A:` mount; FAT12 write on a 2.11 image;
FAT12 read of 2.11 and MS-DOS 5 images; two QEMU/OVMF boot smoke tests (x86_64
and IA32); phase 4/6 semantics with the dual-path comparison; FAT16 write on
32 MB and 1.9 GB volumes; `FCBTEST.COM` exercising FCB calls from real 16-bit
code; the DOS 4.0 conformance probe `DOS4TEST.COM` (DPB, list of lists, extended
error, `AH=6Ch` create/open/replace/refuse, temp file, truename, DBCS, commit —
30 assertions, one `DOS4-OK` line); and the bundled `INFO`/`CAL`/`SUM`/`HEX`
utilities (the calendar month is
cross-checked against the host clock); and `KEYTEST.COM`, which reads five keys through `INT 16h` and prints their scan codes, proving arrow and function keys reach the guest as real BIOS scan codes; the
instruction-level emulator test vectors (`--itest`, 64 cases) plus a brute-force opcode audit (`--ifuzz`); and `RUN64
HELLO64.EXE`, a native 64-bit program built by the SDK, which prints the DOS
version, reads and writes files, lists `*.TXT`, then runs a 16-bit `.COM` through
the interpreter and reports its exit code; a second UEFI volume mounted as
`D:` (`DRIVES`, `VOL D:`, `TYPE D:\SECOND.TXT`); long file names (the native
demo creates `Long Name Report.txt`, `DIR` shows it, `TYPE` opens it by that
name and `longname()` maps `LONGNA~1.TXT` back to it); `INT 10h` mode 13h and
the VESA VBE calls painting a dumpable framebuffer.

Checks that need a Microsoft DOS floppy image are skipped (reported as
`[SKIP]`) when `UEFIDOS_DOS211` / `UEFIDOS_DOS5` are unset.

## Host test harness

`hosttest.exe` runs the same FAT driver, kernel, shell, native API and emulator
on Windows over a file-backed block device — fast iteration without QEMU:

```powershell
build\hosttest.exe build\UEFIDOS.img                 # FAT32 read/write selftest
build\hosttest.exe --shell build\UEFIDOS.img "DIR" "TYPE HELLO.TXT" "APITEST" "UPCALL"
build\hosttest.exe --fatlist  "C:\path\to\floppy.img"  # inspect a FAT12/16/32 image
build\hosttest.exe --phase46  build\t_p46.img          # SHARE / FILES= / FCB / locks / dual path
build\hosttest.exe --fat16    build\fat16_2g.img big   # 1.9 GB volume + 32 MB file
```

Debug knobs (host build only) that make a compatibility run reproducible:

| Variable | Effect |
| --- | --- |
| `UEFIDOS_MAX_STEPS=n` | instruction budget for one program (default 200000000), refilled on every keystroke |
| `UEFIDOS_HARD_STEPS=n` | absolute budget that keystrokes do not refill, so an interactive program still terminates |
| `UEFIDOS_KBD=k1,k2,…` | scripted keys: single characters, `ENTER ESC SPACE TAB BACKSPACE HOME END PGUP PGDN INS DEL UP DOWN LEFT RIGHT F1…F10`, or `CTRLA…CTRLZ` |
| `UEFIDOS_KBD_MAX_POLLS=n` | polls after which an `ESC` is delivered so "wait for key" loops finish (default 100000) |
| `UEFIDOS_PSP_SEG=0x260` | load-segment override for programs linked for a fixed PSP |
| `UEFIDOS_DUMP_VRAM=1` | dump the emulated 80×25 screen to `build/vram_dump.txt` with a column ruler |
| `UEFIDOS_DUMP_MEM=file` | write the low 640 KB as the program ends (compare with a QEMU `pmemsave` of the same program under real DOS) |
| `UEFIDOS_DUMP_AT=CS:IP` (+ `UEFIDOS_DUMP_AT_FILE=file`) | snapshot the low 640 KB the first time execution reaches that address (default `build/mem_at.bin`) — captures a program's arena at the exact instruction where its allocator gives up |
| `UEFIDOS_TRACE_INTS=1`, `UEFIDOS_TRACE_VRAM=n`, `UEFIDOS_TRACE_STEPS=n`, `UEFIDOS_TRACE_WINDOW=CS:LO-HI`, `UEFIDOS_TRACE_PORTS=1` | interrupt, video-write, single-step, address-window and port-I/O traces under `build/` |
| `UEFIDOS_TRACE_IRQ=1` | hardware-interrupt trace: one line per INT 8h/9h frame pushed into a guest handler (`[irq] vec=08 depth=0 from CS:IP to CS:IP`), plus a periodic `[irqdbg]` line with the delivery conditions (IF, irq depth, whether the vector is still ours) |
| `UEFIDOS_TRACE_SS=1` | log every instruction that changes `SS`, with the frame it lands on (`[ss] #1234 1409->0D00 sp=0826->0826 after 0D00:0592 next=0D00:0594 ax=...`) — a program that silently ends up on the wrong stack segment (a PSP instead of its own stack, or a stale frame restored by a bad `IRET`) is invisible in DOS call traces; this is how Windows 1.01's FatalExit `stack overflow` was pinned on a stale frame rather than on a real stack exhaustion |
| `UEFIDOS_TRACE_MOUSE=1` | one line per `INT 33h` call in `build/mouse_trace.txt` (`INT33 AX=.. BX=.. at CS:IP`) — the INT 21h trace cannot see this vector, so this is how "the guest never touches the mouse" becomes provable (Windows 1.01 with no pointing device selected makes zero calls) |
| `UEFIDOS_TRACE_PORT=1` | every access to the emulated Microsoft bus-mouse card (`[port] out 23F = 91 at CS:IP ax=...` in `build/port_trace.txt`) - shows whether the guest actually loaded an InPort driver, and which registers it uses |
| `UEFIDOS_GFX_VERIFY=1` | after every flush, expand the whole CGA bitmap again and compare it with the frame the incremental repaint produced (`[gfx-verify] frames=450 ok (bad frames 0)`; a mismatch prints the first offending pixel) — the guard for "repaint only what changed", because a bug there shows up as stale pixels, not as a crash |
| `UEFIDOS_MOUSE_WAIT=<ms>` | delay the first scripted mouse event (a 24 M-step run finishes in ~4 s, so a script that has to land after the desktop needs `UEFIDOS_MOUSE_WAIT=1000`) |
| `UEFIDOS_MOUSE_SCALE=n` | host counts per count the emulated bus mouse reports (`n` or `nx,ny`; default 2 = about one CGA pixel per count) |
| `UEFIDOS_TICK_STEPS=<n>` | force a fixed number of emulated instructions per BIOS timer tick. By default the period is adjusted continuously so the tick follows **wall-clock time** at 18.2 Hz (real hardware behaviour); the step trigger still decides *where* the interrupt lands. Lower it to stress a guest with more timer interrupts |
| `UEFIDOS_TRACE_FIND=1` | one line per `INT 21h AH=4Eh/AH=4Fh` with the search state recovered from the DTA and the result (`[find] 4F pat=??????????? attr=30 dir=2 idx=155 rc=1 name=`) — the fastest way to tell "a program loops because the search never ends" from "because the search never started" |
| `UEFIDOS_IRQ_OLDGUARD=1` | put back the old "any IRET ends the hardware interrupt" rule, so the frame-accurate guard can be compared with it (the old rule let a timer tick nest inside a guest handler that chains to the previous vector) |
| `UEFIDOS_TRACE_IVT=1` | log every `INT 21h AH=25h` the guest performs (`[ivt] set 08 = 15B7:0244 by CS:IP`) — the fastest way to see which handler a program installs, and whether it installs one at all |
| `UEFIDOS_TRACE_PSP=1`, `UEFIDOS_TRACE_EXEC=1` | print every new PSP's command tail (and the bytes after it) and every `INT 21h AH=4Bh` (path, environment segment, command-line far pointer, caller PSP/DS) plus failing `AH=48h` requests |
| `UEFIDOS_TRACE_MEM=1` | dump the DOS memory arena (MCB chain: segment, `M`/`Z`, owner PSP, size, data range) after `INT 21h AH=4Ah` and whenever `AH=48h` fails — the fastest way to see whether a program's shrink-then-probe memory sizing came out right |
| `UEFIDOS_EXEC_HIGH_IMAGE=1` | an experiment, not the default: load an EXE image flush against the top of the program's block and leave the PSP at the bottom — the geometry a reference MS-DOS 5.00 run shows for Windows 1.01's kernel (default off, because `PSP+0x10` is what programs read as their own segment) |
| `UEFIDOS_TRACE_DIV=1` | print the `CS:IP` and registers whenever a `DIV`/`IDIV` traps with a quotient overflow |
| `UEFIDOS_TRACE_TERM=1` | host only: every `INT 21h AH=4Ch` prints the calling PSP, its terminate address (`PSP:000Ah`) and the parent PSP -- this is how "one task is done" is told apart from "the session is over" |
| `UEFIDOS_TRACE_PSP55=1` | host only: every `INT 21h AH=55h` (create a child PSP) prints the current PSP, `DX` (new PSP segment) and `SI` (block size in paragraphs) |
| `UEFIDOS_DUMP_HALT=1` | host only: write the first megabyte of guest memory to `build/mem_halt.bin` when the program halts, so the task list and kernel cells at that moment can be read offline |
| `UEFIDOS_EXEC_HIGH=1` | place an `EXEC`'d child flush against the top of memory instead of right after its caller (for probing programs that are picky about where DOS put them) |

The last 64 steps are dumped to `build/limit_dump.txt` when a program hits the
instruction limit.

## Benchmark: native 64-bit vs native 32-bit vs the 16-bit DOS VM

`bench/` runs the same eight workloads (ALU, fill, copy, scan, DOS-API calls,
console output, file I/O, directory find) in all three execution worlds, with
identical byte counts and checksums, and [`docs/benchmark.md`](docs/benchmark.md)
explains the method and the caveats. Medians of three runs, same machine:

| workload | 16-bit DOS VM | native 64-bit | ratio |
| --- | ---: | ---: | ---: |
| `alu` (16-bit LCG) | 1,424.8 ns/step | 4.0 ns/step | **358×** |
| `fill` (6 MB of `STOSB`) | 7.6 MB/s | 5,153 MB/s | **675×** |
| `copy` (2.3 MB copied) | 3.9 MB/s | 4,653 MB/s | **1,192×** |
| `scan` (3 MB summed) | 3.4 MB/s | 3,743 MB/s | **1,111×** |
| `api` (`INT 21h` vs SDK) | 2,095 ns/call | 1.1 ns/call | **1,837×** |
| `text` (3.1M chars) | 209.5 ns/char | 21.2 ns/char | **9.9×** |
| `file` (132 KB write+read) | below one 55 ms tick | 110 MB/s | — |
| `find` (1200 entries) | below one 55 ms tick | 4,803 ns/entry | — |

Under QEMU + OVMF with WHPX (the IA32 build, so PE32 and the DOS VM share the
machine): `alu` 178×, `fill` 745×, `copy` 581×, `scan` 1,066×, `api` 199×, and
then **`text` 1.0×, `file` 1.2×, `find` 1.0×** — console output, disk access and
directory scans cost the same in both worlds because the time goes into the
platform (serial mirror, EFI block I/O), not into the guest. In short: a
CPU-bound DOS program pays 200x-1200x for the interpreter, an I/O-bound one
pays nothing.

## Compatibility with real DOS software

No DOS system files, commercial applications or games are distributed with this
repository — the test images contain only programs that `tools/*.py` generates.
To check compatibility against real software, supply your own disk images
(`UEFIDOS_DOS211`, `UEFIDOS_DOS5`) or copy programs onto the FAT32 image.

`docs/compat.md` records a compatibility sweep of CardPuter86-era DOS programs
(games, a terminal, an assembler, a hardware diagnostic) run from images the
author supplied, including the five emulator defects that sweep exposed: ALU
direction bit, shift/rotate selector numbering, the missing BIOS keyboard
buffer at `0040:001A/001C/001E`, non-callable interrupt vectors, and the missing
`INT 1Ah` timer. Those programs are third-party work and are not part of this
repository.

### Windows 1.x / 2.x

In a CGA mode the text services now paint 8x8 glyphs straight into the
interleaved bitmap (`src/common/font8x8.c`, public-domain font, 40 columns in
modes 4/5 and 80 in mode 6), so anything a program prints with `AH=0Eh` after
switching to a CGA mode is visible — that is how Windows 1.01 prints its
start-up banner.  `tools/make_cgatext.py` builds the probe behind the
`graphics: INT 10h text services paint glyphs in CGA mode 4` regression check.

The `INT 10h` CGA modes, the `INT 33h` driver and the hardware-interrupt
injection exist because of Windows. `SETUP` from **Windows 1.01** runs end to end
on a merged install volume: it detects the CGA adapter, copies the build and
utility disks, writes `WIN.COM`/`WIN100.BIN`/`WIN100.OVL` and finishes with
"Microsoft Windows is set up to operate on your computer". `WIN` starts the
kernel once `SETVER WIN.COM 3.30` / `SETVER WIN100.BIN 3.30` satisfy its DOS
version gate — and **the kernel now paints its splash screen**: it switches to
CGA 640x200, draws the "MICROSOFT" logo and only then exits with code 1. Two
EXEC bugs stood between "nothing at all" and that picture, both fixed:

* `INT 21h AH=4Bh` copied just the count-prefixed string and stopped at the first
  CR. DOS copies the caller's **whole 128-byte command-tail buffer**
  (`PSP:0080h..00FFh`) into the child's PSP, and pre-3.0 software hides data
  there: `WIN.COM` stores the word `'XW'` at `PSP:00FEh`, and the Windows kernel
  refuses to boot without that marker (`CS:653Eh`).
* The tail **count byte excludes the terminating CR**, so a bare command line has
  count 0 (the CR still sits at `0081h`), not count 1. `WIN.COM` appends the
  overlay path at `0081h+count` and never updates the count, so being one byte
  off pushed the path two bytes too far and the kernel rebuilt a 30-character
  module name instead of the 13-character `C:WIN100.BIN`.

A third call decided how far the boot got: `INT 21h AH=47h` (get current
directory) prepended a backslash and returned `AX=0000` where real MS-DOS 5
writes the bare path and `AX=0100h`. Windows joins that answer into its overlay
name, so the name grew by two separators and 8 bytes, and the module-database
request grew with it. With the format fixed and the same files laid out in a
flat root (what the real-DOS reference run does), the kernel issues 129 DOS calls
and paints the splash. `INT 21h AH=57h` (get file date/time) was also a stub
answering zeros; handles now carry the directory timestamp and the SFT publishes
it. `docs/windows1x.md` holds the traces, the reference dumps and what is still
open: the kernel opens `WIN100.OVL`, timestamps it, closes it without reading a
byte and still exits 1, because its first `GlobalAlloc` asks for **28
paragraphs (435 bytes)** for the module database while the only block its own
arena builder hands it is **27 paragraphs (428 usable)** — two bytes of
headroom short. Shrinking that request by four bytes in a scratch image makes
the allocation succeed (the kernel then advances past `CS:212Eh` with a real
segment instead of `ES=0001h`), which proves the wall is the size and not the
code path; the next gate, `CS:0E71h`, still returns "0 modules". The `TAILCHK.COM` regression (`COUNT=00 C81=0D`) keeps the
tail semantics from drifting back. **Windows 1.0 Developer Release 5** can be laid
out on a UEFI-DOS image with `tools/install_win1x.py` (and started with
`start_win1x.bat`), but its `KERNEL.EXE` destroys its own code and stack in its
bootstrap before it makes a single `INT 21h` call — the same files hang the same
way on a real Compaq MS-DOS 2.11 boot floppy, so that one is not on UEFI-DOS.

**1.1.6 moves that wall.** `CONFIG.SYS EXECHIGH=ON` loads the kernel image flush
against the top of its block (`load 0x748B + 0x2B35` paragraphs = `0x9FC0`), the
geometry a reference MS-DOS 5.00 run shows for the same file. In it the
module-database allocation succeeds, the kernel opens `WIN100.OVL`, reads
`WIN.INI`, installs its own `INT 21h` front end and critical-error handler, and
its CGA driver at `16CDh` really executes `INT 10h AH=0Fh / AH=00h` (mode 6) `/
AH=0Fh`. The `INT` trace now records the call site (`at CS:IP`), which also
settled who prints the banner: it is `WIN.COM`'s own embedded `LOGO` module
(image offset 0x890, `INT 10h AH=0Eh` from `0D89:012Ch`), and the mode-3 restore
at the end is that module's entry 7 on WIN.COM's exit path, not the kernel
switching back. The kernel still returns to WIN.COM right after the display
driver initialises, so there is no desktop yet; `docs/windows1x.md` has the
decoded interrupt dispatcher, the segment map and the next steps.
Windows/286 2.10 needs 286 protected mode and an EMM, which UEFI-DOS does not
have.

### Handing the screen back to the text console

Returning from a graphics mode is not just "stop writing pixels": the scaled
game frame covers only part of the screen (letterbox bands around it), and the
firmware console repaints only its own character grid. Windows 1.x's splash
therefore survived the mode reset — the `C:\WINDOWS>` prompt came back drawn on
top of two white slabs of the abandoned logo. `plat_video_text()` now wipes the
whole linear framebuffer (`gfx_clear()`) and blanks the 25x80 console grid, and
`plat_video_gfx_begin()` clears it again on the way in, so the letterbox bands
are black instead of holding the previous text screen.

### The firmware graphics path that GCC deleted

Setting a video mode under QEMU/OVMF crashed straight into the firmware's own
exception handler (`#UD` at a `ud2`). The cause was not the emulator: the GOP
lookup helper `gop_find()` was defined but never called, so `g_gop` was provably
NULL and the compiler removed the *entire* graphics path —
`plat_video_gfx_show` compiled to a bare `ret` and `plat_video_gfx_begin` to a
load from address 0x18 followed by `ud2`. Every host-side graphics test still
passed, because the host platform has its own implementation; only a real
firmware boot shows it. 1.1.0 calls `gop_find()` where it belongs and the
regression suite now asserts that the built `BOOTX64.EFI` still contains real
bodies for those functions (`tools/check_efi_symbols.py`).

### SimEarth 1.5 (1993): the milestone case

`docs/simearth.md` is a step-by-step account of running a commercial 1993 game:
the Maxis `INSTALL.EXE` completes its five screens, unpacks the overlays and
writes `SIMEARTH.CFG`, and `SIMEARTH.EXE` then runs in 320×200×256 with its
terrain map and control panels drawn. It is worth reading as a list of what a
program of that era assumes about the machine, because each assumption was a bug
here until it was tested:

* 16-bit code addresses its stack frame with **negative 8-bit displacements**
  (`mov [bp-4],ax`); zero-extending that displacement silently redirected every
  local variable into the caller's frame.
* real-mode addresses **wrap at 1 MB** while the A20 gate is masked — EXEPACK
  loaders read their compressed stream through aliases like `FF69:FFF4`.
* the **BIOS data area** must describe a colour adapter (`40:10h` equipment
  word, `40:63h` CRTC port, `40:85h` character height), or installers decide the
  machine is a mono MDA and refuse to draw.
* `INT 21h AH=48h/49h/4Ah` must really allocate: Microsoft C's startup allocates
  a copy of the environment and aborts with `R6009` otherwise.
* `PSP:0005h` (the CP/M entry point that MS C *calls* to exit) must point at a
  terminate stub, not at a garbage far pointer.
* an MZ image may be **larger than 64 KB** (SimEarth's is 125 KB).
* `INT 21h AH=47h` returns the current directory **without** a drive letter.
* the default `FILES=` must leave more than three usable handles.

Also worth knowing: `INT 21h AH=30h` reports DOS **4.00** (`AX=0x0004`) so that
modern tools take their DOS 4+ code path. Up to 0.8.0 it reported `2.10`
(`AX=0x0A02`) because the Compaq DOS 2.11 `SYS.COM` version gate rejects a minor
revision above 10 — claiming 4.00 costs exactly that: the 2.11 `SYS.COM` now
refuses to run. `src/uefidos_config.h` documents the two-line change that goes
back to the old behaviour (`UEFIDOS_DOSVER_MAJOR 2`, `..._MINOR 11`,
`..._COMPAT_MINOR 10`).

## Layout

```
src/
  uefi/main.c        entry point, boot banner, mount C:, publish the API table, run shell
  uefi/efi_plat.c    UEFI platform: ConOut/ConIn, BlockIo (all volumes), GOP
                     framebuffer, RTC, timer, serial diagnostics, shutdown
  plat.h             platform abstraction (also implemented for the Windows host)
  common/util.c      freestanding mem/str routines (no libc)
  common/divdi3.c    64-bit division helpers (needed for -m32 -nostdlib)
  common/cp437.c     CP437 -> Unicode table for the console mirror
common/font8x8.c   public-domain 8x8 font used for text in CGA graphics modes
  fs/fat.c|h         FAT12/16/32 driver over a block device (write paths for all
                     three) plus VFAT long-name parsing and creation
  kernel/dos.c|h     drives (multi-volume), path resolution incl. long names,
                     file handles, find/DTA, CONFIG.SYS, SHARE modes,
                     byte-range locks, read-only delete protection
  api/dosapi.c|h     native 32/64-bit DOS API (INT 21h parity table + exports)
  api/native.c|h     native PE loader: map, relocate, call uefidos_main() on the real CPU
  shell/shell.c|h    COMMAND.COM-like interpreter and batch execution
  emu/emu.c          8086 real-mode interpreter, PSP/IVT, .COM/.EXE loader, text
                     video mirror + graphics modes and the VGA DAC
  emu/int21.c        INT 21h / INT 10h / INT 13h / INT 16h / INT 1Ah services
  emu/fcb.c          DOS 4.0 FCB call family (AH=0Fh..28h)
  emu/int2f.c        INT 2Fh native multiplex (16-bit -> native upcall)
  emu/dpmi.c         DPMI 0.9 host: INT 2Fh/1687h, INT 31h, INT 60h entry
  emu/int33.c        INT 33h mouse driver (position, buttons, event callback)
  emu/itest.c        instruction-level test vectors (hosttest --itest)
  host/              Windows platform + selftest/shell/fatlist/phase46/fat16 harness
sdk/
  include/uefidos/   dosapi.h (public ABI), runtime.h (freestanding helpers)
  examples/hello64.c the demo native program (file I/O, find, clock, exec16)
  examples/filetool.c  reference utility: 32-bit file I/O, LFN, drives, exec16
  Makefile           builds both examples for both bitnesses with a mingw cross compiler
  build_sdk.ps1      the same build for Windows
tools/
  fat32.py           FAT32 formatter (no dependencies)
  make_gfx_test.py   assembles GFXTEST.COM / VESATEST.COM (graphics probes)
  gfx_shot.ps1       boot an image in QEMU and screenshot the framebuffer
  fat_extract.py     read-only FAT12/16/32 extractor for raw disk images
  make_image.py      image builder (boot files, text files, AUTOEXEC.BAT)
  make_hello.py      generates HELLO.COM / TELETYPE.COM / MZEXE.EXE / RUNNER.COM /
                     VRAM.COM / UPCALL.COM
  make_pm_tests.py   hand-assembles DPMITEST.COM (real -> PM16 -> PM32 -> reflection)
  make_fcb_test.py   hand-assembles FCBTEST.COM (FCB create/write/read/delete)
  make_dos4_test.py  hand-assembles DOS4TEST.COM (DOS 4.0 call surface, 30 checks)
  make_video_test.py hand-assembles CGATEST.COM (mode 4 stripes) and MOUSETST.COM (INT 33h)
  autobanch.asm      autobanch: 8086 benchmark .COM (as/ld/objcopy, see build.ps1)
make_cgatext.py    hand-assembles CGATEXT.COM (INT 10h text into the CGA bitmap)
  make_tools.py      generates the bundled utilities INFO/CAL/SUM/HEX/CLOCK
  mkfat16.py         sparse FAT16 formatter (32 MB .. 1.9 GB test volumes)
  extract_dos_file.py  pull one file out of a raw FAT image by cluster chain
  qemu_probe.py      drive the UEFI shell through the QEMU monitor
  qmon.py qtype.py screenshot.py ppm2png.py   QEMU monitor/keyboard/screenshot helpers
docs/                compatibility report, DOS 4.0 semantics, migration pitfalls,
                     protected-mode/DPMI notes, native SDK, instruction tests,
                     and the VESA/USB/LFN roadmap design
Makefile Dockerfile   one-command build for Linux/macOS/WSL (and in a container)
build.ps1 run_uefi.ps1 run_interactive.ps1 run_tests.ps1 start_qemu.bat
make_release.ps1     stage + zip the ready-to-run package (image + launcher)
start_prebuilt.bat   the launcher shipped as start.bat inside that package
```

## Design notes

- **No BIOS, no V86, no hardware virtualization.** 16-bit DOS software is
  interpreted by a software 8086 CPU over emulated RAM. That is portable to both
  x86_64 and IA32 UEFI (and to any host) and does not depend on VT-x/AMD-V.
- **Interrupt redirection.** Serviced vectors point at a per-interrupt trampoline
  `F000:E000+n*4` (`INT n; IRET`) that the emulator recognises as its own entry,
  so both `pushf; call far [vector]` and `int n` calling conventions behave like
  real BIOS, and programs that install their own handlers (`INT 21h AH=25h`)
  work normally.
- **Video.** The emulated 80×25 text screen lives at `B800:0000` like real
  hardware; writes mark it dirty and it is copied to the UEFI console at
  interrupt boundaries and during long runs.
- **EXEC (AH=4Bh).** A child loads into a fresh segment with its own PSP and runs
  on the same emulated CPU; the parent's registers, DTA and handle table are
  snapshotted and restored when the child terminates.
- **Segment caches and `op32`.** In a 32-bit code segment the default operand and
  address size is 32, and `0x66`/`0x67` switch *back* to 16 — every width
  decision goes through `op_is32()` / `ad_is32()` instead of the raw prefix flags.
- **FAT write paths** are shared by FAT12/16/32; a mounted floppy image owns a
  block-device shim whose writes are forwarded into the backing image file's
  cluster chain on `C:`, and the FAT sector cache is 8 slots deep so chain walks
  do not thrash (a 8-sector write went from 32.7 s to 113 ms in QEMU).
- **Path model**: DOS 8.3 only; `\`, `/`, `.`, `..` normalization, drive letters
  `A:`..`D:`, MBR partition probing with a superfloppy fallback.

## Notable bugs found and fixed

The Windows 1.01 bring-up (post-1.1.0) added six DOS structures and turned up
three more bugs:

- **`AH=25h` used the wrong register for the interrupt number.** The set-vector
  call takes the number in **`AL`**; the handler computed the IVT slot from
  `AH` and therefore wrote the vector into slot 37 whatever the caller asked
  for. Programs that save and restore vectors never noticed; programs that
  *install* handlers — Windows 1.x does it with `INT 22h`, then reads the vector
  back — died with `Incorrect DOS version`, which is where 1.01 had been stuck.
- **`AH=36h` reported zero free space on every drive.** It counted a cluster as
  free when its FAT link read `0FFFFFFFDh`, which never happens (a free cluster
  links to 0), stopped scanning after 30 000 clusters and hard-coded 512 bytes
  per sector, so `BX` always came back 0. Installers check that figure before
  every write — Windows 1.01's `SETUP` among them — and a zero reads as "disk
  full". It now returns the volume's own free-cluster count.
- **`AH=50h` (set current PSP) was a no-op.** It fell through to the "unknown
  call, report success" default, so `g_dos.psp_seg` stayed 0 and every internal
  structure that names the current PSP disagreed with the program.
- **Writing DOS-internal blocks through `emu_wb` breaks under DPMI.** The new
  SFT/SDA/JFT code first wrote them with `emu_wb(e, MAGIC_SEG, ...)`, which in
  protected mode resolves `0F000h` as a selector and latches a protection fault;
  `DPMITEST` lost its 32-bit half (`protected-mode interrupt 3 with no IDT
  gate`). All of it now writes by linear address, and the regression suite
  drives `AH=25h/34h/50h/51h/52h/5D06h` plus the `PSP:18h` job file table so the
  bug cannot come back.

The 1.0.0 batch came from running DOOM 0.99 (see `docs/doom.md`), whose DOS/16M
loader is a ruthless CPU/DOS conformance test:

The 1.0.0 batch came from running DOOM 0.99 (see `docs/doom.md`), whose DOS/16M
loader is a ruthless CPU/DOS conformance test:

- **`D6h` (SALC) was an unknown opcode.**  Windows 1.01's first module, loaded
  from `WIN100.OVL`, executes the undocumented 8086 `D6h`; the emulator stopped
  with `UEFI-DOS: unsupported 8086 opcode 0xD6`.  Implemented as `AL = CF ? 0FFh : 0`
  (no flags affected) with two new instruction-test vectors.
- **`POPF` discarded IOPL and NT** (mask `0x0FD5`, i.e. 8086/286 behaviour). The
  extender writes `FLAGS=0F000h` and reads it back to tell a 386 from a 286, so
  it printed `DOS/16M error: [15] protected mode available only with 386 or 486`
  on an emulator that supported protected mode. A 386/486 keeps bits 12-14
  writable and only bit 15 is reserved; the 16-bit `POPF` now masks `0x7FD5`.
- **`0F BA` was not implemented** (`BT`/`BTS`/`BTR`/`BTC` with an immediate bit
  index) — the very next instruction the loader ran. The emulator stopped with
  `unsupported 386 opcode 0F BA`.
- **the environment block had no program path.** DOS stores the executing
  program's full path at `env:2` after a string-count word, and every DOS
  extender reopens its own image through it. We wrote only `PATH=C:\`, so the
  loader opened `""` and then reported `not a DOS/16M executable 'DOOM.EXE'`.
  `build_psp()` now emits the real DOS layout including the DOS 5 "extra
  strings" trailer.

The 0.10.0 batch came from running SimEarth (see `docs/simearth.md`):

- **16-bit displacements were unsigned**: `mov [bp-4],ax` stored to `[bp+0FCh]`,
  so every Microsoft/Turbo C stack frame wrote its locals into the caller's
  frame — return addresses turned into `FFFFh` and control fled into the
  interrupt vector table. The 32-bit path always had the sign right.
- **`REP`'s repeat condition was inverted for `SCAS`/`CMPS`**: `REPZ` repeated
  while *different*, so `repne scasb` ("find the next zero byte") stopped on the
  first character, and EXEPACK's `repz scasb` backward scan stopped immediately.
- **real-mode addresses did not wrap at 1 MB** (A20 gate), so EXEPACK loaders
  read garbage through aliases like `FF69:FFF4` and reported "Packed file is
  corrupt". The gate is now a real state, controlled by ports `92h`/`64h` and
  `INT 15h AX=2400h..2402h`.
- **no DOS memory manager**: `AH=48h` always failed, which killed every program
  whose runtime allocates (Microsoft C aborts with `R6009 - not enough space for
  environment`). There is now an MCB arena spanning `[PSP, A000h)` with working
  `AH=48h/49h/4Ah/58h` and a first-MCB pointer in the list of lists.
- **`PSP:0005h` held a garbage far pointer**; MS C's runtime *calls* it to exit,
  so the program jumped into segment 0 and executed the IVT. The PSP is now
  complete (`INT 20h`, top of memory, `CALL FAR` to a terminate stub, parent
  PSP, job file table, FCB area).
- **the BIOS data area described no video adapter**, so installers detected an
  MDA card and refused to draw (SimEarth padded its text into `B000h`).
- **the MZ loader rejected images over 64 KB** — normal for large DOS programs.
- **`AH=47h` returned `C:\SIMEARTH`** instead of `\SIMEARTH`, so the program
  asked for `C:\C:\SIMEARTH\GR.OVL` and gave up on its overlays.
- **default `FILES=8`** left three file handles; raised to 20 (programs of that
  era expect the `FILES=20` their manuals recommend).

- `fat_read_chain_byte` double-counted the output offset and re-added the sector
  index when a read spanned more than one 512-byte chunk (multi-cluster
  `.COM`/`.EXE` files loaded shifted by one cluster).
- `INT 21h AH=35h` leaked the internal `F000:FFF0` service marker to programs
  (it must return a far-callable IRET stub).
- ALU direction bit (`0x02/0x03/0x0A/0x0B/…/0x2B/0x3B`): the register form stored
  the result to the *source* operand, so `sub bx,ax` left `BX` alone.
- Shift/rotate selector numbering in `do_shift`: group-2 opcodes were
  `0=SHL,1=SHR,2=SAR,3=ROL,4=ROR,…` instead of the architectural
  `0=ROL,1=ROR,2=RCL,3=RCR,4=SHL,5=SHR,6=SAL,7=SAR`, so `SHL AX,1` ran as
  `ROR AX,1` (this scrambled every game screen).
- FCB calls took their FCB from `ES:DI` instead of `DS:DX` and their I/O buffer
  from `DS:DX` instead of the DTA.
- `dos_fat_name()` dereferenced a NULL "ok" pointer; `dos_unlink()` did not
  reject read-only files.
- The FAT sector cache was a single slot, making chain walks re-read the FAT —
  32,720 ms for an 8-sector write versus 113 ms after the 8-slot cache.
- `INT 21h AH=3Ch` on an existing file looked up the directory entry **into the
  same struct it had just zeroed**, so `fat_find()` restored the old cluster and
  size: the file was left pointing at a cluster that had just been freed
  (`FAT[cluster] == 0`) and the next write reported "corrupt" (DOS error 0x1E).
  Fixed by looking the entry position up in a scratch dirent and shrinking the
  entry *before* releasing the chain. Found because the SDK demo creates its
  output file twice in a row.
- `AllocatePages`/`AllocatePool` were declared with a `UINT64` / `UINT32` memory
  type in `src/efi/efi.h`. `EFI_MEMORY_TYPE` is `UINTN`, so on IA32 the call
  pushed 8 bytes for a 4-byte parameter and *every* native image load failed with
  `native loader: out of memory` — while x86_64, where the widths happened to
  line up, worked. Fixed to `UINTN` plus an `EfiLoaderCode` pool fallback, and
  both firmwares now run their native program (verified in the IA32 smoke test).

**Found by the new instruction tester:** none yet — its first run reported four
failing cases, and all four were wrong expectations in the *tests* (`div` writes
the quotient to `AX`, `lodsb` preserves `AH`, a `mov [bx],ax` fixture that wrote
the same bytes it already contained). That is the intended outcome: the framework
is calibrated before the interpreter is blamed.
- `INT 21h AH=19h` (default drive) always returned `A:`, and `AH=0Eh` never
  actually *selected* a drive; `AH=2Ah` returned a hard-coded weekday of 1
  instead of computing it, so calendars and day-of-week logic were wrong.
  Found while writing the bundled `INFO`/`CAL` utilities.
- **Arrow keys and every other extended key were silently dropped** by both
  platform layers: `plat_kbd_poll()` returned the raw `EFI_INPUT_KEY.ScanCode`
  (or nothing at all when `UnicodeChar == 0`) and the host layer threw away the
  second byte of an `0x00`/`0xE0` sequence. Menus, editors and games that read
  the BIOS keyboard buffer never saw a single cursor key. EFI scan codes are now
  translated to PC scan-code set 1 and delivered as `0x100 | scan`, so they land
  in the BIOS buffer as "scan code, ASCII 0". Two related keyboard bugs went
  with it: `INT 16h AH=01h` consumed the key it was supposed to peek (so
  peek-then-read lost a keystroke and menus jumped two entries), and an
  interactive program could never be stopped because every delivered key refilled
  its instruction budget (the host harness now has `UEFIDOS_HARD_STEPS`).
- **`INT 10h AH=09h/0Ah` did not write video memory at all** — it pushed the
  characters onto the console stream and approximated the position with spaces
  and newlines. The screen mirror then redrew stale (blank) VRAM over it, so a
  program that paints its own screen came up black: the bundled 2.11 editor
  `TE.COM` "could not open a document" although it had read the file correctly.
  All text services (`AH=00h 02h 03h 06h 07h 08h 09h 0Ah 0Eh 13h`) now work on
  the emulated VRAM + BIOS data area cursor (`0040:0050`), with real window
  scrolling. The console is also switched to an 80×25 mode when the firmware
  offers one, so the guest screen fills the display instead of floating above a
  separate console region, and the mirror only redraws rows that changed.
- Opcodes `0x62/0x63/0x69/0x6B/0x6C-0x6F` (BOUND, ARPL, IMUL imm forms,
  INSB/W, OUTSB/W) were missing, which stopped the bundled `MINIGAME` collection
  dead (`unsupported 8086 opcode 0x6F`).

## Limitations / roadmap

- **No ring 3 yet**: DPMI clients still run at ring 0. No TSS, no `CR3` page
  tables or `#PF` dispatch; `AX=0600h` (paging) and `AX=0B00h` (hardware
  interrupts) are stubs.
- Long names: read, create, list and resolve are implemented; *renaming to* a
  long name and Unicode (non-ASCII) names are not (they degrade to `_`).
- Graphics: 8 bpp packed-pixel modes only (`13h`, `100h`, `101h`, `103h`). No
  16/24/32 bpp VESA modes, no 4-bit planar modes, no real vblank pacing, no
  hardware page flipping (`4F07h` is accepted and ignored). 320x200 is shown
  with square pixels rather than the classic 4:3 stretch.
- Drives: up to four (`DOS_MAX_DRIVES`), FAT only — no NTFS/exFAT, no optical
  media, and a 4096-byte-native-sector device is skipped (`BlockSize != 512`).
- `REN` works inside one directory; `COPY` has no wildcards; mounted `A:`/`B:`
  images have a fixed size.
- FAT32 `FSInfo` free-cluster count is not written back (the kernel keeps
  `free_count` in memory).
- TSR residency is not implemented (`INT 27h` only terminates).
- One file is limited to 32 MB per DOS 4.0 semantics (see
  `docs/dos4-semantics.md` §4.6 for the rationale).
- Roadmap: ring 3 + TSS/gates → `CR3` paging + `#PF` → DPMI 0600/0B00 → higher
  colour depths and VESA bank switching → rebuild tools from DOS 2.11-era
  sources → a wider compatibility matrix.

## License

MIT — see [`LICENSE`](LICENSE).

This repository ships **only** original source, generated test programs and
documentation. Microsoft MS-DOS / PC-DOS system files, commercial DOS
applications and games, OVMF/edk2 firmware and QEMU binaries are not
distributed here; obtain them yourself and keep them out of this repository.