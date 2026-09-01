# L5 破坏性端到端用例 (test-plan 第三章 S-H1 ~ S-H10 业务剧本)
#
# 这些用例会**真的删除文件**并还原，必须在独立测试共享上执行。
# 默认全部 SKIP；确认后执行：
#   .\destructive.tests.ps1 -ConfirmDestructive -ShareRoot E:\tmp\share
#
# 安全护栏：脚本会先校验目标确实是"测试共享"（存在 .rb_test_marker），
# 拒绝在任意目录上执行，防止误删真实业务数据。

param(
    [switch]$ConfirmDestructive,
    [string]$ShareRoot = "E:\tmp\share"
)

. (Join-Path (Split-Path -Parent $PSScriptRoot) "lib\Harness.ps1")
Initialize-RbTest -Title "L5 destructive end-to-end (business scenarios)"

$marker = Join-Path $ShareRoot ".rb_test_marker"

if (-not $ConfirmDestructive) {
    Write-Host ""
    Write-Host "  破坏性用例默认跳过：会真实删除并还原文件。"
    Write-Host "  确认在测试共享上执行："
    Write-Host "    .\destructive.tests.ps1 -ConfirmDestructive -ShareRoot <测试共享路径>"
    Write-Host ""
}

# 业务剧本（对应 test-plan 3.8）
$stories = @(
    @{ Id = "S-H1";  Desc = "财务误删中文资料目录（≥20MB，多层，UNC 可见性）" }
    @{ Id = "S-H2";  Desc = "研发误删代码库（大量小文件 + 深嵌套，哈希比对）" }
    @{ Id = "S-H3";  Desc = "Shift+Delete 删除 Excel（网络位置原生永久删除）" }
    @{ Id = "S-H4";  Desc = "备份软件 robocopy /mir 误清空共享" }
    @{ Id = "S-H5";  Desc = "多用户：A 误删 B 的文件，管理员代还原" }
    @{ Id = "S-H6";  Desc = "用户清空桌面回收站后用 API 还原" }
    @{ Id = "S-H7";  Desc = "恶意脚本批量删除上千文件" }
    @{ Id = "S-H8";  Desc = "删除后原地新建同名文件再还原（冲突处理）" }
    @{ Id = "S-H9";  Desc = "长期保留后找回（超期 vs 未超期）" }
    @{ Id = "S-H10"; Desc = "全新部署首删（无 attach 窗口）" }
)

Show-RbSection "业务剧本 ($($stories.Count) 项)"

foreach ($s in $stories) {
    if ($ConfirmDestructive) {
        if (-not (Test-Path -LiteralPath $marker)) {
            Skip-Rb -Name "$($s.Id) $($s.Desc)" `
                -Reason "安全护栏：测试共享缺少 .rb_test_marker，拒绝执行"
        } else {
            Skip-Rb -Name "$($s.Id) $($s.Desc)" `
                -Reason "执行器未实现：需夹具生成器与删除/还原驱动（见 README）"
        }
    } else {
        Skip-Rb -Name "$($s.Id) $($s.Desc)" -Reason "未确认破坏性测试"
    }
}

$res = Complete-RbTest
exit $res.Failed
