@echo off
rem ===========================================================================
rem  Build the standalone test harness:
rem
rem    test\out\bm2dx.dll  - a fake game dll that imports Direct3DCreate9Ex and
rem                          EnumDisplaySettingsA, creates a D3D9 device and
rem                          runs the naive "one logic tick per present" loop
rem    test\out\host.exe   - a tiny spice2x SDK host that loads the fake game dll
rem                          plus iidxfps.dll and runs that loop
rem
rem  Use it to verify the pacing without starting the game at all.
rem ===========================================================================
setlocal

set HERE=%~dp0
set ROOT=%HERE%..
set ZIGEXE=

if defined ZIG if exist "%ZIG%" set ZIGEXE=%ZIG%
if not defined ZIGEXE for %%I in (zig.exe) do if not "%%~$PATH:I"=="" set ZIGEXE=%%~$PATH:I
if not defined ZIGEXE if exist "%ROOT%\tools\zig\zig.exe" set ZIGEXE=%ROOT%\tools\zig\zig.exe

if not defined ZIGEXE (
    echo [error] zig was not found. See build.bat for how to get it.
    exit /b 1
)

if not exist "%HERE%out" mkdir "%HERE%out"

"%ZIGEXE%" c++ -target x86_64-windows-gnu -O2 -shared -o "%HERE%out\bm2dx.dll" ^
    "%HERE%bm2dx_stub.cpp" -ld3d9 -luuid
if errorlevel 1 exit /b 1

"%ZIGEXE%" cc -target x86_64-windows-gnu -O2 -o "%HERE%out\host.exe" ^
    "%HERE%host.c" -I"%ROOT%\src" -lpsapi -lntdll
if errorlevel 1 exit /b 1

rem the harness loads the module from its own directory, so copy what was built
if exist "%ROOT%\build\iidxfps.dll" copy /y "%ROOT%\build\iidxfps.dll" "%HERE%out\iidxfps.dll" >nul

echo [ok] built %HERE%out
echo      run:  cd test\out ^&^& host.exe 4            (iidxfps.dll, default target 60)
endlocal
