@echo off
REM ====================================================================
REM  ovc desktop build  --  MSVC (Ninja) + Qt (QTDIR) + vcpkg (libgit2)
REM  Usage:  build.bat [Release-x64 | Debug-x64]   (default: Release-x64)
REM  Run from any shell; it sets up the MSVC environment itself.
REM ====================================================================
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=Release-x64"

REM -- locate vcvars64.bat (VS 2022; try common editions in both Program Files) --
REM  (!PF86! is expanded lazily so the "(x86)" parens can't break the for-block.)
set "VCVARS="
set "PF86=%ProgramFiles(x86)%"
for %%e in (Community Professional Enterprise BuildTools) do (
  if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat"
  if not defined VCVARS if exist "!PF86!\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=!PF86!\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
  echo ERROR: vcvars64.bat not found. Install Visual Studio 2022 with the "Desktop development with C++" workload.
  exit /b 1
)

echo == MSVC env: "!VCVARS!"
REM  (silence vcvars' chatty output incl. a harmless internal vswhere notice;
REM   a real failure is still caught by the exit code below.)
call "!VCVARS!" >nul 2>&1 || ( echo ERROR: failed to initialise the MSVC environment. & exit /b 1 )

echo == Configure [%PRESET%]  (first run builds vcpkg/libgit2 - can take a few minutes)
cmake --preset %PRESET% || ( echo ERROR: cmake configure failed. & exit /b 1 )

echo == Build [%PRESET%]
cmake --build --preset %PRESET% || ( echo ERROR: build failed. & exit /b 1 )

set "BUILDDIR=out\build\release"
if /i "%PRESET%"=="Debug-x64" set "BUILDDIR=out\build\debug"

echo == Tests [ctest]
ctest --test-dir "%BUILDDIR%" --output-on-failure

echo.
echo == Build complete. Binaries are under %BUILDDIR%\
echo    (restart the ovc desktop app to pick up the new build.)
exit /b 0
