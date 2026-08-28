# 设计说明 (Design)

## 1. 为什么用 Mini-Filter 而不是别的

| 方案 | 问题 |
|---|---|
| Explorer / Shell 钩子 | 远程走 SMB, 不过本地 Explorer |
| 文件服务 (FSD) 替换 | 风险极高, 不可维护 |
| 用户态文件系统 (ProjFS) | 重定向复杂, 不适合拦截删除 |
| **Mini-Filter 驱动** | **在删除 I/O 路径上拿到原始路径/会话, 可重定向, 标准做法** |

## 2. 核心范式: 重定向删除 (Rename-to-Store)

旧范式 "拦截 → 问用户态是否删除" 无法做回收站:
- ALLOW → 文件没了, 来不及移
- DENY  → 删除取消, 用户态去移会再次触发本驱动回调 → 递归/环路

新范式: **在 Pre 回调里把文件 rename 到暂存区, 然后把 DELETE 请求直接 COMPLETE 为成功**。
用户以为删了, 文件其实在 `C:\RBStore\<Sid>\<原路径>`。
- 移动在内核态完成, 零用户态往返, 不阻塞 I/O
- 暂存区在受保护前缀之外, 回调开头 `RbfIsProtected` 放行, 无递归
- 失败 fail-open: 放行真实删除 + 事件日志 (绝不 fail-closed 让用户删不掉)

## 3. 远程判定

`FltGetRequestorSessionId`:
- SessionId == 0 → 本地系统/无会话 → 放行
- SessionId != 0 → 视为远程 (SMB 客户端会话) → 进入拦截流程

> 注意: 本地 RDP 登录用户的会话也非零, 会被拦。如需排除, 可在
> `RbfIsProtected` 之前增加 "排除交互会话" 逻辑 (查 WTS 会话类型)。

## 4. 通信模型 (异步, 不阻塞)

驱动维护内核队列 (`KSPIN_LOCK` + `LIST_ENTRY`), Pre 回调只 `ExInterlockedInsertTailList`
+ `KeSetEvent`, **绝不 `FltSendMessage` 同步等**。用户态 `FilterGetMessage` 阻塞读取,
取到通知后写 SQLite 元数据。驱动崩溃/服务断开最坏只丢元数据, 不影响删除。

## 5. 用户态职责

- `rb_service.py`: 端口读线程 + 维护线程
- `recyclebin_lib.py`:
  - 把 `\RBStore` 文件落地为标准 `$Recycle.Bin\<SID>\$Rxxxx` + `$Ixxxx`
  - 用 WTS 把内核传来的 `S-SESSION-<id>` 解析为真实用户 SID
  - 配额 (`QuotaMB`) 与过期 (`RetentionDays`) 清理
  - SQLite 元数据: 谁、什么文件、何时、从哪台机器删的

## 6. 已知限制 / 后续

- 跨卷移动: 当前 StoreRoot 需与共享同卷 (rename 只能同卷)。跨卷需改为
  "拷贝 + 删源", 复杂度上升, 暂未实现。部署时把 StoreRoot 放到共享所在盘。
- 符号链接/硬链接未特殊处理。
- 完整 SID 解析在内核态代价高, 改为用户态按 SessionId 解析。
- 还原 UI: 当前文件已进相应用户 `$Recycle.Bin`, 该用户可用标准回收站还原;
  管理 API 提供列表, 完整还原接口可后续扩展。
