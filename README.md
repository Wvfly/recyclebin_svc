<div align="center">

# Windows 文件共享回收站

**RecycleBin for SMB**

拦截 SMB 共享上的删除操作，把文件重定向到回收站暂存区而非真正删除，
实现「远程删除 → 进回收站 → 可还原 / 可审计」。

</div>

<div align="center">

![Platform](https://img.shields.io/badge/platform-Windows%20Server%202016%2B-0078D6?logo=windows)
![Core](https://img.shields.io/badge/core-C%20%2F%20Go-555555?logo=go)
![Driver](https://img.shields.io/badge/driver-Mini--Filter%20(WDK)-orange)
![Architecture](https://img.shields.io/badge/architecture-single--writer%20SQLite-blueviolet)
![Tests](https://img.shields.io/badge/contract%20tests-19%2F19%20pass-brightgreen)
![Signing](https://img.shields.io/badge/driver%20signing-required-red)

</div>

用户端**零感知、零改动**：在 Explorer 里按 Delete，文件"消失"，实际进了
他自己的 `$Recycle.Bin`，用标准回收站就能还原。

---

> ## ⚠️ 生产就绪度
>
> 本项目架构与数据模型已完成生产级加固，但**部署前必须了解以下两项**：
>
> | 项 | 状态 | 说明 |
> |---|---|---|
> | **驱动签名** | ❌ 阻塞 | 驱动当前**未签名**，需 `testsigning` 才能加载。生产环境须办理 EV 证书 + Dev Center 认证（2–6 周外部流程） |
> | **内核路径验证** | ⚠️ 未充分 | 驱动侧修复仅经**编译与契约验证**，尚未在配备 Driver Verifier 的测试机上做删除压测 |
>
> 详见 [生产就绪度](#生产就绪度) 与 [docs/buglist.md](docs/buglist.md)。

---

## 目录

- [特性](#特性)
- [架构](#架构)
- [工作流程](#工作流程)
- [目录结构](#目录结构)
- [快速开始](#快速开始)
- [详细部署](#详细部署)
- [配置参考](#配置参考)
- [REST API](#rest-api)
- [运维与监控](#运维与监控)
- [故障排查](#故障排查)
- [已知限制](#已知限制)
- [开发构建](#开发构建)
- [生产就绪度](#生产就绪度)
- [文档索引](#文档索引)

---

## 特性

<table>
<tr>
<td width="50%" valign="top">

### 拦截与重定向

| 特性 | 说明 |
|---|---|
| **内核级拦截** | Mini-Filter（Altitude `370030`）在删除 I/O 路径上拦截，不依赖 Shell 钩子 |
| **零用户态往返** | 重定向在内核态同步完成，删除路径不阻塞等待用户态决策 |
| **Fail-closed 保护** | 暂存失败时**拒绝删除**并保住数据（可切回 fail-open），杜绝静默真删 |
| **变长通知** | 通知按需从内核池分配（典型 ≈1 KB），不再有固定 64 KB 结构 |
| **预留式入队** | 队列槽位在 rename **之前**预留，队列满时文件未动 → **结构上不可能产生孤儿** |

</td>
<td width="50%" valign="top">

### 落地与运维

| 特性 | 说明 |
|---|---|
| **标准回收站兼容** | 落地为 `$Recycle.Bin\<SID>\$Rxxxx` + `$Ixxxx`，用户可用 Explorer 原生还原 |
| **真实用户归属** | 内核通过 `TokenUser` 取**真实客户端 SID**，不靠 session ID 猜 |
| **孤儿对账** | 定期扫描暂存区回收无主文件，可配宽限期，防止撑满共享卷 |
| **在线备份** | SQLite 在线备份 API + 完整性校验，**先校验后备份**，不覆盖最后一份好副本 |
| **自动运维** | 配额、保留期、多卷磁盘水位自动清理；终态记录自动归档 |
| **配置热加载** | 用户态配置可在线重载，无需重启服务 |

</td>
</tr>
</table>

### 安全特性

| 特性 | 说明 |
|---|---|
| **还原路径白名单** | 还原目标必须落在受保护共享内，拦截 `..` 遍历与 UNC/设备路径 |
| **SYSTEM 边界收敛** | 路径校验位于真正执行 rename 的 C 侧，而非可被绕过的调用方 |
| **契约快速失败** | 驱动 ↔ 服务结构体带**编译期静态断言**，布局不一致直接编译失败 |
| **单写者模型** | Go 侧以 `mode=ro` 打开数据库，**物理上无法改坏元数据** |

---

## 架构

```
远程客户端 (\\server\share)
   Explorer 删除 → SMB2 → srv2.sys → 本地 IRP
                              │
                              ▼
        rbminiflt.sys  (Mini-Filter, Altitude 370030)
          PreSetInfo(DeleteFile=1):
            1. 路径命中受保护前缀?  (NT 形式前缀匹配, 大写规范化)
            2. 取请求者真实 SID     (TokenUser)
            3. 预留通知队列槽位     ← 满则拒绝删除, 文件不动
            4. rename 到 <共享同卷>\RBStore\<Sid>\<seq>_<basename>
            5. 成功 → COMPLETE(STATUS_SUCCESS)  // 用户以为删了
            6. 失败 → fail-closed 拒绝删除 (默认) + 计数
            7. 异步入队通知 → 通信端口 \RecycleBinPort
                              │
                              ▼
        RecycleBinSvc = rbservice.exe (C, SYSTEM, 唯一文件系统写者)
           端口线程    读通知 → 写 SQLite 元数据 (status=staged)
           维护线程    暂存区 → 同卷 $Recycle.Bin (status=landed)
                      配额 / 过期 / 多卷水位清理
                      孤儿对账 + 终态归档 + 数据库备份
           ops 线程    消费 ops 表执行还原 (2s 轮询)
           优雅停机    STOP 前跑最后一轮落地
                              │
                              ▼
                        recycle.db (SQLite, WAL)
                              ▲
                              │  只读查询 + ops 入队
        RecycleBinApi = rbapi.exe (Go, 可选, 127.0.0.1)
           - 从不碰文件系统
```

### 两个服务如何解耦

采用**共享 SQLite + 独立进程**：

| 契约 | 保障机制 |
|---|---|
| 进程隔离 | 一方崩溃不影响另一方；Go 挂了 C 照常拦截 |
| 文件系统 | Go 以 `mode=ro` 打开 `items`，**物理上无法改坏元数据** |
| 命令传递 | Go 往 `ops` 表插入请求，C 执行并回写 `state`/`message` |
| 业务规则 | 「能否还原」「目标是否合法」只由 C 判断，Go 不复制该逻辑 |
| DB schema | `db/schema.sql` 唯一真相；C 建表，Go 启动时校验 |
| 版本一致 | `user_version` 校验，不匹配**两侧都拒绝启动** |

> 解耦换来了进程级稳定性，代价是引入数据契约。上面后三行就是把这个契约
> 从「运行期静默失败」拉回「启动期快速失败」的机制。

### 为什么是「内核 rename + COMPLETE」而不是「问用户态」

旧范式「拦截 → 问用户态是否删除」无法做回收站：

- `ALLOW` → 文件已经没了，来不及移
- `DENY` → 删除取消，用户态再去移动会**再次触发本驱动回调** → 递归/环路

新范式在 Pre 回调里直接 rename，然后把 DELETE 请求 COMPLETE 成成功，
从根上消除了环路与时序竞争。

---

## 工作流程

### 两阶段落地

```
阶段 1 (内核, 同步)     D:\Share\a.txt  ──rename──▶  D:\RBStore\S-1-5-21-...\42_a.txt
                                                      ↓ 通知
                                              SQLite: status='staged'

阶段 2 (用户态, 30s内)  D:\RBStore\...\42_a.txt ──rename──▶ D:\$Recycle.Bin\S-1-5-21-...\$Rxxx.txt
                                                      + 写 $Ixxx.txt 元数据
                                              SQLite: status='landed'
```

两段都是**同卷 rename**——原子、零拷贝、瞬时完成。阶段 2 由维护线程异步补，
所以驱动崩溃也不会丢文件（最多停在暂存区）。

### 状态机

```
        ┌──────────────┐
        │   (新条目)    │
        └──────┬───────┘
               ▼
          ┌────────┐   维护线程落地成功   ┌────────┐
          │ staged │ ──────────────────▶ │ landed │
          └────────┘                     └────┬───┘
               │                              │
               │  过期/水位/超配额             │ 还原 / 过期 / 水位 / 超配额
               ▼                              ▼
          ┌────────┐                     ┌──────────┐
          │ purged │                     │ restored │
          └────────┘                     └──────────┘
               │                              │
               └────────────┬─────────────────┘
                            │ 超审计期后由归档清理删除行
                            ▼
                        (行已移除)
```

> 终态行（`purged`/`restored`）保留 `TerminalKeepDays`（默认 90 天）作为审计
> 记录，之后自动归档，避免表无限增长。

---

## 目录结构

```
recyclebin_svc/
  driver/                     内核 Mini-Filter 驱动 (WDK)
    rbminiflt.c                 驱动主体 (拦截/重定向/通知/端口)
    rbminiflt.h                 共享结构与常量 (变长通知布局)
    rbminiflt.inf               安装信息 (含 Altitude 370030)
    rbminiflt.vcxproj           驱动工程
    build.cmd                   命令行构建脚本
  service_c/                  核心服务 (C, SYSTEM, 零运行时依赖)
    rbservice.c                 SCM 入口 + 优雅停机 + 维护/ops 线程
    rbdb.c                      SQLite 封装 (WAL, 单写者, 备份, 归档)
    rbstore.c                   $Recycle.Bin 落地 + $I 元数据
    rbpolicy.c                  配额 / 过期 / 多卷水位
    rbrestore.c                 还原执行 + 目标路径白名单校验
    rbconcile.c                 暂存区孤儿对账
    rbport.c                    内核通信端口读取
    rbvol.c                     NT<->DOS 卷映射 + SID 解析
    rbconfig.c                  注册表配置 + 热加载
    rblog.c                     Windows 事件日志
    rbf_protocol.h              协议结构 + 编译期布局断言
    schema_sql.h                [生成物] 由 db\schema.sql 生成
    sqlite3.c/h                 [下载物] 构建时自动获取
  service_go/                  管理 REST API (Go, 可选)
    main.go                     配置 + 优雅停机
    api/api.go                  7 个 HTTP 端点 + Token 鉴权
    db/db.go                    只读查询 + ops 入队
  db/
    schema.sql                  **数据库 schema 唯一真相**
    gen_schema.ps1              生成 service_c/schema_sql.h
    verify_contract.py          C <-> Go 契约验证 (9 项)
    verify_c_contract.py        C 服务契约验证 (7 项)
  docs/
    design.md                   设计说明 (范式选型/通信模型/限制)
    bugfix-report.md            初版 Python 实现的历史修复记录
    buglist.md                  现存问题清单与整改排期
    bugfix-production.md        生产就绪度修复记录
  build_all.cmd               一键编译 (收集产物到 target\)
  deploy.ps1                  一键部署脚本 (管理员)
  recyclebin_svc.sln          VS 解决方案
  README.md                   本文件
```

---

## 快速开始

> ⚠️ **先读这条**：`StoreRoot` 必须与被保护共享在**同一卷**（内核 rename 不能跨卷）。
> 配错卷会导致文件被拒删或真删。详见[已知限制](#已知限制)。

```powershell
# 1) 开启测试签名 (驱动未正式签名时必须), 然后重启
bcdedit /set testsigning on
Restart-Computer

# 2) 一键编译三个组件 (产出统一收集到 target\Release\)
cd c:\RecycleBin\smb_intercept\recyclebin_svc
.\build_all.cmd Release

# 3) 改配置 (务必确认 StoreRoot 与共享同卷!)
notepad .\deploy.ps1

# 4) 管理员 PowerShell 一键部署
powershell -ExecutionPolicy Bypass -File .\deploy.ps1
```

`target\Release\` 是一个**自包含部署包**，内容可直接拷到目标机：

```
target\Release\
  rbminiflt.sys    内核驱动
  rbminiflt.inf    驱动安装信息
  rbservice.exe    核心服务
  rbapi.exe        管理 API（Go 可用时）
  deploy.ps1       部署脚本
```

---

## 详细部署

### 前置条件

| 项 | 要求 |
|---|---|
| OS | Windows Server 2016+ / Win10 1809+（x64） |
| 权限 | 管理员 PowerShell |
| 构建工具 | VS2022 + WDK 10（驱动、C 服务）；Go 1.22+（可选 API）；PowerShell（schema 生成，仅构建期） |
| 运行时依赖 | **无**。核心服务是原生 exe，目标服务器无需 Python / .NET / Go 运行时 |

### 部署步骤

`deploy.ps1` 共 7 个阶段（1 个前置校验 + 6 个执行步骤）：

| 步骤 | 动作 |
|---|---|
| **[0/6]** | 校验 `StoreRoot` 与受保护共享**同卷**，不匹配直接中止并给出可照抄的修正建议 |
| **[1/6]** | 用 `QueryDosDevice` 把 `D:\Share` 转成 NT 形式 `\Device\HarddiskVolumeN\Share`，分别写入驱动参数键和用户态配置键 |
| **[2/6]** | 创建 `StoreRoot`（如 `D:\RBStore`），加 `HIDDEN\|SYSTEM` 属性，共享用户不可见 |
| **[3/6]** | 拷贝 sys 到 `system32\drivers\`，`pnputil /add-driver ... /install` |
| **[4/6]** | `sc start rbminiflt`，检查退出码并在失败时提示签名/测试模式 |
| **[5/6]** | `sc create RecycleBinSvc ... obj= LocalSystem start= auto` 并启动 |
| **[6/6]** | 安装管理 API `rbapi.exe`（可选，未编译则跳过） |

### 部署包用法（推荐）

`build_all.cmd` 会把驱动、服务、INF 和 `deploy.ps1` 一起收集到 `target\Release\`，
使其成为一个自包含部署包：

```powershell
# 开发机构建
.\build_all.cmd Release

# 拷贝到目标机任意位置，例如 D:\Deploy
xcopy /s /i target\Release D:\Deploy

# 目标机上以管理员运行
cd D:\Deploy
powershell -ExecutionPolicy Bypass -File .\deploy.ps1
```

在部署包内运行时，`deploy.ps1` 优先使用**自身所在目录**的二进制，
无需访问源码树。

### 验证部署

```powershell
# 驱动已加载并 attach 到卷
sc.exe query rbminiflt
fltmc filters | Select-String rbminiflt
fltmc instances -f rbminiflt

# 服务运行中
sc.exe query RecycleBinSvc

# REST 健康检查
Invoke-RestMethod "http://127.0.0.1:8800/health" -Headers @{"X-Auth-Token"="change-me"}
```

### 端到端冒烟

```powershell
# 客户端: 从 \\server\share 删一个测试文件

# 服务端: 确认暂存 -> 落地
dir D:\RBStore /s
Invoke-RestMethod "http://127.0.0.1:8800/items?limit=10" -Headers @{"X-Auth-Token"="change-me"}

# 客户端: 打开回收站, 应能看到该文件并可原生还原
```

### 卸载 / 回滚

```powershell
sc.exe stop RecycleBinSvc;  sc.exe delete RecycleBinSvc
sc.exe stop rbminiflt
pnputil /delete-driver rbminiflt.inf /uninstall /force
del C:\Windows\System32\drivers\rbminiflt.sys
Remove-Item D:\RBStore -Recurse -Force
reg delete "HKLM\SOFTWARE\RecycleBin" /f
bcdedit /set testsigning off
```

---

## 配置参考

### 驱动参数

`HKLM\SYSTEM\CurrentControlSet\Services\rbminiflt\Parameters`：

| 键 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `ProtectedPaths` | REG_MULTI_SZ | *(deploy 写入)* | 受保护共享根（**NT 形式** `\Device\HarddiskVolumeN\...`） |
| `FailClosed` | REG_DWORD | `1` | `1` = 无法暂存时**拒绝删除**保住数据（推荐）<br>`0` = 放行真删（文件永久丢失，仅应急旁路） |

> 驱动参数在**驱动加载时**读取，改动需重启驱动（`sc stop/start rbminiflt`）。

### 用户态配置

`HKLM\SOFTWARE\RecycleBin`：

| 键 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `ProtectedPaths` | REG_MULTI_SZ | `D:\Share` | 受保护共享根（**DOS 形式**；服务与还原白名单使用） |
| `StoreRoot` | REG_SZ | `D:\RBStore` | 暂存区根 + SQLite 库位置。**必须与受保护共享同卷** |
| `QuotaMB` | REG_DWORD | `5120` | 每用户回收站配额 (MB)，超出按最旧优先清理 |
| `RetentionDays` | REG_DWORD | `30` | 保留天数，覆盖 `landed` 与 `staged` |
| `DiskFreeMinMB` | REG_DWORD | `5120` | 磁盘剩余水位 (MB)，低于则清理最旧 landed 条目 |
| `StagedBatch` | REG_DWORD | `500` | 维护线程每轮落地批大小（循环至排空，非每轮上限） |
| `OrphanGraceDays` | REG_DWORD | `7` | 暂存区无主文件回收宽限期（天） |
| `TerminalKeepDays` | REG_DWORD | `90` | `restored`/`purged` 行保留审计期（天），到期归档 |
| `EnableRestApi` | REG_DWORD | `0` | `1` 开启管理 API |
| `RestApiPort` | REG_DWORD | `8800` | API 端口（仅监听 `127.0.0.1`） |
| `RestApiToken` | REG_SZ | *(空)* | `X-Auth-Token`；留空则不鉴权 |
| `PortName` | REG_SZ | `\RecycleBinPort` | 内核通信端口名 |

### 配置热加载

```powershell
# 用户态配置：改完注册表后发送控制码，无需重启服务
sc.exe control RecycleBinSvc 128
```

热加载会**先校验再替换**：非法值（如 `RetentionDays=0`，会导致立即删光文件）
会被拒绝，原配置保持生效。

> ⚠️ `StoreRoot` 虽可热加载，但数据库路径在启动时固定，
> 移动存储位置仍需重启服务（脚本会输出告警）。
> 驱动侧参数（`ProtectedPaths`、`FailClosed`）不支持热加载。

---

## REST API

所有请求需带 `X-Auth-Token` 头（当 `RestApiToken` 非空时）。仅监听 `127.0.0.1`。

### 端点总览

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/health` | 数据库计数 + 驱动连通性 |
| GET | `/stats` | 驱动原始计数器 |
| GET | `/items` | 分页查询（`limit`、`offset`、`status`、`sid`） |
| GET | `/items/{id}` | 单条详情 |
| GET | `/search` | 路径子串搜索（`q`、`limit`） |
| POST | `/ops` | 提交操作（`restore` 单条 / `restore-tree` 目录树） |
| GET | `/ops/{id}` | 查询操作执行结果 |

### GET /health

```json
{
  "ok": true,
  "ts": 1780000000.0,
  "counts": {"staged": 3, "landed": 1200, "purged": 30, "restored": 1},
  "driver": null
}
```

`driver` 为 `null` 表示通信端口未连通。

### GET /stats

| 字段 | 含义 |
|---|---|
| `intercepts` | 命中受保护前缀的删除次数 |
| `rename_ok` | 成功重定向到暂存区 |
| `rename_fail` | 重定向失败次数 |
| `delete_denied` | **fail-closed 拒绝删除次数**（数据已保住，非丢失） |
| `notify_sent` / `notify_dropped` / `notify_queue_full` | 通知投递情况 |
| `queue_depth` / `max_queue_depth` | 队列当前深度 / 历史峰值 |

> ⚠️ **已知问题**：驱动计数器尚未打通到 Go 侧（见 [RB-13](docs/buglist.md)），
> `/stats` 当前恒返回 `503`，`/health` 的 `driver` 字段恒为 `null`。
> 这不影响拦截与还原功能，但意味着**真删/拒删次数暂不可观测**。

### POST /ops

**单条还原**：

```json
{ "type": "restore", "id": 123 }
```

**按前缀批量还原（目录树）**：

```json
{ "type": "restore-tree", "arg": "D:\\Share\\Project" }
```

返回操作 ID，实际执行由 C 服务异步完成，用 `GET /ops/{id}` 查询结果。
批量还原的 message 形如 `restored 41/42; 1 failed (first: id=123: ...)`。

> **删除目录树后如何还原**
>
> SMB 删除目录是**逐个条目**进行的（客户端递归删除），所以一个含 N 个文件、
> M 个子目录的目录树，在回收站里是 **N+M 个分散条目**，而不是一个目录——
> 这与本地 Explorer 删除（一次 rename 整树，显示为单个目录）不同。
>
> 用 `restore-tree` 可一次还原整棵树，无需逐条操作：
>
> ```powershell
> Invoke-RestMethod "http://127.0.0.1:8800/ops" -Method Post -Headers $h `
>                   -Body '{"type":"restore-tree","arg":"D:\\Share\\Project"}' `
>                   -ContentType "application/json"
> ```
>
> 前缀匹配是**真前缀**：`D:\Share\Project` 不会连带匹配 `D:\Share\ProjectBackup`。
> 单次上限 5000 条，超出时收窄前缀分批处理。

> **还原目标安全**：系统只接受还原到**原路径**，或落在受保护共享内的目标；
> 含 `..` 的路径、UNC 路径、设备路径均被拒绝。该校验在 C 侧执行，
> `restore-tree` 的前缀同样受此约束。

### 调用示例

```powershell
$h = @{"X-Auth-Token"="change-me"}
Invoke-RestMethod "http://127.0.0.1:8800/items?status=landed&limit=50" -Headers $h
Invoke-RestMethod "http://127.0.0.1:8800/search?q=report&limit=20" -Headers $h

# 还原单条
Invoke-RestMethod "http://127.0.0.1:8800/ops" -Method Post -Headers $h `
                  -Body '{"type":"restore","id":123}' -ContentType "application/json"

# 还原整个目录树
Invoke-RestMethod "http://127.0.0.1:8800/ops" -Method Post -Headers $h `
                  -Body '{"type":"restore-tree","arg":"D:\\Share\\Project"}' `
                  -ContentType "application/json"
```

---

## 运维与监控

### 关键健康检查

```powershell
# 1) 驱动与服务状态
sc.exe query rbminiflt
sc.exe query RecycleBinSvc

# 2) 暂存区是否有积压（长时间不为空 = 落地异常）
dir D:\RBStore /s | Measure-Object

# 3) 事件日志中的告警（孤儿回收、拒删、备份失败）
Get-WinEvent -LogName Application -Source RecycleBin* -MaxEvents 50
```

### 核心指标

| 指标 | 健康标准 | 异常含义 |
|---|---|---|
| `rename_fail` | 长期为 0 | 非 0 通常说明 StoreRoot 卷不匹配或磁盘满 |
| `delete_denied` | 长期为 0 | 非 0 说明暂存区异常，删除被拒（**数据已保住**） |
| `notify_dropped` | 长期为 0 | 非 0 说明服务离线或通信异常，可能产生孤儿 |
| 暂存区滞留量 | 30s 内清空 | 持续积压说明落地阻塞 |

> 上表中驱动计数器当前不可观测（[RB-13](docs/buglist.md)），
> 建议改用事件日志与暂存区滞留量作为替代监控手段。

### 时间与延迟

| 阶段 | 延迟 |
|---|---|
| 删除 → 文件进暂存区 | 同步，毫秒级（内核 rename） |
| 暂存区 → `$Recycle.Bin` | ≤ 30s（维护线程周期，循环至排空） |
| 提交还原 → 执行完成 | ≤ 2s（独立 ops 线程） |
| 落地 → 用户回收站可见 | 依赖 Explorer 刷新 |

删除后文件**立刻就安全了**（已在暂存区），第二阶段的 30s 只影响"回收站里什么时候看得见"。

### 日志

- **服务**：Windows 事件日志 + 控制台镜像（`console` 模式直接看终端）
- **驱动**：`DbgPrint`（需 DebugView 或内核调试器查看）

### 容量规划

- 暂存区只短暂驻留（≤30s），峰值空间取决于该窗口内的删除量
- 长期占用在 `$Recycle.Bin`，受 `QuotaMB` / `RetentionDays` / `DiskFreeMinMB` 三重约束
- 落地是 rename，不额外占空间
- 数据库每日自动备份至 `<StoreRoot>\recycle.db.bak`（备份前先做完整性校验）

---

## 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| 驱动启动失败 | 未开测试签名 / sys 未签名 | `bcdedit /set testsigning on` 后重启 |
| 删除不拦截 | `ProtectedPaths` 写成了 DOS 形式 | 驱动侧必须是 NT 形式，改完重启驱动 |
| **删除被拒绝（Access Denied）** | fail-closed 生效，暂存区不可用 | 检查 StoreRoot 同卷/权限/磁盘空间；查看事件日志的拒删原因 |
| 文件真删，回收站没有 | `FailClosed=0` 且暂存失败 | 检查事件日志；确认 StoreRoot 与共享同卷 |
| 服务连不上端口 | 驱动未加载，或 `PortName` 不匹配 | `sc query rbminiflt`；`/health` 的 `driver` 为 null 即未连通 |
| 回收站里看不到文件 | 维护线程还没跑，或卷映射失败 | 等 30s；检查服务日志里的卷解析失败 |
| 用户还原不了 | 落地 SID 与用户登录 SID 不符 | 检查 `/items` 里 `sid` 是否为 `S-1-5-...` |
| 还原返回"目标不在受保护共享内" | 目标路径校验拦截 | 还原目标必须落在受保护共享内，或还原到原路径 |
| 磁盘很快被撑满 | 配额/水位阈值太大 | 调小阈值后 `sc control RecycleBinSvc 128` |
| REST 401 | token 不匹配 | 确认注册表 `RestApiToken`；改完热加载 |

---

## 已知限制

1. **StoreRoot 必须与被保护共享同卷** — 内核 `FileRenameInformation` 不支持跨卷。
   保护多卷需分别部署或做驱动改造（"拷贝 + 删源"，复杂度与风险显著上升）。
2. **跨卷还原不支持** — 还原同样依赖同卷 rename。
3. **驱动需签名** — 生产环境应使用 EV 证书，测试模式仅限验证（2–6 周外部流程）。
4. **驱动计数器暂不可观测** — `/stats` 恒 503（[RB-13](docs/buglist.md)）。
5. **驱动参数不支持热加载** — `ProtectedPaths`、`FailClosed` 改动需重启驱动。
6. **符号链接 / 硬链接 / 重解析点未特殊处理** — 按普通文件重定向。
7. **本地删除也会被拦** — 判定依据为路径前缀（原 session ID 过滤会漏掉
   SMB2 在 session 0 执行的删除）。服务端本机操作受保护目录同样进回收站。
8. **`$I` 元数据为 v1 格式** — 与 Windows 回收站 UI 的兼容性待实测确认（[RB-17](docs/buglist.md)）。

---

## 开发构建

### 一键构建（推荐）

```powershell
.\build_all.cmd Release    # 或 Debug / All
```

依次构建驱动、C 服务、Go API，运行契约验证，最后把所有产物收集到
`target\Release\`。Go 工具链缺失时**跳过 API 构建并继续**（API 为可选组件）。

### 分组件构建

```powershell
# 驱动
cd driver
.\build.cmd Release      # 产出 driver\rbminiflt.sys

# 核心服务 (自动从 db\schema.sql 重新生成 DDL)
cd ..\service_c
.\build.cmd Release      # 产出 rbservice.exe

# 管理 API
cd ..\service_go
go build -o rbapi.exe .
```

> C 服务构建时会自动从 sqlite.org 下载 amalgamation（不再 vendored 到仓库）。

### 运行模式

```powershell
rbservice.exe console              # 前台运行（Ctrl+C 停止）
rbservice.exe once                 # 跑一轮维护就退出（手工清空积压）
rbservice.exe once --db D:\x\recycle.db   # 针对指定库维护（无需改注册表）
```

### 契约验证（改完 schema 必跑）

```powershell
python db\verify_contract.py      # 9 项：Go 侧防护 + 跨进程集成
python db\verify_c_contract.py    # 7 项：C 侧版本防护 + ops 往返
```

这两个脚本会**故意破坏契约**（改版本号、改列名、插非法状态），
断言服务**拒绝启动**而不是带病运行。改动 `db\schema.sql` 后必跑。

### 驱动 ↔ 服务协议

`RBF_NOTIFICATION` / `RBF_REPLY` / `RBF_STATS` 在 `driver\rbminiflt.h` 与
`service_c\rbf_protocol.h` 中各有一份定义。

**`rbf_protocol.h` 带编译期静态断言**（`typedef char[...]` 技巧），
字段顺序或宽度不一致 → **编译失败**，而不是运行时静默解析错位。

通知结构体为**变长布局**（48 字节头 + 路径负载），断言覆盖每个字段的偏移量，
并强制单条通知上限 ≤ 8 KB —— 这是防止栈溢出缺陷复发的守卫。

### 数据库 schema 唯一真相

`db\schema.sql` 是 recycle.db 的**唯一定义处**：

```
db\schema.sql  ──gen_schema.ps1──▶  service_c\schema_sql.h  ──编译进──▶  rbservice.exe
                                                                            │
                                                                       建表/修补
                                                                            ▼
                                                                       recycle.db
                                                                            ▲
                                                           启动时校验列名+版本 │
                                                                       rbapi.exe
```

- **C 侧**：唯一建表者。启动时执行内嵌 DDL（全部 `IF NOT EXISTS`，可自动修补缺失对象）
- **Go 侧**：启动时用 `PRAGMA table_info` + `PRAGMA user_version` 校验，
  **不匹配直接拒绝启动**，而不是运行到一半报 500

**改 schema 的正确姿势**：

1. 编辑 `db\schema.sql`
2. 升 `RB_SCHEMA_VERSION`（`service_c/rbsvc.h`）和 `SchemaVersion`（`service_go/db/db.go`）
3. 更新 Go 侧 `expectedItemCols`
4. 重新构建两侧

> ⚠️ 只改一侧会让服务**启动即失败** —— 这是刻意设计的快速失败，
> 好过带着错误的列映射跑起来静默损坏数据。

---

## 生产就绪度

本项目已完成一轮生产就绪度整改，**修复 10 项**（P0 × 4、P1 × 6）。
详见 [docs/bugfix-production.md](docs/bugfix-production.md)。

### 已修复（摘要）

| 编号 | 级别 | 摘要 |
|---|---|---|
| RB-01 | P0 | 通知结构体改变长布局，消除内核栈溢出（原 64.5 KB > 24 KB 栈） |
| RB-04 | P0 | 新增 fail-closed 策略，默认拒绝删除，杜绝静默真删 |
| RB-05 | P0 | 暂存区孤儿对账与老化回收，防止撑满共享卷 |
| RB-06 | P0 | 还原目标路径白名单校验，消除 SYSTEM 任意写入提权面 |
| RB-07 | P1 | 通知内存优化（队列满配 33 MB → 2.4 MB） |
| RB-08 | P1 | 预留式入队，递归删除不再产生孤儿 |
| RB-09 | P1 | 终态记录归档 + LIKE 通配符转义 |
| RB-10 | P1 | 还原独立快车道线程（30s → 2s） |
| RB-11 | P1 | 数据库在线备份 + 完整性校验 |
| RB-12 | P1 | 用户态配置热加载（控制码 128） |

### 遗留项

| 编号 | 事项 | 阻塞原因 |
|---|---|---|
| RB-02 | **驱动签名** | 外部流程：EV 证书 + Dev Center 认证，2–6 周 |
| RB-03 | Pre 回调重构 | 需测试机 + Driver Verifier 验证，风险高，暂缓 |
| RB-12 | 驱动参数热加载 | 需重构驱动配置加载路径 |
| RB-13 ~ RB-17 | 可观测性、测试、安全细节 | 待排期，详见 [docs/buglist.md](docs/buglist.md) |

> **建议**：RB-02 应尽早启动 —— 它是纯外部依赖，且 HLK 测试套件会反向暴露
> 驱动实现缺陷。

---

## 文档索引

| 文档 | 内容 |
|---|---|
| [docs/design.md](docs/design.md) | 设计说明（范式选型 / 通信模型 / 限制） |
| [docs/bugfix-report.md](docs/bugfix-report.md) | 初版 Python 实现的历史修复记录（B1–B7） |
| [docs/buglist.md](docs/buglist.md) | 现存问题清单与整改排期（RB-01 ~ RB-17） |
| [docs/bugfix-production.md](docs/bugfix-production.md) | 生产就绪度修复记录（本轮 10 项） |

---

<div align="center">

**历史修复记录**：初版 Python 实现阶段共修复 7 项（B1–B7），
详见 [docs/bugfix-report.md](docs/bugfix-report.md)。

构建状态：驱动 Release 零错误零警告；契约验证 16/16 通过。

</div>
