# Build the UEFI-DOS benchmark: the 16-bit DOS program, the 32- and 64-bit
# native images, the bootable image and a TSC-frequency helper.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File bench/build_bench.ps1
#
# Output (in build\): BENCH16.COM, BENCH32.EXE, BENCH64.EXE, BENCH.IMG,
# tscfreq.exe, and bench\bench_sizes.h (the shared workload sizes).
param(
    [string]$Gcc = 'C:\msys64\mingw64\bin\gcc.exe',
    [int]$SizeMb = 64,
    [int]$DataFiles = 120
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $Root
$Build = Join-Path $Root 'build'
New-Item -ItemType Directory -Force $Build | Out-Null
$Python = if ($env:UEFIDOS_PYTHON) { $env:UEFIDOS_PYTHON } else { 'python' }
if (-not (Test-Path $Gcc)) { $Gcc = 'gcc' }

# 1. the 16-bit program (also writes bench/bench_sizes.h)
& $Python 'bench/make_bench16.py' $Build
if ($LASTEXITCODE -ne 0) { throw 'BENCH16.COM generation failed' }

# 2. the native images: same source, PE32 and PE32+
$cflags = @(
    '-O2', '-std=gnu11', '-ffreestanding', '-Wall', '-Wextra',
    '-fno-builtin', '-fno-stack-protector', '-fno-asynchronous-unwind-tables',
    '-fno-unwind-tables', '-fno-ident', '-fno-pic', '-Isdk/include'
)
$ldflags = @(
    '-nostdlib', '-Wl,--subsystem,10', '-Wl,-e,uefidos_main',
    '-Wl,--enable-reloc-section', '-Wl,--no-insert-timestamp'
)
foreach ($t in @(@{ n = 'BENCH64.EXE'; m = '-m64' }, @{ n = 'BENCH32.EXE'; m = '-m32' })) {
    Write-Host "bench: $($t.n)"
    & $Gcc @cflags @($t.m) @ldflags -o (Join-Path $Build $t.n) 'bench/bench.c'
    if ($LASTEXITCODE -ne 0) { throw "native build failed for $($t.n)" }
}

# 3. a tiny host helper that measures the TSC frequency, so the driver can turn
#    the native cycle counts into microseconds
& $Gcc '-O2' '-o' (Join-Path $Build 'tscfreq.exe') 'bench/tscfreq.c'
if ($LASTEXITCODE -ne 0) { throw 'tscfreq helper failed' }

# 4. the bootable image: the three programs, the UEFI loaders and a directory
#    with many files (the find workload scans \BDATA 300 times)
$img = Join-Path $Build 'BENCH.IMG'
$imgArgs = @('tools\make_image.py', $img,
             '--size-mb', "$SizeMb", '--boot', (Join-Path $Build 'BOOTX64.EFI'),
             '--boot-ia32', (Join-Path $Build 'BOOTIA32.EFI'),
             '--file', ('{0}=BENCH16.COM' -f (Join-Path $Build 'BENCH16.COM')),
             '--file', ('{0}=BENCH32.EXE' -f (Join-Path $Build 'BENCH32.EXE')),
             '--file', ('{0}=BENCH64.EXE' -f (Join-Path $Build 'BENCH64.EXE')))
for ($i = 1; $i -le $DataFiles; $i++) {
    $imgArgs += @('--text', ('BDATA/D{0:D3}.TXT=payload' -f $i))
}
& $Python @imgArgs
if ($LASTEXITCODE -ne 0) { throw 'image build failed' }
Write-Host "bench: image $img"
