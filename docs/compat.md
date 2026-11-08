# Compatibility report: CardPuter86 DOS programs on UEFI-DOS

The programs in this report are a curated CardPuter86 DOS software set (games,
a terminal, an assembler, a hardware diagnostic). They are **third-party work
and are not distributed with this repository**; the sweep was run against disk
images supplied by the author and extracted read-only with
`tools/fat_extract.py`. To reproduce it, put the same images in place yourself
and extract them onto a test image.

Two source disks were used:

* a 1.44 MB FAT12 MS-DOS 5.0 system disk carrying the games (`MINE.EXE`,
  `SNAKE.EXE`, `G2048.EXE`, `MINIGAME.EXE`), `DIAG.COM`, `SCRDEMO.EXE`,
  `CPPTEST.EXE`, `HELLO.COM`, `VID.COM`, `TE.EXE`, `README.TXT`
* a 360 KB FAT12 Compaq DOS 2.11 disk with `MININASM.COM`, `ASM.EXE`,
  `TE.COM`, `TERM.COM`, `CP86TEST.COM`, `SB.COM`, `HI.EXE`, `VID2.EXE`,
  `DEMO.ASM`, `TOOLS.TXT`

23 files were copied onto the test image (extracted copies live in
`build/imported/`), so they can be started from the `C:\>` prompt exactly like
on the original machine.

## How the tests were run

Programs were started through the host harness
(`build\hosttest.exe --shell build\t_compat.img <PROG>`), i.e. through the
same kernel, emulator and FAT driver the UEFI build uses, with

* `UEFIDOS_MAX_STEPS` – instruction budget, so a spin loop cannot hang the run
* `UEFIDOS_KBD` / `UEFIDOS_KBD_MAX_POLLS` – scripted key injection and a
  deadline after which an `ESC` is delivered so "press any key" loops finish
* `UEFIDOS_DUMP_VRAM` – writes the emulated 80×25 text screen to
  `build/vram_dump.txt` with a column ruler, for exact comparison

The reference screen came from real MS-DOS 5 running in QEMU: booting the MS-DOS
5 system disk as a floppy, typing `mine` through the QEMU monitor and saving a
screenshot with `tools/screenshot.py` + `tools/ppm2png.py`. Those screenshots
are not part of this repository because they show third-party programs.

## Results

| Program | Type | Status | Notes |
| --- | --- | --- | --- |
| `MINE.EXE` | game | **runs** | title bar ` MINE   MINES 0 TIME 0`, 12×8 mine field, help lines; responds to WASD/keys |
| `G2048.EXE` | game | **runs** | `2048  SCORE 0   BEST 0` header and the 4×4 tile grid |
| `SNAKE.EXE` | game | **runs** | ` SNAKE  SCORE 0   LEN 0`, field, `***O` body, `$` food |
| `MINIGAME.EXE` | game pack | **runs** | menu (SNAKE / MINESWEEPER / GOMOKU / TETRIS / PAC-MAN / DINO RUN / MAZE) |
| `SCRDEMO.EXE` | C++ demo | **runs** | framed screen, `CardPuter86 C++ demo`, `class Screen -> direct VRAM 0xB8000` |
| `VID2.EXE` | demo | **runs** | diagonal line drawn char-by-char |
| `SB.COM` | music | **runs** | jukebox screen (System Beeps / Astro Force / …); PC speaker writes are accepted but silent |
| `CP86TEST.COM` | diagnostics | **passes** | RTC/DOS time, BIOS tick counter, INT 13h probe on DL=00/01/80/81, keyboard ASCII read, speaker, COM1 probe (`missing`), USB note |
| `TERM.COM` | terminal | **runs** | banner, waits for a real COM1 device, exits on key |
| `TE` (`TE.COM`, `TE.EXE`) | editor | **runs** | usage line `Usage: TE FILE.TXT` / `Max 40x14 chars` |
| `VID.COM`, `DIAG.COM`, `CPPTEST.EXE`, `HI.EXE`, `MININASM.COM`, `ASM.EXE` | tools | runs, no visible output | see "Open points" |

`MININASM.COM`, `ASM.EXE`, `C86HELLO.COM` and `CPPTEST.EXE` need arguments
(`MININASM DEMO.ASM`, `ASM DEMO.ASM`); `ASM.EXE` then enters its own loop and
is killed by the test timeout. They are listed as open points, not as
working.

## Verified on the real UEFI path

The same programs were booted through `BOOTX64.EFI` under OVMF/QEMU with a
purpose-built image whose `AUTOEXEC.BAT` starts the game directly (the boot was
driven through the QEMU monitor — see `tools/qemu_probe.py`, `tools/qtype.py`,
`tools/screenshot.py`). The resulting screenshots are not distributed here
because they show third-party programs; what they confirmed was:

* `SNAKE` running in UEFI-DOS: header, CP437 box-drawing frame, `***O` body and
  `$` food all present.
* `MINE` running in UEFI-DOS with the counters `MINES 0 TIME 0` and the 12×8
  mine field, matching the reference layout from real MS-DOS 5 in QEMU.

Counting that exposed one more missing service: **`INT 1Ah` (BIOS timer)**.
Games read the tick counter through `INT 1Ah AH=00` and by polling
`0040:006C`; neither existed, so `MINE` displayed `MINES 1992 TIME 65798144`.
`int1a_handler` now implements `AH=00` (read ticks), `AH=01` (set ticks),
`AH=02` (RTC time, BCD) and `AH=04` (RTC date, BCD), and the emulator keeps
`0040:006C` live from the platform clock, which makes the counters sane in
both the host harness and QEMU.

An interactive program that is receiving keys is also no longer killed by the
instruction budget: the budget is refilled whenever a key is delivered, so a
game can sit waiting for input indefinitely while a genuinely stuck program
still hits the limit after 200M instructions.

## Emulator bugs this exposed (all fixed)

The games and diagnostics are far heavier users of the 8086 instruction set
than the previous test programs, and they found four real defects:

1. **ALU direction bit** (`src/emu/emu.c`, opcodes `0x02/0x03/0x0A/0x0B/…
   0x2B/0x3B`): when the register is the destination the result was written
   back to the *source* operand. `sub bx,ax` left `BX` untouched and put the
   difference in `AX`, which corrupted every pointer computation.
2. **Shift/rotate decoding** (`do_shift`): the group-2 selector was numbered
   `0=SHL,1=SHR,2=SAR,3=ROL,4=ROR,5=RCL,…` instead of the architectural
   `0=ROL,1=ROR,2=RCL,3=RCR,4=SHL,5=SHR,7=SAR`, so `SHL AX,1` (`D1 E0`,
   `/4`) executed as `ROR AX,1`. `(y*80+x)<<1` became `ROR 1`, which is why
   every character landed one column off and overlapped its attribute byte.
3. **BIOS keyboard buffer** (`0040:001A` head, `0040:001C` tail,
   `0040:001E` data): Digital Mars' `kbhit()` polls these variables instead
   of calling INT 16h, so a program waiting for a key spun forever. The
   emulator now keeps a real 15-entry ring buffer, and INT 21h/INT 16h read
   through the same buffer.
4. **Interrupt vectors must be callable and end in IRET**: programs fetch a
   vector with `INT 21h AH=35h` and then either `pushf; call far` or use the
   `push flags; iret` trick. Serviced vectors now point at a per-interrupt
   trampoline `F000:E000+n*4` holding `INT n; IRET`, which the emulator
   recognises as its own entry point, so both calling conventions behave
   like real BIOS.

5. **INT 1Ah BIOS timer** was missing (see above), which made game timers
   and RNG seeds garbage.

Additionally the emulator now answers the video-retrace polling port
(`IN 0x3DA` alternates) and keeps a minimal CRTC register set, so text-mode
code that waits for vertical retrace no longer hangs.

## Open points

* `C86HELLO.COM` (and other Digital Mars COMs built for the CardPuter86
  DOS) contain an absolute `mov ax,0x0273; mov ds,ax` startup, i.e. they
  assume they are loaded at PSP `0x0263`. UEFI-DOS loads shell programs at
  PSP `0x0D00` (like a normal DOS machine with COMMAND.COM resident low in
  memory), so those binaries bail out immediately. They do run with
  `UEFIDOS_PSP_SEG=0x0263` in the host harness; a proper fix is to allocate
  the PSP like DOS does (lowest free segment) and to place the environment
  block above the loaded image.
* A handful of characters in the help lines of `MINE`/`SB` are occasionally
  wrong (e.g. `Use WASD / Fn-arrows` renders as `Use WASDP  Fn-P rows`).
  The screens are otherwise correct, so this is a remaining single-character
  path (likely a stale attribute/character write) rather than a layout bug.
* Graphics modes (INT 10h `AH=00` mode 4/6/12/13 and `A000:0000`) are still
  not emulated; only the text screen at `B800:0000` is mirrored.
* `ASM.EXE` with an argument loops; `MININASM`, `DIAG`, `CPPTEST`, `HI`,
  `VID` exit or idle without visible output and need a closer look.
