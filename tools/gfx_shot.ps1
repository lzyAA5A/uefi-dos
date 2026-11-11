# Boot one of the graphics demo images in QEMU, screenshot the framebuffer and
# convert it to PNG.  Usage: tools/gfx_shot.ps1 -Image build/gfxdemo.img -Out build/shot_gfxqemu.png
param(
    [string]$Image = 'build/gfxdemo.img',
    [string]$Out = 'build/shot_gfxqemu.png',
    [int]$Port = 55580,
    [int]$WaitSec = 42
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
Push-Location $Root
try {
    $Qemu = if ($env:UEFIDOS_QEMU) { $env:UEFIDOS_QEMU } else { 'C:\Program Files\qemu\qemu-system-x86_64.exe' }
    $FwDir = if ($env:UEFIDOS_OVMF_DIR) { $env:UEFIDOS_OVMF_DIR } else { 'C:\Program Files\qemu\share' }
    $Python = if ($env:UEFIDOS_PYTHON) { $env:UEFIDOS_PYTHON } else { 'python' }
    Copy-Item (Join-Path $FwDir 'edk2-i386-vars.fd') 'build\vars-gfxshot.fd' -Force
    $argStr = @(
        '-accel tcg', '-m 256', '-smp 1', '-machine pc',
        ('-drive "if=pflash,format=raw,readonly=on,file=' + (Join-Path $FwDir 'edk2-x86_64-code.fd') + '"'),
        '-drive "if=pflash,format=raw,file=build\vars-gfxshot.fd"',
        ('-drive "file=' + $Image + ',if=ide,index=0,media=disk,format=raw"'),
        ('-monitor tcp:127.0.0.1:' + $Port + ',server,nowait'),
        '-serial file:build\serial-gfxshot.log',
        '-display gtk', '-no-reboot', '-net none'
    ) -join ' '
    $p = Start-Process -FilePath $Qemu -ArgumentList $argStr -PassThru
    Start-Sleep -Seconds $WaitSec
    $env:QMON_PORT = "$Port"
    $ppm = ($Out -replace '\.png$', '') + '.ppm'
    & $Python 'tools/qmon.py' ("screendump " + $ppm) | Out-Null
    Start-Sleep -Seconds 2
    & $Python 'tools/ppm2png.py' $ppm $Out
    # stop the guest: quit flushes everything the firmware wrote
    & $Python 'tools/qmon.py' 'quit' | Out-Null
    Start-Sleep -Seconds 1
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Write-Host "screenshot: $Out"
    if (Test-Path 'build\serial-gfxshot.log') {
        Get-Content 'build\serial-gfxshot.log' -Raw
    }
} finally {
    Pop-Location
}
