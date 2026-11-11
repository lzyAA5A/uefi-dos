# UEFI-DOS build script (PowerShell, Windows).
# Builds the x86_64 UEFI application, the Windows host test harness and
# (optionally) a bootable FAT32 image.
#
# Toolchain discovery (override with environment variables if yours differ):
#   UEFIDOS_GCC_DIR  directory holding gcc.exe / objdump.exe (default: MSYS2 mingw64)
#   UEFIDOS_PYTHON   python interpreter used to generate test programs and images
#   UEFIDOS_DOS211   path to a real MS-DOS 2.11 floppy image; when set it is
#                    copied into the test image as DOS211.IMG for the mount
#                    tests.  No such image ships with this repository.
param(
    [switch]$SkipImage,
    [switch]$SkipHost,
    [string]$Dos211Image = $env:UEFIDOS_DOS211
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Mingw = if ($env:UEFIDOS_GCC_DIR) { $env:UEFIDOS_GCC_DIR } else { 'C:\msys64\mingw64\bin' }
$Gcc = Join-Path $Mingw 'gcc.exe'
$Objdump = Join-Path $Mingw 'objdump.exe'
if (-not (Test-Path $Gcc)) { $Gcc = 'gcc' }          # fall back to PATH
if (-not (Test-Path $Objdump)) { $Objdump = 'objdump' }
$Python = if ($env:UEFIDOS_PYTHON) { $env:UEFIDOS_PYTHON } else { 'python' }
$Build = Join-Path $Root 'build'
New-Item -ItemType Directory -Force -Path $Build | Out-Null
Push-Location $Root
try {
    $cflags = @(
        '-m64','-O2','-std=gnu11','-ffreestanding','-Wall','-Wextra',
        '-fno-asynchronous-unwind-tables','-fno-unwind-tables',
        '-fno-stack-protector','-fno-builtin','-mno-red-zone','-mno-stack-arg-probe',
        '-fno-ident','-Isrc'
    )
    $sources = @(
        'src/uefi/main.c',
        'src/uefi/efi_plat.c',
        'src/common/util.c',
        'src/common/divdi3.c',
        'src/common/cp437.c',
        'src/fs/fat.c',
        'src/kernel/dos.c',
        'src/shell/shell.c',
        'src/api/dosapi.c',
        'src/api/native.c',
        'src/emu/emu.c',
        'src/emu/int21.c',
            'src/emu/fcb.c',
        'src/emu/int2f.c',
        'src/emu/dpmi.c'
    )
    $objs = @()
    foreach ($s in $sources) {
        $o = Join-Path $Build ((Split-Path $s -Leaf) -replace '\.c$', '.u.o')
        & $Gcc @cflags -c $s -o $o
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $s" }
        $objs += $o
    }
    $efi = Join-Path $Build 'BOOTX64.EFI'
    & $Gcc '-m64' '-nostdlib' '-o' $efi '-Wl,--subsystem,10' '-Wl,-e,efi_main' '-Wl,--no-insert-timestamp' '-Wl,--enable-reloc-section' @objs '-lgcc'
    if ($LASTEXITCODE -ne 0) { throw 'link failed' }
    Write-Host "built: $efi"
    $hdr = & $Objdump -p $efi | Select-String 'Subsystem|DllCharacteristics|Entry'
    $hdr | ForEach-Object { Write-Host $_ }

    # ---- IA32 (32-bit UEFI) build ----
    $cflags32 = @('-m32','-O2','-std=gnu11','-ffreestanding','-Wall','-Wextra',
        '-fno-asynchronous-unwind-tables','-fno-unwind-tables',
        '-fno-stack-protector','-fno-builtin','-mno-stack-arg-probe',
        '-fno-ident','-Isrc')
    $objs32 = @()
    foreach ($s in $sources) {
        $o = Join-Path $Build ((Split-Path $s -Leaf) -replace '\.c$', '.i.o')
        & $Gcc @cflags32 -c $s -o $o
        if ($LASTEXITCODE -ne 0) { throw "compile32 failed: $s" }
        $objs32 += $o
    }
    $efi32 = Join-Path $Build 'BOOTIA32.EFI'
    & $Gcc '-m32' '-nostdlib' '-o' $efi32 '-Wl,--subsystem,10' '-Wl,-e,efi_main' '-Wl,--no-insert-timestamp' @objs32
    if ($LASTEXITCODE -ne 0) { throw 'link32 failed' }
    Write-Host "built: $efi32"
    (& $Objdump -p $efi32 | Select-String 'Subsystem|Base Relocation|Entry') | ForEach-Object { Write-Host $_ }

    if (-not $SkipHost) {
        $hflags = @('-m64','-O2','-std=gnu11','-Wall','-Wextra','-DUEFIDOS_HOST','-Isrc')
        $hsources = @(
            'src/host/host_main.c',
            'src/host/host_plat.c',
            'src/host/dual_test.c',
            'src/host/fat16_test.c',
            'src/fs/fat.c',
            'src/kernel/dos.c',
            'src/shell/shell.c',
            'src/api/dosapi.c',
            'src/api/native.c',
            'src/emu/itest.c',
            'src/emu/emu.c',
            'src/emu/int21.c',
            'src/emu/fcb.c',
            'src/emu/int2f.c',
            'src/emu/dpmi.c'
        )
        $hostExe = Join-Path $Build 'hosttest.exe'
        & $Gcc @hflags @hsources '-o' $hostExe
        if ($LASTEXITCODE -ne 0) { throw 'host build failed' }
        Write-Host "built: $hostExe"
    }

    if (-not $SkipImage) {
        $img = Join-Path $Build 'UEFIDOS.img'
        $generators = @(
            @{ script = 'tools/make_hello.py';    what = 'demo COM/EXE programs' },
            @{ script = 'tools/make_pm_tests.py'; what = 'protected-mode and DPMI test' },
            @{ script = 'tools/make_fcb_test.py'; what = 'FCB test program' },
            @{ script = 'tools/make_tools.py';    what = 'bundled utility set' }
    @{ script = 'tools/make_keytest.py';  what = 'keyboard scan-code probe' }
        @{ script = 'tools/make_gfx_test.py'; what = 'graphics mode probes (mode 13h + VESA)' }
        @{ script = 'tools/make_dos4_test.py'; what = 'DOS 4.0 conformance probe' }
        )
        foreach ($g in $generators) {
            $gp = Join-Path $Root $g.script
            if (-not (Test-Path $gp)) { Write-Host "skip $($g.what): $($g.script) not present"; continue }
            & $Python $gp $Build
            if ($LASTEXITCODE -ne 0) { throw "$($g.what) generation failed" }
        }
        # native 64-bit SDK examples (sdk/): PE images run on the real CPU
        $sdkScript = Join-Path $Root 'sdk/build_sdk.ps1'
        if ((Test-Path $sdkScript) -and (Test-Path $Gcc)) {
            & powershell -NoProfile -ExecutionPolicy Bypass -File $sdkScript -Gcc $Gcc -OutDir $Build
            if ($LASTEXITCODE -ne 0) { throw 'native SDK examples failed to build' }
        } else {
            Write-Host 'skip native SDK examples'
        }

        # Optional hook: pull a curated third-party game/tool set from local DOS
        # disk images.  Nothing of that kind ships with this repository, so this
        # only runs when you drop in tools/import_cardputer.py yourself.
        $importScript = Join-Path $Root 'tools/import_cardputer.py'
        if (Test-Path $importScript) {
            & $Python $importScript 2>&1 | Select-Object -Last 1 | Write-Host
        }

        $programs = @('HELLO.COM','TELETYPE.COM','MZEXE.EXE','RUNNER.COM','VRAM.COM',
                      'UPCALL.COM','DTATEST.COM','I2FTEST.COM','DPMITEST.COM','FCBTEST.COM',
                      'INFO.COM','CLOCK.COM','HEX.COM','SUM.COM','CAL.COM','KEYTEST.COM',
                            'GFXTEST.COM','VESATEST.COM','DOS4TEST.COM')
        # the native SDK examples are PE images, not DOS programs: optional
        $nativePrograms = @()
        foreach ($p in @('HELLO64.EXE','HELLO32.EXE','FILET64.EXE','FILET32.EXE')) {
            if (Test-Path (Join-Path $Build $p)) { $nativePrograms += $p }
        }
        foreach ($prog in $programs) {
            if (-not (Test-Path (Join-Path $Build $prog))) { throw "missing generated program: $prog" }
        }
        $extraFiles = @()
        foreach ($prog in $programs) { $extraFiles += @('--file', "$Build/$prog=$prog") }
        foreach ($prog in $nativePrograms) { $extraFiles += @('--file', "$Build/$prog=$prog") }

        $autoexecLines = @(
            'VER','DIR','TYPE HELLO.TXT','HELLO','TELETYPE','MZEXE','RUNNER','VRAM',
            'APITEST','UPCALL','DTATEST','I2FTEST','DPMITEST','DOS4TEST',
            'MD TEMP','COPY HELLO.TXT TEMP\COPY.TXT','TYPE TEMP\COPY.TXT','DEL TEMP\COPY.TXT','RD TEMP',
            'ECHO WRITE-OK'
        )
        if ($Dos211Image -and (Test-Path $Dos211Image)) {
            $extraFiles += @('--file', "$Dos211Image=DOS211.IMG")
            $autoexecLines += @('MOUNT A: DOS211.IMG','DIR A:','COPY HELLO.TXT A:UP.TXT',
                                'TYPE A:UP.TXT','DEL A:UP.TXT','ECHO FLOPPY-WRITE-OK','ECHO FLOPPY-OK')
        } else {
            Write-Host 'note: UEFIDOS_DOS211 is not set, so the test image ships without a'
            Write-Host '      2.11 floppy: the MOUNT/floppy-write checks will be skipped.'
        }
        if ($nativePrograms -contains 'HELLO64.EXE') { $autoexecLines += @('RUN64 HELLO64.EXE') }
        if ($nativePrograms -contains 'HELLO32.EXE') { $autoexecLines += @('RUN64 HELLO32.EXE') }
        if ($nativePrograms -contains 'FILET64.EXE') { $autoexecLines += @('RUN64 FILET64.EXE -w NATBIG.TXT 3000','RUN64 FILET64.EXE -s NATBIG.TXT','RUN64 FILET32.EXE -w NATBIG.TXT 3000','RUN64 FILET32.EXE -s NATBIG.TXT','DEL NATBIG.TXT') }
        if (Test-Path (Join-Path $Build 'SECOND.img')) {
            $autoexecLines += @('DRIVES','DIR D:','TYPE D:\SECOND.TXT','ECHO SECOND-DRIVE-OK')
        }
        $autoexecLines += @('DIR *.TXT','CONFIG','FCBTEST','INFO','CAL','SUM HELLO.TXT','HEX HELLO.TXT','SHUTDOWN')

        $imported = Join-Path $Build 'imported'
        if (Test-Path $imported) {
            foreach ($f in Get-ChildItem $imported -File | Sort-Object Name) {
                $extraFiles += @('--file', "$($f.FullName)=$($f.Name)")
            }
        }
        # a second disk: proves D: enumeration (attached as a second IDE disk by
        # run_uefi.ps1 / run_tests.ps1 and mounted by dos_mount_all())
        $img2 = Join-Path $Build 'SECOND.img'
        & $Python (Join-Path $Root 'tools/make_image.py') $img2 --size-mb 48 --noboot `
            --text 'SECOND.TXT=this volume is D:, not C:' --autoexec 'REM second disk' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'second image build failed' }
        Write-Host "built: $img2"

        & $Python (Join-Path $Root 'tools/make_image.py') $img --boot $efi --boot-ia32 $efi32 --size-mb 64 `
            --text "HELLO.TXT=Hello UEFI-DOS!`n" --dir 'TESTDIR' --text 'TESTDIR/INNER.TXT=inner file' `
            --text 'CONFIG.SYS=FILES=20\nFCBS=4\nLASTDRIVE=D\nDEVICE=NONE\n' `
            @extraFiles `
            --autoexec ($autoexecLines -join "`n")
        if ($LASTEXITCODE -ne 0) { throw 'image build failed' }
        Write-Host "built: $img"
    }
} finally {
    Pop-Location
}
