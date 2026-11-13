@echo off
rem Majoras-X: one double-click from your ROM to Majoras-X.iso.
rem
rem   1. put your Majora's Mask ROM in this folder, named baserom.z64
rem   2. run this file (double-click, or "build.bat" in a console)
rem
rem It finds a Python 3.8 or later and hands over to setup.py, which checks the
rem ROM, fetches 2Ship2Harkinian's asset definitions and extractor, builds the
rem extractor, extracts the game from the ROM and builds. Arguments are
rem passed on: build.bat --check, build.bat --skip-build.
setlocal EnableExtensions
set "MMX_SELF=%~f0"
if not defined PATH set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem"
cd /d "%~dp0"
title Majoras-X build

echo.
echo  Majoras-X -- Majora's Mask (2Ship2Harkinian) for the original Xbox
echo.

set "MMX_ROM="
rem --check reports a missing ROM itself, with everything else
if /i "%~1"=="--check" set "MMX_ROM=check"
for %%R in (baserom.z64 baserom.v64 baserom.n64 mm.z64 mm.v64 mm.n64) do (
  if not defined MMX_ROM if exist "%%R" set "MMX_ROM=%%R"
)
if not defined MMX_ROM (
  echo  Your ROM is not here. Copy your Majora's Mask ROM into
  echo      %~dp0
  echo  and name it  baserom.z64  then run this again.
  goto :fail
)

set "MMX_PY="
for %%V in (3.13 3.12 3.14 3.15 3.11 3.10 3.9 3.8) do (
  if not defined MMX_PY (
    py -%%V -c "import sys" >nul 2>nul && set "MMX_PY=py -%%V"
  )
)
if not defined MMX_PY (
  python -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>nul && set "MMX_PY=python"
)
if not defined MMX_PY (
  echo  No Python 3.8 or later was found. Install it from
  echo      https://www.python.org/downloads/
  echo  ^(tick "Add python.exe to PATH"^), then run this again.
  goto :fail
)

echo  Python: %MMX_PY%
echo.
%MMX_PY% setup.py %*
set "MMX_RC=%ERRORLEVEL%"
if not "%MMX_RC%"=="0" goto :failrc
echo.
echo  Done.
call :pause
exit /b 0

:failrc
echo.
echo  The build stopped (exit code %MMX_RC%). The messages above say why.
call :pause
exit /b %MMX_RC%

:fail
echo.
call :pause
exit /b 1

rem ---- keep a double-clicked window open --------------------------------
:pause
if defined MMX_NONINTERACTIVE exit /b 0
echo %cmdcmdline% | find /i "%MMX_SELF%" >nul
if not errorlevel 1 pause
exit /b 0
