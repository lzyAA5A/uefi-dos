# Build the native SDK examples with the local MinGW-w64 gcc (Windows host).
# Unix/macOS users should use sdk/Makefile with a mingw-w64 cross compiler.
param(
    [string]$Gcc = 'C:\msys64\mingw64\bin\gcc.exe',
    [string]$OutDir = 'build\sdk'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
if (-not (Test-Path $Gcc)) { throw "gcc not found: $Gcc" }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$cflags = @(
    '-O2', '-std=gnu11', '-ffreestanding', '-Wall', '-Wextra',
    '-fno-builtin', '-fno-stack-protector', '-fno-asynchronous-unwind-tables',
    '-fno-unwind-tables', '-fno-ident', '-fno-pic', '-Isdk/include'
)
$ldflags = @(
    '-nostdlib', '-Wl,--subsystem,10', '-Wl,-e,uefidos_main',
    '-Wl,--enable-reloc-section', '-Wl,--no-insert-timestamp'
)

$targets = @(
    @{ name = 'HELLO64.EXE'; machine = '-m64' },
    @{ name = 'HELLO32.EXE'; machine = '-m32' }
)
foreach ($t in $targets) {
    $out = Join-Path $OutDir $t.name
    Write-Host "SDK: $($t.name)"
    & $Gcc @cflags @($t.machine) @ldflags -o $out sdk/examples/hello64.c
    if ($LASTEXITCODE -ne 0) { throw "sdk build failed for $($t.name)" }
    $len = (Get-Item $out).Length
    Write-Host ("SDK: {0} ({1} bytes)" -f $out, $len)
}
