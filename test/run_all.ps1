# 测试总入口 — 按 test-plan 的分层依次执行
#
# 用法：
#   .\test\run_all.ps1                 # 安全层（L0/L1/L2/L3/L5 非破坏性）
#   .\test\run_all.ps1 -All            # 追加 L4 骨架与 L5 破坏性骨架（仍默认跳过）
#   .\test\run_all.ps1 -IncludeBuild   # 额外执行编译零警告检查（慢）
#   .\test\run_all.ps1 -Deployed       # 额外核对部署产物 SHA256（常需管理员）
#
# 退出码：0 = 全绿（SKIP 不计失败），1 = 有失败

param(
    [switch]$All,
    [switch]$IncludeBuild,
    [switch]$Deployed
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$testDir = $PSScriptRoot

# 统一 UTF-8，避免中文在 GBK 控制台下变成乱码/问号
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    $OutputEncoding = [System.Text.Encoding]::UTF8
    chcp 65001 | Out-Null
} catch { }

Write-Host ""
Write-Host ("#" * 70)
Write-Host "# RecycleBin for SMB -- test suite (see docs/test-plan.md)"
Write-Host ("#" * 70)

$layers = @()

function Invoke-Python {
    param([string]$Script, [string[]]$ExtraArgs = @())
    Write-Host ""
    $py = "python"
    $args = @((Join-Path $root $Script)) + $ExtraArgs
    $out = & $py @args 2>&1 | Out-String
    Write-Host $out
    if ($out -match "STATUS: GREEN") { return 0 }
    if ($out -match "STATUS: NO-RUN") { return 2 }
    if ($out -match "STATUS: RED") { return 1 }
    return 1
}

function Invoke-PS {
    param([string]$Script, [string[]]$ExtraArgs = @())
    Write-Host ""
    # 子脚本以 exit <失败数> 结束；文本匹配为主，退出码兜底
    # （子脚本在管道中执行 exit 时，捕获到的输出可能不完整）
    $out = & (Join-Path $root $Script) @ExtraArgs 2>&1 | Out-String
    $code = $LASTEXITCODE
    Write-Host $out
    if ($out -match "STATUS: GREEN") { return 0 }
    if ($out -match "STATUS: NO-RUN") { return 2 }
    if ($out -match "STATUS: RED") { return 1 }
    if ($code -eq 0) { return 0 }   # 兜底：语义清晰的退出码
    return 1
}

# ---------------------------------------------------------------------
# L0 静态
# ---------------------------------------------------------------------
$extra = @()
if ($IncludeBuild) { $extra += "--build" }
if ($Deployed) { $extra += "--deployed" }
$layers += [pscustomobject]@{
    Name   = "L0 static"
    Result = (Invoke-Python -Script "test\l0_static\test_l0_static.py" -ExtraArgs $extra)
}

# ---------------------------------------------------------------------
# L1 单元（Go）
# ---------------------------------------------------------------------
Write-Host ""
$goOut = & powershell -NoProfile -File (Join-Path $testDir "l1_unit\go\run.ps1") 2>&1 | Out-String
Write-Host $goOut
$goRes = if ($goOut -match "STATUS: GREEN") { 0 } else { 1 }
$layers += [pscustomobject]@{ Name = "L1 unit (Go)"; Result = $goRes }

# ---------------------------------------------------------------------
# L2 契约
# ---------------------------------------------------------------------
$layers += [pscustomobject]@{
    Name   = "L2 contract"
    Result = (Invoke-PS -Script "test\l2_contract\run.ps1")
}

# ---------------------------------------------------------------------
# L3 集成
# ---------------------------------------------------------------------
$layers += [pscustomobject]@{
    Name   = "L3 integration"
    Result = (Invoke-Python -Script "test\l3_integration\test_l3_integration.py")
}

# ---------------------------------------------------------------------
# L5 非破坏性
# ---------------------------------------------------------------------
$layers += [pscustomobject]@{
    Name   = "L5 e2e (non-destructive)"
    Result = (Invoke-Python -Script "test\l5_e2e\test_l5_nondestructive.py")
}

# ---------------------------------------------------------------------
# 可选：L4 / L5 破坏性（骨架，默认跳过）
# ---------------------------------------------------------------------
if ($All) {
    Write-Host ""
    $k = & (Join-Path $testDir "l4_kernel\kernel.tests.ps1") 2>&1 | Out-String
    Write-Host $k
    $layers += [pscustomobject]@{
        Name   = "L4 kernel (skeleton)"
        Result = $(if ($k -match "STATUS: RED") { 1 } else { 0 })
    }

    Write-Host ""
    $d = & (Join-Path $testDir "l5_e2e\destructive.tests.ps1") 2>&1 | Out-String
    Write-Host $d
    $layers += [pscustomobject]@{
        Name   = "L5 destructive (skeleton)"
        Result = $(if ($d -match "STATUS: RED") { 1 } else { 0 })
    }

    Write-Host ""
    $layers += [pscustomobject]@{
        Name   = "L5 user-scenarios (A~F)"
        Result = (Invoke-Python -Script "test\l5_e2e\test_l5_user_scenarios.py")
    }
}

# ---------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------
Write-Host ""
Write-Host ("=" * 70)
Write-Host "SUMMARY"
Write-Host ("=" * 70)

$failed = 0
foreach ($l in $layers) {
    switch ($l.Result) {
        0 { $state = "GREEN" }
        2 { $state = "NO-RUN" }
        default { $state = "RED"; $failed++ }
    }
    Write-Host ("  {0,-30} {1}" -f $l.Name, $state)
}

Write-Host ("-" * 70)
if ($failed -eq 0) {
    Write-Host "OVERALL: GREEN  (SKIP = 已知未修缺陷 / 环境不满足，见各层输出)"
} else {
    Write-Host "OVERALL: RED ($failed layer(s) failing)"
}
Write-Host ("-" * 70)
Write-Host "提示："
Write-Host "  - L4 内核与 L5 破坏性用例默认跳过，需独立测试机，见 test/l4_kernel/README.md"
Write-Host "  - 加 -IncludeBuild 执行编译零警告检查；加 -Deployed 核对部署产物 SHA256"
Write-Host ""

exit $failed
