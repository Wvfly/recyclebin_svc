@echo off
rem ============================================================
rem build_all.cmd - One-shot build for RecycleBin for SMB
rem
rem Usage:
rem   build_all.cmd            Release build (default)
rem   build_all.cmd Debug      Debug build
rem   build_all.cmd /?         Show this help
rem   build_all.cmd Release nosign   build without driver signing
rem
rem Builds, in order:
rem   [1/3]  driver\rbminiflt.sys      kernel mini-filter   (WDK + MSVC)
rem   [2/3]  service_c\rbservice.exe   core service         (MSVC)
rem   [3/3]  service_go\rbapi.exe      management REST API  (Go 1.22+)
rem
rem Then runs the C/Go contract verification suites when python is
rem available, so a change that breaks the shared-schema contract fails
rem here instead of at deploy time.
rem
rem Finally copies every built binary plus the driver INF and deploy.ps1
rem into one self-contained deploy folder:
rem   target\%Config%\   e.g. target\Release\, target\Debug\
rem   (rbminiflt.sys, rbservice.exe, rbapi.exe, rbminiflt.inf, deploy.ps1)
rem Copy that folder to the target machine and run .\deploy.ps1 there.
rem
rem Notes:
rem   - Stale binaries are deleted first, so a missing output at the end
rem     always means "this build really failed", never "old file left over".
rem   - The Go step is optional. If the Go toolchain is absent the script
rem     warns and continues; rbapi.exe is not required for the core
rem     interception/recover functionality.
rem   - Run from any directory; paths are resolved relative to this script.
rem ============================================================
setlocal

set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"

if /i "%CFG%"=="/?"  goto :usage
if /i "%CFG%"=="-h"  goto :usage
if /i "%CFG%"=="--help" goto :usage

if /i not "%CFG%"=="Release" if /i not "%CFG%"=="Debug" (
    echo [ERROR] Unknown configuration "%CFG%" - expected Release or Debug.
    exit /b 1
)

set "OPT=%~2"
set "DO_SIGN=1"
if /i "%OPT%"=="nosign" set "DO_SIGN=0"
if defined OPT if /i not "%OPT%"=="nosign" (
    echo [ERROR] Unknown option "%OPT%" - expected "nosign".
    exit /b 1
)

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

echo ============================================================
echo  RecycleBin for SMB - full build ^(%CFG%^)
echo ============================================================
echo   Root: %ROOT%
echo.

rem ------------------------------------------------------------
rem Remove stale outputs so "file exists" at the end really means
rem "this build produced it".
rem ------------------------------------------------------------
echo Preparing ^(removing stale binaries^) ...
if exist "%ROOT%\driver\rbminiflt.sys"     del /f /q "%ROOT%\driver\rbminiflt.sys"     >nul 2>&1
if exist "%ROOT%\service_c\rbservice.exe"  del /f /q "%ROOT%\service_c\rbservice.exe"  >nul 2>&1
if exist "%ROOT%\service_go\rbapi.exe"     del /f /q "%ROOT%\service_go\rbapi.exe"     >nul 2>&1
echo   done.
echo.

rem ============================================================
echo [1/3] Kernel driver    -^> driver\rbminiflt.sys
echo ============================================================
pushd "%ROOT%\driver"
if not exist "build.cmd" (
    popd
    echo [ERROR] driver\build.cmd not found.
    exit /b 1
)
call build.cmd %CFG%
if errorlevel 1 (
    popd
    echo.
    echo [FAILED] Driver build failed.
    echo.
    echo   Common causes:
    echo     - WDK not installed, or driver\build.cmd points at a different
    echo       SDK version. Edit WDKINC / WDKLIB at the top of that file to
    echo       match your installed Windows SDK.
    echo     - MSVC path in driver\build.cmd does not match your VS version.
    exit /b 1
)
if not exist "rbminiflt.sys" (
    popd
    echo [FAILED] build.cmd succeeded but rbminiflt.sys is missing.
    exit /b 1
)
popd
echo   [OK] rbminiflt.sys
echo.

rem ------------------------------------------------------------
rem Sign rbminiflt.sys - kernel drivers must be signed to load.
rem
rem   * Default cert: the project signing cert (CurrentUser\My, no /sm),
rem     overridable via RBF_CERT_SHA1 (cert SHA1 thumbprint);
rem     set RBF_SIGN_STORE=machine to read it from LocalMachine\My.
rem   * signtool is located from the installed Win10 SDK bin dirs,
rem     overridable via RBF_SIGNTOOL (full path).
rem   * Skip entirely with:  build_all.cmd Release nosign
rem
rem A missing signtool/certificate only WARNS: the build still
rem succeeds, but the driver then needs test-signing enabled
rem (bcdedit /set testsigning on) or a manual sign before deploy.
rem ------------------------------------------------------------
if "%DO_SIGN%"=="0" (
    echo [SKIP] Driver signing disabled ^(nosign^).
    goto :sign_done
)

set "RBF_CERT_SHA1=%RBF_CERT_SHA1%"
if "%RBF_CERT_SHA1%"=="" set "RBF_CERT_SHA1=F57B8149935CD56C5565965AB5DF66E454B903F9"

rem Sign from the CURRENT USER cert store by default (no /sm). Set
rem RBF_SIGN_STORE=machine to read the certificate from LocalMachine\My.
set "SM_OPTION="
if /i "%RBF_SIGN_STORE%"=="machine" set "SM_OPTION=/sm"

set "SIGNTOOL=%RBF_SIGNTOOL%"
if defined SIGNTOOL if not exist "%SIGNTOOL%" set "SIGNTOOL="
if not defined SIGNTOOL (
    for %%V in (10.0.26100.0 10.0.22621.0 10.0.22000.0 10.0.19041.0) do (
        if not defined SIGNTOOL if exist "C:\Program Files (x86)\Windows Kits\10\bin\%%V\x64\signtool.exe" set "SIGNTOOL=C:\Program Files (x86)\Windows Kits\10\bin\%%V\x64\signtool.exe"
    )
)
if not defined SIGNTOOL (
    echo [WARN] signtool.exe not found - rbminiflt.sys NOT signed.
    echo        Deploy only with test-signing enabled, or sign manually:
    echo        signtool sign /sha1 %RBF_CERT_SHA1% /fd sha256 /tr http://timestamp.digicert.com /td sha256 rbminiflt.sys
    goto :sign_done
)

echo   Signing rbminiflt.sys ^(cert sha1 %RBF_CERT_SHA1%^) ...
pushd "%ROOT%\driver"
"%SIGNTOOL%" sign %SM_OPTION% /sha1 "%RBF_CERT_SHA1%" /fd sha256 /tr http://timestamp.digicert.com /td sha256 rbminiflt.sys
if errorlevel 1 (
    popd
    echo [WARN] Signing failed - rbminiflt.sys NOT signed.
    echo        Deploy only with test-signing enabled, or sign manually:
    echo        signtool sign /sha1 %RBF_CERT_SHA1% /fd sha256 /tr http://timestamp.digicert.com /td sha256 rbminiflt.sys
    goto :sign_done
)
popd
echo   [OK] rbminiflt.sys signed.
echo.

:sign_done

rem ============================================================
echo [2/3] Core service     -^> service_c\rbservice.exe
echo ============================================================
pushd "%ROOT%\service_c"
if not exist "build.cmd" (
    popd
    echo [ERROR] service_c\build.cmd not found.
    exit /b 1
)
call build.cmd %CFG%
if errorlevel 1 (
    popd
    echo.
    echo [FAILED] Core service build failed.
    echo.
    echo   Common causes:
    echo     - Visual Studio 2022 with "Desktop development with C++" missing.
    echo     - powershell.exe not on PATH. service_c\build.cmd shells out to it
    echo       to generate schema_sql.h and to fetch sqlite3 on demand.
    echo     - sqlite3.c / sqlite3.h could not be downloaded. Place them in
    echo       service_c\ manually from https://www.sqlite.org/download.html
    exit /b 1
)
if not exist "rbservice.exe" (
    popd
    echo [FAILED] build.cmd succeeded but rbservice.exe is missing.
    exit /b 1
)
popd
echo   [OK] rbservice.exe
echo.

rem ============================================================
echo [3/3] Management API   -^> service_go\rbapi.exe
echo ============================================================
pushd "%ROOT%\service_go"

where go >nul 2>&1
if errorlevel 1 (
    echo   [SKIP] Go toolchain not found on PATH.
    echo          rbapi.exe is optional - the core service works without it.
    echo          Install Go 1.22+ from https://go.dev/dl/ to build it.
    popd
    set "API_SKIPPED=1"
    goto :after_api
)

rem Default proxy.golang.org is unreachable from some networks; fall back to
rem a mirror unless the user already configured GOPROXY themselves.
rem
rem NOTE: this is plain variable expansion, so it must NOT live inside a
rem parenthesised block -- a %GOPROXY% read in the same block that sets it
rem expands to empty. Hence the goto-based flow below.
set "PROXY_IS_DEFAULT=0"
if "%GOPROXY%"=="" set "PROXY_IS_DEFAULT=1"
if "%PROXY_IS_DEFAULT%"=="1" goto :set_proxy
goto :proxy_done

:set_proxy
set "GOPROXY=https://goproxy.cn,direct"
echo   Using GOPROXY=%GOPROXY%
echo   ^(set GOPROXY yourself to override^)

:proxy_done
if "%GOSUMDB%"=="" set "GOSUMDB=off"

go build -o rbapi.exe .
if errorlevel 1 (
    popd
    echo.
    echo [FAILED] Go build failed.
    echo.
    echo   Common causes:
    echo     - Module download failed. Try:  go mod tidy
    echo     - Network blocked. Try a different GOPROXY, e.g.
    echo         set GOPROXY=https://proxy.golang.org,direct
    exit /b 1
)
if not exist "rbapi.exe" (
    popd
    echo [FAILED] go build succeeded but rbapi.exe is missing.
    exit /b 1
)
echo   [OK] rbapi.exe
popd

:after_api
echo.

rem ============================================================
rem Contract verification
rem ============================================================
where python >nul 2>&1
if errorlevel 1 (
    echo [SKIP] python not found - skipping contract verification.
    goto :collect
)

echo ============================================================
echo  Contract verification
echo ============================================================
pushd "%ROOT%"

if defined API_SKIPPED (
    echo.
    echo   [SKIP] verify_contract.py needs rbapi.exe, which was not built
    echo          ^(Go toolchain missing^). The C ^<-^> Go contract check is
    echo          skipped; C service verification still runs below.
) else (
    echo.
    echo --- C ^<-^> Go shared-schema contract ---
    python "db\verify_contract.py"
    if errorlevel 1 (
        popd
        echo.
        echo [FAILED] Contract verification failed.
        echo          The C service and Go API disagree about the database.
        exit /b 1
    )
)

echo.
echo --- C service version guard + ops round-trip ---
python "db\verify_c_contract.py"
if errorlevel 1 (
    popd
    echo.
    echo [FAILED] C contract verification failed.
    exit /b 1
)

popd
echo.

rem ============================================================
:collect
echo ============================================================
echo  Collecting artifacts -^> target\%CFG%\
echo ============================================================
set "TARGET=%~dp0target\%CFG%"
if not exist "%TARGET%" mkdir "%TARGET%"

rem Remove stale copies so "file exists" means "this build produced it".
if exist "%TARGET%\rbminiflt.sys"  del /f /q "%TARGET%\rbminiflt.sys"  >nul 2>&1
if exist "%TARGET%\rbservice.exe"  del /f /q "%TARGET%\rbservice.exe"  >nul 2>&1
if exist "%TARGET%\rbapi.exe"      del /f /q "%TARGET%\rbapi.exe"      >nul 2>&1
if exist "%TARGET%\rbminiflt.inf"  del /f /q "%TARGET%\rbminiflt.inf"  >nul 2>&1
if exist "%TARGET%\deploy.ps1"     del /f /q "%TARGET%\deploy.ps1"     >nul 2>&1

copy /y "%ROOT%\driver\rbminiflt.sys"   "%TARGET%\rbminiflt.sys"   >nul
if errorlevel 1 goto :collect_failed

copy /y "%ROOT%\service_c\rbservice.exe" "%TARGET%\rbservice.exe" >nul
if errorlevel 1 goto :collect_failed

if not defined API_SKIPPED (
    if exist "%ROOT%\service_go\rbapi.exe" (
        copy /y "%ROOT%\service_go\rbapi.exe" "%TARGET%\rbapi.exe" >nul
        if errorlevel 1 goto :collect_failed
    )
)

rem Deploy assets: INF + deploy script make target self-contained.
copy /y "%ROOT%\driver\rbminiflt.inf"  "%TARGET%\rbminiflt.inf"  >nul
if errorlevel 1 goto :collect_failed

copy /y "%ROOT%\deploy.ps1"            "%TARGET%\deploy.ps1"     >nul
if errorlevel 1 goto :collect_failed

echo   [OK] binaries + INF + deploy.ps1 in %TARGET%
echo.
goto :summary

:collect_failed
echo.
echo [FAILED] Could not collect build artifacts into %TARGET%.
exit /b 1

rem ============================================================
:summary
echo ============================================================
echo  BUILD SUCCESSFUL ^(%CFG%^)
echo ============================================================
echo.
echo   Outputs:

if exist "%ROOT%\driver\rbminiflt.sys" (
    for %%F in ("%ROOT%\driver\rbminiflt.sys") do echo     rbminiflt.sys     %%~zF bytes   driver\rbminiflt.sys
) else (
    echo     rbminiflt.sys     MISSING
)

if exist "%ROOT%\service_c\rbservice.exe" (
    for %%F in ("%ROOT%\service_c\rbservice.exe") do echo     rbservice.exe     %%~zF bytes   service_c\rbservice.exe
) else (
    echo     rbservice.exe     MISSING
)

if defined API_SKIPPED (
    echo     rbapi.exe         not built ^(Go toolchain missing - optional^)
    echo.
    echo   Note: C ^<-^> Go contract verification was skipped too, since it
    echo         needs rbapi.exe. Install Go and re-run to enable it.
) else (
    if exist "%ROOT%\service_go\rbapi.exe" (
        for %%F in ("%ROOT%\service_go\rbapi.exe") do echo     rbapi.exe         %%~zF bytes   service_go\rbapi.exe
    ) else (
        echo     rbapi.exe         MISSING
    )
)

echo.
echo   Deploy package ready: %TARGET%
echo     ^(self-contained: binaries + rbminiflt.inf + deploy.ps1^)
echo.
echo   Next steps:
echo     1. Copy the whole folder above to the target machine
echo     2. Edit deploy.ps1 there: set ProtectedPaths and StoreRoot on the SAME volume
echo     3. bcdedit /set testsigning on   ^(then reboot^)
echo     4. powershell -ExecutionPolicy Bypass -File .\deploy.ps1
echo.
exit /b 0

rem ============================================================
:usage
echo build_all.cmd - One-shot build for RecycleBin for SMB
echo.
echo Usage:
echo   build_all.cmd            Release build ^(default^)
echo   build_all.cmd Debug      Debug build
echo   build_all.cmd /?         Show this help
echo   build_all.cmd Release nosign   build without driver signing
echo.
echo Builds:
echo   [1/3]  driver\rbminiflt.sys      kernel mini-filter   ^(WDK + MSVC^)
echo   [2/3]  service_c\rbservice.exe   core service         ^(MSVC^)
echo   [3/3]  service_go\rbapi.exe      management REST API  ^(Go 1.22+, optional^)
echo.
echo After [1/3], rbminiflt.sys is signed with the project cert
echo ^(CurrentUser\My, SHA1 default F57B8149...; override with
echo RBF_CERT_SHA1, set RBF_SIGN_STORE=machine for LocalMachine\My,
echo or pass "nosign" to skip; signtool path override with
echo RBF_SIGNTOOL^). If signtool/cert is unavailable the build
echo continues unsigned with a warning - such a driver needs
echo test-signing enabled or a manual sign before it will load.
echo.
echo Then runs db\verify_contract.py and db\verify_c_contract.py when python
echo is available.
echo.
echo Finally copies all built binaries plus the driver INF and deploy.ps1 into
echo one self-contained deploy folder: target\%Config%\. Copy that folder to
echo the target machine and run .\deploy.ps1 there.
echo.
echo Stale binaries are deleted before building, so a missing output at the
echo end always means the build genuinely failed.
echo.
exit /b 0

endlocal
