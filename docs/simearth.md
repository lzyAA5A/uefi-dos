# SimEarth 1.5 (Maxis, 1993) on UEFI-DOS — case study

The point of this document is not "a game runs".  It is the list of assumptions
that a real 1993 program makes about a PC, and which UEFI-DOS got wrong until
somebody actually tried to run one.  Every item below was found by running
`INSTALL.EXE` and `SIMEARTH.EXE` from the original 3.5" disks and watching the
program die.

Software: **SimEarth 1.5** (1993), Maxis; 2 × 1.44 MB FAT12 install disks
(`DISK01.IMG` with `INSTALL.EXE` + `SIMEARTH.DAT`, `DISK02.IMG` with the video
drivers).  `INSTALL.EXE` is EXEPACK-packed Microsoft C 6.0 code; `SIMEARTH.EXE`
is a 125,587-byte MZ executable that unpacks its four overlays
(`GLOBE.OVL`, `GR.OVL`, `MAP.OVL`, `SIM.OVL`) at install time.

Result: the installer runs to `SIMEARTH V1.4 Installation complete`, and the
game then runs in 320×200×256 (mode 13h) with its terrain map and control
panels drawn (`build/simearth.png`).

## The bugs it found

| # | Symptom | Cause | Fix |
|---|---------|-------|-----|
| 1 | `INSTALL.EXE` printed `Packed file is corrupt` | real-mode addresses did not wrap at 1 MB.  EXEPACK's unpacker reads its stream through pairs like `FF69:FFF4`, which only alias the intended `0F684` when address bit 20 is masked | `lin_of()` masks real-mode linear addresses to 20 bits unless the A20 gate is enabled; ports `92h`/`64h` and `INT 15h AX=2400h..2402h` control the gate |
| 2 | `run-time error R6009 - not enough space for environment` | `INT 21h AH=48h` always failed: UEFI-DOS had no memory manager at all | a real MCB arena: the program's block spans `[PSP, A000h)`, `AH=48h/49h/4Ah/58h` allocate/free/resize it, `AH=52h` exposes the first MCB |
| 3 | the installer drew into `B000h` and refused graphics ("MDA system detected") | the BIOS data area was almost empty — no equipment word, no CRTC port, no character height, so the adapter probe read "mono, no colour BIOS" | `install_ivt()` now writes the video fields a POST leaves on a VGA machine (`40:10h`, `40:49h..40:66h`, `40:84h..40:87h`) |
| 4 | after startup the program jumped to `0000:F01E` and executed the interrupt vector table | `PSP:0005h` — the "CP/M entry point" that Microsoft C's runtime *calls* to exit — held a garbage far pointer (`9A 1E F0 00 00`) | `build_psp()` writes a complete DOS PSP: `INT 20h` at 0, top of memory, `CALL FAR F000:FFD0` (a `CD 20` terminate stub), parent PSP, 20-byte job file table, JFT size/pointer, SFT, FCB area |
| 5 | `REPZ SCASB` stopped on the first compare even when the byte matched | `REP` repeat condition was inverted for `SCAS`/`CMPS` | repeat while `ZF` set for `REPE/REPZ`, while `ZF` clear for `REPNE` |
| 6 | **everything corrupted its own stack** — variable writes landed in the caller's frame, return addresses became `FFFFh` | the 16-bit address decoder zero-extended an 8-bit displacement, so `mov [bp-4],ax` stored to `[bp+0FCh]`.  Every Microsoft/Turbo C stack frame is built out of negative displacements | `off + (u16)(s16)(s8)disp8` |
| 7 | `SIMEARTH.EXE: program too large or bad format` | the MZ loader refused any image larger than 64 KB | an EXE image may span segments; the check is now "does it fit in the program's memory block" |
| 8 | game asked `Enter directory for GR.OVL` although the file was in the current directory | `INT 21h AH=47h` returned `C:\SIMEARTH` (with the drive); the game prepends the drive itself and asked for `C:\C:\SIMEARTH\GR.OVL` | `AH=47h` returns `\SIMEARTH` (no drive letter, leading backslash, empty string in the root) |
| 9 | `FATAL ERROR: Index file missing` after opening `SHARED.DAT` | `FILES=8` — the MS-DOS default — leaves a program three file handles, and the game keeps a data file, its index, an overlay and a driver open at once | default `FILES=20` (what the manuals told users to put in `CONFIG.SYS`); `CONFIG.SYS FILES=` still overrides |
| 10 | `Could not find fonts` in `/d2` mode | the installer had only copied the EGA driver set; the 256-colour mode needs the `TDYGAIA.*` set as well | documented: install with all data disks, or copy the `.IA`/`.NDX` sets for every mode you want |

Bugs 5 and 6 are the general ones: #6 broke *any* compiler-generated 16-bit
code, and #1/#2/#3/#4/#8 are what every installer of that era needs in order to
run at all.  All ten are now covered by `docs/compat.md`, `docs/dos4-semantics.md`
and the instruction tests (`hosttest.exe --itest`).

## Reproducing it

```powershell
# build a 1.44 MB floppy image holding both install disks (needs UEFI-DOS tools)
python tools/mkfat12.py install.img --dir extracted-disk-files

# an empty FAT32 volume with a CONFIG.SYS, plus the floppy image on it
python tools/make_image.py build\simtest.img --size-mb 64 --noboot `
    --file install.img=INSTALL.IMG --text "CONFIG.SYS=FILES=40`nLASTDRIVE=D`n"

# drive the installer: accept A: as the source, C:\SimEarth as the target
$env:UEFIDOS_KBD='ENTER,ENTER,ENTER,ENTER,ENTER,ENTER,SPACE,SPACE'
$env:UEFIDOS_KBD_GUARD='ENTER'          # the key an unscripted wait receives
build\hosttest.exe --shell build\simtest.img "MOUNT A: INSTALL.IMG" "A:INSTALL.EXE"

# the game itself, 256-colour mode
$env:UEFIDOS_GFX_DUMP='build\simearth.ppm'
build\hosttest.exe --shell build\simtest.img "CD SIMEARTH" "SIMEARTH /d2"
python tools\ppm2png.py build\simearth.ppm build\simearth.png
```

Video modes: `SIMEARTH /dV` (VGA 640×480×16), `/dE`, `/de`, `/d2`
(MCGA 320×200×256 — the one UEFI-DOS renders natively today), `/dT`, `/dM`,
`/dm`, `/dH`; see the game's `README`.

## What still limits it

* Modes other than 320×200×256 and the VESA modes need EGA/VGA **planar**
  16-colour emulation (`INT 10h` mode 12h/10h), which UEFI-DOS does not have
  yet — that is why `/d2` is the mode to use.
* SimEarth checks for EMS (`EMMXXXX0`) and runs fine without it, using its
  overlays instead.
* The harness prints `[open-fail] …` lines for the probes a program is expected
  to fail (an EMS driver, `LANGUAGE.DAT`, its own `.COM` when looking for the
  `.EXE`); they are only informational.
