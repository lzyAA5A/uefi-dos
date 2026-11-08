@echo off
rem ============================================================
rem  UEFI-DOS quick launcher
rem  Double-click to boot UEFI-DOS in a QEMU window.
rem  Ends at the C:\> shell prompt (type HELP / SHUTDOWN).
rem  Options:  -Headless        console-only run (serial log)
rem            -Firmware ia32   32-bit payload
rem ============================================================
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_interactive.ps1" %*
if errorlevel 1 (
    echo.
    echo [UEFI-DOS] start failed - see the error above.
    pause
)
