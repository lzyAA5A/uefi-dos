# Build a ready-to-run UEFI-DOS release folder + zip.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File make_release.ps1
#
# Produces build\release\UEFI-DOS-<version>\ and build\release\UEFI-DOS-<version>.zip
# containing everything needed to boot without a compiler:
#
#   start.bat        double-click launcher (finds QEMU + its bundled OVMF)
#   UEFIDOS.IMG      64 MB bootable FAT32 image: EFI/BOOT/{BOOTX64,BOOTIA32}.EFI,
#                    the bundled utilities, AUTOEXEC.BAT that stops at C:\>
#   firmware/        note about where the firmware comes from (nothing bundled)
#   README.md README.zh-CN.md LICENSE START-HERE.txt
#
# Third-party binaries are deliberately NOT packaged: QEMU/OVMF are expected to
# be installed on the machine (https://www.qemu.org/download/#windows), and the
# DOS disk images used by the optional floppy checks stay with their owners.
param(
    [string]$OutDir = 'build/release',
    [int]$SizeMB = 64
)
$ErrorActionPreference = 'Stop'
$Root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root 'build'
$Python = if ($env:UEFIDOS_PYTHON) { $env:UEFIDOS_PYTHON } else { 'python' }

$Version = (Select-String -Path (Join-Path $Root 'src/uefidos_config.h') `
    -Pattern '#define UEFIDOS_VERSION\s+"([^"]+)"').Matches[0].Groups[1].Value
if (-not $Version) { throw 'could not read UEFIDOS_VERSION from src/uefidos_config.h' }
$Name = "UEFI-DOS-$Version"
$Stage = Join-Path (Join-Path $Root $OutDir) $Name
Write-Host "== release $Version -> $Stage"

# ---- 1. the EFI payload and the bundled programs must be up to date ----
& (Join-Path $Root 'build.ps1') | Out-Null
foreach ($f in 'BOOTX64.EFI','BOOTIA32.EFI','INFO.COM','CAL.COM','SUM.COM','HEX.COM','CLOCK.COM',
               'HELLO.COM','TELETYPE.COM','MZEXE.EXE','RUNNER.COM','VRAM.COM','UPCALL.COM') {
    if (-not (Test-Path (Join-Path $Build $f))) { throw "missing build artifact: $f (run build.ps1)" }
}

# ---- 2. bootable image: runs the demos, then stops at a live C:\> prompt ----
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage -Force | Out-Null
$img = Join-Path $Stage 'UEFIDOS.IMG'
$makeArgs = @($img, '--size-mb', "$SizeMB",
    '--boot',      (Join-Path $Build 'BOOTX64.EFI'),
    '--boot-ia32', (Join-Path $Build 'BOOTIA32.EFI'),
    '--text',      'HELLO.TXT=Hello UEFI-DOS!',
    '--dir',       'DEMO',
    '--text',      'DEMO/INNER.TXT=inside DEMO',
    '--text',      "CONFIG.SYS=FILES=20`nLASTDRIVE=D",
    '--text',      'README.TXT=UEFI-DOS release image. Type HELP for commands.')
foreach ($p in 'HELLO.COM','TELETYPE.COM','MZEXE.EXE','RUNNER.COM','VRAM.COM','UPCALL.COM',
               'INFO.COM','CAL.COM','SUM.COM','HEX.COM','CLOCK.COM') {
    $makeArgs += @('--file', "$Build/$p=$p")
}
# No SHUTDOWN here: the user gets a shell prompt to play with.
$autoexec = @(
    'ECHO UEFI-DOS - DOS on UEFI, no BIOS, no V86.',
    'ECHO.',
    'VER',
    'DIR',
    'INFO',
    'CAL',
    'SUM HELLO.TXT',
    'HEX HELLO.TXT',
    'HELLO',
    'VRAM',
    'ECHO.',
    'ECHO Try: DIR  CD DEMO  TYPE HELLO.TXT  CLOCK (any key)  APITEST',
    'ECHO Type SHUTDOWN to power off.'
) -join "`n"
$makeArgs += @('--autoexec', $autoexec)
& $Python (Join-Path $Root 'tools/make_image.py') @makeArgs | Select-Object -Last 1

# ---- 3. launcher, docs ----
Copy-Item (Join-Path $Root 'start_prebuilt.bat') (Join-Path $Stage 'start.bat') -Force
foreach ($f in 'README.md','README.zh-CN.md','LICENSE') {
    Copy-Item (Join-Path $Root $f) (Join-Path $Stage $f) -Force
}

$startHere = @'
UEFI-DOS {0}
=============={1}

双击 start.bat 即可启动（需要先装 QEMU）:
  winget install SoftwareFreedomConservancy.QEMU
  或从 https://www.qemu.org/download/#windows 下载安装

Double-click start.bat to boot (QEMU must be installed first):
  winget install SoftwareFreedomConservancy.QEMU

它会用 OVMF(UEFI 固件) 从 UEFIDOS.IMG 启动，跑完演示程序后停在 C:\> 提示符。
命令行参数会透传给 QEMU，例如  start.bat -display none -serial file:log.txt

Contents
  start.bat      launcher (finds qemu-system-x86_64.exe and its edk2/OVMF files)
  UEFIDOS.IMG    bootable 64 MB FAT32 image, EFI/BOOT/BOOTX64.EFI + BOOTIA32.EFI
  README.md      English documentation
  README.zh-CN.md  中文文档
  LICENSE        MIT

想改用真机/U 盘：把 UEFIDOS.IMG 原样写到 U 盘（Rufus 的 "DD 镜像" 模式，或
Linux 下 dd），UEFI 机器从该 U 盘启动即可；也可只把镜像里的 EFI\BOOT\ 拷到任意
FAT32 的 ESP 分区根目录。
'@
$startHere = $startHere -f $Version, ('=' * $Version.Length)
Set-Content -Path (Join-Path $Stage 'START-HERE.txt') -Value $startHere -Encoding UTF8

# ---- 4. zip ----
$zip = Join-Path (Join-Path $Root $OutDir) "$Name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $Stage '*') -DestinationPath $zip -CompressionLevel Optimal
$imgMB  = [math]::Round((Get-Item $img).Length / 1MB, 1)
$zipKB  = [math]::Round((Get-Item $zip).Length / 1KB, 0)
Write-Host ("== {0}: image {1} MB, zip {2} KB" -f $Name, $imgMB, $zipKB)
Write-Host "   $zip"
