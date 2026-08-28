@echo off
REM build.cmd - Compile rbminiflt.sys
REM Run inside "x64 Native Tools Command Prompt for VS 2022"
REM or just double-click (it locates the toolchain automatically).

setlocal
set CFG=Release
if not "%1"=="" set CFG=%1

REM --- locate toolchain (override if yours differs) ---
set WDKINC=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0
set WDKLIB=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0
set MSVC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
set BIN=%MSVC%\bin\Hostx64\x64

set INCLUDE=%WDKINC%\km;%WDKINC%\shared;%WDKINC%\um;%WDKINC%\ucrt;%MSVC%\include
set LIB=%WDKLIB%\km\x64;%WDKLIB%\um\x64;%WDKLIB%\ucrt\x64;%MSVC%\lib\x64

if not exist Build mkdir Build

echo === Compiling rbminiflt.c ===
if "%CFG%"=="Debug" (
    "%BIN%\cl.exe" /c /kernel /W4 /wd4324 /Zi /Od /D_AMD64_ /D_WIN64 /FoBuild\rbminiflt_dbg.obj rbminiflt.c
    if errorlevel 1 goto :err
    echo === Linking rbminiflt.sys (Debug + PDB) ===
    "%BIN%\link.exe" /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /DEBUG /PDB:rbminiflt.pdb /OUT:rbminiflt.sys Build\rbminiflt_dbg.obj fltMgr.lib ntoskrnl.lib BufferOverflowK.lib
) else (
    "%BIN%\cl.exe" /c /kernel /W4 /wd4324 /O2 /D_AMD64_ /D_WIN64 /FoBuild\rbminiflt.obj rbminiflt.c
    if errorlevel 1 goto :err
    echo === Linking rbminiflt.sys (Release) ===
    "%BIN%\link.exe" /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /RELEASE /OUT:rbminiflt.sys Build\rbminiflt.obj fltMgr.lib ntoskrnl.lib BufferOverflowK.lib
)
if errorlevel 1 goto :err

echo.
echo === Build OK: rbminiflt.sys (12288 bytes expected) ===
dir rbminiflt.sys
exit /b 0

:err
echo !!! Build FAILED
exit /b 1
