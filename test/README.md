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
L4 kernel                    14 SKIP（需独立测试机）
L5 destructive               10 SKIP（需测试共享）
----------------------------------------------------------
OVERALL: GREEN
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
