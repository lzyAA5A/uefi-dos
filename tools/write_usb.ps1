# NOTE: this is the versioned copy of build\stage_usb.ps1 (build\ is gitignored).
# The three Windows trees it copies are extracted install media and are NOT in
# the repository; point $w101/$w104/$w203 at your own copies.
# Write the staged UEFI-DOS + Windows 1.01 + autobanch tree to the USB stick (D:).
# The stick held a Hackintosh (OpenCore) installation; the user authorized wiping it.
$ErrorActionPreference = 'Continue'
$target = 'D:\'

# --- safety checks: this must be the removable FAT32 stick -------------------
$d = [System.IO.DriveInfo]::new('D')
"drive      : $($d.Name)"
"label      : $($d.VolumeLabel)"
"format     : $($d.DriveFormat)"
"type       : $($d.DriveType)"
"size GB    : " + [math]::Round($d.TotalSize/1GB,2)
"free GB    : " + [math]::Round($d.AvailableFreeSpace/1GB,2)
if ($d.DriveType -ne 'Removable' -or $d.DriveFormat -ne 'FAT32') {
    'REFUSING: not the expected removable FAT32 stick'; return
}

$src = 'F:\1231\UEFI-DOS\build\usb'
if (-not (Test-Path (Join-Path $src 'EFI\BOOT\BOOTX64.EFI'))) { 'REFUSING: staged tree looks wrong'; return }
$files = Get-ChildItem $src -Recurse -File
"staged      : $($files.Count) files, " + [math]::Round((($files | Measure-Object Length -Sum).Sum/1MB),2) + " MB"

# --- wipe --------------------------------------------------------------------
'--- removing old content ---'
Get-ChildItem $target -Force | Where-Object { $_.Name -ne 'System Volume Information' } | ForEach-Object {
    try { Remove-Item $_.FullName -Recurse -Force -ErrorAction Stop; "removed $($_.Name)" }
    catch { "could not remove $($_.Name): $($_.Exception.Message)" }
}

# --- copy --------------------------------------------------------------------
'--- copying ---'
Copy-Item (Join-Path $src '*') $target -Recurse -Force
$copied = Get-ChildItem $target -Recurse -File -Force | Where-Object { $_.Name -ne '' }
"copied      : $($copied.Count) files, " + [math]::Round((($copied | Measure-Object Length -Sum).Sum/1MB),2) + " MB"

# --- verify byte-for-byte sizes ---------------------------------------------
$bad = 0
foreach ($f in $files) {
    $rel = $f.FullName.Substring($src.Length + 1)
    $dstFile = Join-Path $target $rel
    if (-not (Test-Path $dstFile)) { "MISSING ON STICK: $rel"; $bad++; continue }
    if ((Get-Item $dstFile).Length -ne $f.Length) { "SIZE MISMATCH: $rel"; $bad++ }
}
"verify      : $bad problem(s)"
'--- stick root ---'
Get-ChildItem $target -Force | Select-Object Name, Length | Format-Table -AutoSize
'--- stick tree (dirs) ---'
Get-ChildItem $target -Recurse -Directory -Force | Select-Object -ExpandProperty FullName
