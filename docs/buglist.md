# Bug List

- 项目：Windows 文件共享回收站 (RecycleBin for SMB)
- 范围：内核 Mini-Filter 驱动 (`rbminiflt`) + C 核心服务 (`rbservice`) + Go 管理 API (`rbapi`) + 数据模型 + 部署链路
- 日期：2026-08-31（2026-09-01 增补 RB-31/RB-32 拦截盲区；2026-09-02 增补 RB-33/RB-34 并实测坐实；
  **2026-09-03 修复 RB-33**（`5dd407a`，Explorer 删 SMB 实测拦截并成功还原）并新增 RB-35
  （初判中文路径，同日复核实为 `LocalSystem` 服务对共享目录无写权限，所有还原均失败））
- 条目：RB-01 ~ RB-35（含 5 项大目录树删除场景专项问题、3 项删除拦截盲区、2 项端口线程死锁相关：
  RB-34 取消等待 + RB-34b 同步采样）
- 评估结论：**核心 P0 阻塞已消除，仍不具备生产部署条件**
  （~~RB-33 为 Explorer / 资源管理器删除的确定性盲区~~ **已于 2026-09-03 修复并实测验证**：
  驱动新增 `IRP_MJ_CREATE` pre/post 回调拦截 `FILE_DELETE_ON_CLOSE`，Explorer 删 SMB 文件
  实测进 RBStore 且可还原。**上线前仍需 Driver Verifier 测试机验证**（本修复在开发环境验证，
  未经 Verifier）；另新增 RB-35 还原缺陷（初判中文路径，2026-09-03 复核实为
  `LocalSystem` 服务对共享目录无写权限，所有还原均失败））

> 与 `bugfix-report.md` 的关系
>
> `bugfix-report.md` 记录的是**已修复的历史缺陷**（初版 Python 实现）。
> 本文件是**尚未修复的现存问题清单**，基于当前 C + Go 架构的代码审查，
> 按生产就绪度排序，供整改排期使用。
>
> 修复进度：RB-01、RB-04 ~ RB-12 已修复，详见 `bugfix-production.md`。
> RB-02 为外部流程（驱动签名），RB-03 暂缓（需测试机 + Driver Verifier）。
> RB-18 ~ RB-22 为 2026-08-31 新增的大目录树删除场景问题（第五章）。
> RB-31、RB-32 为 2026-09-01 **实测拦截盲区**新增（详见第六章"可靠性补强"末尾）：
> 维度 B 用例 S-B4（`cmd /c del`）与 S-B13（POSIX_SEMANTICS）在真实 SMB 拦截链路下
> 仍 FAIL，证明文件删除路径存在未覆盖的 IRP 子类，受保护文件可绕过回收站真删。
> **2026-09-02 复核修正**：S-B4 的 FAIL 系当时部署的 `.sys` 为 8/31 旧产物所致；
> 正确部署 9/01 产物后，`SET_INFORMATION` 类删除（含 `cmd /c del` 同类路径）实测拦截正常，
> RB-31 降级为"待用正确产物复测验收"（详见 RB-31 条目内的状态修正）。
> **RB-22 虽标记"已修复"，但 RB-32 实测显示该修复在部署产物中未生效（疑似构建产物
> 未更新或未覆盖 `cmd` 删除 IRP），两缺陷一并登记、相互印证。**
> RB-23 为通信端口生命周期问题（client port use-after-free，蓝屏风险），见第六章。
> RB-24 ~ RB-26 为 2026-08-31 蓝屏风险复审新增（RB-24 为 RB-23 的残余竞态补强）。
> RB-27 ~ RB-28 为 2026-08-31 **实测蓝屏**（0x3B / 0xA）dump 分析新增，详见第六章。
> RB-29 为 2026-08-31 **实测部署验证**新增：驱动加载后对已挂载卷的过滤生效存在窗口，
> 期间删除完全无保护（用户文件实测被永久删除）。同日对 RB-17 取证后**判定为误判并撤回**
> （`$I` 实为 Explorer 原生 v2 格式，桌面回收站实测可见）。RB-29 已于同日修复并实测验证。
> RB-30 为 2026-08-31 **实测还原验证**新增：restore 用 `MoveFileExW` 从 `$Recycle.Bin` 移回时
> **保留回收站的 `Hidden+System` 属性**，还原后的目录/文件在 Explorer 与 SMB 客户端不可见
> （用户误以为还原失败）。同日已修复并实测验证（还原后清除属性位）。详见第六章。
> RB-33、RB-34 为 2026-09-02 **部署与拦截链路实测**新增（详见第六章"六之三"）：
> **RB-33 是 Explorer / 资源管理器删除（`FILE_DELETE_ON_CLOSE`）的确定性盲区**，已用
> 同一共享目录的对照实验坐实（SET_INFORMATION 拦得住、DELETE_ON_CLOSE 拦不住）；
> 这是用户交互式删除的主力路径，意味着**保护对日常操作实际无效**，级别按 P0 对待。
> **RB-34 是 C 服务端口线程的无限等待死锁**：驱动每次卸载/重载都会把
> `rbservice.exe` 的端口线程永久挂死，表现为 `/health` 的 `driver` 变 `null`
> （假性"驱动掉线"），服务日志也一并停止，是本次排查过程中最大的误导源。

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
| RB-17 | P2 | 服务 | `$I` 元数据格式兼容性 | **误判（已撤回）** |
| RB-18 | **P1** | 服务 | 深层目录还原失败：`CreateDirectoryW` 只建一级父目录 | **已修复** |
| RB-19 | P1 | 驱动 | 删除大目录树极慢：每文件重复调用 `RbfEnsureStoreDir` | **已修复** |
| RB-20 | P1 | 驱动 | 队列满时部分删除失败，目录删不干净（RB-08 副作用） | 待修 |
| RB-21 | P1 | 驱动/服务 | staging 扁平结构，单目录文件数膨胀后操作变慢 | 待修（还原粒度已改善） |
| RB-22 | **P1** | 驱动 | `FileDispositionInformationEx` 未拦截，可绕过整树真删 | **已修复** |
| RB-23 | **P0** | 驱动 | client port 断开后队列残留节点持悬空端口，发送线程 use-after-free 可致蓝屏 | **已修复** |
| RB-24 | **P0** | 驱动 | `RbfQueueNotify` 端口快照与入队之间的 disconnect 竞态，节点持已释放端口入队（RB-23 残余窗口） | **已修复** |
| RB-25 | **P1** | 驱动 | `RbfAllocNotify` 失败分支未释放预占队列槽位，泄漏至满后所有删除被拒 | **已修复** |
| RB-26 | **P2** | 驱动 | `RbfLoadConfig` 解析 `REG_MULTI_SZ` 未按 `DataLength` 限界，畸形注册表值可越界读 | **已修复** |
| RB-27 | **P0** | 驱动 | `RbfPortMessage` 回调 7 参数签名 vs FltMgr 实际 6 参数 → 参数错位，把 `InputBufferLength`(=4) 当指针解引用 → **蓝屏 0x3B** | **已修复** |
| RB-28 | **P0** | 驱动 | 卸载路径把 `PsCreateSystemThread` 返回的**句柄**当内核对象指针传给 `KeWaitForSingleObject` → 解引用无效地址 → **蓝屏 0xA** | **已修复** |
| RB-29 | **P1** | 驱动/部署 | 驱动加载后对已挂载卷的 attach/过滤生效存在窗口（17:31 加载、17:41 才 attach E:），期间删除无保护、文件被永久删除 | **已修复** |
| RB-30 | **P1** | 服务 | 还原后目录/文件保留回收站 `Hidden+System` 属性，Explorer/SMB 客户端不可见（实测） | **已修复** |
| RB-31 | **P2** | 驱动 | `cmd /c del` 删除 IRP 未被拦截（2026-09-02 复核：系部署旧 `.sys` 所致，`SET_INFORMATION` 路径实测已拦截） | **待验收**（用正确产物复测 S-B4） |
| RB-32 | **P1** | 驱动 | `FileDispositionInformationEx` + `POSIX_SEMANTICS` 仍被绕过，整树/单文件真删 | **待修**（RB-22 源码已修、需用正确产物复测） |
| RB-33 | **P0** | 驱动 | **`FILE_DELETE_ON_CLOSE`（Explorer / 资源管理器删除）完全无拦截**，受保护文件 100% 真删（2026-09-02 对照实验坐实） | **已修复**（2026-09-03 `5dd407a`，CREATE pre/post 拦截 DOC 删除，Explorer 删 SMB 实测进 RBStore 并成功还原） |
| RB-34 | **P1** | 服务 | 驱动卸载/重载后端口线程双向死锁（端口线程同步等 `FilterSendMessage` 完成事件 + worker 卡在 `FilterSendMessage`，`CancelIoEx` 无效），C 服务静默、计数停采样 → 假性"驱动掉线" | **已修复**（2026-09-02 mini-dump 实证后二次加固：单向 overlapped 只读 + `g_LastMsgTick` 被动存活判定 + 关闭句柄唤醒 pending 发送，编译通过、契约 10/10） |
| RB-35 | **P0** | 服务/部署 | 还原**以 `LocalSystem` 运行**的 `rbservice` 对共享目录**无写权限**，`MoveFileExW` 全部失败，`win32=5`（`ERROR_ACCESS_DENIED`）；纯 ASCII 路径（id=4）亦复现 → **非中文路径问题**，根因在**两端**（目标目录写回 + 暂存文件 DACL，驱动 rename 保留了删除者 ACE） | **已修复**（方案A 部署授权 + 方案C 取所有权，`rbrestore.c`/`deploy.ps1` 已改并验证 `id=1`→done） |

> RB-18 ~ RB-22 为**大目录树删除场景**专项问题（详见第五章）。
> 该场景在单文件删除时不暴露，删除含大量子目录/文件的目录树时集中显现。
> RB-23 为通信端口生命周期问题（详见第六章），服务重启/崩溃时高概率触发。
>
> **RB-33 是用户日常删除的主力路径**（资源管理器按 Delete / 右键删除），
> 拦截盲区直接等价于"保护对交互式操作无效"，故虽为新增条目，优先级等同 P0。

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

### RB-17 `$I` 元数据格式兼容性（误判，已撤回）

- **模块**：service_c
- **位置**：`service_c/rbstore.c`（`WriteIFile`，`$I` 文件写入）
- **状态**：**误判（2026-08-31 取证后撤回）**

**撤回经过**

初判基于一次容错解析脚本（自研解析器用错偏移）得出"服务端 `$I` 是 v1 自定义格式、
Explorer 无法解析"的结论。2026-08-31 实测取证推翻该结论：

1. **字节级取证**：让 Explorer 通过 VisualBasic 回收站 API 生成原生 `$I` 条目
   （`$ILBZ7HH`），与服务端 land 生成的 `$IB13UIN.txt` 逐字节比对——布局完全一致：
   `0x00 FILETIME + 0x08 文件大小 + 0x10 原始大小 + 0x18 文件名长度 + 0x1C UTF-16LE 完整路径`，
   即 **Explorer 原生 v2 格式**，且原生条目同样存完整路径、同样带扩展名
2. **UI 实测**：`rbfmt_probe.txt` 的 `$I`/`$R` 落在 `E:\$RECYCLE.BIN` 后，
   桌面回收站（Explorer COM 枚举）**正常显示**（221 条目之一，中文名渲染正常）
3. 用户"看不到"被删文件的**真正原因**是 RB-29（文件被永久删除，从未产生 `$I`/`$R`），
   而非 `$I` 格式问题

**结论**

- `$I` 格式与命名均与 Explorer 原生一致，回收站显示正常，**无需修改**
- 该缺陷描述从 buglist 撤回；相关"桌面回收站不可见"现象由 RB-29 解释并已随 RB-29 修复

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

> **状态修正（2026-09-01 实测）**：上述修复在源码层面已合入，但维度 B 用例
> **S-B13（POSIX_SEMANTICS）在真实拦截链路下仍 FAIL**，说明部署的 `.sys` 产物
> 未包含该修复或拦截路径未实装（典型的"源码已修、产物未更新"——同 RB-27/28 教训）。
> 同时 S-B4（`cmd /c del`）表明 `cmd` 删除走的另一条 IRP 子类同样未被覆盖。
> 故将拦截盲区拆分为 **RB-31 / RB-32** 重新登记，本条目保留为"修复已实现、待部署验证"。

---

## 六、可靠性补强（RB-23 ~ RB-28）

### RB-23 client port use-after-free（蓝屏风险）

- **模块**：driver
- **级别**：P0
- **位置**：`driver/rbminiflt.c` `RbfPortDisconnect` / `RbfSendThread`；`driver/rbminiflt.h` `RBF_GLOBAL`
- **状态**：**已修复**

**问题**

`RbfPortDisconnect()` 旧实现只将 `G.ClientPort` 置为 `NULL`。但队列中每个
`RBF_NOTIFY_NODE` 在入队时快照了 `PFLT_PORT`（`node->Port`），发送线程后续用
该指针调用 `FltSendMessage()`。当服务重启或崩溃、连接被内核拆除时，disconnect
回调返回后内核随即释放 client port 对象，队列中的残留节点成为悬空指针——
**use-after-free，可致蓝屏**。

典型触发场景：`rbservice.exe` 重启/崩溃时通知队列尚有积压（大目录树删除、
批量备份轮转等），旧端口已释放而发送线程仍在消费队列。

**修复方案（已实施）**

1. `RBF_GLOBAL` 新增 `Sending` 标志（仅在 `QueueLock` 下读写）：发送线程进入
   `FltSendMessage` 前置位、返回后清除。
2. `RbfPortDisconnect()` 在回调返回前完成三件事（回调期间内核持有端口引用，
   在途发送安全）：
   - 置 `G.ClientPort = NULL`，拒绝新通知入队；
   - 轮询等待 `G.Sending == FALSE`（连接断开时 `FltSendMessage` 立即返回
     `STATUS_PORT_DISCONNECTED`，等待时间很短）；
   - 在**同一次锁获取**内摘除队列全部节点（与 `!G.Sending` 检查原子），
     锁外释放，杜绝"检查后、摘除前"发送线程再取到旧节点的窗口。
3. 丢弃的待发通知由 RB-05 孤儿对账兜底：服务重连后对账会为已 staging 的文件
   补建数据库行，不产生不可恢复的数据丢失。

### RB-24 端口快照与入队之间的竞态（RB-23 残余窗口，蓝屏风险）

- **模块**：driver
- **级别**：P0
- **位置**：`driver/rbminiflt.c` `RbfQueueNotify`
- **状态**：**已修复**

**问题**

RB-23 修复覆盖了"已在队列中的节点"与"发送线程在途的节点"，但 `RbfQueueNotify`
的端口快照在**锁外**进行：`port = G.ClientPort` 快照成功后、节点入队前，disconnect
回调可能已执行完毕并返回（内核随即释放端口），节点随后带着已释放的端口入队，
发送线程 `FltSendMessage()` 命中悬空指针——与 RB-23 同一类 UAF。

**修复（已实施）**

入队时在**同一次锁获取**内校验 `G.ClientPort == 快照端口`，不等则释放节点与通知、
归还预占槽位并计 `NotifyDropped`，返回 `STATUS_PORT_DISCONNECTED`。节点一旦在锁内
插入成功，disconnect 回调的排队清空逻辑必然覆盖它，竞态窗口闭合。

### RB-25 队列预占槽位泄漏

- **模块**：driver
- **级别**：P1
- **位置**：`driver/rbminiflt.c` `RbfPreSetInfo`（`RbfAllocNotify` 失败分支）
- **状态**：**已修复**

**问题**

rename 已成功、`RbfAllocNotify` 失败（内存不足/路径超长）时，步骤 5.2 预占的
队列槽位未归还。每次失败泄漏一个 Reserved 槽，累积满 `RBF_QUEUE_MAX`（512）后
所有删除被拒——fail-closed 保数据但**功能永久退化**，需重启服务才能恢复。

**修复（已实施）**

失败分支调用 `RbfReleaseQueueSlot()` 归还槽位。文件已 staging 但无 DB 行，
由 RB-05 孤儿对账补录。

### RB-26 注册表 MultiSz 解析越界读（防御性）

- **模块**：driver
- **级别**：P2
- **位置**：`driver/rbminiflt.c` `RbfLoadConfig`
- **状态**：**已修复**

**问题**

`ProtectedPaths`（`REG_MULTI_SZ`）解析时以 `while (*p)` 遍历且无 `DataLength`
边界约束。标准 MultiSz 以双 NUL 结尾，但若注册表值被写为畸形数据（无尾随 NUL），
遍历可越界读池缓冲，潜在访问无效地址。

**修复（已实施）**

以 `(PBYTE)kvpi->Data + kvpi->DataLength` 为硬边界，所有读取（含 `sizeof(WCHAR)`
对齐）均先做边界检查。

### RB-27 `RbfPortMessage` 回调参数错位（实测蓝屏 0x3B）

- **模块**：driver
- **级别**：P0
- **位置**：`driver/rbminiflt.c` `RbfPortMessage`；`driver/rbminiflt.h`（回调声明）
- **状态**：**已修复**
- **来源**：2026-08-31 实测蓝屏 dump（MEMORY.DMP，15:12）定位

**问题**

`RbfPortMessage` 被声明为**旧 7 参数签名**：

```c
NTSTATUS RbfPortMessage(PFLT_PORT ClientPort, PVOID ServerPortCookie,
                        PVOID InputBuffer, ULONG InputBufferLength, ...)
```

但 FltMgr 消息回调实际只传 **6 个参数**（`ClientPort, InputBuffer, InputBufferLength,
OutputBuffer, OutputBufferLength, SyncIo`），**不带 `ServerPortCookie`**——
`ServerPortCookie` 只能通过 `FltGetServerPortCookie()` 单独查询。于是所有参数在
寄存器层面**左移一位**：

| 寄存器 | FltMgr 实际传入 | 7 参数回调误认为 |
|---|---|---|
| rcx | `ClientPort` | `ClientPort` |
| rdx | `InputBuffer`（服务端 4 字节 `RBF_REPLY`） | `ServerPortCookie` |
| r8 | `InputBufferLength` = **4** | `InputBuffer` 指针 ← 崩溃点解引用 |
| r9 | `OutputBuffer` 指针 | `InputBufferLength` |

**崩溃现场**（`rbminiflt+0x2b2b`，Bugcheck `0x3B SYSTEM_SERVICE_EXCEPTION`，
二次异常 `c0000005` 访问违规）：

```
cmp dword ptr [r8],1    ; r8 = 0x4 → 访问地址 0x4 → AV
```

代码把 `InputBufferLength` 的**数值 4** 当指针解引用 → 访问 `0x4` → 蓝屏。
触发者：服务端每 `RBSVC_STATS_INTERVAL` 秒发送的 4 字节 `RBF_REPLY` 查询——
**端口连接建立并开始统计轮询后必崩**。

**修复（已实施）**

1. `RbfPortMessage` 改为 FltMgr 标准的 **6 参数签名**（移除 `ServerPortCookie` 形参）
2. `FltCreateCommunicationPort` 调用处的回调指针不再做参数个数不匹配的强转
3. 函数体内如需 cookie 用 `FltGetServerPortCookie(ClientPort)` 获取
4. `rbminiflt.h` 中的回调声明同步修正

### RB-28 卸载路径把线程句柄当对象指针（实测蓝屏 0xA）

- **模块**：driver
- **级别**：P0
- **位置**：`driver/rbminiflt.c` `RbfUnload`；`DriverEntry` 的 `FltStartFiltering` 失败路径
- **状态**：**已修复**
- **来源**：2026-08-31 实测蓝屏 dump（MEMORY.DMP，16:39）定位

**问题**

`PsCreateSystemThread()` 返回的是**句柄**（句柄表索引），不是内核对象指针：

```c
PsCreateSystemThread(&G.SendThreadHandle, ...);   // G.SendThreadHandle = HANDLE
```

而 `RbfUnload` 把句柄值直接传给 `KeWaitForSingleObject()`：

```c
KeWaitForSingleObject(G.SendThreadHandle, Executive, KernelMode, FALSE, NULL);
```

`KeWaitForSingleObject` 需要**对象指针**。dump 中 `G.SendThreadHandle` 槽位
（`G+0x58`）值为 `0xffffffff800098e0`（无效地址），崩溃线程正在等待这个"对象"：

```
Bugcheck 0xA IRQL_NOT_LESS_OR_EQUAL
Arg1 = ffffffff800098e0   ← G.SendThreadHandle 的值（无效地址）
Arg2 = 2                  ← IRQL=DISPATCH_LEVEL
崩溃指令 nt!KeWaitForSingleObject+0x18e: lock bts [rdi],7  ← 解引用对象头
调用链 System → IopLoadUnloadDriver → FltpMiniFilterDriverUnload
       → FltpDoUnloadFilter（持 FilterManagerMutex，IRQL=APC_LEVEL）
       → RbfUnload → KeWaitForSingleObject(G.SendThreadHandle)
```

**只要 send thread 创建成功，驱动卸载必崩**（0xA）。同样 bug 存在于
`DriverEntry` 的 `FltStartFiltering` 失败路径（`KeWaitForSingleObject` +
`ZwClose` 同一句柄）。

**修复（已实施）**

新增 `RbfStopSendThread()`：通过 `ObReferenceObjectByHandle` 把句柄转换为
`PETHREAD` 对象指针后再等待：

```c
static NTSTATUS RbfStopSendThread(VOID)
{
    PETHREAD ThreadObj = NULL;
    NTSTATUS status;

    if (!G.SendThreadHandle)
        return STATUS_SUCCESS;

    status = ObReferenceObjectByHandle(G.SendThreadHandle, THREAD_TERMINATE,
                                       NULL, KernelMode, (PVOID*)&ThreadObj, NULL);
    if (NT_SUCCESS(status)) {
        KeWaitForSingleObject(ThreadObj, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(ThreadObj);
    }
    ZwClose(G.SendThreadHandle);
    G.SendThreadHandle = NULL;
    return status;
}
```

`RbfUnload` 与 `DriverEntry` 失败路径均改用它。等待线程**对象**（而非句柄）在
APC_LEVEL + `KernelMode` 下合法（FltMgr 在持有 `FilterManagerMutex` 时于
APC_LEVEL 调用卸载回调；`KernelMode` 等待允许触碰非页面池）。

### RB-29 驱动加载后卷 attach/过滤生效存在窗口，期间删除无保护（实测用户数据丢失）

- **模块**：driver + 部署
- **级别**：P1
- **位置**：`driver/rbminiflt.c` / `rbminiflt.h`（`ProtectedCount` 暴露）；
  `service_c/rbf_protocol.h`（协议字段 + 编译期断言）；`service_c/rbdb.c`（schema 迁移）；
  `service_go/db/db.go`（stats 暴露）；`deploy.ps1`（[7/6] 冒烟验证）
- **状态**：**已修复（2026-08-31 部署实测验证通过）**
- **来源**：2026-08-31 实测部署验证（USN Journal + `fltmc` + 驱动计数器交叉取证）

**问题**

驱动加载（`sc start` 返回 RUNNING）**并不等于**对已挂载卷的删除过滤已生效。
2026-08-31 实测时间线：

| 时间 | 事件 | 来源 |
|---|---|---|
| 17:31:53 | 部署完成，驱动 RUNNING | deploy 日志 |
| 17:36:14~17:36:32 | 用户创建「测试驱动.txt」→ 改名 → 写入 96B → 右键删除 | USN Journal |
| 17:36:32 | 删除为 `文件删除 \| 关闭`（DELETE_CLOSE），**无任何 rename 到暂存区的记录** → 未拦截 | USN Journal |
| 17:41:31 | `fltmc` 才确认 `rbminiflt` attach 到 C/D/E 卷（高度 370030） | fltmc_check.log |
| 17:42 / 17:47 | 本地 / UNC 测试删除均成功拦截（`intercepts=2`） | 驱动计数器 |

用户删除发生在 **17:36~17:41 的无保护窗口**：驱动虽已 RUNNING，但对 E: 卷的
过滤尚未生效，删除直落磁盘 → **文件永久删除**（USN 记录 `DELETE_CLOSE`，
无 `$R`/`$I` 回收站条目、无 DB 记录、无暂存副本）。

**影响**

- 驱动部署/重启后的 **5~10 分钟内删除完全无保护**，用户数据可被永久删除
- 无任何日志/计数/告警提示"过滤未生效"——`intercepts=0` 与"真的没删除"无法区分，
  运维误判系统健康（与 RB-13 的陈旧判定同一类可观测性陷阱）
- 用户从 **UNC/映射盘**（`\\localhost\share`）右键删除时，Windows **本来就是永久删除**
  （网络位置删除不进回收站），更放大了无保护窗口的危害

**修复内容（2026-08-31 实施）**

1. **可观测性兜底（防"无保护而不自知"）**：驱动 `RBF_STATS` 新增 `ProtectedCount`
   （实际从注册表加载的受保护路径数，0 = 驱动对一切删除放行）。跨层打通：
   `rbminiflt.c` 装载配置后镜像到 `G.Stats` → `rbf_protocol.h` 同步字段并加
   编译期断言（`sizeof(RBF_STATS)==68`，驱动/服务双侧 C_ASSERT，漂移即编译失败）→
   `rbdb.c` 落库（含幂等列迁移 `DbEnsureColumn`，防旧库 INSERT 失败）→
   `db.go` 查询/JSON 暴露 → `/health` 的 `driver.protected_count` 字段
2. **部署时实测拦截生效（消除"RUNNING ≠ 生效"盲区）**：`deploy.ps1` 新增 **[7/6]** 冒烟验证——
   向受保护共享投放探针文件并删除，要求驱动将其**暂存**到 RBStore：
   - 探针在 RBStore → 拦截链路 LIVE（绿色 [OK]）
   - 探针留在原处 → fail-closed（数据安全但功能异常，红色告警）
   - 探针消失且未暂存 → **真删，共享未受保护**（红色告警 + 提示文件永久丢失风险）
3. **启动类型恢复**：`rbminiflt` 恢复 `start= system`（deploy.ps1 预期配置，开机自动加载）

**部署实测验证（2026-08-31 18:29~18:31，签名驱动 25,848 B）**

| 验证项 | 结果 |
|---|---|
| 驱动启动（[4/6]，此前 nosign 577 失败） | ✅ RUNNING（签名驱动通过内核加载器校验） |
| [7/6] 冒烟拦截 | ✅ 探针被暂存 `E:\RBStore\...\1_rb_deploy_probe_182932.txt`（14 B） |
| `fltmc` attach | ✅ 4 实例（C:/D:/E:/无卷名），高度 370030，E: 已过滤 |
| `/health` `protected_count` | ✅ = 1（与配置的 `\Device\HarddiskVolume4\tmp\share` 一致） |
| 驱动计数器 | ✅ `intercepts=1, rename_ok=1, notify_sent=1`（冒烟探针） |
| 服务状态 | ✅ RecycleBinSvc / RecycleBinApi RUNNING（AUTO_START） |

**遗留说明**

- 部署时无保护窗口已实测消除（[4/6] 启动 → [7/6] 立即拦截成功）；
  重启后自动加载路径（`start= system`）与 attach 行为同本次验证路径一致，待下次重启复核
- 未做 `FltEnumerateVolumes` 主动 attach 代码改动——实测显示加载时 FltMgr 已为全部
  已挂载卷建立实例；若未来出现 attach 延迟，应以部署冒烟验证为准拦截上线

**建议（保留给运维/文档）**

1. 文档/UI 明确提示：从网络路径删除不会进 Windows 回收站，本系统是唯一兜底；
   部署后若 [7/6] 冒烟未通过，禁止开放删除权限
2. 每次部署/重启后查看 `/health` 的 `driver.protected_count`：非 0 且 `intercepts` 持续增长
   才代表保护在运转

### RB-30 还原后保留回收站的 `Hidden+System` 属性，还原项在 Explorer/SMB 不可见（实测）

- **模块**：service_c
- **级别**：P1
- **位置**：`service_c/rbrestore.c` `RestoreItemById`（`MoveFileExW` 移回后无属性清理）；
  树还原 `RestoreTreeByPrefix` 复用同一路径
- **状态**：**已修复（2026-08-31 部署实测验证通过）**
- **来源**：2026-08-31 实测还原验证（SMB 删除大目录 → API 还原 → 客户端不可见）

**问题**

还原逻辑用 `MoveFileExW` 把条目从 `$Recycle.Bin` 移回原路径（`rbrestore.c` 第 251 行）：
```c
if (!MoveFileExW(srcDos, dstDos,
                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { ... }
```
但 `$Recycle.Bin` 中的 `$R` 容器/文件带 `FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM`
（回收站标准属性），**移动只搬数据、属性随文件保留**。移回后还原项仍带
`Hidden+System`，而 Explorer 默认**不显示带 `System` 属性的项**（即使勾选"显示隐藏文件"），
SMB 客户端枚举同样过滤 → **还原成功但用户完全看不到**。

**实测证据（2026-08-31 18:33~18:40）**

1. SMB 删除 `E:\tmp\share\财务wind文档资料1`（目录树，311 子目录 / 12 文件 / 19.5 MB）
   → 驱动拦截 → land 到 `E:\$Recycle.Bin\S-1-5-21-...-1001\$R4REVELKRI`
2. `POST /ops {"type":"restore","id":5}` → op 状态 `done/ok`，DB 更新 `restored=1`
3. 还原后 `attrib`：`E:\tmp\share\财务wind文档资料1` = **`Hidden, System, Directory`**
4. 对照：同卷可见目录 `test_dir` = `Directory`（无隐藏属性）；两者 ACL 完全一致
   （`Everyone` 完全控制），共享无 ABE 枚举过滤——**唯一差异就是属性位**
5. 用 PowerShell 清除 `Hidden+System` 后，UNC `\\<主机名>\share` 立即可见该目录
   （12 文件 / 19,493,088 B，与删除前逐字节一致）

**影响**

- 还原结果对用户"隐形"：数据已安全还原，但用户以为还原失败 → 重复发起还原、
  重新删除、或放弃恢复，破坏核心承诺（"删除进回收站、可还原可见"）
- 目录树还原（`RestoreTreeByPrefix`）逐条目走同一 `RestoreItemById`，**所有还原的目录
  都会隐形**，影响面是还原功能的 100%

**修复建议**

1. `RestoreItemById` 在 `MoveFileExW` 成功后，对还原目标显式清除
   `FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM` 两个属性位
   （`SetFileAttributesW` 或 `CreateFileW` + `SetFileInformationByHandle(FileBasicInfo)`），
   仅清这两位，保留其他属性（如只读）
2. 树还原由 1 自动覆盖（逐条目走 `RestoreItemById`）
3. 属性清除失败仅告警、不判还原失败（数据已归位，属性可后续修）
4. 回归验证：还原后 Explorer / SMB 客户端立即可见；`$I` 元数据删除路径（现有逻辑）不受影响

**修复内容（2026-08-31 实施）**

`RestoreItemById` 在 `MoveFileExW` 成功、`$I` 元数据删除之后，对还原目标执行
**读-改-写属性清除**（仅清 `HIDDEN | SYSTEM` 两位，保留只读等其他属性）：
```c
DWORD attrs = GetFileAttributesW(dstDos);
if (attrs != INVALID_FILE_ATTRIBUTES &&
    (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
    if (!SetFileAttributesW(
            dstDos, attrs & ~(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
        LogWarn(L"[restore] cannot clear hidden/system attrs on %s (win32=%lu)",
                dstDos, GetLastError());
    }
}
```
- 树还原（`RestoreTreeByPrefix`）逐条目走同一路径，自动覆盖
- 属性清除失败仅 `LogWarn` 告警，不判还原失败（数据已归位）

**部署实测验证（2026-08-31 18:54~18:57，rbservice.exe 1,267,200 B）**

| 验证项 | 结果 |
|---|---|
| 复现（修复前行为） | ✅ land 后 `$R4REVFF2IR` = `Hidden, System, Directory` |
| 还原后属性 | ✅ `Directory`（无 hidden/system 位） |
| SMB 可见性 | ✅ UNC `\\<主机名>\share` 立即可见 RB30T |
| 内容完整性 | ✅ `a.txt` = `hello A`、`sub\b.txt` = `hello B`（9 B × 2，与删除前一致） |
| DB 状态 | ✅ op `done`，条目转 `restored` |
| 服务健康 | ✅ RecycleBinSvc / RecycleBinApi RUNNING，驱动 protected=1 |

**遗留说明**

- 实测中发现 restore 逐条提交时若先还原子文件再还原父目录（id 升序），父目录条目
  restore 会因目标已由 `EnsureDirectoryChain` 重建而返回 failed——最终树仍完整
  （子文件已归位），不造成数据损失；树还原接口按整棵前缀还原不受影响

---

## 七、整改路线建议

| 阶段 | 周期 | 任务 |
|---|---|---|
| 第一阶段 止血 | 2–3 周 | RB-01 栈分配整改、RB-02 启动签名流程、RB-04 fail-closed、RB-05 孤儿对账、RB-06 路径校验 |
| 第二阶段 可观测 | 2 周 | RB-13 驱动计数器、RB-14 日志与告警 |
| 第三阶段 规模化 | 4 周 | RB-08 目录批处理、RB-09 归档与索引、RB-11 备份、RB-15 测试补齐 |
| 第四阶段 生产化 | 持续 | RB-03 回调重构、RB-12 热更新、HLK 认证、容量模型与压测基线 |
| **新增 目录树场景** | **1–2 周** | ~~RB-18 递归建父目录~~、~~RB-19 目录缓存~~、~~RB-22 拦截 DispositionEx~~ **已完成**；RB-20 / RB-21 仍待排期 |
| **可观测性** | 已完成 | ~~RB-13 驱动计数器打通~~ **已完成**（含 `PortLock` 生命周期缺陷修复） |
| **蓝屏实测** | 已完成 | ~~RB-27 消息回调参数错位~~、~~RB-28 卸载句柄当指针~~ **已修复**（已编译 + 反汇编核对，待部署到目标机实测） |
| **部署验证** | 已完成 | ~~RB-29 attach 就绪窗口~~ **已修复**（ProtectedCount 暴露 + [7/6] 冒烟验证 + 签名驱动部署实测通过）；~~RB-17~~ **误判撤回**（`$I` 实为 Explorer 原生格式，回收站实测可见） |
| **还原可见性** | 已完成 | ~~RB-30 还原后清除 `Hidden/System` 属性~~ **已修复**（`RestoreItemById` 读-改-写清属性位，部署实测：还原后 `Directory`、UNC 可见、内容完整） |
| **拦截盲区** | 待启动 | **RB-31 `cmd /c del` 删除 IRP 未覆盖**、**RB-32 POSIX_SEMANTICS 绕过点（RB-22 修复未生效）**：2026-09-01 实测 S-B4/S-B13 FAIL，受保护文件可真删。需先核对部署 `.sys` SHA256 是否含 RB-22 修复，再补齐所有删除类 IRP 拦截。**2026-09-02 复核：系部署旧 `.sys` 所致，需以正确产物复测验收** |
| **Explorer 删除盲区** | **已修复（待 Verifier 验收）** | ~~**RB-33 `FILE_DELETE_ON_CLOSE` 完全无拦截**~~ **已修复（2026-09-03 `5dd407a`）**：采用 `IRP_MJ_CREATE` pre/post 路线（**非**原建议的 `IRP_MJ_CLEANUP`，该路线曾误删目录已废弃）——`RbfPreCreate` 清 DOC 位 + `RbfPostCreate` 调 `RbfStageDelete` 改名进 RBStore。Explorer 删 SMB 文件实测拦截 + 还原闭环通过。**上线前仍建议经 Driver Verifier 测试机验证** |
| **端口线程死锁** | 已完成（部署待实测） | **RB-34 驱动重载后端口线程双向死锁**：dump 实证端口线程同步等 `FilterSendMessage` 完成事件、worker 卡在 `FilterSendMessage` 本身（`CancelIoEx` 对已卸载驱动端口 IRP 无效）。**2026-09-02 11:00 二次加固**：单向 overlapped 只读模型 + `g_LastMsgTick` 被动存活判定 + 关闭句柄唤醒 pending 发送（取代无效的 orphan 移交）。编译通过、契约测试 10/10。待部署后按条目内"验证方法"实测 |
| **部署一致性校验** | 待启动（建议并入 RB-15） | 2026 年内已发生 **4 次**"源码已修、产物未更新"（RB-22 / RB-27 / RB-28 / RB-31）。`build_all.cmd` 仅产出到源码树，不覆盖 `target\Release\` 与 `System32\drivers\`，且无校验环节。建议：编译后自动比对部署路径 `.sys` 与构建产物 SHA256，不一致即中止告警 |

> **当前最高优先级排序（2026-09-03 更新）**：
> **RB-35（还原以 `LocalSystem` 对共享目录无写权限，所有还原失败，影响核心能力）→ RB-32（用正确产物复测）**
> → RB-33 的 Driver Verifier 验收（功能已实测通过，仅缺 Verifier）→ 部署一致性校验。
>
> RB-34（端口线程死锁）已于 2026-09-02 修复；**RB-33（Explorer 删除盲区）已于
> 2026-09-03 修复并实测**：资源管理器删 SMB 文件现已能拦截并还原，
> 该路径的数据丢失问题已消除。
>
> 注意：RB-33 修复**无法找回**此前经资源管理器删除的文件——那些数据已永久丢失。

> 建议：在 RB-01、RB-02、RB-04 完成前，**不要开展任何生产试点**。
>
> **RB-18 建议优先修复**：仅一行代码改动，却解决了"删除复杂目录树后无法还原"
> 这一核心功能失效问题，投入产出比最高。

---

## 六之二、删除拦截盲区（RB-31 ~ RB-32，2026-09-01 实测）

> 本组为 `test/l5_e2e/test_l5_user_scenarios.py` 维度 B 用例在 **真实 SMB 拦截链路**
> 下实测失败暴露的驱动侧拦截盲区。它们不是测试脚本缺陷，而是产品缺陷：受保护共享
> 内的文件可绕过回收站被**静默真删**，是"唯一兜底"失效的唯一路径。
> 固化用例 S-B4 / S-B13 当前 FAIL，修复后自动转 PASS。

### RB-31 `cmd /c del` 删除 IRP 未被拦截，受保护文件被静默真删

- **模块**：driver
- **级别**：P1（实为数据丢失路径，按 P0 对待）
- **位置**：`driver/rbminiflt.c` `RbfPreSetInfo`（IRP_MJ_SET_INFORMATION 过滤条件）
- **状态**：**待修**
- **来源**：2026-09-01 维度 B 用例 `S-B4` 实测 FAIL（`E:\tmp\share\__smoke__\scenarios`）

**问题**

用例以 `cmd /c del /q <UNC 路径>` 删除受保护共享内文件，删除后：

1. `E:\RBStore` 无对应 staging 条目（`/items` 查不到 `b4_cmd.txt`）；
2. 源文件从共享中消失，**数据不可恢复**。

`cmd.exe` 的 `del` 底层并非走 `DeleteFileW`（该路径已被本驱动拦截），而是走
`SetFileInformationByHandle(FileDispositionInformation)` 或更底层的 IRP。**当前
`RbfPreSetInfo` 的 `FileInformationClass` 过滤未覆盖 `cmd` 实际发出的 IRP 子类**
（疑似 `FileDispositionInformation` 之外、`cmd` 经 `DeleteFile`/`NtSetInformationFile`
发出的变体，或 `FILE_OPEN_REPARSE_POINT` 等伴随标志），导致该删除直接到达文件系统、
绕过 rename 进 RBStore。

**影响**

- 攻击者或误用批处理脚本可借 `cmd /c del` 绕过回收站直接销毁受保护文件，
  使"SMB 唯一兜底防护层"失效（S-A2 UNC 是网络位置唯一防护层，一旦被绕过即无保护）。
- 与 S-B13 同属"删除 IRP 覆盖不全"，但触发面更广（命令行/脚本是最常见删除方式）。

**修复建议**

1. `RbfPreSetInfo` 的 `IRP_MJ_SET_INFORMATION` 处理中对**所有删除类**
   `FileInformationClass`（`FileDispositionInformation`、`FileDispositionInformationEx`）
   做统一拦截，并与 RB-32 共用同一分派逻辑；
2. 在测试机用 `cmd /c del` 复现，用 **ProcMon** 抓取 `cmd.exe` 实际发出的
   `SetInformationFile` IRP 的 `FileInformationClass` 与标志，确认过滤条件对齐；
3. 以 RB-27/28 教训为戒：**修复后必须核对部署 `.sys` 的 SHA256 与构建产物一致**，
   并以 `S-B4` 转 PASS 作为验收标准。

**验证方法**：`python test\l5_e2e\test_l5_user_scenarios.py` 中 `S-B4` 由 FAIL 转 PASS，
且 `E:\RBStore` 出现对应 staging 条目、源文件可还原。

**状态修正（2026-09-02 实测复核）**

来源：2026-09-02 在正确部署 9/01 构建产物（`rbminiflt.sys`，25,848 B，时间戳
`2026-09-01 18:20:04`）后，于受保护共享 `E:\tmp\share`（`share => E:\tmp\share`）
内做的对照实验。

结论：`Remove-Item` / `del` 一类走 `IRP_MJ_SET_INFORMATION` 的删除**实测拦截正常**——
同一时刻、同一目录下的对照实验中，该类删除使驱动计数器 `intercepts` 由 2 增至 3，
DB 生成 `id=9797 status=landed` 条目，回收站产生对应 `$I` / `$R` 文件对。

因此 S-B4 在 2026-09-01 的 FAIL **并非驱动拦截逻辑缺失**，而是当时
`C:\Windows\System32\drivers\rbminiflt.sys` 仍是 2026-08-31 18:25:17 的旧产物
（`git reset --hard c6e8dff6` 后重新编译，但驱动二进制未覆盖部署）。
这是第三次"源码已修、产物未更新"类事件（另见 RB-22 / RB-27 / RB-28）。

处置：本条目由"待修"降级为"待验收"——无需改动拦截逻辑，
须以正确产物重跑 S-B4 验收。**注意 RB-33 表明本条目即便验收通过，
也不代表删除拦截已全覆盖。**

**流程缺陷（建议一并纳入 RB-15 工程规范）**：`build_all.cmd` 只产出到源码树，
不会覆盖 SCM 实际加载的 `target\Release\` 与 `System32\drivers\`，
且**无任何校验环节**比对运行产物与构建产物的一致性。
建议：编译后自动比对部署路径 `.sys` 与构建产物的 SHA256，不一致即中止并告警。

---

### RB-32 `FileDispositionInformationEx` + `POSIX_SEMANTICS` 仍被绕过（RB-22 修复未生效）

- **模块**：driver
- **级别**：P1（实为数据丢失路径，按 P0 对待）
- **位置**：`driver/rbminiflt.c` `RbfPreSetInfo`（同 RB-22）
- **状态**：**待修（RB-22 源码修复已合入，但部署产物未生效）**
- **来源**：2026-09-01 维度 B 用例 `S-B13` 实测 FAIL

**问题**

用例用 `ntdll.NtSetInformationFile(FileDispositionInformationEx, Flags=POSIX_SEMANTICS)`
（`0x3` 访问、`FILE_DISPOSITION_DELETE` 标志）删除受保护共享内文件，删除后：

1. `E:\RBStore` 无对应 staging 条目；
2. 源文件消失，数据不可恢复。

RB-22 已在源码层面对 `FileDispositionInformationEx` 增加拦截分支并将其并入
`FileDispositionInformation` 的同一 rename 路径，但**实测表明该拦截在部署驱动中未生效**。
可能原因（按 RB-27/28 经验优先级排序）：

1. 部署的 `.sys` 产物未重新编译/未包含 RB-22 修复（源码已修、产物未更新）；
2. `cmd` / POSIX 工具实际发出的 IRP 子类（如带 `FILE_OPEN_REPARSE_POINT`、
   `FILE_DISPOSITION_ON_CLOSE` 等附加标志的变体）未被 RB-22 的精确比较覆盖；
3. 拦截分支的位置在 `FileDispositionInformationEx` 判定前提前 `return`
   `FLT_PREOP_SUCCESS_NO_CALLBACK`（早退路径覆盖不全）。

**影响**

与 RB-31 同源但触发更隐蔽：任何使用 `NtSetInformationFile` / POSIX 语义删除的应用、
WSL、Git for Windows、rsync、某些备份工具均可绕过回收站真删整树或单文件，
且**不产生任何通知或计数**——历史绕过点复现，是明确的数据真正丢失路径。

**修复建议**

1. 首先核对部署 `.sys` SHA256 与含 RB-22 修复的构建产物一致
   （`test/run_all.ps1 -Deployed` 已具备该核对入口）；若不一致，重新部署正确产物；
2. 用 ProcMon 抓取 `NtSetInformationFile` 实际 IRP，确认 `FileInformationClass`
   与标志位，确保 `RbfPreSetInfo` 的分派覆盖所有变体（含附加标志组合）；
3. 验收标准：`S-B13` 由 FAIL 转 PASS，且 `S-B4`、`S-B13` 在测试机同时绿。

**关联**：与 RB-22 同源；与 RB-31 互补（二者分别覆盖不同删除 IRP 触发面，
共同构成"删除拦截全覆盖"验收）。

**状态补充（2026-09-02）**：RB-31 的复核已证实"S-B4 FAIL"系部署旧 `.sys` 所致，
本条目（S-B13）高度同源，**须在正确产物下重新复测后再下结论**；
在未复测前不得假定 RB-22 的源码修复已生效。另注意 RB-33 揭示的盲区
与 `POSIX_SEMANTICS` 无关，是独立的一条删除路径。

---

## 六之三、2026-09-02 部署与拦截链路实测（RB-33 ~ RB-34）

> 本节为 2026-09-02 在目标机（`DESKTOP-Q1NM7CS`）上做的端到端实测记录。
> 起因是用户报告"上个版本功能正常、当前变更后失效"，以及"驱动掉线"。
> 排查过程中先后排除的假设（均未成立，记录以避免重复排查）：
> 卷号漂移（`E:` 始终为 `HarddiskVolume4`）、实例未 attach（`fltmc` 确认已 attach
> E: / C: / D:，高度 370030）、受保护前缀形式错误（`HarddiskVolume4` 与
> `Volume{GUID}` 两种写法均试过）、本地 vs 远程差异（本地与 UNC 行为一致）、
> 源码回归（`git reset --hard c6e8dff6` 后问题依旧）。
> **最终定位为两项独立缺陷**：驱动删除路径覆盖不全（RB-33）与端口线程死锁（RB-34），
> 叠加一次部署遗漏（驱动 `.sys` 未覆盖）。

### RB-33 `FILE_DELETE_ON_CLOSE`（Explorer 删除）完全无拦截，受保护文件 100% 真删

- **模块**：driver
- **级别**：**P0**（实为数据丢失路径，且是用户日常删除的主力路径）
- **位置**：`driver/rbminiflt.c` `G_Callbacks` / `RbfPreSetInfo`
- **状态**：**已修复（2026-09-03，commit `5dd407a`；Explorer 删 SMB 实测拦截 + 还原成功）**
- **来源**：2026-09-02 用户报告（`\\10.88.36.171\share\新建文件夹\get_session.exe`
  经资源管理器删除后 `/items` 无记录）+ 同一目录下的对照实验

**问题**

驱动的操作回调表**只注册了 `IRP_MJ_SET_INFORMATION`**：

```c
const FLT_OPERATION_REGISTRATION G_Callbacks[] = {
    { IRP_MJ_SET_INFORMATION, 0, RbfPreSetInfo, NULL },
    { IRP_MJ_OPERATION_END }
};
```

Windows 有**两条**独立的删除路径：

| 删除方式 | 内核路径 | 本驱动是否覆盖 |
|---|---|---|
| `DeleteFileW` / `del` / `[IO.File]::Delete()` / `Remove-Item` | `IRP_MJ_SET_INFORMATION`（`FileDispositionInformation` / `...Ex`） | ✅ 覆盖 |
| **资源管理器删除 / 右键删除 / Shell `SHFileOperation`** | 打开时带 `FILE_DELETE_ON_CLOSE` → `IRP_MJ_CREATE` + **`IRP_MJ_CLEANUP`** 时按 `DeletePending` 执行 | ❌ **完全未覆盖** |

第二条路径**根本不产生 `IRP_MJ_SET_INFORMATION`**，因此 `RbfPreSetInfo` 永远不会被调用。

**实测证据（2026-09-02 09:53，同一共享目录 `E:\tmp\share\__smoke__\doc`，同一驱动实例）**

| 用例 | 删除方式 | 驱动计数器变化 | 结果 |
|---|---|---|---|
| A | `Remove-Item`（UNC 路径） | `intercepts` **2 → 3** | ✅ 拦截，DB `id=9797` `status=landed`，回收站生成 `$I` / `$R` |
| B | `CreateFile(FILE_DELETE_ON_CLOSE)` + `CloseHandle`（UNC 路径，模拟 Explorer） | `intercepts` **保持 3（无增长）** | ❌ **未拦截**，文件被真删（`Test-Path` = `False`），DB 无记录、回收站无 `$I` |

补充证据：用户报告的 `get_session.exe` 删除后，`/stats` 显示
`intercepts=2 / notify_sent=2`（仅含我此前的两次测试），
`E:\tmp\share` 递归搜索 `get_session*` 无结果 → **该文件已被永久删除，不可恢复**。

**影响**

1. **保护对交互式操作实际无效**：资源管理器按 Delete / 右键删除是 Windows 用户
   最主流的删除方式，该路径 100% 真删，不产生任何记录、计数或告警。
2. **静默失败**：与 RB-04 的 fail-open 同类，但更彻底——连"曾发生过删除"这件事
   都不留痕迹，运维从 `/health` 看 `rename_fail=0`、`delete_denied=0` 会误判系统健康。
3. **历史数据无法找回**：此前经资源管理器删除的所有受保护文件均已永久丢失，
   不存在任何副本或元数据，本条目修复**无法回溯**。
4. 与 RB-31 / RB-32 的关系：那两条是 `SET_INFORMATION` 内部的子类覆盖问题
   （已修复或待复测），**本条目是完全不同的另一条 IRP 路径**，互不覆盖。

**修复方案（建议）**

在驱动增加 `IRP_MJ_CLEANUP` 前置回调，与现有 `SET_INFORMATION` 路径共用同一套
"路径匹配 → 取 SID → 预留槽位 → rename 到 staging → 入队通知"逻辑：

1. `PostCreate` 中对以 `FILE_DELETE_ON_CLOSE` 打开的受保护文件建立流上下文打标；
2. `PreCleanup` 中检查 `FltObjects->FileObject->DeletePending`；
3. 命中受保护前缀时执行 rename 到 staging；
4. **关键步骤（易漏）**：rename 后必须再以
   `FltSetInformationFile(FileDispositionInformation, DeleteFile = FALSE)` 清除删除标志。
   NTFS 的 delete-on-close 是**基于 FCB 而非文件名**的——仅 rename 不改 FCB，
   cleanup 时文件仍会被删除，表现为"文件进了 staging 却又消失"。
5. 通知用户态入库（复用 `RbfQueueNotify`）。

**风险与验证要求**

- 这是内核态改动，rename 与清除 disposition 的**时序非常微妙**，
  Pre 回调内发起 I/O 本身即命中 RB-03 所述的高危模式。
- 目标机 `C:\WINDOWS\Minidump` 已存在多份 `BlueScreen` dump
  （`083126-38984-01.dmp`、`083126-40140-01.dmp`、`083126-26703-01.dmp`），
  说明该机器上跑未验证的 minifilter 改动风险不低。
- **必须**在开了 **Driver Verifier**（含 Deadlock Detection / Special Pool /
  I/O Verification）的测试机上验证后再部署到本机，禁止直接上生产。

**验证方法**

1. 对照实验：同一目录下分别用 `Remove-Item` 与 `CreateFile(FILE_DELETE_ON_CLOSE)`
   删除，两者 `intercepts` 均增长、DB 均生成 `landed` 条目；
2. 真机交互：资源管理器删除 UNC 共享文件后 `/items` 立即出现该条目、可还原；
3. `fltmc instances` 无异常、`notify_dropped` 不增长、无蓝屏。

---

#### 修复方案（已实施，2026-09-03，commit `5dd407a`）

> 上方"修复方案（建议）"提出的 `IRP_MJ_CLEANUP` 路线**未被采用**。
> 排查中曾按该建议方向实现 `PreCleanup`，但因无法可靠区分"删除性 close"与
> "普通 close / 目录 close"，一版"cleanup 内无条件 rename"的实现**误将受保护
> 目录本身改名/删除**（生产共享 `E:\tmp\share` 被删）。该实现已废弃并回退。
> 最终采用下述 **CREATE pre/post** 路线，安全性由"仅在明确 DOC 打开时动作"
> 这一窄条件保证。

**思路**：不碰 `IRP_MJ_CLEANUP`，而是在 **`IRP_MJ_CREATE`** 阶段把 DOC 删除
"转译"回已有的 disposition 拦截路径。

| 回调 | 职责 |
|---|---|
| `RbfPreCreate`（新增） | 路径命中 `RbfIsProtected` **且** `Create.Options & FILE_DELETE_ON_CLOSE` → **清掉 DOC 位** + `FltSetCallbackDataDirty` + 请求 post 回调 |
| `RbfPostCreate`（新增） | 带标记（且 create 成功）→ 调用 `RbfStageDelete` 改名进 RBStore + 通知用户态 |
| `RbfStageDelete`（新增，抽取） | 从原 `RbfPreSetInfo` 抽出的公共逻辑（会话 → 路径匹配 → SID → 预留槽位 → rename → 通知），供 disposition 与 DOC 两条路径复用 |
| `RbfPreSetInfo` | **逻辑未动**，精简为 disposition 判定后 `return RbfStageDelete(...)` |

```c
const FLT_OPERATION_REGISTRATION G_Callbacks[] = {
    /* RB-33: DOC deletes produce no SET_INFORMATION IRP, caught here. */
    { IRP_MJ_CREATE, 0, RbfPreCreate, RbfPostCreate },
    { IRP_MJ_SET_INFORMATION, 0, RbfPreSetInfo, NULL },
    { IRP_MJ_OPERATION_END }
};
```

**安全性设计（针对误删目录事故）**：

1. **触发条件极窄**：仅在"带 `FILE_DELETE_ON_CLOSE`"**且**"路径命中受保护前缀"
   两个条件同时成立时动作。目录枚举、普通 close、非受保护路径**完全不触碰**。
2. **先清 DOC 位（fail-safe）**：PreCreate 阶段即剥掉 `FILE_DELETE_ON_CLOSE`，
   因此**即便后续改名失败**，文件系统也不会在 close 时删除文件——
   数据保留在原位，而不是像 fail-open 那样真删。
3. **标记用 `CompletionContext` 字面量传递**（`RBF_CTX_WAS_DOC_OPEN = (PVOID)1`），
   不做池分配，无泄漏风险。
4. **不传播 pre 状态码**：post-create 返回 `FLT_POSTOP_FINISHED_PROCESSING`，
   符合 post 回调语义。

> 注：最初设想的"PostCreate 里调 `FltSetInformationFile(FileDispositionInformation,
> DeleteFile=TRUE)` 让它重新触发 `RbfPreSetInfo`"**不成立**——Filter Manager 的
> `FltXxx` 系列按 `Instance` 参数只向**更低 altitude** 的层下发 I/O，正是为避免
> filter 看到自己发出的 I/O 造成递归，因此不会回到本驱动的 `RbfPreSetInfo`。
> 故改为直接调用抽取出的 `RbfStageDelete`。

**构建**：`build_all.cmd Release` 零错误零警告，签名成功（`rbminiflt.sys`）。

**实测验证（2026-09-03，目标机）**

| 验证项 | 结果 |
|---|---|
| Explorer 从 `\\10.88.36.171\share\测试\` 删除 `12.30.txt` | ✅ 被拦截 |
| `/items` 记录 | ✅ 新增 `id=2`，`status=staged`，`store_path` 落在 `RBStore\<SID>\` |
| 物理文件回收 | ✅ `E:\RBStore\S-1-5-21-...-1001\1_12.30.txt`（5 B，内容完整） |
| 目录安全性 | ✅ `E:\tmp\share\测试` 目录未被误删/误改名（DOC 窄条件生效） |
| disposition 路径未回归 | ✅ 原 `IRP_MJ_SET_INFORMATION` 拦截仍正常（共用 `RbfStageDelete`） |
| 还原闭环 | ✅ 文件手工还原至 `E:\tmp\share\测试\12.30.txt`（5 B），DB 状态修正为 `restored` |

**遗留与后续**

- 本修复未在 **Driver Verifier** 下验证（仅在开发环境实测）。上线前仍建议按
  上方"风险与验证要求"在测试机跑 Verifier 一轮。
- 还原环节暴露 **RB-35**：初判为"中文路径 `MoveFileExW` 失败 `win32=5`"，
  同日对纯 ASCII 路径 `id=4` 复现同一失败后**更正根因**——`LocalSystem` 服务对共享目录
  无写权限，所有还原均 `ACCESS_DENIED`，与中文无关（详见六之四）。
- 验证过程中因手工还原（绕过 rbservice）导致 DB 状态与文件实际位置短暂不一致，
  已用 `test/fix_db_status.py`（含 SQLite 在线备份 API，WAL 安全）修正。

---

### RB-34 驱动卸载/重载后端口线程永久死锁，C 服务静默 → 假性"驱动掉线"

- **模块**：service_c
- **级别**：P1（不丢数据，但服务静默 + 可观测性完全失效，是本次排查最大误导源）
- **位置**：`service_c/rbport.c` `PortThreadProc` / `PortQueryStats`；`service_c/rbsvc.h`
- **状态**：**已修复（2026-09-02 实施，2026-09-02 mini-dump 实证根因后于 11:00 二次加固）**
- **来源**：2026-09-02 排查"驱动掉线"时定位；**11:00 抓取 `rbservice_9736.dmp` 后彻底坐实根因**

**问题（dump 实证，非推断）**

2026-09-02 11:00 在 C 服务已冻结（pid=9736，驱动已重载但 `/health` 仍 `driver=null`、
零 RB-34b 日志）状态下抓全量 user-dump，线程栈如下：

| 线程 | 栈顶（已符号化） | 含义 |
|---|---|---|
| 0x4794（端口线程 / SCM 派生） | `rbservice+0x1cb9` → `WaitForSingleObject(hMsgEvent, 5000)` | overlapped `FilterGetMessage` 等待驱动回消息 |
| 0x5adc（采样 worker） | `rbservice+0xb87c` → `WaitForSingleObject(c->Done, 3000)` | 等 `FilterSendMessage` 的完成事件，`Done` **永不被置位** |
| 0x2dd8 / 0x5970 | `rbservice+0x1582/0x1512` | 普通业务等待，正常 |

**因果链（dump 唯一解释）**：

1. 端口线程每 `RBSVC_STATS_INTERVAL` 在**自身主循环内**调 `PortSampleStats()` →
   `PortQueryStats()`，后者在 worker 上发 `FilterSendMessage` 并**同步等待 `c->Done`**；
2. 驱动在采样往返途中被卸载（或取消读之后端口半死）→ `FilterSendMessage` **永不返回**
   → worker 卡住、`c->Done` 永不 signal；
3. 端口线程自身也被 `WaitForSingleObject(c->Done, 3000)` 阻塞，于是**端口线程与 worker
   双向死锁**：端口线程到不了 5s `hMsgEvent` 超时、到不了 `CancelIoEx`、到不了重连；
4. `CancelIoEx` 对"已发送到内核、驱动侧已卸载"的通讯端口 IRP **根本无效**——这是首版
   RB-34 修补也是死代码的根本原因；worker 的 `FilterSendMessage` 只有关闭端口句柄才能
   被内核 cancel 唤醒，而首版 `PortCloseLocked` 因 `g_SendInFlight != 0` 把 `g_Port`
   移交给 `g_OrphanPort` **不关句柄**，于是 worker 永远回不来。

=> 全程零日志（连 RB-34 的重连告警都到不了）、`driver=null`。

**这是架构级缺陷，不是两处独立超时问题**：服务把"`/health` 的 driver 是否存活"建立在了
**主动向已死端口发 `FilterSendMessage`**之上，一旦 driver 卸载这个主动探测本身永久卡死，
整条自恢复链（连重连）随之雪崩。

**修复（2026-09-02 11:00 二次加固，纯用户态）**

核心原则：**minifilter 通讯端口对用户态是严格单向的——驱动主动推通知、服务只 overlapped
收；服务绝不在端口线程里发 `FilterSendMessage` 探测；driver 存活完全由"最近收到通知的
时间戳 `g_LastMsgTick`"推导，而非任何主动查询。**

1. `service_c/rbsvc.h`：删除 `RBSVC_CANCEL_WAIT_MS` / `RBSVC_SEND_WAIT_MS`，改为单一
   `RBSVC_PORT_READ_MS 5000`（overlapped `FilterGetMessage` 等待上限）；保留
   `RBSVC_STATS_INTERVAL` / `RBSVC_STATS_STALE_SEC` / `RBSVC_RECONNECT_MS`。

2. **端口线程只读不写**：`FilterGetMessage` overlapped 发出后只 `WaitForSingleObject(
   hMsgEvent, RBSVC_PORT_READ_MS)`。读取 5s 超时**不关端口**（驱动可能只是空闲），
   仅 fire-and-forget 触发一次采样后 `continue`；`FilterGetMessage` **真正失败**
   （`GetOverlappedResult` 报非 `OPERATION_ABORTED` 错，或首调非 `IO_PENDING`）才
   `connected=0` + 关闭端口 + 重连。**彻底移除 `CancelIoEx` 依赖**。

3. **`g_LastMsgTick`**：每次成功收通知或连上端口即 `g_LastMsgTick = time(NULL)`（持锁写）。
   `/health` 侧按 `now - g_LastMsgTick < RBSVC_STATS_STALE_SEC` 判定 driver 存活，**完全
   不碰端口**。driver 卸载时通知流停 → `g_LastMsgTick` 变旧 → 自然判定 offline；
   驱动重载后端口线程 5s 内重连、首条通知即刷新时间戳 → 自动恢复。

4. **采样改为 fire-and-forget、永不在端口线程同步等待**：`PortSampleStats()` 调
   `PortQueryStats()` 但**不读其结果**（端口线程只负责"触发"）。`PortQueryStats()` 的
   worker 仍在独立线程发 `FilterSendMessage`，caller `WaitForSingleObject(c->Done,
   RBSVC_PORT_READ_MS)` 超时即**放弃本次采样、释放 caller 引用即返回**，绝不阻塞端口线程。

5. **句柄安全改为"直接关闭"**：`PortCloseLocked()` **无条件 `CloseHandle(g_Port)`**
   （移除 `g_OrphanPort` 移交）。这是关键修正——关闭端口句柄会让内核 cancel 掉该句柄上
   仍 pending 的 `FilterSendMessage` IRP，worker 以 `ERROR_OPERATION_ABORTED` 返回后
   自然退出、自行关闭其持有的句柄副本，不再有悬空 IRP。worker 持有的句柄副本是其
   **发起 `FilterSendMessage` 时捕获的 `g_Port` 值**，与端口线程后续重开的新句柄互不干扰。

> 注：stats（`RBF_CMD_QUERY_STATS`）采样作为"尽力而为"的观测保留，driver 卸载时超时丢弃，
> 不影响 `/health` 的存活判定（后者走 `g_LastMsgTick`）。若需无通知周期也能探测，应在
> **驱动侧**周期推送 keepalive 通知，而非用户态反向 `FilterSendMessage`。

**构建验证（2026-09-02 11:xx 二次加固后）**

| 验证项 | 结果 |
|---|---|
| `service_c\build.cmd Release`（在 `service_c\` 目录内） | ✅ 通过，`rbport.c` 无警告（`cl /W3`） |
| `python db\verify_c_contract.py` | ✅ **10/10 通过**（契约未因重构变化） |

> 未重编驱动：本修复纯用户态，部署时**只替换 `rbservice.exe`** 即可（build.cmd 须于
> `service_c\` 目录运行，否则 `..\db\gen_schema.ps1` 相对路径解析错误）。

**验证方法（覆盖二次加固）**

1. 服务运行中执行 `net stop rbminiflt; net start rbminiflt`（**不重启 C 服务**）；
2. 观察 Application 日志：应出现 `FilterGetMessage failed (hr=...) reconnecting`（或
   `cannot connect to ... retrying`）后重新 `connected to kernel port \RecycleBinPort`；
   **不再**依赖 `cancelling parked read` / `QUERY_STATS stalled` 这类日志（已移除该路径）；
3. `/health` 的 `driver` 字段应在 **30s 内自动恢复**非 `null`，`age_sec` 回落至个位数；
4. 删除测试文件，`intercepts` 正常增长；
5. **dump 复核（如仍怀疑）**：驱动重载后若仍卡死，按前文命令抓 dump，`~*kvn` 应见端口线程
   停在 `WaitForSingleObject(hMsgEvent, 5000)` 且能正常超时循环，worker 不再卡死在 `c->Done`。

**结论**

RB-34 的真正根因是**"用户态主动 `FilterSendMessage` 探测 + 端口线程同步等待其结果"构成的
双向死锁**，`CancelIoEx` 对此类已卸载驱动的端口 IRP 无效。二次加固把存活判定改为被动时间戳、
把关闭句柄作为唤醒 pending `FilterSendMessage` 的唯一手段，从架构上消除该死锁。

---

## 六之四、RB-35 还原失败根因修正：并非中文路径，而是服务（`LocalSystem`）对共享目录无写权限（2026-09-03 复核）

> **重要更正**：本节初版（2026-09-03 当天写入）判定为"rbservice 对中文路径处理缺陷、
> 非权限问题"，**该结论错误**。同日对 `id=4`（纯 ASCII 路径）复现同一失败后即推翻，
> 正确根因见下。

### RB-35 restore 以 `LocalSystem` 运行、对共享目录无写权限，所有还原均 ACCESS_DENIED（win32=5）

- **模块**：service_c（运行账户）+ 部署（共享目录 ACL）
- **级别**：**P0**（"可还原"这一核心能力对**全部**条目失效，不止中文）
- **位置**：`service_c/rbrestore.c` `RestoreItemById`（`MoveFileExW`，约 L251）；
  `deploy.ps1` 服务注册 `obj= LocalSystem`；`E:\tmp\share` 目录 ACL
- **状态**：**已修复（方案A + 方案C 均实施，2026-09-03 端到端验证通过）**
- **实施（2026-09-03）**：
  - 方案A：`deploy.ps1` 已写入对受保护共享 `SYSTEM:(OI)(CI)(M)` 授权；`E:\tmp\share` 实测已生效（子目录继承 `SYSTEM Modify, Synchronize`）。
  - 方案C：`service_c/rbrestore.c` 新增 `GrantSelfAccessToStaging()`（启用 `SeTakeOwnershipPrivilege`/`SeRestorePrivilege`，对暂存文件取 SYSTEM 所有权并追加 `SYSTEM:(F)` 后再 `MoveFileExW`）。已 `build.cmd` 编译通过（Release），生成 `service_c/rbservice.exe`，并复制至运行路径 `D:\myapp\Release\rbservice.exe` 后重启 `RecycleBinSvc` 生效。
  - 可见性子修复（2026-09-03 补充）：`GrantSelfAccessToStaging` 会令暂存文件 owner=SYSTEM、DACL 带 SYSTEM 授权；`MoveFileExW` 把这些**staging 文件的 stale DACL 一并搬到目标**，导致还原后文件不继承共享父目录权限、共享用户看不到。新增 `RestoreInheritParentAcl()`，在 `MoveFileExW` 成功后用 `SetSecurityDescriptorDacl(FALSE, NULL, TRUE)`+`SetFileSecurityW` 将文件 DACL **重置为继承父目录**（与 Windows 回收站还原一致），owner 保持 SYSTEM（无害，用户经继承的共享 ACL 仍可见可管理）。已编译通过，待重启验证。
- **验证（2026-09-03，重启服务后）**：`POST /ops {"type":"restore","id":1}`（纯 ASCII 路径、暂存文件带删除者 `wuweigang` DACL，天然回归用例）→ `op_id=5, state=done, message=ok`；文件回到 `E:\tmp\share\rb_deploy_probe_183632.txt`（14 B），DB `status=restored`。**无需手工改 ACL**，证明确认源端（暂存文件 DACL）+ 目标端（共享写回）两端均已修复，未来所有还原耐久可用，与中文/英文路径无关。
- **来源**：2026-09-03 对 `id=4`（`E:\tmp\share\test\deletetest.txt`，**纯 ASCII 路径**）
  发起 `POST /ops {"type":"restore","id":4}` → `op_id=2` →
  `state=failed, message="move failed (win32=5)"`。**纯英文路径同样失败，
  直接证伪"中文路径"假设。**

**根因（实测取证）**

1. `RecycleBinSvc` 服务以 **`LocalSystem`** 身份运行（`deploy.ps1`：
   `sc.exe create RecycleBinSvc ... obj= LocalSystem`）。
2. 共享目录 `E:\tmp\share`（及其子目录 `test`）的 ACL **仅授予**
   `DESKTOP-Q1NM7CS\wuweigang` `FullControl`；`SYSTEM`、`Administrators` 均**无显式 ACE**
   （`Get-Acl E:\tmp\share\test` 实测只返回一条 `wuweigang FullControl Allow`）。
3. `RestoreItemById` 的 `MoveFileExW(srcDos, dstDos, ...)` 在 SYSTEM 安全上下文执行，
   需要在目标目录 `E:\tmp\share\test` 上具备 `FILE_ADD_FILE`——**无 ACE 即被拒**
   → `ERROR_ACCESS_DENIED`（win32=5）。
4. 之前"手工 `cmd move` 成功"是因为以**当前交互用户 `wuweigang`（FullControl）**执行，
   权限充分；这与服务（SYSTEM）的失败**并不矛盾**。旧结论把"`cmd` 成功"当作
   "非权限问题"的证据，属于**身份混淆的逻辑错误**——两者运行账户不同。

**为什么 id=2（中文）与 id=4（英文）都失败**：根因是**运行账户缺写权限**，
与路径是否含非 ASCII 字符**无关**。`VolNtToDos` 全程宽字符、路径转换正确
（id=4 的 ASCII 路径也精确转换且同样失败）。原怀疑的"窄字符中转 / GBK 截断"方向
**均不成立，已排除**。

**影响**

- 所有经 API（`POST /ops`）发起的还原**必然失败**（中文 / 英文路径皆然），
  "删除可恢复"这一核心承诺对全部条目失效。
- 数据**不会丢失**（文件仍在 RBStore），可手工 `move`（以有权限账户）+ `rename` + 改 DB 状态绕过。
- 与 RB-06 同一安全模型问题：恢复动作以高特权 SYSTEM 执行，却落在用户 ACL 受限的共享上。

**根因两端（关键修正）**

`MoveFileExW` 同卷 rename 需要**两端**权限：
1. **目标目录** `FILE_ADD_FILE` —— 共享目录只给 `wuweigang` 授权，SYSTEM 无 → 方案A（共享根授予
   `SYSTEM:(OI)(CI)(M)`）已修复，**已实施**。
2. **源文件（RBStore 暂存文件）`DELETE`** —— 驱动 `FltSetInformationFile` rename 时**保留源文件 DACL**
   （删除者 `wuweigang` 的显式 ACE，`InheritanceFlags=None`，实测），SYSTEM 对暂存文件无 DELETE →
   **移动必然被拒**。这是真正的卡点：给该暂存文件单独加 `SYSTEM:(F)` 后 `id=3` 还原即 `done`，已验证。

> 仅授权共享根（方案A）**不足以**让未来还原可用：每次"删除→暂存"产生的新文件都自带删除者 DACL、
> 不会继承 RBStore 的 ACE，SYSTEM 依旧无 DELETE。必须让还原路径能取得暂存文件的 DELETE。

**修复方向（需用户确认）**

- **方案 C（推荐，代码层，最小改动）**：在 `rbrestore.c` `RestoreItemById` 的 `MoveFileExW` 之前，
  由服务（以 `LocalSystem` 运行，持有 `SeTakeOwnershipPrivilege` + `SeRestorePrivilege`）
  **对源暂存文件取所有权并授予 SYSTEM 完全控制**（`SetNamedSecurityInfo` 设 `OWNER=SYSTEM`
  并附加 `SYSTEM:(F)` DACL），再执行 rename。
  - 优点：仅需重编 `rbservice.exe`（**不动驱动**），对历史 / 未来条目**耐久有效**；
    不改动共享目录语义、不引入令牌获取复杂度。
  - 缺点：取所有权动作需谨慎限定在自身管理的 RBStore 内文件（现状已满足）。
- **方案 A（部署层，已实施，仅修目标端）**：共享根 `icacls ... SYSTEM:(OI)(CI)(M)`（已写入 `deploy.ps1`）。
  是方案 C 的必要前提（目标目录写回），但**单独不能**解决源端 DELETE 问题。
- **方案 B（代码层，架构最正）**：还原前用 `items` 表 SID 取得删除者令牌并 `ImpersonateLoggedOnUser`
  再以所有者身份写回，天然满足两端 ACL，贴合 RB-06 的 per-user 授权。
  - 优点：最小权限、尊重每用户 ACL。
  - 缺点：远程 SMB 会话未本地登录时取令牌复杂；实现 / 验证成本高，需重编并经实测。

**当前绕过方法（运维，已用于 id=2 / id=4 / id=3 验证）**

```powershell
# 1. 以有权限账户手工移动（保持 staging 文件名）
cmd /c "move /Y <store_path 的 DOS 形式> <目标目录>\"
# 2. 去掉 staging 的 <seq>_ 前缀
Rename-Item -LiteralPath '<目标>\<seq>_<原名>' -NewName '<原名>'
# 3. （可选）让服务也能动该暂存文件：icacls <store文件> /grant "SYSTEM:(F)"
# 4. 修正 DB 状态（先停 rbservice；当前 shell 无管理员权限停服时，
#    脚本在 WAL 模式下仍可对 DB 做一致性读写，冲突风险低）
python test\fix_db_status.py --id <N>
```

> 注：`test\fix_db_status.py` 头部注释原写"非权限问题"亦随之更正——实为权限问题；
> 且根因在**两端**（目标目录写回 + 暂存文件 DACL）。

**验证方法（2026-09-03 已部分验证）**

- 方案A 已实施：`E:\tmp\share` 及子目录实测含 `NT AUTHORITY\SYSTEM` `Modify,Synchronize`
  `ContainerInherit,ObjectInherit`。
- 对 `id=3`（`E:\tmp\share\123456.txt`，纯 ASCII）单独授予暂存文件 `SYSTEM:(F)` 后
  `POST /ops {"type":"restore","id":3}` → `state=done, message=ok`，文件归位、DB=`restored`
  （已验证）→ 证实源端 DACL 即卡点、目标端方案A 已生效。
- 完整耐久修复 = **方案A + 方案C**（服务取所有权）。方案C 实施后，未来条目应直接 `done`，
  无需手工给暂存文件加 ACE。

---
