# try_install.ps1 -- drive SimEarth's INSTALL.EXE inside UEFI-DOS and show what it draws.
#
#   powershell -File tools\try_install.ps1 -Keys "ENTER,ENTER" -Steps 6000000
#
# The harness image must already contain INSTALL.IMG (the 1.44 MB floppy with the
# SimEarth install disk).  The script mounts it as A: -- the installer only looks
# at floppy letters, because that is where a 1993 installer expects its disks.
param(
    [string]$Keys = '',
    [int]$Steps = 6000000,
    [string]$Image = 'build\simtest.img'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$env:UEFIDOS_MAX_STEPS = '200000000'
$env:UEFIDOS_HARD_STEPS = "$Steps"
$env:UEFIDOS_KBD = $Keys
$env:UEFIDOS_KBD_MAX_POLLS = '200000'
$env:UEFIDOS_KBD_WAIT_MS = '40'
$env:UEFIDOS_KBD_GUARD = 'ENTER'
$env:UEFIDOS_DUMP_VRAM = '1'
$env:UEFIDOS_TRACE_INTS = '1'
$env:UEFIDOS_EXTRA_DRIVES = ''
Remove-Item build\vram_dump.txt, build\int_trace.txt -ErrorAction SilentlyContinue

& build\hosttest.exe --shell $Image 'MOUNT A: INSTALL.IMG' 'A:INSTALL.EXE' | Out-File build\si_out.txt -Encoding utf8

'--- console ---'
Get-Content build\si_out.txt | Select-Object -Last 25
'--- screen ---'
if (Test-Path build\vram_dump.txt) { Get-Content build\vram_dump.txt } else { 'no screen dump' }
