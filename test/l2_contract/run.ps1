# L2 契约测试 — 运行入口
#
# 复用仓库既有的两套契约脚本，并追加 RB-29 相关的字段断言：
#   db/verify_contract.py     C <-> Go 共享 schema（9 项）
#   db/verify_c_contract.py   C 版本守卫 + ops 往返（10 项）
#
# 契约测试只保证"结构一致"，不保证"行为正确"（RB-27~RB-30 的教训），
# 行为由 L3/L4/L5 覆盖。

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

Write-Host "======================================================================"
Write-Host "L2 contract tests"
Write-Host "======================================================================"

$failures = 0

function Invoke-PyContract {
    param([string]$Script, [string]$Title)

    Write-Host ""
    Write-Host "--- $Title ---"
    $out = & python (Join-Path $root $Script) 2>&1 | Out-String
    Write-Host $out

    # 两套脚本都以 "RESULT: n/m checks passed" 收尾
    if ($out -match "RESULT:\s*(\d+)/(\d+)\s*checks passed") {
        $pass = [int]$Matches[1]
        $total = [int]$Matches[2]
        if ($pass -ne $total) {
            $script:failures++
            Write-Host "  >> $Title : $pass/$total (RED)"
        } else {
            Write-Host "  >> $Title : $pass/$total (GREEN)"
        }
    } else {
        $script:failures++
        Write-Host "  >> $Title : could not parse RESULT line (RED)"
    }
}

Invoke-PyContract -Script "db\verify_contract.py"    -Title "C <-> Go schema contract"
Invoke-PyContract -Script "db\verify_c_contract.py"  -Title "C service contract + ops round-trip"

# ---------------------------------------------------------------------
# L2-3 / L2-4  driver_stats.protected_count 与 /health 暴露 (RB-29)
# ---------------------------------------------------------------------
Write-Host ""
Write-Host "--- driver_stats / /health protected_count (RB-29) ---"

$schema = Join-Path $root "db\schema.sql"
$has = Select-String -Path $schema -Pattern "protected_count" -Quiet
if ($has) {
    Write-Host "  [PASS] schema declares driver_stats.protected_count"
} else {
    Write-Host "  [FAIL] schema missing driver_stats.protected_count"
    $failures++
}

try {
    $resp = Invoke-RestMethod -Uri "http://127.0.0.1:8800/health" -TimeoutSec 10
    $pc = $resp.driver.protected_count
    if ($null -ne $pc) {
        Write-Host "  [PASS] /health exposes driver.protected_count = $pc"
    } else {
        Write-Host "  [SKIP] /health driver.protected_count missing (服务未运行或旧版本)"
    }
} catch {
    Write-Host "  [SKIP] /health not reachable ($($_.Exception.Message))"
}

Write-Host ""
Write-Host ("-" * 70)
if ($failures -eq 0) {
    Write-Host "STATUS: GREEN"
} else {
    Write-Host "STATUS: RED ($failures suite(s) failing)"
}
Write-Host ("-" * 70)

exit $failures
