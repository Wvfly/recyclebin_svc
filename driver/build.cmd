@echo off
REM build.cmd - Compile rbminiflt.sys
REM Run inside "x64 Native Tools Command Prompt for VS 2022"
REM or just double-click (it locates the toolchain automatically).

setlocal EnableDelayedExpansion

REM ---- colored output (ANSI; degrades to plain text if ESC unavailable) ----
for /f "delims=" %%i in ('powershell -NoProfile -Command "Write-Output ([char]27)" 2^>nul') do set "ESC=%%i"
if not defined ESC for /f "delims=" %%i in ('prompt $E ^& cmd /c "exit /b"') do set "ESC=%%i"
if defined ESC (
    set "C_ERR=%ESC%[91m"
    set "C_OK=%ESC%[92m"
    set "C_RST=%ESC%[0m"
) else (
    set "C_ERR=" & set "C_OK=" & set "C_RST="
)
powershell -NoProfile -Command "try{$h=[Console]::OpenStandardOutput().Handle;Add-Type 'using System;using System.Runtime.InteropServices;public class RBK{[DllImport(\"kernel32\")]public static extern bool GetConsoleMode(IntPtr h,out uint m);[DllImport(\"kernel32\")]public static extern bool SetConsoleMode(IntPtr h,uint m);}';uint m;if([RBK]::GetConsoleMode($h,[ref]$m)){[RBK]::SetConsoleMode($h,$m -bor 4)}|Out-Null}catch{}" >nul 2>&1

set CFG=Release
if not "%1"=="" set CFG=%1

REM --- auto-locate toolchain (set MSVC / WDKINC / WDKLIB env vars to override) ---
set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022"
if not exist "%VSROOT%" set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022"
if not exist "%VSROOT%" set "VSROOT=C:\Program Files\Microsoft Visual Studio\2019"
if not exist "%VSROOT%" set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2019"

if defined MSVC goto :msvc_ok
call :find_msvc
if errorlevel 1 exit /b 1
:msvc_ok

if defined WDKINC goto :wdk_ok
call :find_wdk
if errorlevel 1 exit /b 1
:wdk_ok

set "BIN=%MSVC%\bin\Hostx64\x64"

set "INCLUDE=%WDKINC%\km;%WDKINC%\shared;%WDKINC%\um;%WDKINC%\ucrt;%MSVC%\include"
set "LIB=%WDKLIB%\km\x64;%WDKLIB%\um\x64;%WDKLIB%\ucrt\x64;%MSVC%\lib\x64"
goto :detect_end

:find_msvc
set "VSED="
for %%E in (Community Professional Enterprise BuildTools) do (
    if not defined VSED if exist "!VSROOT!\%%E\VC\Tools\MSVC" set "VSED=%%E"
)
if not defined VSED (
    echo %C_ERR%[ERROR] Cannot find Visual Studio 2019/2022 under !VSROOT!.%C_RST%
    echo         Install with the "Desktop development with C++" workload.
    exit /b 1
)
set "MSVCVER="
for /f "delims=" %%V in ('dir /b /ad "!VSROOT!\!VSED!\VC\Tools\MSVC\14.*" 2^>nul ^| sort /r') do (
    if not defined MSVCVER set "MSVCVER=%%V"
)
if not defined MSVCVER (
    echo %C_ERR%[ERROR] No MSVC toolset under !VSROOT!\!VSED!\VC\Tools\MSVC.%C_RST%
    exit /b 1
)
set "MSVC=!VSROOT!\!VSED!\VC\Tools\MSVC\!MSVCVER!"
exit /b 0

:find_wdk
set "WDKROOT=C:\Program Files (x86)\Windows Kits\10"
if not exist "!WDKROOT!\Include" set "WDKROOT=C:\Program Files\Windows Kits\10"
set "WDKVER="
for /f "delims=" %%V in ('dir /b /ad "!WDKROOT!\Include\10.*" 2^>nul ^| sort /r') do (
    if not defined WDKVER set "WDKVER=%%V"
)
if not defined WDKVER (
    echo %C_ERR%[ERROR] Cannot find the Windows WDK under !WDKROOT!\Include.%C_RST%
    echo         Install the WDK matching your Windows SDK version.
    exit /b 1
)
set "WDKINC=!WDKROOT!\Include\!WDKVER!"
set "WDKLIB=!WDKROOT!\Lib\!WDKVER!"
exit /b 0

:detect_end

if not exist Build mkdir Build

echo === RecycleBin for SMB - driver build (author: wuweigang) ===
echo === Repo: https://github.com/Wvfly/recyclebin_svc ===
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
echo === Build OK: rbminiflt.sys ===
dir rbminiflt.sys
exit /b 0

:err
echo !!! Build FAILED
exit /b 1
