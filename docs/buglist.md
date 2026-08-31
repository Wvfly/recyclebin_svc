# Bug List

- 项目：Windows 文件共享回收站 (RecycleBin for SMB)
- 范围：内核 Mini-Filter 驱动 (`rbminiflt`) + C 核心服务 (`rbservice`) + Go 管理 API (`rbapi`) + 数据模型 + 部署链路
- 日期：2026-08-31
- 条目：RB-01 ~ RB-22（含 5 项大目录树删除场景专项问题）
- 评估结论：**核心 P0 阻塞已消除，仍不具备生产部署条件**

> 与 `bugfix-report.md` 的关系
>
> `bugfix-report.md` 记录的是**已修复的历史缺陷**（初版 Python 实现）。
> 本文件是**尚未修复的现存问题清单**，基于当前 C + Go 架构的代码审查，
> 按生产就绪度排序，供整改排期使用。
>
> 修复进度：RB-01、RB-04 ~ RB-12 已修复，详见 `bugfix-production.md`。
> RB-02 为外部流程（驱动签名），RB-03 暂缓（需测试机 + Driver Verifier）。
> RB-18 ~ RB-22 为 2026-08-31 新增的大目录树删除场景问题（第五章）。

---

## 一、问题总览

| 编号 | 级别 | 模块 | 问题摘要 | 状态 |
|---|---|---|---|---|
| RB-01 | P0 | 驱动 | 通知结构体在内核栈上分配约 64.5 KB，远超 24 KB 内核栈 | **已修复** |
| RB-02 | P0 | 驱动/签名 | 依赖 `testsigning`，生产环境无合法签名无法加载 | 待修（外部流程） |
| RB-03 | P0 | 驱动 | Pre 回调内执行创建目录 / rename / 查询，存在死锁与延迟叠加 | 暂缓 |
| RB-04 | P0 | 驱动 | 5 处失败路径静默放行真删（fail-open），无兜底 | **已修复** |
| RB-05 | P0 | 驱动/服务 | 通知丢失产生孤儿文件，无对账机制，可撑满共享卷 | **已修复** |
| RB-06 | P0 | 服务/Go | restore 目标路径不校验，持有 token 可提权至 SYSTEM 任意写入 | **已修复** |
| RB-07 | P1 | 驱动 | 通知结构体固定 64.5 KB，队列满配约 33 MB 常驻，热路径清零开销大 | **已修复** |
| RB-08 | P1 | 驱动 | 递归删除逐文件触发通知，队列 512 瞬间击穿 | **已修复** |
| RB-09 | P1 | 数据模型 | `items` 表无归档清理；LIKE 前置通配符注入与全表扫描 | **已修复** |
| RB-10 | P1 | 服务 | 还原操作挂在 30 秒维护周期上，最长等待 30 秒 | **已修复** |
| RB-11 | P1 | 服务 | 数据库无备份、无完整性检查、无损坏恢复预案 | **已修复** |
| RB-12 | P1 | 运维 | 配置无法热更新，变更需重启服务 | 部分（用户态配置已热加载） |
| RB-13 | P2 | Go API | 驱动计数器恒为 `nil`，`/stats` 恒 503，`/health` 的 driver 字段恒 null | **已修复** |
| RB-14 | P2 | 全局 | 无监控指标导出、事件日志无 manifest、无告警规则 | 待修 |
| RB-15 | P2 | 工程 | 测试覆盖为零（无单元测试、压力测试、故障注入测试） | 待修 |
| RB-16 | P2 | Go API | Token 非恒定时间比较、注册表明文存储、无限流、无操作审计 | 待修 |
| RB-17 | P2 | 服务 | `$I` 元数据用 v1 格式，可能与 Win10+ 回收站 UI 不兼容 | 待确认 |
| RB-18 | **P1** | 服务 | 深层目录还原失败：`CreateDirectoryW` 只建一级父目录 | **已修复** |
| RB-19 | P1 | 驱动 | 删除大目录树极慢：每文件重复调用 `RbfEnsureStoreDir` | **已修复** |
| RB-20 | P1 | 驱动 | 队列满时部分删除失败，目录删不干净（RB-08 副作用） | 待修 |
| RB-21 | P1 | 驱动/服务 | staging 扁平结构，单目录文件数膨胀后操作变慢 | 待修（还原粒度已改善） |
| RB-22 | **P1** | 驱动 | `FileDispositionInformationEx` 未拦截，可绕过整树真删 | **已修复** |

> RB-18 ~ RB-22 为**大目录树删除场景**专项问题（详见第五章）。
> 该场景在单文件删除时不暴露，删除含大量子目录/文件的目录树时集中显现。

---

## 二、P0 — 硬阻塞（不得上线）

### RB-01 通知结构体在内核栈上分配约 64.5 KB，必然栈溢出

- **模块**：driver
- **位置**：`driver/rbminiflt.c` `RbfPreSetInfo`（局部变量 `RBF_NOTIFICATION note = {0};`）
- **结构定义**：`driver/rbminiflt.h` `RBF_NOTIFICATION`，其中 `FilePath[16383]`、`StorePath[16383]`、`SidString[256]`

**问题分析**

结构体实际大小约 66,068 字节（≈64.5 KB），而 x64 Windows 内核栈默认仅 24 KB。该变量声明在 Pre 回调栈上，且 `= {0}` 初始化会生成全量内存填充，`&note` 又被传入 `RbfQueueNotify`，编译器无法优化掉栈分配。

**影响**

第一次命中保护路径的删除极可能触发内核栈溢出蓝屏。这同时说明**删除拦截路径很可能从未在真实机器上端到端验证过**——现有契约测试只覆盖 C 服务的数据库逻辑，不经过内核路径。

**修复建议**

1. 改为从 Paged 池 / Lookaside 列表动态分配通知节点
2. 将 `RBF_MAX_PATH` 从 16383 降到合理值（实际业务路径通常 < 1 KB）
3. 结构体改为变长布局：定长头部 + 紧随其后的路径数据，避免固定 64 KB

**验证方法**：在测试机执行一次命中保护路径的删除，确认不蓝屏且通知内容正确。

---

### RB-02 驱动未签名，依赖 `testsigning` 无法满足生产安全基线

- **模块**：driver / 交付
- **位置**：`deploy.ps1`（步骤 2 要求 `bcdedit /set testsigning on` 并重启）

**问题分析**

开启 `testsigning` 意味着全机关闭驱动强制签名，任何未签名内核代码均可加载。

**影响**

- 等保三级、ISO 27001 及多数甲方安全基线直接不通过
- 系统显示测试模式水印，部分安全软件拒绝启动

**修复建议**

1. 立即启动 EV 代码签名证书采购
2. 走 Windows Hardware Dev Center **attestation signing**（成本最低）或完整 **WHQL / HLK** 认证（推荐）
3. 该流程外部依赖 2–6 周，应最先启动；HLK 测试套件会反向约束驱动代码质量

---

### RB-03 Pre 回调内执行文件系统写操作

- **模块**：driver
- **位置**：`driver/rbminiflt.c` `RbfPreSetInfo` 中依次调用
  - `RbfEnsureStoreDir`（创建目录，写操作）
  - `RbfMoveToStore`（rename，写操作）
  - `FltQueryInformationFile`（查询）
- **相关**：`driver/rbminiflt.h` Altitude `370030`（Activity Monitor 范围）

**问题分析**

在 minifilter 的 Pre 回调中发起新的 I/O 是高危模式：

1. **重入与死锁**：rename 会重新进入过滤器栈，经过杀软 / DLP / 备份 agent 的过滤器；若对方锁顺序相反即死锁
2. **延迟叠加**：每次删除都需同步等待目录创建与 rename 完成，高并发下删除延迟放大数倍
3. **Altitude 冲突**：`370030` 与部分安全产品范围重叠

**修复建议**

- Pre 回调只做标记，将 rename 移入 Post 回调或专用工作线程
- 至少将 `FltQueryInformationFile` 等查询移出关键路径
- 复查 Altitude 取值，避开常见安全产品

---

### RB-04 多处失败路径静默放行真删（fail-open 无兜底）

- **模块**：driver
- **位置**：`driver/rbminiflt.c` `RbfPreSetInfo` 中 4 处 `return FLT_PREOP_SUCCESS_NO_CALLBACK`

| 触发条件 | 说明 |
|---|---|
| `FltGetFileNameInformation` 失败 | 拿不到路径即放行 |
| 获取请求者 SID 失败 | SID 解析波动即放行 |
| 创建 staging 目录失败 | staging 不可用即放行 |
| 构造 staging 路径失败 | 路径构造失败即放行 |

**影响**

对回收站产品而言"静默真删"是最坏的失败模式：用户以为文件进了回收站，实际永久丢失，且系统无任何记录。

**修复建议**

1. 新增 `FailClosed` 策略开关：失败时返回 `STATUS_ACCESS_DENIED` 拒绝删除，并写事件日志
2. 生产默认 **fail-closed**；fail-open 仅作为磁盘故障时的应急开关，且必须伴随告警

---

### RB-05 通知丢失产生孤儿文件，无对账机制，可撑满共享卷

- **模块**：driver + service
- **位置**：`driver/rbminiflt.c`
  - 服务未连接时丢弃（`NotifyDropped` 计数）
  - 队列深度达上限时丢弃（`NotifyQueueFull` 计数）
  - 内存分配失败时丢弃

**问题分析**

数据流为"内核 rename 成功 → 异步通知用户态 → 写 DB"。通知丢失时，**文件已存在于 staging，但 DB 无记录**，形成孤儿文件：

- 无原路径记录 → 不可恢复
- 不出现在任何查询中 → 运维不可见
- 配额管理仅统计 `status='landed'` 的记录 → 不受配额约束
- 清理逻辑依赖 DB 记录 → 永不被清理

**影响**

staging 与业务共享同一卷（同卷 rename 约束），孤儿文件持续膨胀可致**共享卷写满、业务中断**。

**修复建议**

1. 服务启动时执行 staging ↔ DB 对账扫描，识别孤儿文件
2. 孤儿按 mtime 老化清理，并写事件日志告警
3. 将 `NotifyDropped` / `NotifyQueueFull` 纳入监控与告警

---

### RB-06 restore 目标路径不校验，存在 SYSTEM 级任意写入提权面

- **模块**：service_go + service_c
- **位置**：`service_go/api/api.go` `RestoreArg`（`Arg` 字段直接作为还原目标），`service_go/db/db.go` `RestoreItemById(itemId, argOverride, ...)`

**问题分析**

两侧均未校验：

- 还原目标是否落在受保护路径前缀内
- 请求者是否为文件所有者（无 per-user 授权，仅一个全局 token）

而 `rbservice.exe` 以 `SYSTEM` 身份执行 rename。叠加 RB-16 的弱 token 比较，构成完整提权链条。

**影响**

任何持有 token 者可将任意用户的已删除文件还原至 `C:\Windows\System32\` 等任意位置。

**修复建议**

1. 校验还原目标必须落在受保护前缀内
2. `Arg` 白名单化，默认仅允许同目录还原
3. 记录请求者身份到审计日志

---

## 三、P1 — 规模化与可靠性

### RB-07 通知结构体内存浪费

- **模块**：driver
- **位置**：`driver/rbminiflt.h` `RBF_NOTIFICATION`

每条通知固定约 64.5 KB，队列上限 512 条 → 约 33 MB 池内存常驻；典型业务路径仅 50–200 字符，利用率不足 0.3%。同时 `= {0}` 的 64 KB 清零发生在删除热路径上。

**修复建议**：随 RB-01 一并解决（变长结构 + 动态分配），队列容量按平均长度重新计算。

---

### RB-08 递归删除击穿通知队列

- **模块**：driver

**问题分析**

`rm -rf` / `rd /s` 由用户态逐文件删除，**每个文件触发一次 rename + 一次通知**。删除含 10 万文件的目录即产生 10 万条通知，队列（上限 512）瞬间打满，绝大多数退化为孤儿文件（放大 RB-05）。

**修复建议**

支持目录级批处理：对受保护目录下的子树删除，一次 rename 整棵子树 + 仅发一条通知。

---

### RB-09 表无限增长，查询全表扫描

- **模块**：数据模型 + Go API
- **位置**：`db/schema.sql`（`items.status` 含 `purged` 枚举），`service_go/db/db.go` 搜索（`orig_path LIKE '%q%'`）

**问题分析**

1. `items` 仅改 `status` 不删行，每天百万次删除 → 一年约 3.6 亿行
2. `ops` 表无清理机制，持续增长
3. `/search` 使用前置通配符 `LIKE '%q%'`，**无法使用索引**；且 `orig_path` 无索引 → 每次搜索全表扫描
4. LIKE 未转义 `%` / `_` → 通配符注入

**修复建议**

1. `items` / `ops` 按时间归档或分区，定期清理终态记录
2. 为 `orig_path` 建索引；搜索改为后缀匹配或接入全文索引
3. 对用户输入中的 `%`、`_` 做转义

---

### RB-10 落地批处理瓶颈导致 RPO 劣化

- **模块**：service_c
- **位置**：`service_c/rbsvc.h`（`RBSVC_MAINTAIN_MS` 30000、`StagedBatch` 500）

**问题分析**

维护周期 30 秒、每批 500 条。积压超过 500 条时需多个周期才能追平，恢复点目标显著劣化。此外 `ops` 轮询间隔 2 秒 → 还原操作最低 2 秒延迟。

**修复建议**

批处理改为自适应：队列中仍有积压时立即续跑下一批，而非等待下一个周期。

---

### RB-11 数据库无备份、无完整性检查、无损坏恢复预案

- **模块**：service_c / 运维

**问题分析**

`recycle.db` 为单文件、单写者，无备份策略。DB 损坏即全部元数据丢失，staging 与 `$Recycle.Bin` 中的文件全部退化为不可定位的孤儿（放大 RB-05）。

**修复建议**

1. 定期 `VACUUM` 与 `PRAGMA integrity_check`
2. 定期备份 `recycle.db`（利用 SQLite 在线备份 API，避免停服）
3. 制定 DB 损坏后的恢复预案并演练

---

### RB-12 驱动与配置无法热更新

- **模块**：运维

**问题分析**

minifilter 卸载常被未决 I/O 阻塞 → 驱动更新需重启服务器；改注册表配置需重启服务；schema 升级需停服。生产环境无重启窗口即无法变更。

**修复建议**

1. 配置支持热加载（信号或控制码触发重读）
2. 制定驱动更新的维护窗口流程与回滚方案
3. schema 变更走前向兼容的迁移脚本

---

## 四、P2 — 安全与可观测性

### RB-13 驱动计数器恒不可达

- **模块**：service_go
- **位置**：`service_go/main.go`（`api.New(database, cfg.Token, nil)`，第三个参数硬编码 `nil`）

**问题分析**

导致 `/stats` 恒返回 503、`/health` 的 `driver` 字段恒为 `null`。而 `RBF_STATS`（`driver/rbminiflt.h`）中的 `RenameFail`（真删次数）、`NotifyDropped`、`NotifyQueueFull`、`MaxQueueDepth` 恰是最关键的健康指标。

**影响**

运维完全看不到"文件被静默真删"的次数，使 RB-04 与 RB-05 在生产上不可见。

**修复方案（已实施）**

**关键约束**：通信端口 `MaxConnections = 1`（`driver/rbminiflt.c` 的
`FltCreateCommunicationPort` 末位参数），且该连接已被 C 服务占用，
因此 **Go 侧无法自行连接端口查询**——`FilterSendMessage` 会被拒绝。
`main.go` 中原注释已预留此方案。

采用 **C 服务采样 → 数据库快照 → Go 只读** 的链路：

```
rbservice (ops 线程, 每 5s)               rbapi (只读)
    PortQueryStats()                          │
         │                                    ▼
         ▼                            GET /stats, /health
  driver_stats 单行表  ─────────────────────────┘
```

- `db/schema.sql` 新增 `driver_stats` 单行表（`CHECK (id = 1)`），
  含全部累计计数器 + `queue_depth` / `max_queue_depth` + 采样时间 `ts`
- C 侧 `PortSampleStats()` 采样写入，`RBSVC_STATS_INTERVAL = 5` 秒
- Go 侧 `DriverStatsMap()` 读取并接入 `api.New(..., database.DriverStatsMap)`

**陈旧判定（关键设计）**

这些是**累计计数器**，"驱动没应答"与"真的 0 次删除"在数值上无法区分。
若直接返回 0，运维会看到 `delete_denied = 0` 而误判系统健康。

因此引入 `RBSVC_STATS_STALE_SEC = 30`：快照超过 30 秒未更新即判定
**驱动离线**，返回 503 而非数值。三态语义：

| 状态 | 表现 |
|---|---|
| 无快照 | 503（unknown） |
| 快照新鲜 | 返回真实计数器 |
| 快照陈旧 | 503（offline） |

**顺带修复的缺陷**：`g_PortLock` 原先在端口线程内 `InitializeCriticalSection`
/ `DeleteCriticalSection`，导致 `once` / console 模式（不启动端口线程）调用
`PortQueryStats()` 时访问未初始化的临界区而崩溃——由契约测试捕获。
已重构为 `PortInit()` / `PortFini()` 跟随进程生命周期（幂等，
`InterlockedCompareExchange` 保护）。

**验证**：新增 `db/verify_stats_endpoint.ps1`，启动真实 `rbapi.exe` 实测三种
状态（无快照 / 新鲜 / 陈旧），均通过。

> 该脚本需占用端口且要拉起进程，故未并入 `build_all.cmd` 默认流程，
> 以免 CI 上的端口冲突让构建变得不稳定；需要时手动运行。

---

### RB-14 无监控指标、事件日志无 manifest、无告警规则

- **模块**：全局

**现状**

- 无 Prometheus / WMI / 性能计数器导出
- C 服务写事件日志但未注册 event manifest → 事件查看器显示"找不到描述"
- 无结构化日志，无法接入 ELK / Splunk
- 未定义任何告警规则

**修复建议**

1. 注册 event manifest，输出结构化日志
2. 导出关键指标：真删次数、丢弃次数、队列深度、staging 占用、DB 大小
3. 定义告警阈值（`RenameFail > 0`、`NotifyDropped > 0`、磁盘水位等）

---

### RB-15 测试覆盖为零

- **模块**：工程

**现状**

全仓库无 Go 单元测试文件；无驱动 HLK 测试、无压力测试、无故障注入测试（服务崩溃、DB 锁、磁盘满场景）。现有 `db/verify_contract.py`（9 项）与 `db/verify_c_contract.py`（7 项）仅覆盖 schema 契约，不覆盖行为。

**修复建议**

1. 补 Go 层单元测试与 API 测试
2. 补故障注入测试（服务中断、DB 锁、磁盘满）
3. 建立删除压测基线（并发删除、递归删除）

---

### RB-16 Token 与审计安全细节

- **模块**：service_go
- **位置**：`service_go/api/api.go`（token 比较）、`service_go/main.go`（注册表读写 token）

| 子项 | 问题 |
|---|---|
| Token 比较 | 非恒定时间比较，存在时序侧信道 |
| Token 存储 | 注册表明文存储 |
| 访问控制 | 仅单一全局 token，无 per-user 授权 |
| 限流 | 无限流保护 |
| 审计 | 无操作审计（谁在何时还原了什么） |

**修复建议**

改用 `subtle.ConstantTimeCompare`；存储时加密；引入 per-user 授权与速率限制；`ops` 表增加请求者字段。

---

### RB-17 `$I` 元数据格式版本兼容性（待确认）

- **模块**：service_c
- **位置**：`service_c/rbstore.c`（`$I` 文件写入）

**问题**

当前写入 v1 格式（WinXP 时代的 20 字节头）。Win10+ 回收站使用 v2 格式（支持长路径与更大时间戳）。

**影响**

可能导致 Windows 回收站 UI 或第三方工具解析异常。

**建议**

在真实 Win10/Win11 环境验证 `$I` 文件可被系统正确解析；若存在兼容问题，升级到 v2 格式并在 schema 中记录格式版本。

---

## 五、大目录树删除场景（RB-18 ~ RB-22）

> **场景背景**
>
> Windows 删除目录是**用户态递归**：Explorer / `rd /s` / `rm -rf` 逐个枚举文件并
> 对每个条目发起删除 I/O。因此删除含 N 个文件的目录树 = **N 次独立拦截**，
> 每次都要走一遍 `RbfPreSetInfo` 的完整流程。
>
> 单文件时被掩盖的问题（每次调用的小开销、队列容量、父目录创建）在此场景下
> 被放大 N 倍，并暴露出几个单文件场景根本看不到的问题。

### RB-18 深层目录还原失败

- **模块**：service_c
- **位置**：`service_c/rbrestore.c`（还原前创建父目录）
- **级别**：P1（数据不丢，但核心的"可还原"能力失效）

**问题**

还原前用 `CreateDirectoryW` 创建父目录，而该函数**只能创建单级**：

```c
WCHAR *slash = wcsrchr(dstDos, L'\\');
if (slash && slash != dstDos) {
    ...
    /* CreateDirectoryW is recursive-ish only if parents exist;
       use SHCreateDirectoryExW-equivalent via CreateDirectory loop */
    CreateDirectoryW(parent, NULL);      // 只创建一级
}
```

代码注释本身已承认该限制（"recursive-ish only if parents exist"）。

**复现路径**

1. 删除目录树 `D:\Share\proj\`（含 `src\main.c`）
2. 每个文件独立进入回收站，`proj` 目录本身也被删除
3. 还原 `main.c` → 目标 `D:\Share\proj\src\main.c`
4. parent = `D:\Share\proj\src`，但 `D:\Share\proj` 不存在
5. `CreateDirectoryW` 失败 → `MoveFileEx` 到不存在路径 → **还原失败**

**影响**

删除复杂目录树后，深层文件不可还原。文件仍在 `$Recycle.Bin` 中（数据未丢），
但用户无法通过正常途径取回——**产品核心价值失效**。

**修复方案（已实施）**

在 `rbrestore.c` 中实现 `EnsureDirectoryChain()`，逐级调用 `CreateDirectoryW`
创建整个父目录链，把 `ERROR_ALREADY_EXISTS` 视为成功（幂等）。

路径为驱动器绝对路径（`DestIsAllowed()` 已在此前拒绝 UNC、设备路径与 `..`
组件），长度用 `RBSVC_MAX_RECON_PATH`（1024）而非 `MAX_PATH`。

> 未采用 `SHCreateDirectoryExW` 的原因：本项目定义了 `WIN32_LEAN_AND_MEAN`，
> 引入 `shlwapi.h` 后该函数仍未被声明，产生 `C4013`（编译器假设返回 int，
> 在 x64 下存在返回值截断风险）。改为自实现可消除额外链接库依赖与 SDK
> 版本差异，行为完全可控。

---

### RB-19 删除大目录树极慢

- **模块**：driver
- **位置**：`driver/rbminiflt.c` `RbfPreSetInfo`

**问题**

每个文件删除在 Pre 回调内**同步串行**执行：

| 操作 | 每个文件调用次数 |
|---|---|
| `RbfEnsureStoreDir` | 1 次（内部 2 次 `ZwCreateFile`：`<vol>\RBStore` 与 `<vol>\RBStore\<Sid>`） |
| `RbfMoveToStore` | 1 次 rename |
| `FltQueryInformationFile` | 1 次 |

`RbfEnsureStoreDir` **对每个文件都重复调用**，即使目录早已存在——仍要执行
完整的打开 + 关闭内核对象流程。

删除 5000 个文件 = 10000 次目录打开 + 5000 次 rename + 5000 次查询，
全部**同步阻塞**在删除路径上。

**影响**

按每次增加 1 ms 估算，5000 文件额外耗时约 5 秒；磁盘繁忙时（每次 5 ms）
可达 25 秒以上。用户表现为进度条停滞、资源管理器假死。

注意：这是**慢**，不是死锁——所有操作最终都会返回。

**修复方案（已实施）**

在 `rbminiflt.c` 中增加 **staging 目录指纹缓存**（RB-19）：

- 对 `(卷名, SID)` 计算 FNV-1a 指纹，16 槽定长数组
- `RbfEnsureStoreDir` 命中缓存直接返回，**跳过池分配与两次 `ZwCreateFile`**
- 槽位用 `InterlockedCompareExchange` 读写，**无锁、快路径不阻塞**
- 槽位用尽时轮转淘汰（丢失条目仅多一次目录打开，无正确性影响）

**陈旧条目自愈**：若管理员删除了 `\RBStore` 或卷被卸载，缓存会误判目录存在，
此时 rename 失败 → `RbfStoreCacheForget()` 清除指纹 → 下一次删除重建目录。
配合 fail-closed，数据在任何情况下都不会丢失。

> 更彻底的解法是把这些 I/O 移出 Pre 回调（RB-03），但那属于高风险重构，
> 需测试机 + Driver Verifier 验证，暂缓。

---

### RB-20 队列满时部分删除失败

- **模块**：driver
- **位置**：`driver/rbminiflt.c`（`RbfReserveQueueSlot` 失败分支）
- **关联**：RB-08 修复的副作用

**问题**

RB-08 将入队改为"预留式"（rename 前占位），消除了孤儿文件，但引入了新的
用户可见行为：队列满（512）时**拒绝删除**。

删除大目录树时：

```
前 512 个文件 → rename 成功，进入回收站   ✅
第 513 个    → 队列满 → ACCESS_DENIED    ❌（文件留在原地）
```

**影响**

**目录删不干净**：部分文件已进回收站，部分仍在原目录。用户需反复重试。
数据未丢失（fail-closed 生效），但体验很差，且留下"半删除"状态。

**修复建议**

1. 扩大队列容量（当前 512，对大目录树偏小）
2. 或改为按 SID 分片队列，避免单个用户的大批量删除挤占全部容量
3. 或在删除失败时向用户态返回可重试提示，由 Explorer 的重试机制消化

---

### RB-21 staging 扁平结构导致目录膨胀

- **模块**：driver + service_c
- **位置**：`driver/rbminiflt.c` `RbfBuildStorePath`

**问题**

暂存路径为**扁平**结构：

```
<vol>\RBStore\<Sid>\<seq>_<basename>
```

不保留原目录层级。删除 5000 文件的目录树 → staging 单目录下塞入 5000 个文件。

**影响**

1. **NTFS 单目录膨胀**：文件数达数万后，目录索引查找变慢，
   而 `RbfEnsureStoreDir` 每次删除都要打开该目录 → 与 RB-19 叠加恶化
2. **落地后回收站膨胀**：`$Recycle.Bin\<Sid>\` 变成 N 个 `$R`/`$I` 对，
   Explorer 打开回收站明显卡顿
3. **还原粒度问题**：目录树退化为 N 个独立条目，用户无法"整体还原目录"
   —— **已通过新增 `restore-tree` 批量还原缓解**（见下方说明）

**修复建议**

1. staging 路径保留部分层级（如按原路径 hash 分桶到子目录），控制单目录文件数
2. 还原 API 支持"按前缀批量还原"，把目录树还原作为一个操作

**部分实施（还原粒度已解决）**

新增 `restore-tree` op 类型，一次请求还原整个目录树：

```powershell
Invoke-RestMethod "http://127.0.0.1:8800/ops" -Method Post -Headers $h `
  -Body '{"type":"restore-tree","arg":"D:\\Share\\Project"}' `
  -ContentType "application/json"
```

实现要点：

- **安全校验复用**：前缀经 `DestIsAllowed()` 同一 allow-list 校验（RB-06）。
  该 rename 以 SYSTEM 执行且请求经共享 `ops` 表传入，未校验的前缀等同于
  任意写入原语——因此校验位于 C 侧执行点，而非 Go 侧调用方
- **真前缀匹配**：`orig_path LIKE ? ESCAPE '\'` 且要求分隔符边界，
  `D:\Share\Project` 不会连带匹配 `D:\Share\ProjectBackup`
- **LIKE 转义**：用户输入的 `%`、`_`、`\` 均转义，避免通配符注入
- **限量与可中断**：单次上限 `RBSVC_MAX_TREE_RESTORE`（5000），
  条目间检查 `g_StopEvent` 以便优雅停机
- **部分失败如实报告**：各条目 rename 相互独立，不回滚已成功项，
  结果消息形如 `restored 41/42; 1 failed (first: id=123: ...)`

> 未对 `orig_path` 建索引：每次拦截删除都插入行，索引会持续拖累热路径；
> 而批量还原是低频管理操作，配合 RB-09 的终态归档，全表扫描可接受。

**仍待解决**：staging 扁平结构本身（RB-21a 前半部分）未改动——
扁平化是 B1 的正确权衡（避免在内核态重建深层目录树导致 rename 失败），
仅在大目录树场景有性能影响，优先级低。

---

### RB-22 `FileDispositionInformationEx` 未被拦截（可绕过）

- **模块**：driver
- **位置**：`driver/rbminiflt.c` `RbfPreSetInfo` 的过滤条件

**问题**

```c
if (Data->Iopb->Parameters.SetFileInformation.FileInformationClass
        != FileDispositionInformation)
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
```

只拦截 `FileDispositionInformation`，**不拦截** `FileDispositionInformationEx`。

Windows 10 1709+ 引入的 `FileDispositionInformationEx` 支持
`FILE_DISPOSITION_POSIX_SEMANTICS` 标志，可**一次性删除非空目录**。

**影响**

使用该 API 的应用程序可绕过拦截，**整棵目录树被静默真删**，
且完全不产生任何通知或计数——这是唯一会导致数据真正丢失的路径。

当前 Explorer 与 `rd /s` 仍使用传统递归删除（会逐个触发本驱动），
所以实际影响暂未显现，但这是明确的绕过面。

**修复方案（已实施）**

`RbfPreSetInfo` 的过滤条件由"仅接受 `FileDispositionInformation`"改为
按类别分派：

```c
if (fic == FileDispositionInformation) {
    /* 旧式：检查 dispInfo->DeleteFile 布尔标志 */
} else if (fic == FileDispositionInformationEx) {
    /* 新式：检查 Flags & FILE_DISPOSITION_DELETE */
    /* FILE_DISPOSITION_DO_NOT_DELETE (0) 表示"取消删除"，直接放行 */
} else {
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}
```

两类删除标记此后走同一条拦截路径（路径匹配 → 取 SID → 预留槽位 →
rename → 入队），整树删除不再能绕过。

> 注：`FILE_DISPOSITION_INFORMATION_EX` 与 `FILE_DISPOSITION_DELETE` 直接使用
> WDK 头文件定义，未做兼容性别名——SDK 对该结构体的守卫宏名无法跨版本可靠
> 检测，自行 typedef 会与真实定义冲突。构建本驱动需 1709 或更新版本的 WDK。

---

## 六、整改路线建议

| 阶段 | 周期 | 任务 |
|---|---|---|
| 第一阶段 止血 | 2–3 周 | RB-01 栈分配整改、RB-02 启动签名流程、RB-04 fail-closed、RB-05 孤儿对账、RB-06 路径校验 |
| 第二阶段 可观测 | 2 周 | RB-13 驱动计数器、RB-14 日志与告警 |
| 第三阶段 规模化 | 4 周 | RB-08 目录批处理、RB-09 归档与索引、RB-11 备份、RB-15 测试补齐 |
| 第四阶段 生产化 | 持续 | RB-03 回调重构、RB-12 热更新、HLK 认证、容量模型与压测基线 |
| **新增 目录树场景** | **1–2 周** | ~~RB-18 递归建父目录~~、~~RB-19 目录缓存~~、~~RB-22 拦截 DispositionEx~~ **已完成**；RB-20 / RB-21 仍待排期 |
| **可观测性** | 已完成 | ~~RB-13 驱动计数器打通~~ **已完成**（含 `PortLock` 生命周期缺陷修复） |

> 建议：在 RB-01、RB-02、RB-04 完成前，**不要开展任何生产试点**。
>
> **RB-18 建议优先修复**：仅一行代码改动，却解决了"删除复杂目录树后无法还原"
> 这一核心功能失效问题，投入产出比最高。
