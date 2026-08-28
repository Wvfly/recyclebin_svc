# Bugfix 报告

- 项目：Windows 文件共享回收站 (RecycleBin for SMB)
- 范围：内核 Mini-Filter 驱动 (`rbminiflt`) + 用户态服务 + 部署脚本
- 日期：2026-08-28
- 结论：驱动 Release 构建零错误零警告（`rbminiflt.sys` 15360B），关键格式/逻辑均已验证

> **📌 历史说明**
>
> 本报告记录的是对**初版 Python 实现**（原 `service/rb_service.py`、
> `service/recyclebin_lib.py`、`service/config.py`）的修复。
>
> 该 Python 实现**已被移除**，用户态已用 C（`service_c/rbservice.exe`）
> 重写，管理 API 用 Go（`service_go/rbapi.exe`）实现。
>
> 下列修复中**与语言无关的设计结论**（扁平化暂存目录、真实 SID 获取、
> `$I` v1 格式、多卷水位、同卷 rename 约束等）已在 C 版中保留并延续；
> 涉及具体文件名/代码位置的部分仅作历史参考。

---

## 一、修复总览

| 编号 | 严重度 | 模块 | 问题摘要 |
|---|---|---|---|
| B1 | 高 | 驱动 | 删除重定向的暂存目录/命名方案存在冲突与递归风险 |
| B2 | 高 | 驱动 | 暂存目录 ACL 不当，且依赖精简 WDK 缺失的 ACL API |
| B3 | 高 | 驱动 | 用会话 ID 拼 SID，无法反映真实客户端用户 |
| B4 | 中 | 驱动 | 会话 ID 过滤误伤 SMB2 删除 + 通知同步发送无超时 |
| B5 | 高 | 服务 | 回收站 `$I` 元数据文件格式错误、SID 带前导反斜杠 |
| B6 | 中 | 服务 | 磁盘水位仅检查单卷，多卷场景清理失效 |
| B7 | 中 | 服务/部署 | REST token 热更新不生效、部署脚本路径不兼容 |

---

## 二、详细修复说明

### B1 暂存目录布局扁平化（驱动）

**问题**：原暂存路径按原始相对路径镜像建目录，深层路径在重命名目标不存在时 `RenameInformation` 会失败；且文件名冲突无保护，并发删除同名文件互相覆盖。

**修复**：
- 暂存布局改为扁平化：`<vol>\RBStore\<Sid>\<seq>_<basename>`
- 序列号用 `InterlockedIncrement64(&G.StageSeq)` 原子递增，保证并发安全与全局唯一

```c
// rbminiflt.c - RbfBuildStorePath
// 扁平布局: <vol>\RBStore\<Sid>\<seq>_<basename>
```

### B2 暂存区安全描述符手写 DACL（驱动）

**问题**：
1. 原代码依赖 `RtlInitializeAcl` / `RtlAddAccessAllowedAce` 等 API，精简 WDK 的 `ntoskrnl.lib` 缺失 5 个 `Rtl*` 符号，链接失败；
2. 缺失 `ntseapi.h` / `ntdef.h` 头文件；
3. ACL 语义上暂存目录需要允许所有用户向其中写入（add-only），默认继承的安全描述符不满足。

**修复**：改为纯字节布局手写自相对安全描述符（不依赖任何缺失 API）：

```c
#pragma pack(push, 1)
typedef struct _RBF_STAGING_SD {
    SECURITY_DESCRIPTOR_RELATIVE Sd;        // off 0, size 20
    ACL                          Acl;       // off 20, size 8
    ACE_HEADER                   AceHeader; // off 28, size 4
    ACCESS_MASK                  AceMask;   // off 32, size 4
    UCHAR                        SidBytes[8]; // off 36, S-1-0-0 (Everyone)
} RBF_STAGING_SD;
#pragma pack(pop)
```

- `Dacl` 偏移 `20`，`AclSize=24`，`AceSize=16`
- 掩码：`FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE`（add-only，禁止读/写/删已有文件）
- SID 用 `S-1-0-0`（Everyone）——配合 B3 的真实 SID 子目录使用

### B3 获取真实客户端 SID（驱动）

**问题**：原先按 `RequestorSessionId` 拼占位 SID（`S-SESSION-<id>`），无法定位到真实用户，`$Recycle.Bin\<SID>` 还原归属错误。

**修复**：
- `RbfGetRequestorSid` 改用 `SeCaptureSubjectContext` + `SeQueryInformationToken` + `TokenUser` 读取真实客户端 SID
- 失败时回退占位 SID `\S-SESSION-<id>`，用户态 `_real_sid` 再解析
- `SeReleaseSubjectContext` 正确释放

### B4 删除过滤逻辑与通知超时（驱动）

**问题**：
1. 原 `RbfPreSetInfo` 过滤 `sessionId != 0` 才拦截——SMB2 删除实际在 session 0 上下文执行，导致远程删除全部漏拦；
2. `FltSendMessage` 无超时，用户态服务无响应时驱动线程可能长期挂起。

**修复**：
- 移除 `sessionId != 0` 过滤，以路径是否命中受保护前缀为准
- `FltSendMessage` 增加 30 秒相对超时：

```c
LARGE_INTEGER timeout;
timeout.QuadPart = -30LL * 10 * 1000 * 1000;   // 30s
status = FltSendMessage(G.Filter, &port, &node->Notification,
                        sizeof(RBF_NOTIFICATION), NULL, NULL, &timeout);
```

### B5 回收站 `$I` 元数据格式与 SID 规范化（服务）

**问题**：
1. `$I` v1 格式字段类型错误：`PathLen` 应为 `I`(4B) 而非 `Q`(8B)，导致元数据错位、回收站无法还原；
2. SID 带前导反斜杠（`\S-1-5-...`），与 `$Recycle.Bin\<SID>` 目录名不符。

**修复**（`recyclebin_lib.py`）：
- `_build_i_file`：`struct.pack("<IQQI", 1, file_size or 0, ft, len(orig_path))`（`<IQQI` = Version + FileSize + FILETIME + PathLen）
- `add_item`：SID 统一 `lstrip("\\")`
- `_real_sid`：去掉前导反斜杠，兼容 `S-SESSION-*` 占位回退

### B6 多卷磁盘水位检查（服务）

**问题**：`enforce_disk_watermark` 只检查 StoreRoot 所在卷，其余受保护卷的暂存区超限后不清理。

**修复**：
- 新增 `_protected_volumes()`：枚举所有受保护卷 + StoreRoot 卷
- 水位清理改为逐卷循环，每卷最多清理 200 条，避免单卷饿死

```python
def _protected_volumes():
    volumes = {os.path.splitdrive(os.path.normpath(root))[0] for root in ProtectedPaths}
    volumes.add(os.path.splitdrive(os.path.normpath(StoreRoot))[0])
    return volumes
```

### B7 部署脚本路径兼容 + REST token 热更新（服务/部署）

**问题**：
1. `deploy.ps1` 硬编码驱动输出路径，Release 构建目录不同时拷贝失败；
2. `_auth` 启动时缓存 token，`/config/reload` 后仍用旧 token 鉴权。

**修复**：
- `deploy.ps1`：`$sys` 优先取 `driver\rbminiflt.sys`，备选 `x64\Release\` 路径
- `rb_service.py`：`_auth` 每次动态读取 `config.CONFIG.get("RestApiToken","")`，配置重载即时生效

---

## 三、验证结果

| 项目 | 结果 |
|---|---|
| `driver\build.cmd Release` | 通过，零错误零警告，`rbminiflt.sys` 15360B |
| Python `py_compile`（3 个模块） | 通过 |
| `$I` 格式验证 | `version=1, filesize=12345, pathlen=18, path` 正确，60 字节 |
| SID 规范化 | 前导 `\` 去除正常 |
| 多卷水位 | `C:\ D:\ E:\` 各卷独立检查 |

---

## 四、遗留事项

- 驱动需启用测试签名（`bcdedit /set testsigning on`）后方可加载
- 生产部署前需做端到端验证：SMB 删除 → 进 `$Recycle.Bin` → 标准回收站还原
- 跨卷移动仍未实现（当前要求 StoreRoot 与共享同卷）
