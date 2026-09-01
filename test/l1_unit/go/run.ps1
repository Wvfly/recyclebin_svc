# L1 单元测试 — Go 层运行入口
#
# 说明：Go 要求 *_test.go 与被测包位于同一目录（语言约束），
# 因此测试源码位于：
#   service_go/db/db_test.go     — 数据访问层 (likeEscape / DriverStats / 只读句柄)
#   service_go/api/api_test.go   — 管理 API 层 (鉴权 / 端点 / 请求体上限)
# 本脚本是它们的统一运行入口，使 CI 与 test/run_all.ps1 只面向 test/ 目录。

param([switch]$Verbose)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$svcGo = Join-Path $root "service_go"
$log = Join-Path $root "test\_go_all.log"

Write-Host "======================================================================"
Write-Host "L1 unit tests (Go)"
Write-Host "======================================================================"
Write-Host "package dir: $svcGo"

Push-Location $svcGo
try {
    if ($Verbose) {
        go test ./... -v 2>&1 | Out-File -FilePath $log -Encoding utf8
    } else {
        go test ./... 2>&1 | Out-File -FilePath $log -Encoding utf8
    }
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

if (Test-Path $log) {
    Get-Content $log -Encoding UTF8 | ForEach-Object { Write-Host $_ }
}

Write-Host ""
Write-Host ("-" * 70)
if ($code -eq 0) {
    Write-Host "STATUS: GREEN"
} else {
    Write-Host "STATUS: RED (go test exit=$code)"
}
Write-Host ("-" * 70)

exit $code
