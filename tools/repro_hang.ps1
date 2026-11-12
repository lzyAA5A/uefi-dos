param(
    [int]$WaitSec = 40,
    [string]$Firmware = 'x64'
)
# Rebuild a pristine test image, boot it under OVMF with a QEMU monitor, wait,
# then dump the serial log and a few RIP samples.  Debug aid for the floppy
# write stall.
$ErrorActionPreference = 'Stop'
Set-Location 'F:\1231\UEFI-DOS'
Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$py = 'C:\Users\asus\AppData\Local\Programs\Python\Python310\python.exe'
$img = 'build\repro.img'
Remove-Item $img -ErrorAction SilentlyContinue
& $py tools\make_image.py $img --boot build\BOOTX64.EFI --size-mb 64 `
    --text 'HELLO.TXT=Hello UEFI-DOS!' `
    --file 'F:\1231\_tmp\C86S\data\dos211.img=DOS211.IMG' `
    --autoexec "MOUNT A: DOS211.IMG`nCOPY HELLO.TXT A:UP.TXT`nECHO DONE" | Out-Null

Copy-Item 'C:\Program Files\qemu\share\edk2-i386-vars.fd' build\vars-repro.fd -Force
Remove-Item build\repro.log -ErrorAction SilentlyContinue
$code = if ($Firmware -eq 'ia32') { 'edk2-i386-code.fd' } else { 'edk2-x86_64-code.fd' }
$exe = if ($Firmware -eq 'ia32') { 'qemu-system-i386.exe' } else { 'qemu-system-x86_64.exe' }
$args = @(
    '-m','256','-smp','1','-machine','pc',
    '-drive',('if=pflash,format=raw,readonly=on,file="C:\Program Files\qemu\share\' + $code + '"'),
    '-drive',('if=pflash,format=raw,file="F:\1231\UEFI-DOS\build\vars-repro.fd"'),
    '-drive',('file="F:\1231\UEFI-DOS\' + $img + '",if=ide,index=0,media=disk,format=raw'),
    '-display','none',
    '-monitor','tcp:127.0.0.1:55558,server,nowait',
    '-serial','file:F:\1231\UEFI-DOS\build\repro.log',
    '-no-reboot','-net','none'
)
$p = Start-Process -FilePath "C:\Program Files\qemu\$exe" -ArgumentList $args -PassThru
Write-Host "qemu pid $($p.Id); waiting $WaitSec s"
Start-Sleep -Seconds $WaitSec

$env:QMON_PORT = '55558'
foreach ($i in 1..2) {
    & $py tools\qmon.py 'info registers' 2>&1 | Select-String 'RIP=' | ForEach-Object { $_.Line.Trim() }
    Start-Sleep -Milliseconds 500
}
Write-Host '--- serial tail ---'
if (Test-Path build\repro.log) { Get-Content build\repro.log | Select-Object -Last 12 }
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
