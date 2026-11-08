@echo off
rem ===========================================================================
rem  UEFI-DOS quick launcher -- boots UEFIDOS.IMG under OVMF (UEFI) in QEMU.
rem  Double-click it, or run:  start.bat [-display none] [-serial file:log.txt]
rem  Extra arguments are passed straight through to qemu-system-x86_64.exe.
rem
rem  Requires QEMU for Windows (it ships the OVMF firmware files):
rem      winget install SoftwareFreedomConservancy.QEMU
rem  Environment overrides: UEFIDOS_QEMU, UEFIDOS_OVMF_DIR
rem ===========================================================================
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "IMG=%~dp0UEFIDOS.IMG"
if not exist "%IMG%" set "IMG=%~dp0build\UEFIDOS.img"
if not exist "%IMG%" (
    echo [UEFI-DOS] UEFIDOS.IMG not found next to this script or in build\.
    echo             Build it first with:  powershell -ExecutionPolicy Bypass -File build.ps1
    if "%~1"=="" pause
    exit /b 1
)

rem ---- locate qemu-system-x86_64.exe -------------------------------------
set "QEMU=%UEFIDOS_QEMU%"
if not defined QEMU if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" set "QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe"
if not defined QEMU for %%I in (qemu-system-x86_64.exe) do set "QEMU=%%~$PATH:I"
if not defined QEMU (
    echo [UEFI-DOS] QEMU was not found.  Install it first:
    echo.
    echo     winget install SoftwareFreedomConservancy.QEMU
    echo.
    echo   or download it from https://www.qemu.org/download/#windows
    echo   or set UEFIDOS_QEMU to the full path of qemu-system-x86_64.exe
    if "%~1"=="" pause
    exit /b 1
)
for %%D in ("%QEMU%") do set "QEMUDIR=%%~dpD"

rem ---- locate the OVMF firmware that QEMU ships ---------------------------
set "FWDIR=%UEFIDOS_OVMF_DIR%"
if not defined FWDIR if exist "%~dp0firmware\edk2-x86_64-code.fd" set "FWDIR=%~dp0firmware"
if not defined FWDIR if exist "!QEMUDIR!share\edk2-x86_64-code.fd" set "FWDIR=!QEMUDIR!share"
if not defined FWDIR if exist "C:\Program Files\qemu\share\edk2-x86_64-code.fd" set "FWDIR=C:\Program Files\qemu\share"
if not defined FWDIR (
    echo [UEFI-DOS] OVMF firmware ^(edk2-x86_64-code.fd^) was not found.
    echo             It normally sits in QEMU's share\ directory, or drop the
    echo             two .fd files into a firmware\ folder next to this script.
    echo             Alternatively set UEFIDOS_OVMF_DIR.
    if "%~1"=="" pause
    exit /b 1
)

rem ---- the vars file must be writable, so boot from a private copy ---------
set "VARS=%TEMP%\uefidos-vars-%RANDOM%.fd"
copy /y "!FWDIR!\edk2-i386-vars.fd" "!VARS!" >nul 2>&1
if errorlevel 1 (
    echo [UEFI-DOS] cannot copy edk2-i386-vars.fd out of "!FWDIR!"
    if "%~1"=="" pause
    exit /b 1
)

echo [UEFI-DOS] qemu  : %QEMU%
echo [UEFI-DOS] fw    : !FWDIR!
echo [UEFI-DOS] image : %IMG%
echo [UEFI-DOS] booting ... (close the QEMU window, or type SHUTDOWN in the guest)

"%QEMU%" -m 256 -smp 1 -machine pc ^
    -drive if=pflash,format=raw,readonly=on,file="!FWDIR!\edk2-x86_64-code.fd" ^
    -drive if=pflash,format=raw,file="!VARS!" ^
    -drive file="%IMG%",if=ide,index=0,media=disk,format=raw ^
    -net none %*
set "RC=%ERRORLEVEL%"

del "!VARS!" >nul 2>&1
echo [UEFI-DOS] qemu exited with code %RC%
if "%~1"=="" pause
exit /b %RC%
