# Bug List

- 项目：Windows 文件共享回收站 (RecycleBin for SMB)
- 范围：内核 Mini-Filter 驱动 (`rbminiflt`) + C 核心服务 (`rbservice`) + Go 管理 API (`rbapi`) + 数据模型 + 部署链路
- 日期：2026-08-31
- 评估结论：**当前状态为可演示的工程原型，不具备生产部署条件**

> 与 `bugfix-report.md` 的关系
>
> `bugfix-report.md` 记录的是**已修复的历史缺陷**（初版 Python 实现）。
> 本文件是**尚未修复的现存问题清单**，基于当前 C + Go 架构的代码审查，
> 按生产就绪度排序，供整改排期使用。
>
> 修复进度：RB-01、RB-04 ~ RB-12 已修复，详见 `bugfix-production.md`。
> RB-02 为外部流程（驱动签名），RB-03 暂缓（需测试机 + Driver Verifier）。

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
| RB-13 | P2 | Go API | 驱动计数器恒为 `nil`，`/stats` 恒 503，`/health` 的 driver 字段恒 null | 待修 |
| RB-14 | P2 | 全局 | 无监控指标导出、事件日志无 manifest、无告警规则 | 待修 |
| RB-15 | P2 | 工程 | 测试覆盖为零（无单元测试、压力测试、故障注入测试） | 待修 |
| RB-16 | P2 | Go API | Token 非恒定时间比较、注册表明文存储、无限流、无操作审计 | 待修 |
| RB-17 | P2 | 服务 | `$I` 元数据用 v1 格式，可能与 Win10+ 回收站 UI 不兼容 | 待确认 |

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

**修复建议**

由 Go 通过命名管道或 C 服务代理读取驱动统计，实现 `Stats` 接口并接入 `/stats`。

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

## 五、整改路线建议

| 阶段 | 周期 | 任务 |
|---|---|---|
| 第一阶段 止血 | 2–3 周 | RB-01 栈分配整改、RB-02 启动签名流程、RB-04 fail-closed、RB-05 孤儿对账、RB-06 路径校验 |
| 第二阶段 可观测 | 2 周 | RB-13 驱动计数器、RB-14 日志与告警 |
| 第三阶段 规模化 | 4 周 | RB-08 目录批处理、RB-09 归档与索引、RB-11 备份、RB-15 测试补齐 |
| 第四阶段 生产化 | 持续 | RB-03 回调重构、RB-12 热更新、HLK 认证、容量模型与压测基线 |

> 建议：在 RB-01、RB-02、RB-04 完成前，**不要开展任何生产试点**。
