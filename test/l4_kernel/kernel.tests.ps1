# L4 内核 / 驱动测试 (test-plan L4-1 ~ L4-14)
#
# 危险：本层用例会触发蓝屏、需要 Driver Verifier、可能损坏测试机数据。
# 默认全部 SKIP，必须显式确认后才执行。
#
# 前置条件（执行前逐项确认）：
#   1. 独立测试机（禁止在任何承载真实业务数据的机器上运行）
#   2. 已创建可回滚快照
#   3. 内核 dump 设为 Complete/Kernel memory dump，符号指向构建 PDB
#   4. Driver Verifier 已对 rbminiflt.sys 启用：
#        Special Pool / Pool Tracking / I/O Verification /
#        Deadlock Detection / DMA Checking
#   5. 部署的 .sys SHA256 与构建产物一致（RB-27/28 版本漂移教训）
#
# 执行：
#   .\kernel.tests.ps1 -ConfirmKernelTest -IAcceptBlueScreenRisk

param(
    [switch]$ConfirmKernelTest,
    [switch]$IAcceptBlueScreenRisk
)

. (Join-Path (Split-Path -Parent $PSScriptRoot) "lib\Harness.ps1")
Initialize-RbTest -Title "L4 kernel / driver tests"

$armed = $ConfirmKernelTest -and $IAcceptBlueScreenRisk

if (-not $armed) {
    Write-Host ""
    Write-Host "  本层用例默认全部跳过：内核测试会蓝屏、需要 Verifier 与独立测试机。"
    Write-Host "  确认就绪后执行："
    Write-Host "    .\kernel.tests.ps1 -ConfirmKernelTest -IAcceptBlueScreenRisk"
    Write-Host ""
}

# 用例清单（占位实现：真实执行依赖测试机上的驱动与夹具）
$cases = @(
    @{ Id = "L4-1";  Desc = "基本拦截：受保护路径内删除被拦截" },
    @{ Id = "L4-2";  Desc = "内核栈：长路径/深目录删除不溢出 (RB-01)" },
    @{ Id = "L4-3";  Desc = "fail-closed：暂存区不可写时拒绝删除 (RB-04)" },
    @{ Id = "L4-4";  Desc = "递归删除大目录树 ≥5000 文件 (RB-08/19)" },
    @{ Id = "L4-5";  Desc = "FileDispositionInformationEx 被拦截 (RB-22)" },
    @{ Id = "L4-6";  Desc = "队列打满：孤儿数为 0 (RB-08/20)" },
    @{ Id = "L4-7";  Desc = "RbfAllocNotify 失败释放预留槽位 (RB-25)" },
    @{ Id = "L4-8";  Desc = "服务反复重启：无 use-after-free (RB-23/24)" },
    @{ Id = "L4-9";  Desc = "sc stop 卸载不触发 0xA (RB-28)" },
    @{ Id = "L4-10"; Desc = "stats 轮询 ≥30min 不触发 0x3B (RB-27)" },
    @{ Id = "L4-11"; Desc = "畸形 REG_MULTI_SZ 不越界 (RB-26)" },
    @{ Id = "L4-12"; Desc = "加载后立即删除无保护窗口 (RB-29)" },
    @{ Id = "L4-13"; Desc = "卷卸载/RBStore 被删后自愈 (RB-19)" },
    @{ Id = "L4-14"; Desc = "Verifier 全程无违规" }
)

Show-RbSection "用例清单 ($($cases.Count) 项)"

foreach ($c in $cases) {
    if ($armed) {
        # 真实执行需测试机夹具与驱动控制逻辑（sc / fltmc / 故障注入）。
        # 此处保留骨架：执行器应在独立测试机上实现具体步骤。
        Skip-Rb -Name "$($c.Id) $($c.Desc)" `
            -Reason "执行器未实现：需独立测试机夹具（见 l4_kernel/README.md）"
    } else {
        Skip-Rb -Name "$($c.Id) $($c.Desc)" -Reason "未确认内核测试前置条件"
    }
}

$res = Complete-RbTest
exit $res.Failed
