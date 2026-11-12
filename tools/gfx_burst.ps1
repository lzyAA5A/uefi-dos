# Boot a UEFI-DOS image in QEMU with a window and take a burst of screendumps,
# so a frame that is only on screen briefly (Windows 1.x's splash) can be caught.
param(
    [string]$Image = 'build\win1x.img',
    [string]$OutDir = 'build\burst',
    [int]$Port = 55601,
    [int]$StartSec = 18,
    [int]$Count = 60,
    [int]$IntervalMs = 300,
    [int]$MemoryMB = 256,
    [string]$Firmware = 'x64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Definition)
$Qemu = if ($env:UEFIDOS_QEMU) { $env:UEFIDOS_QEMU } else { 'C:\Program Files\qemu\qemu-system-x86_64.exe' }
$FwDir = if ($env:UEFIDOS_OVMF_DIR) { $env:UEFIDOS_OVMF_DIR } else { 'C:\Program Files\qemu\share' }
$Python = if ($env:UEFIDOS_PYTHON) { $env:UEFIDOS_PYTHON } else { 'python' }
$code = if ($Firmware -eq 'ia32') { 'edk2-i386-code.fd' } else { 'edk2-x86_64-code.fd' }
$codePath = Join-Path $FwDir $code
$varsSrc = Join-Path $FwDir 'edk2-i386-vars.fd'
$vars = Join-Path $root 'build\vars-burst.fd'
$img = if ([IO.Path]::IsPathRooted($Image)) { $Image } else { Join-Path $root $Image }
if (-not (Test-Path $img)) { throw "image not found: $img" }
Copy-Item $varsSrc $vars -Force
$log = Join-Path $root 'build\serial-burst.log'
if (Test-Path $log) { Remove-Item $log -Force }

# QEMU wants the quotes around the *path*, not around the whole "file=..."
# option: -serial "file=C:\x.log" makes QEMU treat "file=C:\x.log" as a driver
# name.  One quoted string for Start-Process as well (an argument array gets
# mangled for -drive values).
$argStr = @(
    "-m $MemoryMB", '-smp 1', '-machine pc',
    "-drive if=pflash,format=raw,readonly=on,file=`"$codePath`"",
    "-drive if=pflash,format=raw,file=`"$vars`"",
    "-drive file=`"$img`",if=ide,index=0,media=disk,format=raw",
    "-serial file:`"$log`"",
    "-monitor tcp:127.0.0.1:$Port,server,nowait",
    '-no-reboot', '-net none'
) -join ' '
Write-Host "booting $img (windowed, monitor on $Port) ..."
$q = Start-Process -FilePath $Qemu -ArgumentList $argStr -PassThru
Start-Sleep -Seconds $StartSec
try {
    & $Python (Join-Path $root 'tools\qburst.py') $Port $OutDir $Count $IntervalMs 'shot'
} finally {
    if (-not $q.HasExited) { Stop-Process -Id $q.Id -Force }
}
