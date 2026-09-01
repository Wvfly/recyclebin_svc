# 测试套件

按 `docs/test-plan.md` 的分层实施。**总入口：`test\run_all.ps1`**

## 快速开始

```powershell
.\test\run_all.ps1                  # 安全层（默认，约 30 秒）
.\test\run_all.ps1 -All             # 追加 L4/L5 骨架层（仍默认跳过）
.\test\run_all.ps1 -IncludeBuild    # 额外执行编译零警告检查（慢）
.\test\run_all.ps1 -Deployed        # 额外核对部署产物 SHA256（常需管理员）
```

退出码：`0` = 全绿，`1` = 有失败。**SKIP 不计失败**，但每个 SKIP 都必须带原因。

## 目录结构

```
test/
  run_all.ps1                  总入口：分层执行 + 汇总
  lib/
    common.py                  Python 断言框架（UTF-8、pass/fail/skip 汇总）
    Harness.ps1                PowerShell 断言框架
  l0_static/
    test_l0_static.py          编译期守卫：结构体断言、schema 派生、产物核对
  l1_unit/
    go/run.ps1                 Go 单测入口（源码在 service_go 包内，见其 README）
    go/README.md
  l2_contract/
    run.ps1                    复用 db/verify_*.py + RB-29 字段断言
  l3_integration/
    test_l3_integration.py     用户态集成（rbservice once --db，无需驱动）
  l4_kernel/
    kernel.tests.ps1           内核用例骨架（默认跳过，危险）
    README.md                  测试机与 Verifier 前置条件
  l5_e2e/
    test_l5_nondestructive.py  只读端到端（查真实运行的服务）
    test_l5_smb_real.py        真实 SMB 拦截/还原（维度 G/H，隔离于 __smoke__）
    test_l5_user_scenarios.py  用户场景矩阵（维度 A~F，真实删除+还原，隔离于 __smoke__/scenarios）
    destructive.tests.ps1      业务剧本骨架（默认跳过，带安全护栏）
  l6_nonfunctional/
    README.md                  压测/故障注入待实现清单
```

## 当前结果

```
L0 static                    16 passed, 0 failed, 2 skipped
L1 unit (Go)                 13 passed, 0 failed, 1 skipped
  rbapi/db                    7 PASS
  rbapi/api                   6 PASS, 1 SKIP
L2 contract                   9/9 + 10/10 checks passed + RB-29 字段
L3 integration               11 passed, 0 failed, 1 skipped
L5 e2e (non-destructive)     11 passed, 0 failed, 2 skipped
L5 user-scenarios (A~F)      56 passed, 2 failed, 4 skipped   (2026-09-01 实测)
L4 kernel                    14 SKIP（需独立测试机）
L5 destructive               10 SKIP（需测试共享）
----------------------------------------------------------
OVERALL: 维度 A~F 主体 GREEN，2 个真实拦截缺陷待修（S-B4/S-B13，见下）
```

## SKIP 项说明（均为已知未修缺陷或环境限制）

| 层 | 项 | 原因 |
|---|---|---|
| L0 | 编译零警告 | 需 MSVC，加 `-IncludeBuild` 启用 |
| L0 | 部署产物 SHA256 | 需读取 `System32\drivers`，加 `-Deployed` 启用 |
| L1 | `TestHealthWhenDatabaseUnavailable` | **新发现缺陷**，见下 |
| L3 | 终态归档触发 | 需长周期维护，用例骨架待补（RB-09 回归） |
| L5 | fltmc 驱动加载 | 需管理员权限 |
| L5 | 未授权访问被拒 | 当前未启用 token（开发语义）；生产应启用 |
| L4 | 全部 14 项 | 需独立测试机 + Verifier，会蓝屏 |
| L5 | 全部 10 剧本 | 需测试共享（会真实删除文件） |
| L5 | S-A4 RDP 会话(非0 SessionId) | 单会话环境无法复现，需 `RB_TEST_MULTISESSION=1` 多会话 |
| L5 | S-E1 多用户 SID 隔离 | 同上，需多会话测试机 |
| L5 | S-F5 磁盘满 fail-closed | 需注入暂存区满，需 `RB_TEST_FAULT_INJECT=1` |
| L5 | S-F13 服务重启对账 | 破坏性，需独立测试机停止/启动 rbservice |

## 测试执行中发现的问题

### 1. `/health` 在数据库未就绪时 panic（建议修复）

`service_go/api/api.go:90` 的 `handleHealth` 直接调用 `s.DB.Stats()`，未做
nil 检查。对比同文件的 `handleStats`（第 119 行）已对 `s.Stats == nil`
返回 503 —— 两者处理不一致。

影响：数据库未就绪（`openWithRetry` 重试耗尽）时，`/health` 会 panic 而不是
返回明确的 5xx。而 `/health` 正是运维判断保护是否生效的入口（RB-29 依赖其
`protected_count` 告警），此时监控只会看到"连接被重置"而非可判读的状态。

建议修复（与 `handleStats` 对齐）：

```go
if s.DB == nil {
    writeErr(w, http.StatusServiceUnavailable, "database unavailable")
    return
}
```

用例 `TestHealthWhenDatabaseUnavailable` 已固化该行为，修复后会自动转为 PASS。

### 2. RB-16 token 比较仍为非常量时间（已知待修）

`authorized()` 使用 `==` 比较。用例 `TestTokenCompareTiming_RB16Pending`
尝试用时序测量固化它，但 32 字节 token 的差异在噪声内，未能稳定检出——
该用例目前 PASS 但会在日志中记录说明。缺陷本身已在 `buglist.md` 的 RB-16 中登记。

### 3. 删除拦截盲区：`cmd del` 与 POSIX_SEMANTICS 真删（新增缺陷，建议修复）

`test/l5_e2e/test_l5_user_scenarios.py` 在 2026-09-01 实测中发现两条**驱动侧拦截盲区**
（非用例缺陷，系产品缺陷）：

- **S-B4 `cmd /c del /q <UNC>` 真删**：文件在受保护共享内被 `cmd.exe` 删除后未进回收站，
  `E:\RBStore` 无对应条目，源文件消失即丢失。minifilter 未覆盖 `cmd` 走的删除 IRP 路径
  （疑似 `SetFileInformationByHandle(FileDispositionInfo)` 之外的变体或 `FILE_OPEN_REPARSE_POINT` 等）。
- **S-B13 POSIX_SEMANTICS 删除真删**：用 `NtSetInformationFile(FileDispositionInformationEx,
  POSIX_SEMANTICS)` 直接删除同样绕过拦截（历史绕过点复现）。minifilter 对
  `FileDispositionInformationEx` 类 IRP 未做等价拦截。

影响：攻击者或误用脚本可借 `cmd /c del` 或 POSIX 工具绕过回收站直接销毁受保护文件，
使"唯一兜底"失效（S-A2 UNC 是网络位置唯一防护层，一旦被绕过即无保护）。

建议修复（驱动侧）：在 `rbminiflt` 的 IRP_MJ_SET_INFORMATION 预处理中，对
`FileDispositionInformation` 与 `FileDispositionInformationEx`（含 `POSIX_SEMANTICS` 标志）
均做拦截，并将其重定向为"移动进 RBStore"而非放行删除。

固化用例：`test_l5_user_scenarios.py` 中 `dim_b` 的 `S-B4`/`S-B13` 已断言"被拦截"，
修复后将从 FAIL 转为 PASS。当前状态：**FAIL（暴露真实缺陷）**。

已登记进 `docs/buglist.md`：
**RB-31**（`cmd /c del` 删除 IRP 未拦截，静默真删）、
**RB-32**（`FileDispositionInformationEx` + `POSIX_SEMANTICS` 绕过点，RB-22 修复在部署产物中未生效）。
二者修复验收标准即 `S-B4`/`S-B13` 由 FAIL 转 PASS。

## A~F 维度用户场景自动化 (test_l5_user_scenarios.py)

`test-plan.md` 第三章定义了用户场景矩阵（访问方式/删除方式/删除对象/还原/并发/异常）。
本文件把维度 A~F 逐条映射为可执行的真实拦截链路用例，与 `test_l5_smb_real.py` 同样
**隔离于 `E:\tmp\share\__smoke__\scenarios`**，所有删除被 rbminiflt 拦截进 `E:\RBStore` 可还原，
绝不触碰业务文件。

| 维度 | 覆盖场景 ID | 关键断言 |
|---|---|---|
| A 访问方式 | S-A1/A2/A3/A6/A7/A9/A11/A12 | UNC/映射盘/管理共享/localhost/subst 删除被拦截；受保护外与异卷不拦截（反向） |
| B 删除方式 | S-B1/B3/B4/B5/B8/B12/B13/B14/B15 | Explorer/Del、Shift+Delete、cmd del、PS Remove-Item、批量、DeleteFileW、POSIX_SEMANTICS 均被拦截；move/rename 不触发（反向） |
| C 删除对象 | S-C1/C2/C4/C5/C9/C10/C16/C18 | 单文件/空目录/0字节/只读(留只读位)/中文emoji/特殊字符/共享根/同名分目录 均正确 |
| D 还原场景 | S-D1/D5/D7/D8/D10/D16 | 立即还原、深层父目录重建、目录树全树还原、重复还原幂等、无 HIDDEN/SYSTEM、越权目标被拒 |
| E 并发 | S-E1/E3/E4 | 多用户 SID 隔离（需多会话）、单用户并发 200 文件无遗漏、交叉删/还原无竞态 |
| F 异常 | S-F5/F6/F12/F13 | 磁盘满 fail-closed（需注入）、独占锁定拒绝删除、600 洪峰孤儿 0、服务重启对账（独立机） |

环境开关（由 `run_all.ps1 -All` 或 CI 设置）：
- `RB_TEST_MULTISESSION=1`：启用 S-A4/S-E1 多会话用例（默认单会话跳过）。
- `RB_TEST_FAULT_INJECT=1`：启用 S-F5 磁盘满注入用例（默认跳过）。

运行方式：
```powershell
.\test\run_all.ps1 -All          # 含本文件 (需真实服务与共享)
python test\l5_e2e\test_l5_user_scenarios.py   # 单独运行
```

## 约定

1. **SKIP 必须带原因**：无原因的跳过等于隐藏问题。
2. **数据安全优先**：L4/L5 破坏性用例禁止在承载真实数据的机器上执行；
   L5 破坏性脚本要求目标目录存在 `.rb_test_marker` 才运行。
3. **以构建产物为准**：内核相关验证必须核对部署的 `.sys` SHA256 与构建产物
   一致（RB-27/28 教训：源码已修但产物未更新会伪造"已修复"）。
4. **中文与编码**：Windows 控制台默认 GBK，会破坏中文输出与脚本解析。
   - Python 用例：入口处 `sys.stdout.reconfigure(encoding="utf-8")`
   - PowerShell 脚本：文件保存为 **UTF-8 with BOM**，否则 PS 5.1 按 GBK 解析
     会导致中文字符串引号错乱（本次实施已踩）
5. **运行产物**：`test/_*.log` 为运行日志，已在 `.gitignore` 中忽略。
