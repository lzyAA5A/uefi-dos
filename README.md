# UEFI-DOS

**English** | [简体中文](README.zh-CN.md)

**DOS, rewritten for UEFI.** A small DOS-compatible operating environment that
boots as a UEFI application on x86_64 and IA32 firmware, mounts FAT32 itself,
runs a `COMMAND.COM`-like shell, and executes 16-bit real-mode DOS programs
through an internal 8086 interpreter — a "V86-like" software compatibility layer
with no BIOS and no virtualization support required.

The compatibility target starts at **MS-DOS 2.11** semantics (that is what
programs see) and then adds the things DOS 2.11 never had: FAT32, 64-bit UEFI
boot, FAT16 large volumes, DOS 4.0 file-sharing/locking/FCB semantics, a native
32/64-bit DOS API, and 386 protected mode with a DPMI 0.9 host.

Version 0.5.0 — 13 of 13 automated checks pass on the Windows host harness and
on QEMU + OVMF (x86_64 and IA32).

## Status

| Area | Status |
| --- | --- |
| UEFI boot, x86_64 | works — PE32+ `BOOTX64.EFI`, boots from the disk boot entry |
| UEFI boot, IA32 | works — PE32 `BOOTIA32.EFI`, same image holds both files |
| Boot volume discovery | LoadedImage → `DeviceHandle` → `BlockIo` → MBR partition (superfloppy fallback) |
| FAT32 | read + write (create/delete/rename/mkdir/rmdir, cluster alloc/free) |
| FAT12 / FAT16 | read + write |
| Floppy/disk image mount | `MOUNT A: IMAGE.IMG` exposes a file on `C:` as a read-write drive |
| Shell | `DIR CD MD RD DEL REN TYPE COPY CLS ECHO VER VOL DATE TIME PROMPT PATH SET MEM HELP APITEST MOUNT UNMOUNT CONFIG FCB EXIT SHUTDOWN`, batch files, `AUTOEXEC.BAT` |
| DOS kernel | 8.3 names, per-drive current directory, DOS error codes, **DOS 4.0 semantics** (see below) |
| 8086 emulator | 8086/80186 real-mode interpreter, 16 MiB emulated RAM, PSP, IVT, `.COM` + MZ `.EXE` loader with relocations, 8087 escapes treated as 8086 no-ops |
| INT 21h | console, file I/O, find first/next + DTA, dirs, date/time, version, vector get/set, IOCTL, dup, PSP, memory stubs, **EXEC (AH=4Bh)**, **AH=67h**, **AH=5Ch** byte locks, **SHARE modes** |
| FCB family | AH=0Fh 10h 11h 12h 13h 14h 15h 16h 17h 21h 22h 23h 27h 28h (`src/emu/fcb.c`) |
| INT 10h / 16h / 1Ah / 13h | teletype + cursor, blocking keyboard, BIOS tick counter, disk read/write/geometry |
| Video | direct writes to the emulated 80×25 text screen at `B800:0000` are mirrored to the UEFI console |
| Native 32/64-bit API | `g_dosapi` table — same operations and error codes as INT 21h, native pointers, one source for both firmware widths |
| 16-bit bridge | `exec16()` downcall (native → real mode) and `INT 2Fh AX=5544h` upcall (real mode → native) |
| 386 protected mode | real → protected transition, GDT/LDT parsing, hidden segment caches with limit checks, `MOV CRn`/`LGDT`/`LIDT`/`LLDT`/`CLTS`, 32-bit addressing (SIB), `0F xx` opcode set, protected-mode IDT delivery |
| DPMI 0.9 host | `INT 2Fh AX=1687h` bootstrap, `INT 31h` descriptors/DoS memory/real-mode reflection — see [`docs/protected-mode.md`](docs/protected-mode.md) |

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
APITEST: DOS version 2.10, native pointer size 8
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
  `INT 2Fh AX=5544h` into `src/emu/int2f.c` — `DX=1` returns `AX=0100h` and
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
| File size | kept `u32`, practical 32 MB ceiling per DOS 4.0 semantics (verified on a 1.9 GB FAT16 volume) |

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

Requirements:

- Windows with MSYS2 `mingw64` gcc — one x86_64 toolchain builds both the
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

## Regression suite

`run_tests.ps1` builds everything and runs 13 checks: FAT32 read/write selftest;
host shell DIR/TYPE/VER/COM/EXE/EXEC/video plus native-API and upcall demos;
host shell write path; floppy-image `A:` mount; FAT12 write on a 2.11 image;
FAT12 read of 2.11 and MS-DOS 5 images; two QEMU/OVMF boot smoke tests (x86_64
and IA32); phase 4/6 semantics with the dual-path comparison; FAT16 write on
32 MB and 1.9 GB volumes; and `FCBTEST.COM` exercising FCB calls from real
16-bit code.

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
| `UEFIDOS_MAX_STEPS=n` | instruction budget for one program (default 200000000) |
| `UEFIDOS_KBD=k1,k2,…` | scripted keys: single characters or `ENTER ESC SPACE TAB BACKSPACE UP DOWN LEFT RIGHT` |
| `UEFIDOS_KBD_MAX_POLLS=n` | polls after which an `ESC` is delivered so "wait for key" loops finish (default 100000) |
| `UEFIDOS_PSP_SEG=0x260` | load-segment override for programs linked for a fixed PSP |
| `UEFIDOS_DUMP_VRAM=1` | dump the emulated 80×25 screen to `build/vram_dump.txt` with a column ruler |
| `UEFIDOS_TRACE_INTS=1`, `UEFIDOS_TRACE_VRAM=n`, `UEFIDOS_TRACE_STEPS=n`, `UEFIDOS_TRACE_WINDOW=CS:LO-HI`, `UEFIDOS_TRACE_PORTS=1` | interrupt, video-write, single-step, address-window and port-I/O traces under `build/` |

The last 64 steps are dumped to `build/limit_dump.txt` when a program hits the
instruction limit.

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

Also worth knowing: `INT 21h AH=30h` reports DOS **2.10** (`AX=0x0A02`) because
2.11-era tools (`SYS.COM` among them) reject a minor revision above 10;
`VER` still says 2.11 because that is the semantics the kernel implements.

## Layout

```
src/
  uefi/main.c        entry point, boot banner, mount C:, publish the API table, run shell
  uefi/efi_plat.c    UEFI platform: ConOut/ConIn, BlockIo, RTC, timer, shutdown
  plat.h             platform abstraction (also implemented for the Windows host)
  common/util.c      freestanding mem/str routines (no libc)
  common/divdi3.c    64-bit division helpers (needed for -m32 -nostdlib)
  common/cp437.c     CP437 -> Unicode table for the console mirror
  fs/fat.c|h         FAT12/16/32 driver over a block device (write paths for all three)
  kernel/dos.c|h     drives, path resolution, file handles, find/DTA, CONFIG.SYS,
                     SHARE modes, byte-range locks, read-only delete protection
  api/dosapi.c|h     native 32/64-bit DOS API (INT 21h parity table + exports)
  shell/shell.c|h    COMMAND.COM-like interpreter and batch execution
  emu/emu.c          8086 real-mode interpreter, PSP/IVT, .COM/.EXE loader, video mirror
  emu/int21.c        INT 21h / INT 10h / INT 13h / INT 16h / INT 1Ah services
  emu/fcb.c          DOS 4.0 FCB call family (AH=0Fh..28h)
  emu/int2f.c        INT 2Fh native multiplex (16-bit -> native upcall)
  emu/dpmi.c         DPMI 0.9 host: INT 2Fh/1687h, INT 31h, INT 60h entry
  host/              Windows platform + selftest/shell/fatlist/phase46/fat16 harness
tools/
  fat32.py           FAT32 formatter (no dependencies)
  make_image.py      image builder (boot files, text files, AUTOEXEC.BAT)
  make_hello.py      generates HELLO.COM / TELETYPE.COM / MZEXE.EXE / RUNNER.COM /
                     VRAM.COM / UPCALL.COM
  make_pm_tests.py   hand-assembles DPMITEST.COM (real -> PM16 -> PM32 -> reflection)
  make_fcb_test.py   hand-assembles FCBTEST.COM (FCB create/write/read/delete)
  mkfat16.py         sparse FAT16 formatter (32 MB .. 1.9 GB test volumes)
  fat_extract.py     read-only FAT12/16/32 extractor (list/get/extract)
  extract_dos_file.py  pull one file out of a raw FAT image by cluster chain
  qemu_probe.py      drive the UEFI shell through the QEMU monitor
  qmon.py qtype.py screenshot.py ppm2png.py   QEMU monitor/keyboard/screenshot helpers
docs/                compatibility report, DOS 4.0 semantics, migration pitfalls,
                     protected-mode/DPMI notes
build.ps1 run_uefi.ps1 run_interactive.ps1 run_tests.ps1 start_qemu.bat
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

## Limitations / roadmap

- **No ring 3 yet**: DPMI clients still run at ring 0. No TSS, no `CR3` page
  tables or `#PF` dispatch; `AX=0600h` (paging) and `AX=0B00h` (hardware
  interrupts) are stubs.
- 8.3 names only (no LFN creation); text mode only (`INT 10h` graphics modes are
  not emulated).
- `REN` works inside one directory; `COPY` has no wildcards; mounted `A:`/`B:`
  images have a fixed size.
- FAT32 `FSInfo` free-cluster count is not written back (the kernel keeps
  `free_count` in memory).
- TSR residency is not implemented (`INT 27h` only terminates).
- One file is limited to 32 MB per DOS 4.0 semantics (see
  `docs/dos4-semantics.md` §4.6 for the rationale).
- Roadmap: ring 3 + TSS/gates → `CR3` paging + `#PF` → DPMI 0600/0B00 → LFN
  reads → graphics modes → rebuild tools from DOS 2.11-era sources → a wider
  compatibility matrix.

## License

MIT — see [`LICENSE`](LICENSE).

This repository ships **only** original source, generated test programs and
documentation. Microsoft MS-DOS / PC-DOS system files, commercial DOS
applications and games, OVMF/edk2 firmware and QEMU binaries are not
distributed here; obtain them yourself and keep them out of this repository.
