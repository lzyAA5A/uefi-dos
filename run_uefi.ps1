# UEFI-DOS QEMU + OVMF smoke/integration runner.
# Boots build/UEFIDOS.img under the QEMU-shipped edk2 (OVMF) firmware,
# captures the serial log and asserts markers.
#
# Environment overrides:
#   UEFIDOS_QEMU      full path of qemu-system-x86_64.exe (default: C:\Program Files\qemu)
#   UEFIDOS_OVMF_DIR  directory holding edk2-*-code.fd / edk2-i386-vars.fd
param(
    [string]$Image = '',
    [int]$TimeoutSec = 120,
    [ValidateSet('x64','ia32')]
    [string]$Firmware = 'x64',
    [switch]$SkipFloppy        # image was built without DOS211.IMG: drop the floppy markers
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root 'build'
if (-not $Image) { $Image = Join-Path $Build 'UEFIDOS.img' }
if (-not (Test-Path $Image)) { throw "missing image: $Image" }

$Qemu = if ($env:UEFIDOS_QEMU) { $env:UEFIDOS_QEMU } else { 'C:\Program Files\qemu\qemu-system-x86_64.exe' }
$FwDir = if ($env:UEFIDOS_OVMF_DIR) { $env:UEFIDOS_OVMF_DIR } else { 'C:\Program Files\qemu\share' }
if (-not (Test-Path $Qemu)) { throw "qemu-system-x86_64 not found: $Qemu (set UEFIDOS_QEMU)" }
if ($Firmware -eq 'ia32') {
    $Fw = Join-Path $FwDir 'edk2-i386-code.fd'
} else {
    $Fw = Join-Path $FwDir 'edk2-x86_64-code.fd'
}
$VarsSrc = Join-Path $FwDir 'edk2-i386-vars.fd'   # OVMF VAR store layout is arch-neutral (same FDF)
if (-not (Test-Path $Fw)) { throw "OVMF code image not found: $Fw (set UEFIDOS_OVMF_DIR)" }
if (-not (Test-Path $VarsSrc)) { throw "OVMF vars image not found: $VarsSrc (set UEFIDOS_OVMF_DIR)" }
$Vars = Join-Path $Build 'vars.fd'
Copy-Item $VarsSrc $Vars -Force
$Serial = Join-Path $Build 'serial.log'
if (Test-Path $Serial) { Remove-Item $Serial -Force }

$qargs = @(
    '-m','256','-smp','1','-machine','pc',
    '-drive',"if=pflash,format=raw,readonly=on,file=$Fw",
    '-drive',"if=pflash,format=raw,file=$Vars",
    '-drive',"file=$Image,format=raw,if=ide,index=0,media=disk"
)
# second disk (D:) -- proves multi-volume enumeration under real firmware
$second = Join-Path $Build 'SECOND.img'
if (Test-Path $second) {
    $qargs += @('-drive', "file=$second,format=raw,if=ide,index=1,media=disk")
}
$qargs += @('-display','none','-serial',"file:$Serial",'-no-reboot','-net','none')

$argStr = ($qargs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
$p = Start-Process -FilePath $Qemu -ArgumentList $argStr -PassThru -NoNewWindow
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
if (-not $p.HasExited) {
    Write-Host 'qemu still running after timeout; killing'
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    $code = 'timeout'
} else {
    $code = $p.ExitCode
}
Write-Host "qemu exit: $code"
if (Test-Path $Serial) {
    Write-Host '---- serial log ----'
    Get-Content $Serial -Raw
    Write-Host '--------------------'
}
# assertions
$text = if (Test-Path $Serial) { Get-Content $Serial -Raw } else { '' }
$ok = $true
$markers = @('UEFI-DOS', 'BOOT OK', 'Hello from 8086 code!', 'TELETYPE OK!', 'Hello from MZ EXE!', 'RUNNER DONE!', 'VIDEO-OK!', 'API-OK', 'UPCALL-OK','DTA-OK','INT2F-OK','DPMI-HOST-OK','PM16-OK','PM32-DOS-OK','RMREFLECT-OK','DOSMEM-OK','REFLECTED-21H-OK','ABI-OK','TICKS-OK', 'UEFI-DOS native API v1.01', 'WRITE-OK', 'FCB-OK', 'FILES=20', '=== UEFI-DOS system info ===', 'Su Mo Tu We Th Fr Sa', 'bytes, checksum', 'Hello UEFI-DOS!.', 'RUN64: loading HELLO64.EXE', 'NATIVE-OK', 'long name    : created "Long Name Report.txt"', '8.3 alias    : LONGNA~1.TXT -> Long Name Report.txt')
if ($Firmware -eq 'ia32') {
    $markers += @('pointer size : 32 bit')   # HELLO32.EXE is the one that runs
} else {
    $markers += @('pointer size : 64 bit')   # HELLO64.EXE is the one that runs
}
if (-not $SkipFloppy) { $markers += @('FLOPPY-WRITE-OK', 'FLOPPY-OK', 'IBMBIO') }
if (Test-Path (Join-Path $Build 'SECOND.img')) {
    $markers += @('Drive D: mounted', 'SECOND-DRIVE-OK', 'this volume is D:, not C:')
}
foreach ($m in $markers) {
    if ($text -notmatch [regex]::Escape($m)) { Write-Host "MISSING marker: $m"; $ok = $false }
}
if (-not $ok) { exit 1 }
Write-Host 'smoke test: PASS'
