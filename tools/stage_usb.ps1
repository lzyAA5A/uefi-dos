# NOTE: this is the versioned copy of build\stage_usb.ps1 (build\ is gitignored).
# The three Windows trees it copies are extracted install media and are NOT in
# the repository; point $w101 / $w104 / $w203 at your own copies.

# Stage the USB stick content in build\usb (what gets copied to the stick).
#
# Contents: UEFI-DOS 1.8.0 loaders, three Windows trees (1.01, 1.04, 2.03), the
# tools directory (autobanch + the test programs), and a README that also covers
# the "the BIOS will not boot it" cases (Secure Boot, CSM, boot-menu entry).
$ErrorActionPreference = 'Stop'
Set-Location 'F:\1231\UEFI-DOS'

$stage = 'build\usb'
$w101  = 'F:\1231\_tmp\win\w101mouse\WINDOWS'
$w104  = 'F:\1231\_tmp\win104\tree\WINDOWS'
$w203  = 'F:\1231\_tmp\w203full\WINDOWS'

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path "$stage\EFI\BOOT", "$stage\TOOLS" -Force | Out-Null

# --- boot loaders (built from the current source tree) ----------------------
Copy-Item build\BOOTX64.EFI, build\BOOTIA32.EFI "$stage\EFI\BOOT\" -Force

# --- the Windows trees, each in its own directory ---------------------------
foreach ($pair in @(@('W101', $w101), @('W104', $w104), @('W203', $w203))) {
    $dst = Join-Path $stage $pair[0]
    if (-not (Test-Path $pair[1])) { "MISSING tree: $($pair[1])"; continue }
    New-Item -ItemType Directory -Path $dst -Force | Out-Null
    Copy-Item (Join-Path $pair[1] '*') $dst -Recurse -Force
    "staged $($pair[0]) <- $($pair[1])"
}

# --- tools ------------------------------------------------------------------
$tools = @('AUTOBANC.COM', 'INFO.COM', 'CAL.COM', 'SUM.COM', 'HEX.COM', 'CLOCK.COM',
           'HELLO.COM', 'MOUSETST.COM', 'CGATEST.COM', 'CGATEXT.COM', 'VRAM.COM',
           'VESATEST.COM', 'GFXTEST.COM', 'KEYTEST.COM', 'DOS4TEST.COM')
foreach ($t in $tools) {
    $src = "build\$t"
    if (Test-Path $src) { Copy-Item $src "$stage\TOOLS\$t" -Force } else { "MISSING $src" }
}
# native 32/64-bit benchmarks (sdk/build_sdk.ps1)
foreach ($t in @('BENCH64.EXE', 'BENCH32.EXE')) {
    $src = "build\sdk\$t"
    if (Test-Path $src) { Copy-Item $src "$stage\TOOLS\$t" -Force } else { "MISSING $src" }
}

function WriteLf([string]$path, [string[]]$lines) {
    [System.IO.File]::WriteAllText((Join-Path (Get-Location) $path), (($lines -join "`n") + "`n"),
        (New-Object System.Text.ASCIIEncoding))
}

WriteLf "$stage\CONFIG.SYS" @(
    'FILES=40',
    'LASTDRIVE=D',
    'EXECHIGH=ON',
    'MOUSESPEED=2'
)

WriteLf "$stage\AUTOEXEC.BAT" @(
    'ECHO.',
    'ECHO   UEFI-DOS 1.10.1 on real hardware -- booted from this USB stick',
    'ECHO   ==========================================================',
    'ECHO.',
    'ECHO   BENCH    autobanch: 16 tests in 16/64-bit, saved to B+date.TXT',
    'ECHO   WIN101   Windows 1.01  (desktop + pointer)',
    'ECHO   WIN104   Windows 1.04  (desktop + pointer, needs SETVER 3.30)',
    'ECHO   WIN203   Windows 2.03  (286 -- still stops in its loader)',
    'ECHO   MOUSE    what the firmware says about the mouse (10 s probe)',
    'ECHO   TOOLS\   test programs (MOUSETST, CGATEST, VRAM, VESATEST ...)',
    'ECHO   TYPE README.TXT',
    'ECHO.',
    'PATH \TOOLS',
    'SETVER WIN.COM 3.30',
    'SETVER WIN100.BIN 3.30',
    'SETVER MSDOS.EXE 3.30'
)

# The shell's batch runner has no IF and skips GOTO lines (see run_batch_text in
# src/shell/shell.c), so these stay straight-line: change directory, start WIN.
WriteLf "$stage\WIN101.BAT" @(
    'ECHO Starting Windows 1.01 ...',
    'CD \W101',
    'WIN'
)

WriteLf "$stage\WIN104.BAT" @(
    'ECHO Starting Windows 1.04 ...',
    'CD \W104',
    'WIN'
)

WriteLf "$stage\WIN203.BAT" @(
    'ECHO Starting Windows 2.03 (286) ...',
    'CD \W203',
    'WIN'
)

# One file, three views of the same machine: 16-bit through the emulator,
# then the same box natively in 64 bits.  Both append to B<yymmdd>.TXT, so
# the timings end up on one page.
WriteLf "$stage\BENCH.BAT" @(
    'ECHO ==== UEFI-DOS performance probe ====',
    'ECHO   16-bit real mode (this is the Windows 1.x path) ...',
    'TOOLS\AUTOBANC',
    'ECHO',
    'ECHO   64-bit native ...',
    'RUN64 TOOLS\BENCH64.EXE',
    'ECHO',
    'ECHO   BENCH32.EXE is the 32-bit twin, but a 64-bit kernel runs only',
    'ECHO   64-bit images: boot BOOTIA32.EFI to use it.',
    'ECHO',
    'ECHO ==== results file ====',
    'TYPE B*.TXT'
)

$readme = @(
    'UEFI-DOS -- USB stick',
    '=====================',
    '',
    'This stick boots UEFI-DOS on x86-64 or x86 UEFI firmware:',
    '  EFI\BOOT\BOOTX64.EFI   64-bit firmware',
    '  EFI\BOOT\BOOTIA32.EFI  32-bit firmware',
    'The firmware is the only requirement: no BIOS, no CSM, no install,',
    'no boot manager.  The rest of the stick is C:.',
    '',
    'WHAT IS ON IT',
    '  W101\   Windows 1.01   -- desktop and pointer work',
    '  W104\   Windows 1.04   -- desktop and pointer work (the SETVER 3.30',
    '                           lines are already in AUTOEXEC.BAT)',
    '  W203\   Windows 2.03   -- 286 build; its loader still faults, see below',
    '  TOOLS\  AUTOBANC.COM   autobanch, the performance test',
    '          MOUSETST.COM   INT 33h mouse test',
    '          CGATEST.COM    CGA 320x200x4 colour test',
    '          VRAM.COM       video RAM / mode dump',
    '          VESATEST.COM   VESA BIOS extension report',
    '          BENCH.BAT / WIN101.BAT / WIN104.BAT / WIN203.BAT',
    '',
    'IF YOU SEE "Secure Boot Violation / Invalid signature detected"',
    '---------------------------------------------------------------',
    'That is the firmware refusing this loader because it is not signed by',
    'Microsoft -- and it is good news about everything else: the firmware found',
    'EFI\BOOT\BOOTX64.EFI on this stick and tried to start it, so the filesystem,',
    'the partition and the boot entry are all fine.  Only the signature failed.',
    '',
    'ASUS TUF / ROG laptop (TUF Gaming F16, "TianXuan 6 Pro"):',
    '  power on and tap F2, press F7 for Advanced Mode, then',
    '    Security -> Secure Boot -> Secure Boot Control = Disabled',
    '  and F10 to save and exit.  Plug the stick in again and pick',
    '  "UEFI: 1401" in the boot menu (F8 on ASUS).',
    '',
    'If "Secure Boot Control" is greyed out, in this order:',
    '  a) Boot menu -> Fast Boot = Disabled, save, back to Security.',
    '  b) Security -> Secure Boot -> Key Management -> "Delete all Secure Boot',
    '     Keys" (this puts the firmware in Setup Mode, where unsigned loaders',
    '     run), save, then try the stick again.',
    '  c) Security -> Secure Boot -> "Secure Boot Mode" = Custom, then back to',
    '     Standard with Control = Disabled.',
    '',
    'Windows is otherwise unaffected; re-enabling Secure Boot later restores the',
    'old behaviour ("Restore Factory Keys").  If this laptop uses BitLocker or',
    'Windows device encryption, have the recovery key at hand before changing',
    'Secure Boot settings.',
    '',
    'IF THE BIOS DOES NOT OFFER THE STICK',
    '------------------------------------',
    '1. SECURE BOOT.  This is the usual reason.  BOOTX64.EFI is not signed by',
    '   Microsoft, so a Secure Boot machine refuses it without saying much.',
    '   Turn Secure Boot OFF (Security -> Secure Boot -> Disabled), or put the',
    '   firmware in "Other OS" / setup-mode trust, then try again.',
    '2. CSM / LEGACY.  UEFI-DOS is a pure UEFI application: there is no boot',
    '   sector, so Legacy/CSM-only mode cannot start it.  Set the firmware to',
    '   UEFI mode (CSM off, "Boot mode: UEFI only").',
    '3. PICK THE "UEFI:" ENTRY.  The boot menu usually lists the stick twice:',
    '   once as "UEFI: 1401" (this one) and once as a legacy device (which',
    '   cannot work).  Some firmware hides the UEFI entry until CSM is off.',
    '4. FAST BOOT / USB.  With Fast Boot on, some boards skip USB enumeration.',
    '   Turn Fast Boot off and/or enable "USB legacy support" for the setup',
    '   screens only.',
    '5. PORT.  Front-panel USB3 ports on some boards are not enumerated by the',
    '   firmware before the boot menu; use a rear (chipset) port, ideally USB2.',
    '6. PARTITION TYPE.  This volume is FAT32.  A few firmwares only boot from',
    '   a partition whose type byte is EFh (EFI System) rather than 0Ch',
    '   (FAT32 LBA); if everything above fails, reformat with a tool that makes',
    '   an EFI System Partition, or patch the MBR type byte to EF.',
    '',
    'WHAT IT CAN DO ON REAL HARDWARE',
    '  video   the firmware own GOP mode is used; a 640x200 CGA picture is',
    '          upscaled (integer scale, up to 4x) and centred 4:3',
    '  mouse   EFI_SIMPLE_POINTER_PROTOCOL, i.e. any USB mouse the firmware',
    '          supports, presented to programs as a Microsoft bus mouse',
    '          (MOUSETST / Windows 1.x pointer)',
    '  colour  CGA 320x200x4 colours, plus text',
    '  speed   run BENCH: 16-bit, 32-bit and 64-bit probes, one result file',
    '',
    'IF THE MOUSE DOES NOT MOVE',
    '--------------------------',
    'Type MOUSE at the C: prompt.  It prints, first, what the firmware',
    'published:',
    '  simple-pointer=1 (in use)   the firmware gave us a relative pointer --',
    '                              the usual answer for a USB mouse',
    '  absolute-pointer=1         an absolute device (tablet, some touchpads)',
    '  both 0                     no pointing device at all in the firmware',
    'Then it lists each device it holds (moves=, last=) -- that table is what',
    'says WHICH handle is moving -- and for ten seconds prints one line per',
    'second (events / moving / total dx,dy / buttons).  Move the mouse and',
    'click while it runs.',
    '',
    'The table is printed BEFORE and AFTER the ten seconds, so one run tells you',
    'what each device did.  last= is what we pass to the guest, raw= is what the',
    'firmware itself reported, and the last line says whether the PS/2 fallback',
    '(the i8042 controller) decoded any packet.',
    '',
    'If the pointer moves but jumps around: only ONE device moves the pointer',
    'per poll (the one that moved most), because firmware often publishes the',
    'same mouse twice and adding both doubled every movement.  A handle that',
    'repeats the same counts for a third of a second is treated as stuck and',
    'ignored.',
    '',
    '  counts appear      the firmware side is fine; the problem is then the',
    '                     guest side -- report the numbers',
    '  one axis only      every count arrives on X and Y stays 0: that was a',
    '                     wrong structure layout in 1.10.0 and is fixed here',
    '  no protocol (0)    the firmware never initialised the mouse: Fast Boot',
    '                     off, plug the mouse in before powering on, and avoid',
    '                     hubs and USB-C docks for the test',
    '  protocol but 0      the driver was bound to a device that is not the',
    '                     mouse (typically the built-in I2C touchpad).  Plug in',
    '                     a USB mouse and reboot so the firmware scans it.',
    '',
    'A laptop touchpad wired to I2C is not reachable from a UEFI application:',
    'the firmware usually does not publish it, and it never reaches the PS/2',
    'controller either, so a real USB mouse is the reliable answer.',
    '',
    'BENCHMARKS -- WHERE THE NUMBERS GO',
    '----------------------------------',
    'BENCH runs the probes and writes every figure into ONE file on this stick,',
    'named B plus today s date (B261128.TXT for 28 Nov 2026):',
    '',
    '  TOOLS\AUTOBANC.COM   16-bit real mode, the Windows 1.x path.  Sixteen',
    '                       one-second windows: ALU, MUL, DIV, memory copy /',
    '                       fill / read, INT 21h calls, disk read, x87 FPU',
    '                       chain, FSQRT, a 640x200 frame written by hand, CGA',
    '                       text fill, sieve of Eratosthenes, insertion sort,',
    '                       CRC16, 8x8 matrix multiply',
    '  TOOLS\BENCH64.EXE    64-bit native (started with RUN64), TSC-timed:',
    '                       ALU, MUL, DIV, copy, fill, double FPU chain, Newton',
    '                       sqrt, sieve to 65536, insertion sort 1024, CRC32,',
    '                       32x32 matrix multiply',
    '  TOOLS\BENCH32.EXE    the 32-bit twin of BENCH64, but a 64-bit kernel',
    '                       runs only 64-bit images: it needs the 32-bit',
    '                       loader (EFI\BOOT\BOOTIA32.EFI)',
    '',
    'TYPE B*.TXT prints the file (TYPE accepts wildcards).  Compare two machines',
    'line by line; AUTOBANC total summed rate is a rough single figure.',
    '',
    'ABOUT WINDOWS 2.03',
    '  The 2.03 media is the installer set: WIN.COM and SYSTEM.INI are created',
    '  by SETUP, so this directory was assembled by hand (CGA machine profile).',
    '  WIN.COM starts and loads WIN200.BIN, but its real-mode init copies data',
    '  to segment 0 and destroys the interrupt vector table.  Expect a stop;',
    '  W101 and W104 are the working ones.',
    '',
    'UEFI-DOS is MIT licensed. This stick is disposable: rewriting it is fine.'
)
Set-Content -Path "$stage\README.TXT" -Value $readme -Encoding Ascii

$files = Get-ChildItem $stage -Recurse -File
"staged total: $($files.Count) files, " + [math]::Round((($files | Measure-Object Length -Sum).Sum / 1MB), 2) + " MB"
