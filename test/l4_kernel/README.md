# L4 内核 / 驱动测试

## 为什么不默认执行

本层用例会**主动触发蓝屏**（RB-27、RB-28 就是这么发现的），并且必须在
开启 Driver Verifier 的机器上运行。在任何承载真实业务数据的机器上执行
都可能造成不可逆损失。

因此 `kernel.tests.ps1` 默认把所有用例标记为 SKIP，需要两个开关同时确认
才会真正执行：

```powershell
.\kernel.tests.ps1 -ConfirmKernelTest -IAcceptBlueScreenRisk
```

## 前置条件（逐项确认后方可执行）

1. **独立测试机** —— 物理机或 VM，与生产网络隔离，**禁止**使用承载真实数据的机器
2. **已创建可回滚快照** —— 每次上机前恢复，保证环境可复现
3. **内核 dump** —— 设为 Complete 或 Kernel memory dump；符号路径指向构建产物 PDB
4. **Driver Verifier** —— 对 `rbminiflt.sys` 启用：
   - Special Pool
   - Pool Tracking
   - I/O Verification
   - Deadlock Detection
   - DMA Checking
5. **产物核对** —— 部署的 `.sys` SHA256 必须等于构建产物。
   这是 RB-27 / RB-28 的核心教训："源码看似已修、产物实际未编译进去"会
   伪造出"已修复"的假象。

```powershell
verifier /standard /driver rbminiflt.sys
(Get-FileHash C:\Windows\System32\drivers\rbminiflt.sys).Hash
(Get-FileHash target\Release\rbminiflt.sys).Hash
```

## 用例与缺陷映射

| 用例 | 验证点 | 缺陷 |
|---|---|---|
| L4-1 | 基本拦截 | 基础 |
| L4-2 | 长路径/深目录不栈溢出 | RB-01、RB-07 |
| L4-3 | 暂存区不可写 → 拒绝删除、数据不丢 | RB-04 |
| L4-4 | 递归删除大目录树 ≥5000 文件 | RB-08、RB-19 |
| L4-5 | `FileDispositionInformationEx` 被拦截 | RB-22 |
| L4-6 | 队列打满，孤儿数 0 | RB-08、RB-20 |
| L4-7 | 分配失败释放预留槽位 | RB-25 |
| L4-8 | 服务反复重启无 UAF | RB-23、RB-24 |
| L4-9 | `sc stop` 卸载不触发 0xA | RB-28 |
| L4-10 | stats 轮询 ≥30 min 无 0x3B | RB-27 |
| L4-11 | 畸形 `REG_MULTI_SZ` 不越界 | RB-26 |
| L4-12 | 加载后立即删除无保护窗口 | RB-29 |
| L4-13 | 卷卸载 / RBStore 被删后自愈 | RB-19 |
| L4-14 | Verifier 全程无违规 | 全部 |

## 执行方式建议

内核用例难以完全脚本化，建议按"**脚本驱动 + 人工判读**"执行：

- 脚本负责：部署、启停服务/驱动、发起删除、采集计数器与 dump
- 人工负责：Verifier 违规判读、dump 反汇编核对、性能基线校准

每次执行后归档：`fltmc` 输出、`/health` 快照、`MEMORY.DMP`、反汇编片段。
