# Windows 文件共享回收站 (RecycleBin for SMB)

> 拦截通过 SMB 远程访问共享的用户的删除操作，把文件**重定向到回收站暂存区**而非真正删除，
> 实现「远程删除 → 进回收站 → 可还原 / 可审计」。

用户端零感知、零改动：在 Explorer 里按 Delete，文件"消失"，实际进了他自己的
`$Recycle.Bin`，用标准回收站就能还原。

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
- [Bugfix 记录](#bugfix-记录)

---

## 特性

| 特性 | 说明 |
|---|---|
| **内核级拦截** | Mini-Filter 驱动（Altitude `370030`，Activity Monitor 区间）在删除 I/O 路径上拦截，不依赖 Shell 钩子 |
| **零用户态往返** | 重定向（rename）在内核态同步完成，删除路径不阻塞等待用户态决策 |
| **标准回收站兼容** | 最终落地为 `$Recycle.Bin\<SID>\$Rxxxx` + `$Ixxxx`（v1 格式），用户可用 Explorer 原生回收站还原 |
| **真实用户归属** | 内核通过 `SeCaptureSubjectContext` + `TokenUser` 取**真实客户端 SID**，不再用 session ID 猜 |
| **Fail-open 安全** | 重定向失败则放行真实删除 + 记日志，**绝不**让用户"删不掉" |
| **异步通知** | 有界队列（512）+ 单发送线程投递元数据，服务离线最坏只丢元数据 |
| **自动运维** | 配额、保留期、多卷磁盘水位自动清理 |
| **可选 REST API** | Token 鉴权，支持查询 / 还原 / 配置热重载 |
| **单写者 SQLite** | WAL + 索引 + 聚合 SQL，支撑 TB 级 / 亿级条目 |

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
            2. 取请求者真实 SID     (TokenUser, 失败回退 S-SESSION-<id>)
            3. rename 到 <共享同卷>\RBStore\<Sid>\<seq>_<basename>
            4. 成功 → COMPLETE(STATUS_SUCCESS)  // 用户以为删了
            5. 失败 → 放行真删 + 事件日志 (fail-open)
            6. 异步入队通知 → 通信端口 \RecycleBinPort
                              │
                              ▼
        RecycleBinSvc (SYSTEM 服务, Python)
           - 读端口通知 → 写 SQLite 元数据 (status=staged)
           - 维护线程(30s): 暂存区 → 同卷 $Recycle.Bin (status=landed)
           - 配额 / 过期 / 多卷磁盘水位清理
           - 可选 REST 管理 API (127.0.0.1)
```

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
```

---

## 目录结构

```
recyclebin_svc/
  driver/                     内核 Mini-Filter 驱动 (WDK)
    rbminiflt.c                 驱动主体 (拦截/重定向/通知/端口)
    rbminiflt.h                 共享结构与常量 (与用户态 ctypes 严格对齐)
    rbminiflt.inf               安装信息 (含 Altitude 370030)
    rbminiflt.vcxproj           驱动工程
    build.cmd                   命令行构建脚本
    syms.txt                    符号导出清单
  service/                    用户态 SYSTEM 服务 (Python 3.10+)
    rb_service.py               服务主程序 (端口读线程 + 维护线程 + REST)
    recyclebin_lib.py           回收站落地 / $I 元数据 / 配额 / 还原
    config.py                   配置加载 (注册表优先, 回退内置默认)
    requirements.txt
  docs/
    design.md                   设计说明 (范式选型/通信模型/限制)
    bugfix-report.md            本轮 bugfix 详细报告
  deploy.ps1                  一键部署脚本 (管理员)
  recyclebin_svc.sln          VS 解决方案
  README.md                   本文件
```

---

## 快速开始

> ⚠️ **先读这条**：`StoreRoot` 必须与被保护共享在**同一卷**（内核 rename 不能跨卷）。
> 配错卷会导致 fail-open —— 文件被真删且回收站里没有。详见[已知限制](#已知限制)。

```powershell
# 1) 开启测试签名 (驱动未正式签名时必须), 然后重启
bcdedit /set testsigning on
Restart-Computer

# 2) 编译驱动 (WDK 10 + VS2022)
cd c:\RecycleBin\smb_intercept\recyclebin_svc\driver
.\build.cmd Release

# 3) 装依赖
pip install -r ..\service\requirements.txt

# 4) 改配置 (务必确认 StoreRoot 与共享同卷!)
notepad ..\deploy.ps1

# 5) 管理员 PowerShell 一键部署
cd ..
powershell -ExecutionPolicy Bypass -File .\deploy.ps1
```

---

## 详细部署

### 前置条件

| 项 | 要求 |
|---|---|
| OS | Windows Server 2016+ / Win10 1809+（x64） |
| 权限 | 管理员 PowerShell |
| 驱动 | 已编译产出 `driver\rbminiflt.sys` |
| Python | 3.10+，且 `Get-Command python` 可解析 |
| 依赖 | `pip install -r service\requirements.txt`（仅可选 psutil） |

### 部署步骤

`deploy.ps1` 自动完成 5 步：

| 步骤 | 动作 |
|---|---|
| **1/5** | 用 `QueryDosDevice` 把 `D:\Share` 转成 NT 形式 `\Device\HarddiskVolumeN\Share`，分别写入驱动参数键和用户态配置键 |
| **2/5** | 创建 `StoreRoot`（如 `D:\RBStore`），加 `HIDDEN|SYSTEM` 属性，共享用户不可见 |
| **3/5** | 拷贝 sys 到 `system32\drivers\`，`pnputil /add-driver ... /install` |
| **4/5** | `sc start rbminiflt` |
| **5/5** | `sc create RecycleBinSvc ... obj= LocalSystem start= auto` 并启动 |

注册表落点：

```
HKLM\SYSTEM\CurrentControlSet\Services\rbminiflt\Parameters
    ProtectedPaths  (REG_MULTI_SZ)  ← NT 形式, 驱动读
HKLM\SOFTWARE\RecycleBin
    ProtectedPaths  (REG_MULTI_SZ)  ← DOS 形式, 服务读
    StoreRoot / QuotaMB / RetentionDays / EnableRestApi / RestApiPort
    RestApiToken / DiskFreeMinMB
```

### 验证部署

```powershell
# 驱动已加载并 attach 到卷
sc.exe query rbminiflt
fltmc filters | Select-String rbminiflt
fltmc instances -f rbminiflt

# 服务运行中
sc.exe query RecycleBinSvc

# REST 健康检查 (driver 字段非 null = 通信端口已连通)
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

### 手动部署（不用脚本）

```powershell
copy driver\rbminiflt.sys C:\Windows\System32\drivers\
pnputil /add-driver driver\rbminiflt.inf /install
sc.exe start rbminiflt

reg add "HKLM\SYSTEM\CurrentControlSet\Services\rbminiflt\Parameters" ^
      /v ProtectedPaths /t REG_MULTI_SZ /d "\Device\HarddiskVolume3\Share" /f
reg add "HKLM\SOFTWARE\RecycleBin" /v StoreRoot /t REG_SZ /d "D:\RBStore" /f

python service\rb_service.py run    # 前台调试

sc.exe create RecycleBinSvc binPath= "\"C:\Python312\python.exe\" \"C:\path\service\rb_service.py\" run" type= own start= auto obj= LocalSystem
sc.exe start RecycleBinSvc
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

注册表 `HKLM\SOFTWARE\RecycleBin`（不存在时用 `config.py` 的 `DEFAULTS`）：

| 键 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `ProtectedPaths` | REG_MULTI_SZ | `D:\Share`, `E:\Public` | 受保护共享根（**DOS 形式**；驱动侧由 deploy 转 NT 形式） |
| `StoreRoot` | REG_SZ | `C:\RBStore` | 暂存区根 + SQLite 库位置。<br>**必须与受保护共享同卷** |
| `QuotaMB` | REG_DWORD | `5120` | 每用户回收站配额 (MB)，超出按最旧优先清理 |
| `RetentionDays` | REG_DWORD | `30` | 保留天数，覆盖 `landed` 与 `staged` |
| `DiskFreeMinMB` | REG_DWORD | `5120` | 磁盘剩余水位 (MB)，低于则清理最旧 landed 条目 |
| `StagedBatch` | REG_DWORD | `500` | 维护线程每轮落地批大小 |
| `EnableRestApi` | REG_DWORD | `0` | `1` 开启管理 API |
| `RestApiPort` | REG_DWORD | `8800` | API 端口（仅监听 `127.0.0.1`） |
| `RestApiToken` | REG_SZ | *(空)* | `X-Auth-Token`；留空则不鉴权 |
| `PortName` | REG_SZ | `\RecycleBinPort` | 内核通信端口名 |

> 改完注册表，用户态侧调 `POST /config/reload` 即生效，无需重启服务；
> **`ProtectedPaths` 变了必须重启驱动**（`sc stop/start rbminiflt`），因为驱动在加载时缓存了路径前缀。

---

## REST API

所有请求需带 `X-Auth-Token` 头（当 `RestApiToken` 非空时）。仅监听 `127.0.0.1`。

### GET /health

```json
{
  "total_items": 1234,
  "counts": {"staged": 3, "landed": 1200, "purged": 30, "restored": 1},
  "disk": {"free": 107374182400, "used": ..., "total": ...},
  "db": "D:\\RBStore\\recycle.db",
  "ts": 1780000000.0,
  "driver": { "intercepts": 500, "rename_ok": 498, "rename_fail": 2, ... }
}
```

`driver` 为 `null` 表示通信端口未连通。

### GET /stats

驱动计数器：

| 字段 | 含义 |
|---|---|
| `intercepts` | 命中受保护前缀的删除次数 |
| `rename_ok` | 成功重定向到暂存区 |
| `rename_fail` | **重定向失败 → 真删**（应长期为 0，非 0 说明 StoreRoot 配错卷） |
| `notify_sent` / `notify_dropped` / `notify_queue_full` | 通知投递情况 |
| `queue_depth` / `max_queue_depth` | 队列当前深度 / 历史峰值 |

### GET /items

分页查询。参数：`limit`（1-1000，默认 100）、`offset`、`status`、`sid`。

### GET /items/{id}

单条详情。

### POST /restore

```json
{ "id": 123 }
```
→ `{ "ok": true, "msg": "ok" }`，失败返回 409。

### POST /config/reload

热重载注册表配置（含 token），返回 `{ "ok": true }`。

### 调用示例

```powershell
$h = @{"X-Auth-Token"="change-me"}
Invoke-RestMethod "http://127.0.0.1:8800/items?status=landed&limit=50" -Headers $h
Invoke-RestMethod "http://127.0.0.1:8800/restore" -Method Post -Headers $h `
                  -Body '{"id":123}' -ContentType "application/json"
```

---

## 运维与监控

### 关键健康检查

```powershell
# 最该盯的一个数: rename_fail 应恒为 0
(Invoke-RestMethod "http://127.0.0.1:8800/stats" -Headers $h).rename_fail
```

`rename_fail > 0` 意味着有文件被**真删且没进回收站**——通常是 StoreRoot 卷不匹配、
权限不足或磁盘满。

### 时间与延迟

| 阶段 | 延迟 |
|---|---|
| 删除 → 文件进暂存区 | 同步，毫秒级（内核 rename） |
| 暂存区 → `$Recycle.Bin` | ≤ 30s（维护线程周期） |
| 落地 → 用户回收站可见 | 依赖 Explorer 刷新 |

删除后文件**立刻就安全了**（已在暂存区），第二阶段的 30s 只影响"回收站里什么时候看得见"。

### 日志

- 服务：`stdout` / 运行目录日志（前台 `run` 模式直接看控制台）
- 驱动：Windows 事件日志（失败路径会记录）

### 容量规划

- 暂存区只短暂驻留（≤30s），峰值空间取决于该窗口内的删除量
- 长期占用在 `$Recycle.Bin`，受 `QuotaMB` / `RetentionDays` / `DiskFreeMinMB` 三重约束
- 落地是 rename，不额外占空间

---

## 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| 驱动启动失败 | 未开测试签名 / sys 未签名 | `bcdedit /set testsigning on` 后重启 |
| 删除不拦截 | `ProtectedPaths` 写成了 DOS 形式 | 驱动侧必须是 NT 形式 `\Device\HarddiskVolumeN\...`，改完重启驱动 |
| **文件真删，回收站没有** | StoreRoot 与共享不同卷 | 检查 `/stats` 的 `rename_fail`；把 StoreRoot 移到共享所在卷 |
| 服务连不上端口 | 驱动未加载，或 `PortName` 不匹配 | `sc query rbminiflt`；`/health` 的 `driver` 为 null 即未连通 |
| 回收站里看不到文件 | 维护线程还没跑，或卷映射失败 | 等 30s；检查服务日志里的 `[land] 无法解析卷` |
| 用户还原不了 | 落地 SID 与用户登录 SID 不符 | 检查 `/items` 里 `sid` 是否为 `S-1-5-...`；`S-SESSION-*` 说明内核取 SID 失败 |
| 磁盘很快被撑满 | 配额/水位阈值太大 | 调小 `QuotaMB` / `DiskFreeMinMB` / `RetentionDays`，`POST /config/reload` |
| REST 401 | token 不匹配 | 确认注册表 `RestApiToken`；改完调 `/config/reload` |

---

## 已知限制

1. **StoreRoot 必须与被保护共享同卷** — 内核 `FileRenameInformation` 不支持跨卷。
   保护多卷需分别部署或先做驱动改造（改为"拷贝 + 删源"，复杂度与风险显著上升）。
2. **跨卷还原不支持** — `restore_item` 同样依赖同卷 rename。
3. **符号链接 / 硬链接 / 重解析点未特殊处理** — 按普通文件重定向。
4. **本地删除也会被拦** — 判定依据改为路径前缀（原先的 session ID 过滤会漏掉
   SMB2 在 session 0 执行的删除）。服务端本机操作受保护目录同样进回收站。
5. **驱动需签名** — 生产环境应使用 EV 证书，测试模式仅限验证。
6. **元数据可能丢失** — 服务离线期间的通知会丢弃（最坏丢审计记录，不丢文件）。
7. **目录删除** — 整个目录会被 rename 成单一条目，还原时整体恢复。

---

## 开发构建

### 驱动

```powershell
cd driver
.\build.cmd Release      # 产出 driver\rbminiflt.sys
```

或 VS2022 打开 `driver\rbminiflt.vcxproj`，平台 x64，Target Windows10，Driver Type WDM。

> 精简版 WDK 缺少 `ntseapi.h` 与部分 `Rtl*` ACL 符号，因此暂存目录的安全描述符
> 是**手写自相对 SD**（纯字节布局），不依赖任何缺失 API。详见
> `docs/bugfix-report.md` 的 B2 条目。

### 用户态

```powershell
python -m py_compile service\rb_service.py service\recyclebin_lib.py service\config.py
python service\rb_service.py run       # 前台调试
```

### 驱动 ↔ 用户态协议

`RBF_NOTIFICATION` / `RBF_REPLY` / `RBF_STATS` 在 `driver\rbminiflt.h` 与
`service\rb_service.py`（ctypes）中各有一份定义，**必须严格对齐**（`#pragma pack(1)`
+ `_pack_ = 1`，字段顺序与宽度一致）。改动一侧必须同步改另一侧。

---

## Bugfix 记录

本轮共修复 7 项（B1–B7），详见 **[docs/bugfix-report.md](docs/bugfix-report.md)**：

| 编号 | 严重度 | 摘要 |
|---|---|---|
| B1 | 高 | 暂存目录扁平化 `<seq>_<basename>` + 原子序号，消除路径冲突 |
| B2 | 高 | 手写自相对安全描述符绕过精简 WDK 缺失的 `Rtl*` ACL API |
| B3 | 高 | 改用 `TokenUser` 取真实客户端 SID，修复回收站归属错误 |
| B4 | 中 | 移除 session ID 过滤（误伤 SMB2 删除）+ `FltSendMessage` 30s 超时 |
| B5 | 高 | `$I` v1 格式修正为 `<IQQI`（PathLen 应为 4B 非 8B）+ SID 去前导反斜杠 |
| B6 | 中 | 磁盘水位改为多卷检查 |
| B7 | 中 | deploy 路径兼容 + REST token 热更新 |

构建状态：驱动 Release **零错误零警告**（`rbminiflt.sys` 15360B），Python 模块编译通过。
