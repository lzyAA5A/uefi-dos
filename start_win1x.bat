@echo off
rem Double-click launcher for Windows 1.x on UEFI-DOS.
rem
rem It finds the Windows media by itself, in this order:
rem   1. a folder dragged onto this .bat (or passed as the first argument)
rem   2. a folder named "win1x-src" next to this file
rem   3. the already extracted media this project was developed against
rem and it prefers an *installed* C:\WINDOWS tree when one is around, because
rem that is what actually starts the kernel.
rem
rem The Microsoft release files are not distributed with UEFI-DOS; they are only
rem read on this machine to build build\win1x.img (git-ignored).
setlocal
cd /d "%~dp0"

set "SRC=%~1"
if "%SRC%"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_win1x.ps1" %2 %3 %4 %5
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_win1x.ps1" -Source "%SRC%" %2 %3 %4 %5
)
if errorlevel 1 (
  echo.
  echo   Launch failed. See the message above.
  echo.
  echo   Quick fixes:
  echo     * put an extracted Windows 1.x media folder next to this file as
  echo       "win1x-src", or drag that folder onto this .bat
  echo     * for Windows 1.01 the SETUP step has to run once: see
  echo       docs\windows1x.md, then pass -Installed ^<dir^> with WINDOWS\WIN100.BIN
  echo.
  pause
)
