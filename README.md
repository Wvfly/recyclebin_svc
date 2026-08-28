# Windows 文件共享回收站 (RecycleBin for SMB)

拦截通过 SMB 远程访问共享的用户的删除操作，将文件重定向到回收站暂存区，
而非真正删除，从而实现"远程删除 → 进回收站 → 可还原/可审计"。

## 架构

```
远程客户端 (\\server\share)
   Explorer 删除 → SMB2 → srv2.sys → 本地 IRP
                              │
                              ▼
        rbminiflt.sys  (Mini-Filter, Altitude 370030)
          PreSetInfo(DeleteFile=1):
            1. 取 RequestorSessionId，仅拦远程会话
            2. 路径命中受保护共享?
            3. rename 到 \RBStore\<VolGuid>\<Sid>\...
            4. 成功 → COMPLETE(STATUS_SUCCESS)  // 用户以为删了
            5. 失败 → 放行真删 + 事件日志
            6. 异步入队通知 → 通信端口
                              │
                              ▼
        rb_service.exe (SYSTEM 服务)
           - 收通知写 SQLite 元数据
           - 周期落地 $Recycle.Bin ($R / $I 条目)
           - 配额 / 过期清理 / 审计
           - 可选 REST 管理 API
```

## 目录

```
recyclebin_svc/
  driver/           内核 Mini-Filter 驱动 (WDK)
    rbminiflt.c        驱动主体
    rbminiflt.h        共享结构/常量
    rbminiflt.inf      安装信息(含 Altitude)
    rbminiflt.vcxproj  驱动工程
  service/          用户态 SYSTEM 服务 (Python)
    rb_service.py      服务主程序(端口读线程 + 整理线程)
    recyclebin_lib.py  回收站落地 / $I 元数据 / 配额
    config.py          配置(注册表或 ini)
    requirements.txt
  docs/
    design.md          设计说明
```

## 构建

### 驱动 (需要 WDK 10 + VS2022)
用 VS 打开 `driver/rbminiflt.vcxproj`，平台选 x64，
Target = Windows10, Driver Type = WDM (纯 Mini-Filter)。
输出 `rbminiflt.sys`。

### 服务
```
pip install -r service/requirements.txt
python service/rb_service.py install   # 注册为 SYSTEM 服务
python service/rb_service.py start
```

## 配置 (注册表 HKLM\SOFTWARE\RecycleBin)
- `ProtectedPaths` (REG_MULTI_SZ): 受保护共享根, 如 `D:\Share`
- `StoreRoot` (REG_SZ): 暂存区根, 默认 `C:\RBStore`
- `QuotaMB` (REG_DWORD): 每用户配额, 默认 5120
- `RetentionDays` (REG_DWORD): 保留天数, 默认 30
- `EnableRestApi` (REG_DWORD): 1 开启管理 API
- `RestApiPort` (REG_DWORD): 默认 8800
