# Boot a Windows 1.x install (Windows 1.0 Developer Release 5, or an installed
# Windows 1.0x tree) on top of UEFI-DOS, so the 1984/1985 runtime can be tried
# on real firmware without a BIOS boot sector or a DOS install.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File run_win1x.ps1 `
#       -Source "F:\path\to\extracted\dr5" [-Exe KERNEL.EXE] [-Setver KERNEL.EXE]
#
# The Microsoft files stay on your machine: they are only read to build
# build\win1x.img, which is a local test image (build\ is git-ignored).
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Exe = 'KERNEL.EXE',
    [string[]]$Setver = @('KERNEL.EXE'),
    [string]$Dir = 'WINDOWS\TEST',
    [string]$Image = 'build\win1x.img',
    [int]$MemoryMB = 256,
    [switch]$Headless,
    [int]$TimeoutSec = 60
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

$Python = if ($env:UEFIDOS_PYTHON) { $env:UEFIDOS_PYTHON } else { 'python' }
$Qemu = if ($env:UEFIDOS_QEMU) { $env:UEFIDOS_QEMU } else { 'C:\Program Files\qemu\qemu-system-x86_64.exe' }
$FwDir = if ($env:UEFIDOS_OVMF_DIR) { $env:UEFIDOS_OVMF_DIR } else { 'C:\Program Files\qemu\share' }

if (-not (Test-Path $Qemu)) { throw "QEMU not found: $Qemu (set UEFIDOS_QEMU)" }
if (-not (Test-Path (Join-Path $FwDir 'edk2-x86_64-code.fd'))) { throw "OVMF not found in $FwDir (set UEFIDOS_OVMF_DIR)" }
if (-not (Test-Path $Source)) { throw "source directory not found: $Source" }
if (-not (Test-Path 'build\BOOTX64.EFI') -or -not (Test-Path 'build\BOOTIA32.EFI')) {
    throw 'build\BOOTX64.EFI missing - run build.ps1 (or make) once first'
}

$install = @($Python, 'tools\install_win1x.py', $Source, $Image, '--dir', $Dir, '--exe', $Exe)
foreach ($p in $Setver) { $install += @('--setver', $p) }
& $install[0] $install[1..($install.Count - 1)]
if ($LASTEXITCODE -ne 0) { throw 'install_win1x.py failed' }

Copy-Item (Join-Path $FwDir 'edk2-i386-vars.fd') 'build\vars-win1x.fd' -Force
$args = "-m $MemoryMB -smp 1 -machine pc " +
        "-drive if=pflash,format=raw,readonly=on,file=`"$(Join-Path $FwDir 'edk2-x86_64-code.fd')`" " +
        "-drive if=pflash,format=raw,file=build/vars-win1x.fd " +
        "-drive file=$Image,if=ide,index=0,media=disk,format=raw " +
        "-boot menu=off -net none"
$log = 'build\serial-win1x.log'
if ($Headless) {
    Remove-Item $log -ErrorAction SilentlyContinue
    $args += " -display none -serial file:$log"
    Write-Host "booting $Image headless for ${TimeoutSec}s; serial log: $log"
}

$proc = Start-Process -FilePath $Qemu -ArgumentList $args -PassThru
Write-Host "QEMU pid $($proc.Id). Windows 1.x starts from AUTOEXEC.BAT."
if ($Headless) {
    Start-Sleep -Seconds $TimeoutSec
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    if (Test-Path $log) {
        Write-Host '--- serial log tail ---'
        Get-Content $log -Tail 25
    }
} else {
    Write-Host 'Close the QEMU window to stop it.'
    Wait-Process -Id $proc.Id -Timeout ($TimeoutSec * 60) -ErrorAction SilentlyContinue
}
