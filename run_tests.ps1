# UEFI-DOS automated regression tests.
# Builds everything, then runs the host harness and both QEMU/OVMF smoke tests.
#
# Checks that need a real Microsoft DOS floppy image are skipped unless you point
# UEFIDOS_DOS211 / UEFIDOS_DOS5 at one of your own copies; no such image is
# redistributed here.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root 'build'
$HostExe = Join-Path $Build 'hosttest.exe'
$Python = if ($env:UEFIDOS_PYTHON) { $env:UEFIDOS_PYTHON } else { 'python' }
$Dos211Image = $env:UEFIDOS_DOS211
$Dos5Image = $env:UEFIDOS_DOS5
$fail = 0
$pass = 0
$skip = 0

function Run-Check([string]$name, [scriptblock]$body, [string[]]$mustMatch) {
    Write-Host ""
    Write-Host "== $name =="
    $out = & $body 2>&1 | Out-String
    Write-Host $out
    $good = $true
    foreach ($m in $mustMatch) {
        if ($out -notmatch [regex]::Escape($m)) { $good = $false; Write-Host "missing: $m" }
    }
    if ($good) { $script:pass++; Write-Host "[PASS] $name" }
    else { $script:fail++; Write-Host "[FAIL] $name" }
}

function Skip-Check([string]$name, [string]$why) {
    $script:skip++
    Write-Host ""
    Write-Host "[SKIP] $name -- $why"
}

Write-Host "== build =="
$buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Root 'build.ps1'))
if ($Dos211Image) { $buildArgs += @('-Dos211Image', $Dos211Image) }
& powershell @buildArgs
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

# fresh copies so each test starts from the pristine image
Copy-Item (Join-Path $Build 'UEFIDOS.img') (Join-Path $Build 't_fat.img') -Force
Copy-Item (Join-Path $Build 'UEFIDOS.img') (Join-Path $Build 't_shell.img') -Force
Copy-Item (Join-Path $Build 'UEFIDOS.img') (Join-Path $Build 't_aw.img') -Force

Run-Check 'FAT32 read/write/create/mkdir/rename/delete selftest' {
    & $HostExe (Join-Path $Build 't_fat.img')
} @('SELFTEST PASSED')

Run-Check 'host shell: DIR/TYPE/VER/external COM/EXE/EXEC/VRAM/native API/upcall' {
    & $HostExe --shell (Join-Path $Build 't_shell.img') "DIR" "TYPE HELLO.TXT" "VER" "HELLO" "TELETYPE" "MZEXE" "RUNNER" "VRAM" "APITEST" "UPCALL" "DTATEST" "I2FTEST" "DPMITEST"
} @('EFI', '<DIR>', 'Hello UEFI-DOS!', 'UEFI-DOS Version', 'Hello from 8086 code!', 'TELETYPE OK!', 'Hello from MZ EXE!', 'RUNNER DONE!', 'VIDEO-OK!', 'APITEST: size', 'API-OK', 'UEFI-DOS native API v1.01', 'UPCALL-OK','DTA-OK','INT2F-OK','DPMI-HOST-OK','PM16-OK','PM32-DOS-OK','RMREFLECT-OK','DOSMEM-OK','REFLECTED-21H-OK','TICKS-OK', 'SHELLDONE')

Run-Check 'DOS: AH=47h cwd format (no drive, no leading backslash) and AH=57h timestamps' {
    & $HostExe --shell (Join-Path $Build 't_shell.img') "CWDCHK"
} @('CWDCHK-DONE', 'SUB    CF=0 AX=0100 [CWDTMP]', 'ROOT   CF=0 AX=0100 []', 'SUB-C:  CF=0 AX=0100 [CWDTMP]')

Run-Check 'host shell: MD/COPY/TYPE/DEL/RD write path' {
    & $HostExe --shell (Join-Path $Build 't_shell.img') "MD T2" "COPY HELLO.TXT T2\A.TXT" "TYPE T2\A.TXT" "DEL T2\A.TXT" "RD T2"
} @('1 file(s) copied', 'Hello UEFI-DOS!', 'SHELLDONE')

if ($Dos211Image -and (Test-Path $Dos211Image)) {
    Run-Check 'host shell: mount real MS-DOS 2.11 floppy image as A:' {
        & $HostExe --shell (Join-Path $Build 't_shell.img') "MOUNT A: DOS211.IMG" "DIR A:" "UNMOUNT A:"
    } @('Mounted A:', 'IBMBIO', 'COMMAND', 'Unmounted')

    Run-Check 'host shell: FAT12 write on 2.11 image (create/rename/delete/mkdir)' {
        & $HostExe --shell (Join-Path $Build 't_aw.img') "MOUNT A: DOS211.IMG" "COPY HELLO.TXT A:NEW.TXT" "TYPE A:NEW.TXT" "MD A:TMPD" "COPY HELLO.TXT A:TMPD\X.TXT" "REN A:TMPD\X.TXT Y.TXT" "TYPE A:TMPD\Y.TXT" "DEL A:TMPD\Y.TXT" "RD A:TMPD" "DEL A:NEW.TXT" "UNMOUNT A:"
    } @('Mounted A:', '1 file(s) copied', 'Hello UEFI-DOS!', 'Unmounted')

    Run-Check 'FAT12 read: MS-DOS 2.11 floppy image' {
        & $HostExe --fatlist $Dos211Image
    } @('type=FAT12', 'IBMBIO', 'COMMAND', 'MININASM')
} else {
    Skip-Check 'MS-DOS 2.11 floppy checks (mount / FAT12 write / FAT12 read)' 'UEFIDOS_DOS211 not set'
}

if ($Dos5Image -and (Test-Path $Dos5Image)) {
    Run-Check 'FAT12 read: MS-DOS 5 floppy image' {
        & $HostExe --fatlist $Dos5Image
    } @('type=FAT12', 'COMMAND', 'MSDOS')
} else {
    Skip-Check 'FAT12 read: MS-DOS 5 floppy image' 'UEFIDOS_DOS5 not set'
}

$qemuArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Root 'run_uefi.ps1'))
$floppyArg = @()
if (-not ($Dos211Image -and (Test-Path $Dos211Image))) { $floppyArg = @('-SkipFloppy') }

Run-Check 'QEMU+OVMF x86_64 boot smoke test' {
    & powershell @($qemuArgs + @('-Firmware', 'x64') + $floppyArg)
} @('smoke test: PASS')

Run-Check 'QEMU+OVMF IA32 boot smoke test' {
    & powershell @($qemuArgs + @('-Firmware', 'ia32') + $floppyArg)
} @('smoke test: PASS')

# ---- Phase 4: DOS 4.0 real-mode semantics + Phase 6.1 dual path ----
Copy-Item (Join-Path $Build 'UEFIDOS.img') (Join-Path $Build 't_p46.img') -Force
Run-Check 'phase 4/6: SHARE modes, FILES=, FCB calls, locks, dual path' {
    & $HostExe --phase46 (Join-Path $Build 't_p46.img')
} @('PHASE46 PASSED', 'FCB support OK', 'dual path OK', 'share semantics OK', 'FILES= handling OK')

# ---- Phase 4.4/4.5: FAT16 write path on a 32 MB volume ----
$f16 = Join-Path $Build 'fat16_32.img'
if (-not (Test-Path $f16)) {
    & $Python (Join-Path $Root 'tools/mkfat16.py') $f16 --size-mb 32 --spc 4 | Out-Null
}
Run-Check 'FAT16 create/write/delete/recreate (32 MB volume)' {
    & $HostExe --fat16 $f16 small
} @('FAT16 PASSED', 'created every test file', 'recreated the files after deletion')

# ---- Phase 4.4/4.6: 1.9 GB FAT16 volume and a 32 MB file ----
$f16b = Join-Path $Build 'fat16_2g.img'
if (-not (Test-Path $f16b)) {
    & $Python (Join-Path $Root 'tools/mkfat16.py') $f16b --size-mb 1900 --spc 64 --label FAT16BIG | Out-Null
}
Run-Check 'FAT16 1.9 GB volume: 32 MB file write + free' {
    & $HostExe --fat16 $f16b big
} @('FAT16 PASSED', 'wrote 33554432 bytes', 'directory entry reports the full 32 MB size')

# ---- FCB calls from real 16-bit code inside the emulator ----
Copy-Item (Join-Path $Build 'UEFIDOS.img') (Join-Path $Build 't_fcb.img') -Force
Run-Check 'FCBTEST.COM: FCB create/write/read/delete from 16-bit code' {
    & $HostExe --shell (Join-Path $Build 't_fcb.img') 'FCBTEST'
} @('FCB-OK')

# ---- DOS 4.0 real-mode surface: AH=32h/52h/59h/60h/63h/68h/6Ch + 5Ah ----
Copy-Item (Join-Path $Build 'UEFIDOS.img') (Join-Path $Build 't_dos4.img') -Force
Run-Check 'DOS4TEST.COM: DPB, list of lists, extended error, AH=6Ch/5Ah/60h' {
    & $HostExe --shell (Join-Path $Build 't_dos4.img') 'DOS4TEST'
} @('DOS version 4.00', 'version: OK', 'dpb-bps: OK', 'dpb-nfats: OK', 'lol-dpb: OK',
    'exterr-class: OK', '6c-create: OK', '6c-write: OK', '68h: OK', '6c-read: OK',
    '6c-trunc: OK', '6c-exists: OK', '6c-nofile: OK', '5a-create: OK', '5a-find: OK',
    '5a-del: OK', '60h-drive: OK', '60h-root: OK', 'C:\hello.txt', '63h-dbcs: OK',
    'DOS4-OK')

# ---- bundled utilities (INFO / CAL / SUM / HEX) ----
# CAL's month name is cross-checked against the host clock, which also proves
# the INT 21h AH=2Ah wiring end to end.
$monthName = (Get-Date).ToString('MMMM', [System.Globalization.CultureInfo]::InvariantCulture)
Copy-Item (Join-Path $Build 'UEFIDOS.img') (Join-Path $Build 't_tools.img') -Force
Run-Check 'bundled tools: INFO panel (version/date/time/drive/dir/native API)' {
    & $HostExe --shell (Join-Path $Build 't_tools.img') 'INFO'
} @('=== UEFI-DOS system info ===', 'DOS version : 4.00', 'Native API  : 01/64-bit pointers')

Run-Check 'bundled tools: CAL prints the current month grid' {
    & $HostExe --shell (Join-Path $Build 't_tools.img') 'CAL'
} @('Su Mo Tu We Th Fr Sa', $monthName)

Run-Check 'bundled tools: SUM checksum + HEX dump of a file (INT 21h handles)' {
    & $HostExe --shell (Join-Path $Build 't_tools.img') 'SUM HELLO.TXT' 'HEX HELLO.TXT'
} @('bytes, checksum', 'Hello UEFI-DOS!.')

# Extended keys (arrows, F-keys) must reach the guest as PC scan codes with
# ASCII 0; they used to be dropped by the platform layer, so menus, editors and
# games ignored the cursor keys entirely.
$env:UEFIDOS_KBD = 'DOWN,UP,RIGHT,LEFT,ESC'
$env:UEFIDOS_KBD_MAX_POLLS = '20000'
# DOS fills the child PSP tail at 0080h: the count byte excludes the trailing
# CR (a bare command line is count 0), and bytes past it are the caller's tail
# buffer verbatim.  WIN.COM of real-mode Windows 1.x depends on both.
Run-Check 'DOS: command tail is built like DOS (count excludes CR)' {
    & $HostExe --shell (Join-Path $Build 't_tools.img') 'TAILCHK' 'TAILCHK ABC'
} @('COUNT=00 C81=0D', 'COUNT=04 C81=20', 'TAILCHK-DONE')

Run-Check 'keyboard: arrow keys arrive as BIOS scan codes (INT 16h)' {
    & $HostExe --shell (Join-Path $Build 't_tools.img') 'KEYTEST'
} @('S=50 A=00', 'S=48 A=00', 'S=4D A=00', 'S=4B A=00', 'S=01 A=1B', 'KEYTEST-DONE')
Remove-Item Env:UEFIDOS_KBD -ErrorAction SilentlyContinue
Remove-Item Env:UEFIDOS_KBD_MAX_POLLS -ErrorAction SilentlyContinue


# Multi-volume support: every block device the platform reports becomes a drive
# letter, so a second disk (or a USB stick) shows up as D:.  The host platform
# fakes that with UEFIDOS_EXTRA_DRIVES; QEMU proves it against real firmware.
$env:UEFIDOS_EXTRA_DRIVES = (Resolve-Path (Join-Path $Build 'SECOND.img')).Path
Run-Check 'drives: a second UEFI volume is mounted as D:' {
    & $HostExe --shell (Join-Path $Build 't_tools.img') 'DRIVES' 'VOL D:' 'TYPE D:\SECOND.TXT'
} @('C: >FAT32', 'D:  FAT32', 'Volume in drive D is', 'this volume is D:, not C:')
Remove-Item Env:UEFIDOS_EXTRA_DRIVES -ErrorAction SilentlyContinue

# Long file names: the 64-bit native API creates "Long Name Report.txt" (VFAT LFN
# + generated 8.3 alias), DIR shows the long name, TYPE opens it by that name,
# and the longname() call maps the alias back.
Run-Check 'FAT: long file names (create via native API, DIR/TYPE by long name)' {
    & $HostExe --shell (Join-Path $Build 'UEFIDOS.img') 'RUN64 HELLO64.EXE' 'DIR *.TXT' 'TYPE Long Name Report.txt'
} @('long name    : created "Long Name Report.txt"', 'long read    : this file has a VFAT long name',
    '8.3 alias    : LONGNA~1.TXT -> Long Name Report.txt', 'Long Name Report.txt',
    'this file has a VFAT long name')

# CGA: INT 10h mode 4 keeps the bitmap at B800 in the even/odd interleaved
# layout, so the platform layer only ever sees an expanded 8bpp frame.
Run-Check 'graphics: CGA mode 4 (320x200x4) paints the interleaved bitmap' {
    $env:UEFIDOS_GFX_DUMP = (Join-Path $Build 't_cga.ppm')
    Remove-Item $env:UEFIDOS_GFX_DUMP -ErrorAction SilentlyContinue
    & $HostExe --shell (Join-Path $Build 'UEFIDOS.img') 'CGATEST'
    $sz = 0
    if (Test-Path $env:UEFIDOS_GFX_DUMP) { $sz = (Get-Item $env:UEFIDOS_GFX_DUMP).Length }
    Write-Host "CGA framebuffer dump: $sz bytes"
} @('gfx: mode set 320x200x8', 'gfx: 320x200 framebuffer ->', 'CGA-OK 320x200x4', 'CGA-TEXT-OK')

# INT 33h: the driver must report a present mouse, clamp/track the host pointer
# and expose the motion counters (UEFIDOS_MOUSE scripts the pointer for testing).
# The firmware side of the graphics path is what actually reaches the screen.
# GCC deletes it outright when it can prove g_gop is NULL (that shipped once as a
# #UD inside OVMF), so the built EFI must still contain real function bodies.
Run-Check 'firmware: GOP graphics code is present in the built EFI (not dead-code)' {
    & $Python (Join-Path $Root 'tools/check_efi_symbols.py') (Join-Path $Build 'BOOTX64.EFI') `
        'plat_video_gfx_begin=64' 'plat_video_gfx_show=256' 'plat_video_text=16'
} @('EFI SYMBOLS OK', 'plat_video_gfx_show:')

Run-Check 'mouse: INT 33h reset/position/motion counters' {
    $env:UEFIDOS_MOUSE = '40,20,0;0,0,1;0,0,0'
    & $HostExe --shell (Join-Path $Build 'UEFIDOS.img') 'MOUSETST'
    Remove-Item Env:UEFIDOS_MOUSE -ErrorAction SilentlyContinue
} @('MOUSE-OK', ' reset=AX FFFF', 'x=0168', 'y=0078', 'motion=0028,0014')

# Graphics: INT 10h mode 13h and the VESA VBE calls must switch the display to a
# pixel framebuffer whose contents we can dump (the host platform writes a PPM;
# under QEMU the same code writes the GOP framebuffer directly).
$env:UEFIDOS_KBD = 'SPACE'
$env:UEFIDOS_HARD_STEPS = '80000000'
Run-Check 'graphics: INT 10h mode 13h paints the 320x200 framebuffer' {
    $env:UEFIDOS_GFX_DUMP = (Join-Path $Build 't_gfx13.ppm')
    Remove-Item $env:UEFIDOS_GFX_DUMP -ErrorAction SilentlyContinue
    & $HostExe --shell (Join-Path $Build 'UEFIDOS.img') 'GFXTEST'
    $sz = 0
    if (Test-Path $env:UEFIDOS_GFX_DUMP) { $sz = (Get-Item $env:UEFIDOS_GFX_DUMP).Length }
    Write-Host "framebuffer dump: $sz bytes"
} @('gfx: mode set 320x200x8', 'gfx: 320x200 framebuffer ->', 'GFX-OK 320x200x256', 'GFX-TEXT-OK')

Run-Check 'graphics: VESA VBE 4F00/4F01/4F02 sets 640x480x256' {
    $env:UEFIDOS_GFX_DUMP = (Join-Path $Build 't_vesa.ppm')
    Remove-Item $env:UEFIDOS_GFX_DUMP -ErrorAction SilentlyContinue
    & $HostExe --shell (Join-Path $Build 'UEFIDOS.img') 'VESATEST'
    $sz = 0
    if (Test-Path $env:UEFIDOS_GFX_DUMP) { $sz = (Get-Item $env:UEFIDOS_GFX_DUMP).Length }
    Write-Host "framebuffer dump: $sz bytes"
} @('VESA-OK', 'gfx: mode set 640x480x8', 'gfx: 640x480 framebuffer ->', 'VESA-MODE-OK 640x480x256', 'VESA-TEXT-OK')
Remove-Item Env:UEFIDOS_GFX_DUMP -ErrorAction SilentlyContinue
Remove-Item Env:UEFIDOS_KBD -ErrorAction SilentlyContinue
Remove-Item Env:UEFIDOS_HARD_STEPS -ErrorAction SilentlyContinue

# AH=3Ch on an existing file must truncate it, not corrupt its chain: create a
# file, write it, delete the content by recreating it, write again, read back.
Run-Check 'FAT: create-over-existing truncates (AH=3Ch) and keeps the chain valid' {
    & $HostExe --shell (Join-Path $Build 't_tools.img') 'COPY HELLO.TXT TRUNC.TXT' 'DEL TRUNC.TXT' 'COPY HELLO.TXT TRUNC.TXT' 'TYPE TRUNC.TXT' 'SUM TRUNC.TXT'
} @('Hello UEFI-DOS!', 'TRUNC.TXT: 16 bytes')

# Instruction-level emulator tests (emu/itest.c): every opcode we care about is
# a byte stream plus a register/memory expectation, so a decoder regression is
# caught here instead of as "a game renders garbage".
Run-Check 'emulator: 8086 instruction test vectors (ITEST)' {
    & $HostExe --itest
} @('ITEST PASSED', 'itest: 64 passed, 0 failed')

# Native 64-bit SDK program: a PE image built by sdk/ is mapped, relocated and
# executed on the real CPU, with the DOS API and a 16-bit downcall inside.
# run it twice on the same image: the second run recreates NATIVE.TXT, i.e. it
# goes through INT 21h AH=3Ch truncation of an existing file.
Run-Check 'native SDK: RUN64 runs a 64-bit PE program (dosapi + exec16)' {
    & $HostExe --shell (Join-Path $Build 't_tools.img') 'RUN64 HELLO64.EXE' 'RUN64 HELLO64.EXE'
} @('RUN64: loading HELLO64.EXE', 'pointer size : 64 bit', 'DOS version  : 4.00',
    'reading      : HELLO.TXT', 'wrote        : NATIVE.TXT', 'exec16       : HELLO.COM -> exit 0',
    'NATIVE-OK', 'exit code 0')

# Native SDK file tool: whole-file write/read with 32-bit sizes, directory
# listing with long names, drive/FS table and a 16-bit downcall, all driven by
# the command tail (RUN64 FILET64.EXE -w ... 3000).
Run-Check 'native SDK: FILETOOL writes/verifies a 3000-byte file and lists drives' {
    & $HostExe --shell (Join-Path $Build 't_tools.img') 'RUN64 FILET64.EXE -w NATBIG.TXT 3000' 'RUN64 FILET64.EXE -s NATBIG.TXT' 'RUN64 FILET64.EXE -l *.TXT' 'DEL NATBIG.TXT'
} @('wrote 3000 bytes to NATBIG.TXT', 'read back 3000 bytes, mismatches: 0', 'FILEIO-OK',
    'size  : 3000 bytes', 'HELLO.TXT', 'FILETOOL-DONE')

# Brute-force opcode audit: every first-byte opcode (and every 0F second
# byte) is fed into a fresh emulator.  The 8086 set must be complete apart
# from the three documented holes: 0F (the 386 escape), D6 (SALC) and F1
# (ICEBP) -- both undocumented on the 8086 and unused by DOS software.
Run-Check 'emulator: brute-force opcode audit (IFUZZ)' {
    & $HostExe --ifuzz
} @('IFUZZ DONE', 'ifuzz: 8086 opcodes missing: 0F D6 (2 total)')

Write-Host ""
Write-Host "=============================="
if ($skip -gt 0) { Write-Host "RESULTS: $pass passed, $fail failed, $skip skipped" }
else { Write-Host "RESULTS: $pass passed, $fail failed" }
Write-Host "=============================="
if ($fail -ne 0) { exit 1 }
exit 0
