# UEFI-DOS interactive QEMU launcher (double-click friendly through start_qemu.bat).
#
# Builds a demo FAT32 image whose AUTOEXEC.BAT runs the smoke-test commands and
# then STOPS at the "C:\>" shell prompt (no auto SHUTDOWN), so you can type
# commands yourself. Then boots it in a QEMU window with OVMF (edk2).
#
#   run_interactive.ps1               # window mode (gtk/sdl), boot to C:\> prompt
#   run_interactive.ps1 -Headless     # no window, serial log only
#   run_interactive.ps1 -Firmware ia32
param(
    [switch]$Headless,
    [int]$MemoryMB = 256,
    [ValidateSet('x64','ia32')][string]$Firmware = 'x64',
    [int]$TimeoutSec = 45          # headless only: auto-kill after this many seconds
)
$ErrorActionPreference = 'Stop'
$Root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root 'build'
$Qemu  = if ($env:UEFIDOS_QEMU) { $env:UEFIDOS_QEMU } else { 'C:\Program Files\qemu\qemu-system-x86_64.exe' }
$FwDir = if ($env:UEFIDOS_OVMF_DIR) { $env:UEFIDOS_OVMF_DIR } else { 'C:\Program Files\qemu\share' }
$Python = if ($env:UEFIDOS_PYTHON) { $env:UEFIDOS_PYTHON } else { 'python' }
if (-not (Test-Path $Qemu)) { throw "qemu-system-x86_64 not found: $Qemu (set UEFIDOS_QEMU)" }
if (-not (Test-Path $FwDir)) { throw "OVMF firmware dir not found: $FwDir (set UEFIDOS_OVMF_DIR)" }

# ---- 1. make sure the EFI payload and demo programs are built ----
$need = @('BOOTX64.EFI','BOOTIA32.EFI','HELLO.COM','TELETYPE.COM','MZEXE.EXE','RUNNER.COM','VRAM.COM')
$missing = @($need | Where-Object { -not (Test-Path (Join-Path $Build $_)) })
if ($missing.Count -gt 0) {
    Write-Host ('== build artifacts missing ({0}); running build.ps1 ...' -f ($missing -join ', '))
    & (Join-Path $Root 'build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'build.ps1 failed' }
}

# ---- 2. interactive demo image: AUTOEXEC stops at the shell prompt ----
$Img = Join-Path $Build 'interactive.img'
$lines = @(
    'VER'
    'DIR'
    'TYPE HELLO.TXT'
    'HELLO'
    'TELETYPE'
    'MZEXE'
    'RUNNER'
    'VRAM'
    'APITEST'
    'UPCALL'
    'INFO'
    'CAL'
    'SUM HELLO.TXT'
    'HEX HELLO.TXT'
)
$makeArgs = @($Img, '--size-mb', '64',
    '--boot',    (Join-Path $Build 'BOOTX64.EFI'),
    '--boot-ia32',(Join-Path $Build 'BOOTIA32.EFI'),
    '--text',    'HELLO.TXT=Hello UEFI-DOS!',
    '--dir',     'TESTDIR',
    '--text',    'TESTDIR/INNER.TXT=inner file',
    '--file',    ((Join-Path $Build 'HELLO.COM')   + '=HELLO.COM'),
    '--file',    ((Join-Path $Build 'TELETYPE.COM')+ '=TELETYPE.COM'),
    '--file',    ((Join-Path $Build 'MZEXE.EXE')   + '=MZEXE.EXE'),
    '--file',    ((Join-Path $Build 'RUNNER.COM')  + '=RUNNER.COM'),
    '--file',    ((Join-Path $Build 'VRAM.COM')    + '=VRAM.COM'),
    '--file',    ((Join-Path $Build 'UPCALL.COM')  + '=UPCALL.COM'),
    '--file',    ((Join-Path $Build 'INFO.COM')    + '=INFO.COM'),
    '--file',    ((Join-Path $Build 'CLOCK.COM')   + '=CLOCK.COM'),
    '--file',    ((Join-Path $Build 'HEX.COM')     + '=HEX.COM'),
    '--file',    ((Join-Path $Build 'SUM.COM')     + '=SUM.COM'),
    '--file',    ((Join-Path $Build 'CAL.COM')     + '=CAL.COM'))
# the curated third-party games/tools that build.ps1 may have extracted from your
# own DOS disk images (nothing of that kind ships with this repository)
$imported = Join-Path $Build 'imported'
if (Test-Path $imported) {
    foreach ($f in Get-ChildItem $imported -File | Sort-Object Name) {
        $makeArgs += @('--file', "$($f.FullName)=$($f.Name)")
    }
}
# optional: mount a real MS-DOS 2.11 floppy image as A: if you have one
$dos211 = $env:UEFIDOS_DOS211
if ($dos211 -and (Test-Path $dos211)) {
    $makeArgs += @('--file', "$dos211=DOS211.IMG")
    $lines += 'MOUNT A: DOS211.IMG'
    $lines += 'DIR A:'
}
$lines += 'ECHO UEFI-DOS interactive shell ready. Type HELP for commands.'
$lines += 'ECHO Type SHUTDOWN to power off, EXIT to return to firmware.'
$makeArgs += @('--autoexec', ($lines -join '\n'))
Write-Host '== building interactive.img ...'
& $Python (Join-Path $Root 'tools\make_image.py') @makeArgs
if ($LASTEXITCODE -ne 0) { throw 'make_image.py failed' }

# ---- 3. boots the image under OVMF ----
$VarsSrc = Join-Path $FwDir 'edk2-i386-vars.fd'   # arch-neutral var store layout
$Vars = Join-Path $Build 'vars-interactive.fd'
Copy-Item $VarsSrc $Vars -Force
$Fw = if ($Firmware -eq 'ia32') { Join-Path $FwDir 'edk2-i386-code.fd' } else { Join-Path $FwDir 'edk2-x86_64-code.fd' }
$Serial = Join-Path $Build 'serial-interactive.log'
if (Test-Path $Serial) { Remove-Item $Serial -Force }

$qargs = @(
    '-m', "$MemoryMB", '-smp', '1', '-machine', 'pc',
    '-drive', "if=pflash,format=raw,readonly=on,file=$Fw",
    '-drive', "if=pflash,format=raw,file=$Vars",
    '-drive', "file=$Img,format=raw,if=ide,index=0,media=disk",
    '-no-reboot', '-net', 'none',
    '-serial', "file:$Serial"
)
if ($Headless) { $qargs += @('-display', 'none') }

$argStr = ($qargs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
Write-Host ("== launching qemu in {0} mode ..." -f $(if ($Headless) { 'headless' } else { 'window' }))
$p = Start-Process -FilePath $Qemu -ArgumentList $argStr -PassThru -NoNewWindow

if ($Headless) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
} else {
    $p.WaitForExit()
}

# ---- 4. report ----
Write-Host "qemu exit: $($p.ExitCode)"
if ($Headless -and (Test-Path $Serial)) {
    Write-Host '---- serial tail ----'
    Get-Content $Serial -Tail 60
    Write-Host '---------------------'
}
