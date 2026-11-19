# UEFI-DOS benchmark driver: run the same eight workloads in the 16-bit DOS VM,
# in a 32-bit native image and in a 64-bit native image, then print and save the
# comparison.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File bench/run_bench.ps1
#   ... -Runs 5            more repetitions (the median is reported)
#   ... -SkipUefi          host harness only (dos16 + native64)
#   ... -Accel tcg         force TCG instead of WHPX for the UEFI 32-bit run
#
# Why two environments: a 64-bit UEFI-DOS build runs only 64-bit native images
# and the IA32 build only 32-bit ones (bitness is not emulated), so the 32-bit
# native world is measured with the IA32 image under QEMU.  WHPX runs the guest
# on the real CPU, which is what "native" has to mean; with TCG the numbers are
# QEMU's interpreter and only the ratios inside one run are meaningful.
param(
    [int]$Runs = 3,
    [switch]$SkipUefi,
    [string]$Accel = 'whpx',
    [string]$OutFile = 'bench/results.md',
    [switch]$ReuseHostLogs,
    [int]$UefiTimeout = 900
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $Root
$Build = Join-Path $Root 'build'
$HostExe = Join-Path $Build 'hosttest.exe'
$Qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
$Fw = 'C:\Program Files\qemu\share'
$Python = if ($env:UEFIDOS_PYTHON) { $env:UEFIDOS_PYTHON } else { 'python' }
# one BIOS tick is 65536/1193182 s
$TickUs = 54925.4
$Workloads = @('alu', 'fill', 'copy', 'scan', 'api', 'text', 'file', 'find')
# keep these in sync with bench/make_bench16.py (they generate bench/bench_sizes.h)
$FileBytes = 132096          # BENCH_FILE_N
$FindEntries = 10 * 120      # BENCH_FIND_R * number of files in \BDATA
$TextChars = 3145536         # BENCH_TEXT_N (TEXT_INNER * TEXT_OUTER)

function Write-Step($msg) { Write-Host ""; Write-Host "== $msg ==" }

# ---------------------------------------------------------------- build -----
Write-Step 'build'
if (-not (Test-Path (Join-Path $Build 'BOOTX64.EFI'))) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'build.ps1')
}
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'bench/build_bench.ps1')

$tsc = [double](& (Join-Path $Build 'tscfreq.exe'))
if ($tsc -le 0) { throw 'tscfreq failed' }
Write-Host ("TSC frequency: {0:N0} Hz ({1:N2} GHz)" -f $tsc, ($tsc / 1e9))

# ------------------------------------------------------------ parsing -------
function Parse-Bench([string]$text) {
    # returns @{ world = @{ workload = @{ us = <double>; cs = <string> } } }
    $res = @{}
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -notmatch 'BENCH\s+(\w+)\s+(tick|cycles|centi)=(\d+)\s+cs=([0-9A-Fa-f]{4})') { continue }
        $name = $Matches[1]; $unit = $Matches[2]; $val = [double]$Matches[3]; $cs = $Matches[4].ToUpper()
        if ($name -notin $Workloads) { continue }
        # centi = INT 21h AH=2Ch (10 ms), tick = legacy INT 1Ah, cycles = native RDTSC
        $us = if ($unit -eq 'centi') { $val * 10000.0 } elseif ($unit -eq 'tick') { $val * $TickUs } else { $val * 1e6 / $tsc }
        if (-not $res.ContainsKey($name)) { $res[$name] = @() }
        $res[$name] += , @{ us = $us; cs = $cs }
    }
    return $res
}

function Median([double[]]$v) {
    if ($v.Count -eq 0) { return [double]::NaN }
    $s = $v | Sort-Object
    if ($s.Count % 2) { return [double]$s[[int](($s.Count - 1) / 2)] }
    return ([double]$s[$s.Count / 2 - 1] + [double]$s[$s.Count / 2]) / 2.0
}

# ------------------------------------------------------- host harness -------
# dos16 + native64, on the real CPU, one process per run
$hostDos = @{}; $hostNat64 = @{}
if (-not $ReuseHostLogs) {
    for ($r = 1; $r -le $Runs; $r++) {
        Write-Step "host harness run $r/$Runs (dos16 + native64)"
        $log = Join-Path $Build "bench_host_$r.log"
        $env:UEFIDOS_HARD_STEPS = '4000000000'
        & $HostExe --shell (Join-Path $Build 'BENCH.IMG') 'BENCH16' 'RUN64 BENCH64.EXE' *> $log
        Remove-Item Env:UEFIDOS_HARD_STEPS -ErrorAction SilentlyContinue
        $text = [System.IO.File]::ReadAllText($log)
        # BENCH16 prints before the RUN64 line, so split the log there
        $split = $text.IndexOf('RUN64: loading')
        if ($split -lt 0) { throw "harness log $log has no RUN64 section" }
        $dos = Parse-Bench $text.Substring(0, $split)
        $nat = Parse-Bench $text.Substring($split)
        foreach ($k in $dos.Keys) { if (-not $hostDos.ContainsKey($k)) { $hostDos[$k] = @() }; $hostDos[$k] += , $dos[$k][0] }
        foreach ($k in $nat.Keys) { if (-not $hostNat64.ContainsKey($k)) { $hostNat64[$k] = @() }; $hostNat64[$k] += , $nat[$k][0] }
        Write-Host ("  dos16: {0} workloads, native64: {1} workloads" -f $dos.Count, $nat.Count)
    }
}

if ($ReuseHostLogs) {
    $hostDos = @{}; $hostNat64 = @{}
    foreach ($log in (Get-ChildItem (Join-Path $Build 'bench_host_*.log') | Sort-Object Name)) {
        $text = [System.IO.File]::ReadAllText($log.FullName)
        $split = $text.IndexOf('RUN64: loading')
        if ($split -lt 0) { continue }
        $dos = Parse-Bench $text.Substring(0, $split)
        $nat = Parse-Bench $text.Substring($split)
        foreach ($k in $dos.Keys) { if (-not $hostDos.ContainsKey($k)) { $hostDos[$k] = @() }; $hostDos[$k] += , $dos[$k][0] }
        foreach ($k in $nat.Keys) { if (-not $hostNat64.ContainsKey($k)) { $hostNat64[$k] = @() }; $hostNat64[$k] += , $nat[$k][0] }
        Write-Host ("  re-used {0}: dos16 {1} / native64 {2} workloads" -f $log.Name, $dos.Count, $nat.Count)
    }
}

# ------------------------------------------------------------- UEFI ---------
# native32 needs the IA32 build; run both bitnesses under QEMU so the 32- and
# 64-bit native numbers come from the same (accelerated) environment.
function New-BenchImage([string]$path, [string]$bits, [int]$sizeMb) {
    $boot = Join-Path $Build ($(if ($bits -eq '64') { 'BOOTX64.EFI' } else { 'BOOTIA32.EFI' }))
    $bootIa = Join-Path $Build ($(if ($bits -eq '64') { 'BOOTIA32.EFI' } else { 'BOOTX64.EFI' }))
    $auto = "BENCH16`nRUN64 BENCH$bits.EXE`nECHO BENCH-DONE`n"
    $imgArgs = @('tools\make_image.py', $path, '--size-mb', "$sizeMb", '--boot', $boot,
                 '--boot-ia32', $bootIa, '--autoexec', $auto,
                 '--file', ('{0}=BENCH16.COM' -f (Join-Path $Build 'BENCH16.COM')),
                 '--file', ('{0}=BENCH{1}.EXE' -f (Join-Path $Build "BENCH$bits.EXE"), $bits))
    for ($i = 1; $i -le 120; $i++) { $imgArgs += @('--text', ('BDATA/D{0:D3}.TXT=payload' -f $i)) }
    & $Python @imgArgs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "image build failed for $img" }
}

function Read-Shared([string]$path) {
    # QEMU keeps its -serial file open, so a plain ReadAllText would fail
    try {
        $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open,
                                     [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $sr = New-Object System.IO.StreamReader($fs)
        $t = $sr.ReadToEnd()
        $sr.Dispose(); $fs.Dispose()
        return $t
    } catch { return '' }
}

function Invoke-UefiRun([string]$bits, [string]$tag) {
    $img = Join-Path $Build "BENCH$bits.IMG"
    New-BenchImage $img $bits 64
    $vars = Join-Path $Build "vars-bench$bits.fd"
    Copy-Item (Join-Path $Fw 'edk2-i386-vars.fd') $vars -Force
    $code = if ($bits -eq '64') { 'edk2-x86_64-code.fd' } else { 'edk2-i386-code.fd' }
    $serial = Join-Path $Build "bench_uefi_$tag.log"
    Remove-Item $serial -ErrorAction SilentlyContinue
    $qargs = @('-m', '256', '-smp', '1', '-machine', 'pc', '-accel', $Accel,
               '-drive', ('if=pflash,format=raw,readonly=on,file=' + (Join-Path $Fw $code)),
               '-drive', ('if=pflash,format=raw,file=' + $vars),
               '-drive', ('file=' + $img + ',if=ide,index=0,media=disk,format=raw'),
               '-boot', 'menu=off', '-net', 'none', '-display', 'none',
               '-serial', ('file:' + $serial))
    $cmdFile = Join-Path $Build ("run_qemu_$tag.cmd")
    $quoted = ($qargs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    Set-Content -Path $cmdFile -Encoding ASCII -Value @(
        '@echo off',
        ('cd /d "{0}"' -f $Root),
        ('"{0}" {1} > build\bench_uefi_stderr.log 2>&1' -f $Qemu, $quoted))
    $job = Start-Process -FilePath $cmdFile -PassThru -WindowStyle Hidden
    $deadline = (Get-Date).AddSeconds($UefiTimeout)
    $done = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        if (Test-Path $serial) {
            $txt = Read-Shared $serial
            if ($txt -match 'BENCH-DONE') { $done = $true; break }
        }
        if ($job.HasExited) { break }
    }
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
    try { $job.Kill() } catch { }
    Start-Sleep -Seconds 1
    $text = if (Test-Path $serial) { Read-Shared $serial } else { '' }
    if (-not $done) { Write-Host "  WARNING: $tag did not reach BENCH-DONE" }
    return $text
}

$uefiDos = @{}; $uefiNat = @{}
if (-not $SkipUefi) {
    foreach ($bits in @('64', '32')) {
        Write-Step "UEFI $bits-bit image ($Accel): dos16 + native$bits"
        $text = Invoke-UefiRun $bits "x$bits"
        $split = $text.IndexOf('RUN64: loading')
        $dos = Parse-Bench $(if ($split -gt 0) { $text.Substring(0, $split) } else { $text })
        $nat = Parse-Bench $(if ($split -gt 0) { $text.Substring($split) } else { '' })
        Write-Host ("  dos16: {0} workloads, native{1}: {2} workloads" -f $dos.Count, $bits, $nat.Count)
        foreach ($k in $dos.Keys) { if (-not $uefiDos.ContainsKey($k)) { $uefiDos[$k] = @() }; $uefiDos[$k] += , $dos[$k][0] }
        if ($bits -eq '32') {
            foreach ($k in $nat.Keys) { if (-not $uefiNat.ContainsKey($k)) { $uefiNat[$k] = @() }; $uefiNat[$k] += , $nat[$k][0] }
        }
    }
}

# ------------------------------------------------------------- report -------
function Us-Median($table, $name) {
    if (-not $table.ContainsKey($name) -or $table[$name].Count -eq 0) { return [double]::NaN }
    return Median ([double[]]($table[$name] | ForEach-Object { $_.us }))
}

function Per-Unit($name, [double]$us) {
    switch ($name) {
        'alu'  { return $us * 1000.0 / 655360 }              # ns per LCG step
        'fill' { return (192 * 32768) / ($us / 1e6) / 1e6 }  # MB/s
        'copy' { return (144 * 16384) / ($us / 1e6) / 1e6 }  # MB/s
        'scan' { return (96 * 32768) / ($us / 1e6) / 1e6 }   # MB/s
        'api'  { return $us * 1000.0 / 524256 }              # ns per call
        'text' { return $us * 1000.0 / $TextChars }             # ns per char
        'file' { return ($FileBytes * 2) / ($us / 1e6) / 1e6 }  # MB/s (write+read)
        'find' { return $us * 1000.0 / $FindEntries }           # ns per directory entry
    }
    return $us
}

$unit = @{ alu = 'ns/step'; fill = 'MB/s'; copy = 'MB/s'; scan = 'MB/s';
           api = 'ns/call'; text = 'ns/char'; file = 'MB/s'; find = 'ns/entry' }

$rows = @()
foreach ($w in $Workloads) {
    $d = Us-Median $hostDos $w
    $n64 = Us-Median $hostNat64 $w
    $n32 = Us-Median $uefiNat $w
    $dU = Us-Median $uefiDos $w
    $rows += [pscustomobject]@{
        workload = $w; unit = $unit[$w]
        dos16_us = $d; native64_us = $n64; native32_us = $n32
        dos16_uefi_us = $dU
        speedup64 = if ($d -gt 0 -and $n64 -gt 0) { $d / $n64 } else { [double]::NaN }
        # compare within one environment: the 32-bit native world only exists in the IA32 UEFI image
        speedup32 = if ($dU -gt 0 -and $n32 -gt 0) { $dU / $n32 } else { [double]::NaN }
        # the host DOS world cannot time file/find: they finish below one 10 ms clock tick
        rate_dos = if ($d -gt 0) { Per-Unit $w $d } else { [double]::NaN }
        rate_n64 = Per-Unit $w $n64
        rate_n32 = Per-Unit $w $n32
    }
}

$sb = New-Object System.Text.StringBuilder
function Add-Line($s) { [void]$sb.AppendLine($s) }
Add-Line '# UEFI-DOS benchmark results'
Add-Line ''
Add-Line ('Generated {0}; TSC {1:N0} Hz; host harness runs: {2}; UEFI accelerator: {3}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm'), $tsc, $Runs, $(if ($SkipUefi) { 'skipped' } else { $Accel }))
Add-Line ''
Add-Line '| workload | unit | 16-bit DOS VM | native 64-bit | native 32-bit | 64-bit vs DOS | 32-bit vs DOS |'
Add-Line '| --- | --- | ---: | ---: | ---: | ---: | ---: |'
foreach ($r in $rows) {
    $fmt = { param($v) if ([double]::IsNaN($v)) { '-' } else { $v.ToString('N1') } }
    Add-Line ('| {0} | {1} | {2} | {3} | {4} | {5} | {6} |' -f $r.workload, $r.unit,
              (& $fmt $r.rate_dos), (& $fmt $r.rate_n64), (& $fmt $r.rate_n32),
              (& $fmt $r.speedup64), (& $fmt $r.speedup32))
}
Add-Line ''
Add-Line 'Time per workload (microseconds, median of the runs):'
Add-Line ''
Add-Line '| workload | 16-bit DOS VM (host) | native 64-bit (host) | 16-bit DOS VM (UEFI) | native 32-bit (UEFI) |'
Add-Line '| --- | ---: | ---: | ---: | ---: |'
foreach ($r in $rows) {
    $fmt = { param($v) if ([double]::IsNaN($v)) { '-' } else { $v.ToString('N0') } }
    Add-Line ('| {0} | {1} | {2} | {3} | {4} |' -f $r.workload, (& $fmt $r.dos16_us),
              (& $fmt $r.native64_us), (& $fmt $r.dos16_uefi_us), (& $fmt $r.native32_us))
}
$null = New-Item -ItemType Directory -Force (Split-Path -Parent (Join-Path $Root $OutFile))
[System.IO.File]::WriteAllText((Join-Path $Root $OutFile), $sb.ToString())
Write-Host ''
Write-Host $sb.ToString()
Write-Host ("results written to {0}" -f (Join-Path $Root $OutFile))
