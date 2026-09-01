# 共用断言框架 (test/lib/Harness.ps1)
# 供 Pester 与独立 PowerShell 用例复用：统一 UTF-8 输出与结果汇总。

$script:RbResults = @()

function Initialize-RbTest {
    param([string]$Title)
    $script:RbResults = @()
    $script:RbTitle = $Title
    # 中文输出：强制 UTF-8，避免 GBK 代码页把中文打成问号（本项目实测已踩）
    try {
        [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
        $OutputEncoding = [System.Text.Encoding]::UTF8
    } catch { }
    Write-Host ("=" * 70)
    Write-Host $Title
    Write-Host ("=" * 70)
}

function Assert-Rb {
    param(
        [string]$Name,
        [bool]$Condition,
        [string]$Detail = ""
    )
    $state = if ($Condition) { "PASS" } else { "FAIL" }
    $script:RbResults += [pscustomobject]@{ Name = $Name; State = $state; Detail = $Detail }
    Write-Host "  [$state] $Name"
    if ($Detail) { Write-Host "         $Detail" }
    return $Condition
}

function Skip-Rb {
    param([string]$Name, [string]$Reason = "")
    $script:RbResults += [pscustomobject]@{ Name = $Name; State = "SKIP"; Detail = $Reason }
    Write-Host "  [SKIP] $Name"
    if ($Reason) { Write-Host "         $Reason" }
}

function Show-RbSection {
    param([string]$Title)
    Write-Host ""
    Write-Host "--- $Title ---"
}

function Complete-RbTest {
    $p = @($script:RbResults | Where-Object { $_.State -eq "PASS" }).Count
    $f = @($script:RbResults | Where-Object { $_.State -eq "FAIL" }).Count
    $s = @($script:RbResults | Where-Object { $_.State -eq "SKIP" }).Count
    Write-Host ""
    Write-Host ("-" * 70)
    Write-Host "RESULT $($script:RbTitle): $p passed, $f failed, $s skipped (total $($script:RbResults.Count))"
    if ($f -gt 0) { Write-Host "STATUS: RED" }
    elseif ($p -gt 0) { Write-Host "STATUS: GREEN" }
    else { Write-Host "STATUS: NO-RUN" }
    Write-Host ("-" * 70)
    # 供 run_all.ps1 解析
    return [pscustomobject]@{ Passed = $p; Failed = $f; Skipped = $s; Total = $script:RbResults.Count }
}

function Get-RbRepoRoot {
    # test/lib/Harness.ps1 -> 上级三级为仓库根
    return (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
}

function Get-RbSha256 {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
