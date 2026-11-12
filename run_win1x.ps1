# Boot a Windows 1.x runtime on top of UEFI-DOS, so a 1984/1985 GUI can be tried
# on real firmware without a BIOS boot sector or a DOS install.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File run_win1x.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File run_win1x.ps1 -Source <media dir>
#   powershell -NoProfile -ExecutionPolicy Bypass -File run_win1x.ps1 -Installed <C:\WINDOWS tree>
#
# Two ways in:
#
#   * a *Windows 1.01* media directory (has WIN.CNF / CGA.DRV) -- UEFI-DOS already
#     carries a hand-built install of it, so the kernel itself runs; that is what
#     -Installed points at (default: an existing installed tree, else the media
#     directory's own files are laid down and you still have to run SETUP once).
#   * a *Windows 1.0 Developer Release 5* media directory (has IBMCOLOR.DIN) --
#     laid out with tools\install_win1x.py and started with KERNEL.EXE.  Be aware
#     that DR5's KERNEL.EXE zeroes its own code and stack in BOOTSTRAP before its
#     first INT 21h, so it stops on real Compaq MS-DOS 2.11 too (docs/windows1x.md).
#
# The Microsoft files stay on this machine: they are only read to build
# build\win1x.img, which is a local test image (build\ is git-ignored).
param(
    [string]$Source,
    [string]$Installed,
    [string]$Exe,
    [string[]]$Setver,
    [string]$Dir = 'WINDOWS\TEST',
    [string]$Image = 'build\win1x.img',
    [int]$MemoryMB = 256,
    [switch]$Rebuild,
    [switch]$Headless,
    [int]$TimeoutSec = 60
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

$Python = if ($env:UEFIDOS_PYTHON) { $env:UEFIDOS_PYTHON } else { 'python' }
$Qemu = if ($env:UEFIDOS_QEMU) { $env:UEFIDOS_QEMU } else { 'C:\Program Files\qemu\qemu-system-x86_64.exe' }
$FwDir = if ($env:UEFIDOS_OVMF_DIR) { $env:UEFIDOS_OVMF_DIR } else { 'C:\Program Files\qemu\share' }

function Find-Media {
    $candidates = @()
    if ($env:UEFIDOS_WIN1X_SRC) { $candidates += $env:UEFIDOS_WIN1X_SRC }
    $candidates += @(
        (Join-Path $root 'win1x-src'),
        'F:\1231\_tmp\win\w101',
        'F:\1231\_tmp\win\dr5'
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c 'KERNEL.EXE'))) { return (Resolve-Path $c).Path }
    }
    return $null
}

function Find-Installed {
    $candidates = @()
    if ($env:UEFIDOS_WIN1X_INSTALLED) { $candidates += $env:UEFIDOS_WIN1X_INSTALLED }
    $candidates += @(
        (Join-Path $root 'win1x-src\installed'),
        'F:\1231\_tmp\win\w101run'
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c 'WINDOWS\WIN100.BIN'))) { return (Resolve-Path $c).Path }
    }
    return $null
}

if (-not (Test-Path $Qemu)) { throw "QEMU not found: $Qemu (set UEFIDOS_QEMU)" }
if (-not (Test-Path (Join-Path $FwDir 'edk2-x86_64-code.fd'))) { throw "OVMF not found in $FwDir (set UEFIDOS_OVMF_DIR)" }
if (-not (Test-Path 'build\BOOTX64.EFI') -or -not (Test-Path 'build\BOOTIA32.EFI')) {
    throw 'build\BOOTX64.EFI missing - run build.ps1 (or make) once first'
}

if (-not $Installed) { $Installed = Find-Installed }
if (-not $Source) { $Source = Find-Media }

$mounted = $null

if ($Installed) {
    # An already installed C:\WINDOWS tree: mirror it into an image and let
    # AUTOEXEC.BAT (SETVER + CD + WIN) start Windows at boot.
    Write-Host "Windows 1.x installed tree: $Installed"
    Write-Host '  status: the kernel runs (all of its DOS-internals checks pass) but the'
    Write-Host '          desktop is not drawn yet -- docs\windows1x.md says where it stops.'
    $auto_path = Join-Path $Installed 'AUTOEXEC.BAT'
    $needs_auto = $true
    if (Test-Path $auto_path) {
        # keep a batch file the user wrote, but replace one that does not start
        # Windows at all (a bare SETVER/CD leftover is useless as a demo boot)
        $txt = Get-Content $auto_path -Raw
        if ($txt -match '(?im)^\s*WIN(\.COM)?\s*$') { $needs_auto = $false }
    }
    if ($needs_auto) {
        $auto = "SETVER WIN.COM 3.30`r`nSETVER WIN100.BIN 3.30`r`nSETVER MSDOS.EXE 3.30`r`nCD \WINDOWS`r`nWIN`r`n"
        Set-Content -Path $auto_path -Value $auto -NoNewline -Encoding ASCII
        Write-Host '  wrote AUTOEXEC.BAT (SETVER + CD \WINDOWS + WIN)'
    }
    # the firmware needs EFI\BOOT in the image, and an installed C:\WINDOWS tree
    # does not carry it -- drop the freshly built loaders in before imaging
    $efiDir = Join-Path $Installed 'EFI\BOOT'
    New-Item -ItemType Directory -Force -Path $efiDir | Out-Null
    Copy-Item 'build\BOOTX64.EFI' (Join-Path $efiDir 'BOOTX64.EFI') -Force
    Copy-Item 'build\BOOTIA32.EFI' (Join-Path $efiDir 'BOOTIA32.EFI') -Force
    & $Python 'tools\make_tree_image.py' $Image $Installed '--size-mb' '64'
    if ($LASTEXITCODE -ne 0) { throw 'make_tree_image.py failed' }
} elseif ($Source) {
    $is101 = (Test-Path (Join-Path $Source 'WIN.CNF')) -or (Test-Path (Join-Path $Source 'CGA.DRV'))
    $isDR5 = Test-Path (Join-Path $Source 'IBMCOLOR.DIN')
    Write-Host "Windows media directory: $Source"
    if ($is101 -and -not $isDR5) {
        Write-Host '  this looks like Windows 1.01 media (WIN.CNF / CGA.DRV).'
        Write-Host '  Its SETUP has to run once -- see docs/windows1x.md -- and then point'
        Write-Host '  this script at the installed tree with -Installed <dir containing WINDOWS\WIN100.BIN>.'
        throw 'no installed Windows 1.x tree found yet'
    }
    if (-not $Exe) { $Exe = 'KERNEL.EXE' }
    if (-not $Setver) { $Setver = @('KERNEL.EXE') }
    Write-Host "  laying it out in \WINDOWS\TEST and starting $Exe"
    Write-Host '  note: DR5''s KERNEL.EXE corrupts its own memory before its first INT 21h;'
    Write-Host '        the same files hang on real Compaq MS-DOS 2.11 (docs/windows1x.md).'
    $install = @($Python, 'tools\install_win1x.py', $Source, $Image, '--dir', $Dir, '--exe', $Exe)
    foreach ($p in $Setver) { $install += @('--setver', $p) }
    & $install[0] $install[1..($install.Count - 1)]
    if ($LASTEXITCODE -ne 0) { throw 'install_win1x.py failed' }
} else {
    throw 'no Windows media and no installed tree found - see docs/windows1x.md'
}

Copy-Item (Join-Path $FwDir 'edk2-i386-vars.fd') 'build\vars-win1x.fd' -Force
$qargs = "-m $MemoryMB -smp 1 -machine pc " +
        "-drive if=pflash,format=raw,readonly=on,file=`"$(Join-Path $FwDir 'edk2-x86_64-code.fd')`" " +
        "-drive if=pflash,format=raw,file=build/vars-win1x.fd " +
        "-drive file=$Image,if=ide,index=0,media=disk,format=raw " +
        "-boot menu=off -net none"
$log = 'build\serial-win1x.log'
if ($Headless) {
    Remove-Item $log -ErrorAction SilentlyContinue
    $qargs += " -display none -serial file:$log"
    Write-Host "booting $Image headless for ${TimeoutSec}s; serial log: $log"
}

$proc = Start-Process -FilePath $Qemu -ArgumentList $qargs -PassThru
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
