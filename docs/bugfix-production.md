# 生产就绪度修复记录

- 项目：Windows 文件共享回收站 (RecycleBin for SMB)
- 范围：内核 Mini-Filter 驱动 (`rbminiflt`) + C 核心服务 (`rbservice`) + Go 管理 API (`rbapi`) + 数据模型 + 部署链路
- 日期：2026-08-31
- 本轮成果：**修复 10 项**（P0 × 4、P1 × 6），另修正原评估中的 2 处判断偏差
- 追加轮次：**蓝屏实测修复 2 项**（RB-27 / RB-28，2026-08-31 两次 BSOD dump 定位）
- 追加轮次：**部署/还原实测修复 2 项**（RB-29 / RB-30，2026-08-31 部署与还原实测新增）

> 文档关系
>
> - `bugfix-report.md` — 初版 Python 实现的历史缺陷（已修复）
> - `buglist.md` — 现存问题清单与整改排期（本轮修复已同步状态）
> - `bugfix-production.md` — 本文件，记录本轮**生产就绪度**修复的详细方案与根因

---

## 一、修复总览

| 编号 | 级别 | 模块 | 问题 | 状态 |
|---|---|---|---|---|
| RB-01 | P0 | 驱动 | 通知结构体内核栈溢出，首次删除即 BSOD | 已修复 |
| RB-04 | P0 | 驱动 | 5 处失败路径静默放行真删 | 已修复 |
| RB-05 | P0 | 驱动/服务 | 孤儿文件无对账，可撑满共享卷 | 已修复 |
| RB-06 | P0 | 服务/Go | restore 目标路径不校验 → SYSTEM 任意写入 | 已修复 |
| RB-07 | P1 | 驱动 | 通知结构体内存浪费（64.5 KB / 条） | 已修复 |
| RB-08 | P1 | 驱动 | 递归删除击穿队列，丢弃即产生孤儿 | 已修复 |
| RB-09 | P1 | 数据模型 | 表无限增长、LIKE 通配符注入 | 已修复 |
| RB-10 | P1 | 服务 | 还原命令最长等待 30 秒 | 已修复 |
| RB-11 | P1 | 服务 | 数据库无备份与完整性检查 | 已修复 |
| RB-12 | P1 | 运维 | 配置变更需重启服务 | 部分（用户态配置已热加载） |

### 大目录树场景（第二轮，同日追加）

| 编号 | 级别 | 问题 | 状态 |
|---|---|---|---|
| RB-18 | P1 | 深层目录还原失败（`CreateDirectoryW` 只建一级） | 已修复 |
| RB-19 | P1 | 删除大目录树极慢（每文件重复创建目录） | 已修复 |
| RB-22 | P1 | `FileDispositionInformationEx` 未拦截，可绕过整树真删 | 已修复 |

### 蓝屏实测（第三轮，同日追加）

| 编号 | 级别 | 问题 | 状态 |
|---|---|---|---|
| RB-27 | P0 | `RbfPortMessage` 回调 7 参数签名 vs FltMgr 实际 6 参数 → 参数错位把 `InputBufferLength`(=4) 当指针解引用 → **0x3B** | 已修复 |
| RB-28 | P0 | 卸载路径把 `PsCreateSystemThread` 返回的句柄当内核对象指针传给 `KeWaitForSingleObject` → **0xA** | 已修复 |

### 部署/还原实测（第四轮，同日追加）

| 编号 | 级别 | 问题 | 状态 |
|---|---|---|---|
| RB-29 | P1 | 驱动加载后对已挂载卷的 attach/过滤生效存在窗口，期间删除无保护、用户文件被永久删除 | 已修复 |
| RB-30 | P1 | 还原后保留回收站 `Hidden+System` 属性，还原项在 Explorer/SMB 客户端不可见 | 已修复 |

**未包含在本轮**：RB-02 驱动签名（外部流程，需采购证书 + 提交 Dev Center，2–6 周）、RB-03 Pre 回调重构（需测试机 + Driver Verifier，风险高，暂缓）、RB-20 / RB-21（属结构性设计优化，收益/风险比低于上述三项，待单独立项）。

---

## 二、P0 修复详情

### RB-01 内核栈溢出（致命）

**问题**：`RbfPreSetInfo` 在栈上声明 `RBF_NOTIFICATION note = {0};`。按原定义该结构体约 **66,068 字节（64.5 KB）**，而 x64 Windows 内核栈默认仅 **24 KB**。

**根因**：结构体含两个 `WCHAR[16383]` 路径缓冲区，固定内联布局。`= {0}` 初始化生成全量填充，且 `&note` 被传入 `RbfQueueNotify`，编译器无法优化掉栈分配 —— 命中保护路径的**第一次删除就会栈溢出蓝屏**。

该缺陷同时说明：删除拦截路径此前**很可能从未在真实机器上端到端验证过**。既有契约测试覆盖的是 C 服务的数据库逻辑，不经过内核路径。

**修复**：改为**变长布局**（48 字节定长头 + 按需路径负载），从 Paged 池分配，栈上仅保留指针。

| 指标 | 修复前 | 修复后 |
|---|---|---|
| 单次通知（典型） | 64.5 KB | ≈1 KB |
| 最坏情况 | 64.5 KB | 4.6 KB |
| 内核栈占用 | 超出 24 KB 栈 | 几十字节 |
| 队列满配（512 条） | ≈33 MB | ≈2.4 MB |

配套改动：

- `RbfAllocNotify()` 按实际长度分配；`RbfNotifySetPath()` 等 setter 从**相邻偏移推导容量**，无法越界写入
- 新增 `RBF_NOTIFY_MAGIC` 校验，拒绝畸形缓冲区
- 用户态接收缓冲改为 `RBF_NOTIFY_MAX_SIZE` 字节数组 + `ReplyLength` 长度校验
- `rbf_protocol.h` 静态断言由"仅校验总大小"升级为**逐字段偏移校验**，并新增"单条通知 ≤ 8 KB"守卫，**防止该缺陷复发**

### RB-04 静默真删

**问题**：5 处失败路径（获取文件名、获取 SID、创建暂存目录、构造暂存路径、rename 失败）均执行 `return FLT_PREOP_SUCCESS_NO_CALLBACK`，即"放行真删"。

**根因**：fail-open 是唯一策略，且无任何计数器记录。对回收站产品而言这是最坏的失败模式 —— 用户以为文件进了回收站，实际永久丢失，系统无任何痕迹。

**修复**：

- 新增 `RbfFailDelete()` 统一判定，所有失败路径改走该函数
- 新增 `FailClosed` 注册表开关，**默认 1 = 拒绝删除保住数据**（返回 `STATUS_ACCESS_DENIED`，用户重试即可）
- 新增 `DeleteDenied` 计数器，暂存区故障变为**可观测**
- `deploy.ps1` 同步写入该配置；`FailClosed=0` 时输出黄色警告

### RB-05 孤儿文件

**问题**：数据流为"内核 rename → 异步通知 → 写 DB"。通知丢失（服务未连接、队列满、分配失败）时，文件已在暂存区但无 DB 记录 —— 不可恢复、不进查询、不受配额约束、永不被清理，持续膨胀直至共享卷写满。

**修复**：新增 `service_c/rbconcile.c`，遍历暂存目录逐个查 `items.store_path`，未登记且超宽限期（默认 7 天）则回收。

关键设计：**宽限期是必需的**。刚 rename 的文件尚未落库，"不在 DB" 不等于孤儿；只有"超期 + 未登记"才可安全删除。

配套：新增 `VolDosToNt()`（DOS→NT 转换）与 `DbStorePathExists()`；跳过 `recycle.db` 及 WAL/SHM 旁车文件；启动后 2 分钟首扫，之后每小时一次。

### RB-06 restore 提权面

**问题**：`Arg` 字段直接作为还原目标，两侧均不校验；`rbservice.exe` 以 SYSTEM 执行 rename。任何持有 token 者可将文件还原至 `C:\Windows\System32\` 等任意位置。

**修复**：在 **C 侧**（真正的 SYSTEM 执行点）新增 `DestIsAllowed()`：

- 拒绝非盘符绝对路径 —— 拦截 UNC `\\server\share`、`\\?\`、相对路径
- 拒绝 `..` 路径组件 —— 拦截 `D:\Share\..\..\Windows` 这类绕过前缀检查的遍历
- 目标必须落在受保护前缀内，且必须是该目录**或其子路径**（防止 `D:\Share` 授权了 `D:\ShareSecret`）
- 未配置白名单时 fail-closed

> 校验放在 C 侧是关键：请求经由共享 `ops` 表传递，Go 侧校验可被绕过，**只有 rename 执行点才是可信防线**。

---

## 三、P1 修复详情

### RB-07 通知结构体内存浪费

随 RB-01 一并解决（变长结构 + 动态分配）。队列满配内存从 ≈33 MB 降至 ≈2.4 MB，且删除热路径不再有 64 KB 清零开销。

### RB-08 递归删除击穿队列

**问题**：`rd /s` / 清理作业逐文件删除，每个文件触发一次通知，队列（512）瞬间打满。

**原报告判断需修正**：真正的问题不只是"队列被击穿"，而是**丢弃发生在 rename 之后** —— 每条被丢弃的通知都对应一个已离开原位置的文件，即一个孤儿。

**修复**：**预留式入队**。`RbfReserveQueueSlot()` 在 rename **之前**占位，`RbfQueueNotify()` 兑现槽位。队列满时文件尚未移动，直接 fail-closed 拒绝删除 —— **孤儿在结构上不可能产生**。所有失败路径正确释放预留。

同时 `StoreDeleteEntry` 补上 `RemoveDirectoryW`，使目录子树能被回收（`DeleteFileW` 对目录必然失败，会永久残留）。

### RB-09 表增长与查询

- **终态归档**：新增 `DbReapTerminalRows()`，分批（每批 1000，单轮上限 5 万）删除 `restored`/`purged` 且超期（默认 90 天）的行，保留审计期；新增 `idx_items_terminal(status, delete_time)` 支撑该扫描
- **LIKE 转义**（Go）：新增 `likeEscape()` + `ESCAPE '\'`，修复通配符注入 —— 搜索 `50%` 原本匹配任意路径

> **对原报告的一处修正**：报告建议给 `orig_path` 建索引。但 `LIKE '%x%'` 是前置通配符，**B-tree 索引完全用不上**，建了只会拖慢写入并浪费空间。真正有效的是"缩小表体积（归档）+ 转义"，这也是实际采用的方案。

### RB-10 还原延迟

**排查发现原报告判断偏差**：落地循环**早已是自适应**的（`for(;;)` 排空至不满批），并非"30 秒 / 500 条"瓶颈。

真实瓶颈在别处：`RBSVC_OPS_POLL_MS 2000` **定义了却从未使用**，`RestoreDrainOps()` 挂在 30 秒维护周期上 —— 用户点"还原"最多等 30 秒。

**修复**：新增独立 `OpsThreadProc`（2 秒轮询）作为还原快车道。`MaintenancePass()` 增加 `drainOps` 参数：服务运行时传 0（避免两线程争抢 ops 行），`once` 模式与停机前传 1 兜底。

### RB-11 数据库备份

新增 `DbBackupTo()`（SQLite 在线备份 API，**无需停写**）与 `DbCheckIntegrity()`。

关键设计：**先校验再备份**。否则会把损坏的映像覆盖掉最后一份好备份 —— WAL 恢复只能重放已提交帧，检测到损坏时必须停止信任该文件并保留上一份已知良好副本。默认每天一次，失败后间隔 1 小时重试（不每 30 秒重试）。

### RB-12 配置热加载

新增自定义服务控制码 `128`：`sc control RecycleBinSvc 128` 即可重读注册表，无需重启。

`ReloadConfig()` 先解码到临时结构、校验通过后才原子替换 —— `RetentionDays=0` 这类会立即删光文件的值会被拒绝，旧配置保持生效；并正确释放旧路径数组。`StoreRoot` 变更时明确告警（数据库路径在启动时固定，仍需重启）。

**部分完成**：仅覆盖用户态配置（`HKLM\SOFTWARE\RecycleBin`）。驱动侧参数（`ProtectedPaths`、`FailClosed`）在驱动加载时读取，仍需重启。

---

## 三之二、大目录树删除场景修复

> **场景背景**
>
> Windows 删除目录是**用户态递归**：Explorer / `rd /s` 逐个枚举并删除条目，
> 所以含 N 个文件的目录树 = **N 次独立拦截**。单文件场景被掩盖的问题
> （每次调用的小开销、队列容量、父目录创建）在此场景下放大 N 倍并集中暴露。

### RB-18 深层目录还原失败

**问题**：还原前用 `CreateDirectoryW` 创建父目录，而它**只创建单级**。
删除目录树后 `D:\Share\proj\src\main.c` 的 `proj` 与 `proj\src` 均已消失，
创建 `D:\Share\proj\src` 失败 → `MoveFileEx` 报路径不存在 → **还原失败**。

文件仍在 `$Recycle.Bin`（数据未丢），但用户取不回 —— 产品核心价值失效。

**修复**：新增 `EnsureDirectoryChain()`，逐级调用 `CreateDirectoryW` 创建完整
父目录链，将 `ERROR_ALREADY_EXISTS` 视为成功（幂等）。路径缓冲改用
`RBSVC_MAX_RECON_PATH`（1024）而非 `MAX_PATH`。

> **未采用 `SHCreateDirectoryExW` 的原因**：项目定义了 `WIN32_LEAN_AND_MEAN`，
> 即便引入 `shlwapi.h` 该函数仍未声明，产生 `C4013` 警告——编译器假设返回
> `int`，在 x64 下存在返回值截断风险。自实现消除了该警告、额外链接库依赖
> 与 SDK 版本差异。

### RB-19 删除大目录树极慢

**问题**：`RbfEnsureStoreDir` 对每个文件都完整执行一遍——池分配 + 两次
`ZwCreateFile`（打开已存在的目录再关闭），全部在 Pre 回调内**同步阻塞**删除路径。
5000 个文件即 10000 次目录打开，累积延迟表现为进度条停滞。

**修复**：新增 **(卷名, SID) 指纹缓存**：

- FNV-1a 指纹，16 槽定长数组，命中即跳过全部 I/O
- 槽位用 `InterlockedCompareExchange` 读写，**无锁、快路径不阻塞**
- 槽位用尽轮转淘汰（丢失条目仅多一次目录打开，无正确性影响）

**陈旧条目自愈**：若 `\RBStore` 被管理员删除或卷被卸载，缓存会误判目录存在；
此时 rename 失败 → `RbfStoreCacheForget()` 清除指纹 → 下次删除重建目录。
配合 fail-closed，数据在任何路径下都不会丢失。

### RB-22 `FileDispositionInformationEx` 绕过

**问题**：驱动只拦截 `FileDispositionInformation`。Windows 10 1709+ 的
`FileDispositionInformationEx` + `FILE_DISPOSITION_POSIX_SEMANTICS` 可
**一次性删除非空目录**，不被拦截 → 整树静默真删，无通知、无计数。
这是唯一会导致数据真正丢失的路径。

**修复**：过滤条件改为按类别分派，两类删除标记走同一拦截路径：

```c
if (fic == FileDispositionInformation) {
    /* 旧式：dispInfo->DeleteFile */
} else if (fic == FileDispositionInformationEx) {
    /* 新式：Flags & FILE_DISPOSITION_DELETE
       FILE_DISPOSITION_DO_NOT_DELETE (0) 表示取消删除，放行 */
} else {
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}
```

> 直接用 WDK 头文件的 `FILE_DISPOSITION_INFORMATION_EX`，未做兼容性别名：
> SDK 对该结构体的守卫宏名无法跨版本可靠检测（实测自定义守卫宏失效导致
> `C2011` 重复定义错误）。构建本驱动需 1709 或更新版本 WDK。

---

## 三之三、蓝屏实测修复（RB-27 / RB-28）

> **背景**：2026-08-31 测试机连续两次蓝屏，先后分析两份 `MEMORY.DMP`
> 转储（15:12 与 16:39），分别定位为**消息回调参数错位**与**卸载路径
> 句柄误用**。两项都是驱动在真实机器上的首个端到端暴露——此前契约测试
> 不经过内核消息路径，未能捕获。

### RB-27 消息回调参数错位（0x3B）

**问题**：`RbfPortMessage` 声明为 7 参数签名（含 `ServerPortCookie`），
而 FltMgr 消息回调实际只传 6 参数——`ServerPortCookie` 只能通过
`FltGetServerPortCookie()` 单独查询。参数在寄存器层面左移一位：

| 寄存器 | FltMgr 实际传入 | 回调误认为 |
|---|---|---|
| r8 | `InputBufferLength` = 4 | `InputBuffer` 指针 |
| r9 | `OutputBuffer` 指针 | `InputBufferLength` |

**崩溃现场**：`cmp dword ptr [r8],1` 解引用地址 `0x4` → `c0000005` →
Bugcheck `0x3B`。触发者是服务端每 `RBSVC_STATS_INTERVAL` 秒发送的 4 字节
`RBF_REPLY`（stats 采样轮询）——**连接建立并开始轮询后必崩**。

**修复**：签名改回 FltMgr 标准 6 参数（移除 `ServerPortCookie` 形参），
`FltCreateCommunicationPort` 回调指针不再强转；需要时用
`FltGetServerPortCookie(ClientPort)` 获取。

**反汇编核对**：修复前驱动含 `cmp [r8],1`（`41 83 38 01`）特征码；修复后
该特征为 0 次，代之以 `cmp [rdx],1`（`83 3a 01`）——解引用正确的
`InputBuffer` 指针。

### RB-28 卸载路径句柄当对象指针（0xA）

**问题**：`PsCreateSystemThread` 返回**句柄**，`RbfUnload` 却把句柄值直接
传给 `KeWaitForSingleObject`。dump 中 `G.SendThreadHandle` 槽位（`G+0x58`）
= `0xffffffff800098e0`（无效地址），崩溃线程正等待该"对象"：

```
Bugcheck 0xA IRQL_NOT_LESS_OR_EQUAL
Arg1 = ffffffff800098e0      ← G.SendThreadHandle 的值
Arg2 = 2 (IRQL=DISPATCH_LEVEL)
崩溃指令 nt!KeWaitForSingleObject+0x18e: lock bts [rdi],7
```

调用链：`System → IopLoadUnloadDriver → FltpMiniFilterDriverUnload
→ FltpDoUnloadFilter（持 FilterManagerMutex）→ RbfUnload`。
**只要 send thread 创建成功，卸载必崩**；`DriverEntry` 的
`FltStartFiltering` 失败路径同样中招。

**修复**：新增 `RbfStopSendThread()`：

- `ObReferenceObjectByHandle` 把句柄转成 `PETHREAD` 对象指针
- `KeWaitForSingleObject(对象, Executive, KernelMode, FALSE, NULL)` 等待线程退出
  （APC_LEVEL 下等待对象指针合法；`KernelMode` 允许触碰非页面池）
- `ObDereferenceObject` + `ZwClose(句柄)`

`RbfUnload` 与 `DriverEntry` 失败路径均改用它。

**反汇编核对**：新驱动 IAT 新增 `ObReferenceObjectByHandle` 导入；
`RbfStopSendThread` 反汇编确认 `ObReferenceObjectByHandle` →
`KeWaitForSingleObject(ThreadObj)` → `ObDereferenceObject` → `ZwClose`
序列完整。

> **共性问题提示（RB-27 / RB-28）**：两次蓝屏分析均靠 dump 反汇编发现，
> 此前存在"源码看似已修、产物实际未编译进去"的版本漂移。本次修复后必须
> **以构建产物为准**（反汇编/IAT 核对）再部署，避免再次踩坑。

---

## 三之四、部署实测与还原实测修复（RB-29 / RB-30）

> **背景**：2026-08-31 部署与还原两项实测各暴露一个此前被忽略的问题——
> "驱动 RUNNING ≠ 删除过滤已生效"的无保护窗口，与"还原成功但用户看不到"的
> 属性残留。两项均在当日修复并部署实测通过。

### RB-29 驱动加载后卷 attach/过滤生效存在窗口，期间删除无保护（实测用户数据丢失）

**问题**：`sc start` 返回 RUNNING 并不等于对已挂载卷的删除过滤已生效。
实测时间线：

| 时间 | 事件 | 来源 |
|---|---|---|
| 17:31:53 | 部署完成，驱动 RUNNING | deploy 日志 |
| 17:36:14~17:36:32 | 用户创建「测试驱动.txt」→ 写入 96B → 右键删除 | USN Journal |
| 17:36:32 | 删除为 `文件删除 \| 关闭`（DELETE_CLOSE），**无 rename 到暂存区记录** → 未拦截 | USN Journal |
| 17:41:31 | `fltmc` 才确认 `rbminiflt` attach 到 C/D/E 卷（高度 370030） | fltmc_check.log |
| 17:42 / 17:47 | 本地 / UNC 测试删除均成功拦截（`intercepts=2`） | 驱动计数器 |

用户删除落在 **17:36~17:41 无保护窗口**：文件直落磁盘被永久删除，无 `$R`/`$I`、
无 DB 记录、无暂存副本。且从 UNC/映射盘右键删除 Windows 本来就是永久删除，
更放大了危害；`intercepts=0` 与"真的没删除"无法区分，运维会误判系统健康。

**修复**（2026-08-31 实施）：

1. **可观测性兜底**：驱动 `RBF_STATS` 新增 `ProtectedCount`（实际从注册表加载的
   受保护路径数，0 = 驱动对一切删除放行）。跨层打通：`rbminiflt.c` 装载配置后镜像到
   `G.Stats` → `rbf_protocol.h` 同步字段 + 编译期断言（`sizeof(RBF_STATS)==68`，
   驱动/服务双侧 C_ASSERT，漂移即编译失败）→ `rbdb.c` 幂等列迁移落库 →
   `db.go` 查询/JSON 暴露 → `/health` 的 `driver.protected_count`
2. **部署冒烟验证 [7/6]**：向受保护共享投放探针文件并删除，要求驱动将其**暂存**到
   RBStore：暂存 → 拦截链路 LIVE；留在原处 → fail-closed（数据安全但功能异常）；
   消失且未暂存 → **真删、共享未受保护**（红色告警 + 文件永久丢失风险提示）
3. **启动类型恢复**：`rbminiflt` 恢复 `start= system`（开机自动加载）

**部署实测验证**（2026-08-31 18:29~18:31，签名驱动 25,848 B）：

| 验证项 | 结果 |
|---|---|
| 驱动启动（[4/6]） | RUNNING（签名驱动通过内核加载器校验） |
| [7/6] 冒烟拦截 | 探针被暂存 `E:\RBStore\...\1_rb_deploy_probe_182932.txt`（14 B） |
| `fltmc` attach | 4 实例（C:/D:/E:/无卷名），高度 370030，E: 已过滤 |
| `/health` `protected_count` | = 1（与配置的 `\Device\HarddiskVolume4\tmp\share` 一致） |
| 驱动计数器 | `intercepts=1, rename_ok=1, notify_sent=1` |
| 服务状态 | RecycleBinSvc / RecycleBinApi RUNNING（AUTO_START） |

**遗留**：重启后自动加载路径（`start= system`）与本次验证路径一致，待下次重启
复核；未做 `FltEnumerateVolumes` 主动 attach 代码改动（实测加载时 FltMgr 已为
全部已挂载卷建立实例）。

### RB-30 还原后保留回收站的 `Hidden+System` 属性，还原项在 Explorer/SMB 不可见（实测）

**问题**：`RestoreItemById` 用 `MoveFileExW` 把条目从 `$Recycle.Bin` 移回原路径
（`rbrestore.c` 第 251 行）——移动只搬数据、属性随文件保留，`$R` 容器/文件自带的
`FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM`（回收站标准属性）被一并带回。
Explorer 默认不显示带 `System` 属性的项（即使勾选"显示隐藏文件"），SMB 客户端枚举
同样过滤 → **还原成功但用户完全看不到**。

**实测证据**（2026-08-31 18:33~18:40，SMB 删除大目录树「财务wind文档资料1」，
311 子目录 / 12 文件 / 19.5 MB）：

1. 删除 → 拦截 → land `E:\$Recycle.Bin\S-1-5-21-...-1001\$R4REVELKRI`
2. `POST /ops {"type":"restore","id":5}` → op `done/ok`，DB 更新 `restored=1`
3. 还原后 `attrib` = **`Hidden, System, Directory`**；对照可见目录 `test_dir` =
   `Directory`，两者 ACL 完全一致——**唯一差异就是属性位**
4. PowerShell 清除 `Hidden+System` 后，UNC `\\<主机名>\share` 立即可见
   （12 文件 / 19,493,088 B，与删除前逐字节一致）

**影响**：还原结果对用户"隐形"（以为还原失败 → 重复还原/重新删除/放弃恢复），
破坏核心承诺（"删除进回收站、可还原可见"）；目录树还原逐条目走同一
`RestoreItemById`，影响面是还原功能的 **100%**。

**修复**（2026-08-31 实施）：`MoveFileExW` 成功、`$I` 元数据删除之后，对还原目标
**读-改-写清除属性位**（仅清 `HIDDEN | SYSTEM` 两位，保留只读等其他属性）：

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

- 树还原（`RestoreTreeByPrefix`）逐条目复用同一路径，自动覆盖
- 清除失败仅 `LogWarn` 告警、不判还原失败（数据已归位，属性可后续修）

**部署实测验证**（2026-08-31 18:54~18:57，rbservice.exe 1,267,200 B）：

| 验证项 | 结果 |
|---|---|
| 复现（修复前行为） | land 后 `$R4REVFF2IR` = `Hidden, System, Directory` |
| 还原后属性 | `Directory`（无 hidden/system 位） |
| SMB 可见性 | UNC `\\<主机名>\share` 立即可见 RB30T |
| 内容完整性 | `a.txt` = `hello A`、`sub\b.txt` = `hello B`（9 B × 2，与删除前一致） |
| DB 状态 | op `done`，条目转 `restored` |
| 服务健康 | RecycleBinSvc / RecycleBinApi RUNNING，驱动 protected=1 |

**遗留**：逐条 restore 按 id 升序提交时，若先还原子文件再还原父目录，父目录条目会
因目标已由 `EnsureDirectoryChain` 重建而返回 failed——最终树仍完整（子文件已归位），
不造成数据损失；树还原接口按整棵前缀还原不受影响。

---

## 四、验证

每项修复均执行全量重新编译（`build_all.cmd Release`）并运行两套契约测试：

| 验证项 | 结果 |
|---|---|
| `verify_contract.py`（C ↔ Go 共享 schema） | 9/9 PASS |
| `verify_c_contract.py`（C 版本守卫 + ops 往返） | 7/7 PASS |
| 驱动 / C 服务 / Go API 编译 | 全部成功，**零警告** |

第二轮（RB-18 / RB-19 / RB-22）完成后同样通过全量编译与契约测试。

第三轮（RB-27 / RB-28）修复后再次全量构建：驱动签名成功，契约验证
`verify_contract.py` 9/9 + `verify_c_contract.py` 10/10 通过；并用
`dumpbin /disasm` 与 IAT 核对新驱动已包含两项修复（RB-27：无
`cmp [r8],1` 特征码；RB-28：IAT 新增 `ObReferenceObjectByHandle`）。

第四轮（RB-29 / RB-30）修复后完成签名驱动部署实测：契约验证通过；部署冒烟
[7/6] 探针暂存成功、`/health` 的 `protected_count=1`；还原实测属性清除、UNC
可见、内容完整。部署后服务持续运行未再触发 0x3B（RB-27 对应验证通过）；
RB-28 的卸载路径待 `sc stop` 实测确认。

### 契约测试捕获的真实回归

将 ops 移出维护周期后，`once` 模式不再排空 ops，`verify_c_contract.py` 的 "op drained by rbservice" 从 7/7 掉到 6/7。

这正是契约测试存在的价值 —— 它捕获了一个会让**运维手动执行维护时丢失还原命令**的真实回归。修复（`MaintenancePass(1)`）后恢复 7/7。

---

## 五、遗留与后续

| 编号 | 事项 | 阻塞原因 |
|---|---|---|
| RB-02 | 驱动签名 | 外部流程：EV 证书采购 + Dev Center attestation / WHQL，2–6 周 |
| RB-03 | Pre 回调重构 | 改变同步语义，需真实测试机 + Driver Verifier 验证，风险高 |
| RB-12 | 驱动参数热更新 | 需重构驱动配置加载路径 |
| RB-13 ~ RB-17 | P2 项（可观测性、测试、安全细节） | 待排期 |

**建议**：RB-02 应尽早启动 —— 它是纯外部依赖，且 HLK 测试套件会反向暴露驱动实现缺陷，可能影响 RB-03 的整改方向。

**风险提示**：本轮修复**仅经过编译与契约验证**。内核路径（RB-01、RB-04、RB-08、RB-19、RB-22）的正确性需要在配备 Driver Verifier 的测试机上做真实删除拦截压测后才能确认，当前环境不具备该条件。

RB-27 / RB-28 已随 2026-08-31 签名驱动部署（18:29，25,848 B）。服务端 stats
轮询持续运行未再触发 0x3B（RB-27 实测通过）；RB-28 的卸载路径仍需 `sc stop` /
驱动更新实测确认（本次部署未执行卸载操作）。
另需核对系统 `System32\drivers\rbminiflt.sys` 的 SHA256 与构建产物一致，
避免版本漂移（此前系统里部署的是 16:03:58 构建版，未含任何修复）。

RB-29 / RB-30 遗留：驱动重启后自动加载（`start= system`）的 attach 行为待下次
重启复核；逐条 restore 按 id 升序时的"父目录条目返回 failed、树仍完整"属已知
边界（树还原接口不受影响），如后续开放逐条还原入口需一并处理。

RB-19 的指纹缓存是新增的内核状态，压测时应重点验证：
多用户并发删除、缓存淘汰轮转、以及删除 `\RBStore` 后的自愈路径。
