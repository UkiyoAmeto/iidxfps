@echo off
rem ===========================================================================
rem  Build iidxfps.dll  (v1 - the released module)
rem
rem  Requirements: a C compiler.  Either
rem    * zig 0.14.x   (recommended, no installation needed, no MSVC required)
rem        - put zig.exe on PATH, or
rem        - set ZIG=C:\path\to\zig.exe, or
rem        - extract the zip to  tools\zig\zig.exe
rem      download: https://ziglang.org/download/    (zig-x86_64-windows-*.zip)
rem    * or any MSVC/mingw toolchain if you prefer to adapt the command below
rem
rem  Output: build\iidxfps.dll
rem ===========================================================================
setlocal

set ROOT=%~dp0
set ZIGEXE=

if defined ZIG if exist "%ZIG%" set ZIGEXE=%ZIG%
if not defined ZIGEXE for %%I in (zig.exe) do if not "%%~$PATH:I"=="" set ZIGEXE=%%~$PATH:I
if not defined ZIGEXE if exist "%ROOT%tools\zig\zig.exe" set ZIGEXE=%ROOT%tools\zig\zig.exe

if not defined ZIGEXE (
    echo [error] zig was not found.
    echo         Put zig.exe on PATH, or set ZIG=C:\path\to\zig.exe,
    echo         or extract it to  tools\zig\zig.exe
    echo         Download: https://ziglang.org/download/  ^(zig-x86_64-windows-*.zip^)
    exit /b 1
)

echo [info] using %ZIGEXE%
if not exist "%ROOT%build" mkdir "%ROOT%build"

"%ZIGEXE%" cc -target x86_64-windows-gnu -O2 -Wall -Wextra -shared ^
    -o "%ROOT%build\iidxfps.dll" ^
    "%ROOT%src\iidxfps.c" ^
    "%ROOT%src\iidxfps.def"
if errorlevel 1 (
    echo [error] build failed
    exit /b 1
)

echo [ok] built %ROOT%build\iidxfps.dll
endlocal
