@echo off
rem build.cmd - Build the C user-mode service (rbservice.exe)
rem
rem Usage:
rem   build.cmd            Release build (/O2)
rem   build.cmd Debug      Debug build (/Od /Zi)
rem
rem Requires Visual Studio 2022 (Community/Pro/Enterprise) with the
rem "Desktop development with C++" workload. The Windows SDK provides
rem fltuser.h and fltlib.lib (kernel communication port APIs).

setlocal

REM ---- colored output (ANSI; degrades to plain text if ESC unavailable) ----
for /f "delims=" %%i in ('powershell -NoProfile -Command "Write-Output ([char]27)" 2^>nul') do set "ESC=%%i"
if not defined ESC for /f "delims=" %%i in ('prompt $E ^& cmd /c "exit /b"') do set "ESC=%%i"
if defined ESC (
    set "C_OK=%ESC%[92m"
    set "C_WARN=%ESC%[93m"
    set "C_ERR=%ESC%[91m"
    set "C_SKIP=%ESC%[93m"
    set "C_HDR=%ESC%[96m"
    set "C_RST=%ESC%[0m"
) else (
    set "C_OK=" & set "C_WARN=" & set "C_ERR=" & set "C_SKIP=" & set "C_HDR=" & set "C_RST="
)
powershell -NoProfile -Command "try{$h=[Console]::OpenStandardOutput().Handle;Add-Type 'using System;using System.Runtime.InteropServices;public class RBK{[DllImport(\"kernel32\")]public static extern bool GetConsoleMode(IntPtr h,out uint m);[DllImport(\"kernel32\")]public static extern bool SetConsoleMode(IntPtr h,uint m);}';uint m;if([RBK]::GetConsoleMode($h,[ref]$m)){[RBK]::SetConsoleMode($h,$m -bor 4)}|Out-Null}catch{}" >nul 2>&1

set BUILD_TYPE=%1
if "%BUILD_TYPE%"=="" set BUILD_TYPE=Release

rem ---- Locate the VS toolchain (probes install dir / year / edition) -----
set "FOUND_VCVARS="
for %%B in ("C:\Program Files" "C:\Program Files (x86)") do (
    for %%Y in (2022 2019) do (
        for %%E in (Community Professional Enterprise BuildTools) do (
            if not defined FOUND_VCVARS if exist "%%~B\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars64.bat" (
                set "VCVARS=%%~B\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars64.bat"
                set "FOUND_VCVARS=1"
            )
        )
    )
)
if not defined FOUND_VCVARS (
    call :c_err [ERROR] Cannot find vcvars64.bat. Install Visual Studio 2019/2022 with
    echo         the "Desktop development with C++" workload.
    exit /b 1
)

call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    call :c_err [ERROR] vcvars64.bat failed to initialize the build environment.
    exit /b 1
)

rem ---- Regenerate schema_sql.h from the shared db\schema.sql --------------
rem The SQL lives in exactly one place so the C writer and the Go reader
rem cannot drift apart. This guarantees the embedded DDL always matches.
echo Regenerating schema_sql.h from ..\db\schema.sql ...
powershell -NoProfile -ExecutionPolicy Bypass -File "..\db\gen_schema.ps1"
if errorlevel 1 (
    call :c_err [ERROR] Failed to generate schema_sql.h from db\schema.sql.
    exit /b 1
)

rem ---- Ensure the SQLite amalgamation is present --------------------------
rem sqlite3.c/h are NOT vendored (too large); they are fetched on demand.
if not exist "sqlite3.c" goto :need_sqlite
if not exist "sqlite3.h" goto :need_sqlite
goto :have_sqlite

:need_sqlite
echo.
echo [INFO] sqlite3.c / sqlite3.h not found, downloading SQLite amalgamation...
echo        (These files are gitignored; see .gitignore)
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference='Stop';" ^
    "$zip = Join-Path $env:TEMP 'sqlite-amalgamation.zip';" ^
    "Invoke-WebRequest -Uri 'https://www.sqlite.org/2024/sqlite-amalgamation-3450000.zip' -OutFile $zip -TimeoutSec 120;" ^
    "Expand-Archive -Path $zip -DestinationPath $env:TEMP -Force;" ^
    "$src = Get-ChildItem -Path $env:TEMP -Recurse -Filter 'sqlite3.c' | Select-Object -First 1;" ^
    "Copy-Item $src.FullName . -Force;" ^
    "Copy-Item (Join-Path $src.DirectoryName 'sqlite3.h') . -Force;" ^
    "Remove-Item $zip -Force;" ^
    "Write-Host '  sqlite3.c/h ready'"
if errorlevel 1 (
    echo.
    call :c_err [ERROR] Could not download the SQLite amalgamation.
    echo         Manually place sqlite3.c and sqlite3.h in this directory, or
    echo         download from: https://www.sqlite.org/download.html
    exit /b 1
)
if not exist "sqlite3.c" (
    call :c_err [ERROR] sqlite3.c still missing after download attempt.
    exit /b 1
)

:have_sqlite

rem ---- Compile flags ------------------------------------------------------
set CFLAGS=/nologo /W3 /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WINDOWS /I.

if /i "%BUILD_TYPE%"=="Debug" (
    set CFLAGS=%CFLAGS% /Od /Zi /D_DEBUG /RTC1
    set LFLAGS=/DEBUG
) else (
    set CFLAGS=%CFLAGS% /O2 /GL
    set LFLAGS=/LTCG
)

set LIBS=fltlib.lib wtsapi32.lib advapi32.lib shell32.lib user32.lib netapi32.lib

set SOURCES=rbservice.c rbdb.c rbstore.c rbvol.c rbpolicy.c rbrestore.c rbport.c rbconfig.c rblog.c rbconcile.c rbsession.c sqlite3.c

echo RecycleBin for SMB - service build (author: wuweigang)
echo Repo: https://github.com/Wvfly/recyclebin_svc
echo Building rbservice.exe (%BUILD_TYPE%)...
echo.

cl %CFLAGS% %SOURCES% /link %LFLAGS% %LIBS% /OUT:rbservice.exe

if errorlevel 1 (
    echo.
    call :c_err [FAILED] Compilation errors above.
    exit /b 1
)

echo.
call :c_ok [OK] rbservice.exe built.

rem ---- Clean intermediates ------------------------------------------------
for %%f in (%SOURCES%) do (
    if exist "%%~nf.obj" del "%%~nf.obj" >nul 2>&1
)

call :c_ok [OK] Cleaned intermediate files.
endlocal

rem ============================================================
rem Colorized echo helpers
rem ============================================================
:c_ok
echo %C_OK%%*%C_RST%
exit /b 0
:c_warn
echo %C_WARN%%*%C_RST%
exit /b 0
:c_err
echo %C_ERR%%*%C_RST%
exit /b 0
:c_skip
echo %C_SKIP%%*%C_RST%
exit /b 0
:c_hdr
echo %C_HDR%%*%C_RST%
exit /b 0
