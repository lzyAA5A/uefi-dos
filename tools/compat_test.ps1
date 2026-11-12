param(
    [string]$Image = "build/t_compat.img",
    [int]$TimeoutSec = 40,
    [string[]]$Programs,
    [string]$Kbd = "",
    [int]$MaxSteps = 40000000,
    [int]$KbdMaxMs = 2000,
    [switch]$ShowAll
)
# Compatibility sweep: run each DOS program under the UEFI-DOS host harness
# (same kernel/emulator code as the UEFI build) with a hard timeout and report
# what each one printed.  Output goes to temp files so a redrawing game cannot
# blow up the shell's memory.
$ErrorActionPreference = 'Stop'
$env:UEFIDOS_MAX_STEPS = "$MaxSteps"
$env:UEFIDOS_KBD_MAX_MS = "$KbdMaxMs"
if ($Kbd) { $env:UEFIDOS_KBD = $Kbd } else { Remove-Item Env:\UEFIDOS_KBD -ErrorAction SilentlyContinue }
$Root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $Root 'build\hosttest.exe'
if (-not (Test-Path $exe)) { throw "missing $exe (run build.ps1)" }
$imgPath = if ([System.IO.Path]::IsPathRooted($Image)) { $Image } else { Join-Path $Root $Image }
if (-not (Test-Path $imgPath)) { throw "missing image $imgPath" }

if (-not $Programs) {
    $Programs = @('SNAKE', 'MINE', 'G2048', 'MINIGAME', 'DIAG', 'VID', 'SCRDEMO',
                  'CPPTEST', 'C86HELLO', 'TE', 'MININASM', 'ASM', 'HI', 'VID2',
                  'SB', 'CP86TEST', 'TERM')
}

$outFile = Join-Path $Root 'build\compat_out.txt'
$errFile = Join-Path $Root 'build\compat_err.txt'
function Clean([string]$s) {
    if (-not $s) { return '' }
    $s = $s -replace "`e\[[0-9;?]*[A-Za-z]", ''
    $s = $s -replace "[\x00-\x08\x0b\x0c\x0e-\x1f]", ''
    return $s
}

$rows = @()
foreach ($p in $Programs) {
    if (Test-Path $outFile) { Remove-Item $outFile -Force }
    if (Test-Path $errFile) { Remove-Item $errFile -Force }
    $proc = Start-Process -FilePath $exe -ArgumentList '--shell', $imgPath, $p `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -NoNewWindow
    $status = ''
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        try { $proc.Kill($true) } catch { }
        $status = 'TIMEOUT'
    } else {
        $status = "exit=$($proc.ExitCode)"
    }
    $out = if (Test-Path $outFile) { Clean (Get-Content $outFile -Raw) } else { '' }
    $err = if (Test-Path $errFile) { Clean (Get-Content $errFile -Raw) } else { '' }
    # drop the harness framing lines
    $body = ($out -split "`n" | Where-Object { $_ -notmatch '^mount C: rc=|^>> |^SHELLDONE\s*$' }) -join "`n"
    $body = $body.Trim()
    $tail = ''
    if ($body -match 'exceeded instruction limit') { $tail = ' [hit instruction limit]' }
    if ($err.Trim()) { $tail += ' [stderr:' + ($err.Trim() -replace "`n", ' ') + ']' }
    $rows += [pscustomobject]@{ Prog = $p; Status = $status; Out = $body; Tail = $tail }
    Write-Host ('=== {0,-10} {1}{2}' -f $p, $status, $tail)
    $preview = $body
    if (-not $ShowAll -and $preview.Length -gt 900) { $preview = $preview.Substring(0, 900) + '...' }
    if ($preview) { Write-Host $preview }
    Write-Host ''
}
Write-Host '---- SUMMARY ----'
foreach ($r in $rows) {
    $first = ($r.Out -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
    if (-not $first) { $first = '(no output)' }
    Write-Host ('{0,-10} {1,-9} {2}{3}' -f $r.Prog, $r.Status, $first.Trim(), $r.Tail)
}
