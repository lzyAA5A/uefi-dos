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
} @('EFI', '<DIR>', 'Hello UEFI-DOS!', 'UEFI-DOS Version', 'Hello from 8086 code!', 'TELETYPE OK!', 'Hello from MZ EXE!', 'RUNNER DONE!', 'VIDEO-OK!', 'APITEST: size', 'API-OK', 'UEFI-DOS native API v1.00', 'UPCALL-OK','DTA-OK','INT2F-OK','DPMI-HOST-OK','PM16-OK','PM32-DOS-OK','RMREFLECT-OK','DOSMEM-OK','REFLECTED-21H-OK','TICKS-OK', 'SHELLDONE')

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


Write-Host ""
Write-Host "=============================="
if ($skip -gt 0) { Write-Host "RESULTS: $pass passed, $fail failed, $skip skipped" }
else { Write-Host "RESULTS: $pass passed, $fail failed" }
Write-Host "=============================="
if ($fail -ne 0) { exit 1 }
exit 0
