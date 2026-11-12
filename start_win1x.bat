@echo off
rem Double-click launcher: builds build\win1x.img from a Windows 1.x source
rem directory and boots it under UEFI-DOS in QEMU.
rem
rem Put your extracted Windows 1.0 Developer Release 5 files into a folder
rem named "win1x-src" next to this file, or drag that folder onto this .bat.
setlocal
cd /d "%~dp0"

set "SRC=%~1"
if "%SRC%"=="" set "SRC=%~dp0win1x-src"
if not exist "%SRC%\" (
  echo.
  echo   Windows 1.x source directory not found:
  echo     %SRC%
  echo.
  echo   Extract your own copy of Windows 1.0 DR5 / 1.0x into that folder
  echo   ^(the release files themselves are not distributed with UEFI-DOS^),
  echo   or drag the folder onto this .bat file.
  echo.
  pause
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_win1x.ps1" -Source "%SRC%" %2 %3 %4 %5
if errorlevel 1 (
  echo.
  echo   Launch failed. See the message above.
  pause
)
