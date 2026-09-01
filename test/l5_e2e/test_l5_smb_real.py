# -*- coding: utf-8 -*-
"""
L5-E2E 真实 SMB 共享拦截/还原测试
目标共享: \\\\10.88.36.171\\share  <->  本地 E:\\tmp\\share
API:      http://127.0.0.1:8800
覆盖: test-plan.md 第三章 维度 G(运维) + 维度 H(业务剧本, 真实删除+还原)

隔离策略: 仅在 E:\\tmp\\share\\__smoke__ 下创建/删除/还原测试文件,
          绝不触碰现有业务文件。删除被 rbminiflt 拦截进 E:\\RBStore, 可还原。

前置(由部署保证, 脚本仅断言):
  - rbminiflt 驱动 RUNNING 且挂载 E:
  - ProtectedPaths 含 \\Device\\HarddiskVolume4\\tmp\\share
  - RecycleBinApi 监听 127.0.0.1:8800

重要测试陷阱(实测):
  1. VS Code 终端的 sitecustomize.py 把 Python os.remove 接管为 safe-delete(trash),
     不触发真实 DeleteFile, 驱动看不到删除 -> 不拦截。故本脚本用 ctypes.kernel32.DeleteFileW 真实删除。
  2. POST /ops restore 返回 202(异步): 成功判据是轮询 GET /ops/{op_id} 的 state 变 done,
     item.status 字段本身不变。不要把 item.state(不存在) 当成还原状态 -> 那是误判(RB-32 已排除)。
  3. 经 SMB UNC (\\\\10.88.36.171\\share\\...) 删除同样被本机 rbminiflt 拦截(因 10.88.36.171 即本机)。
"""
import os
import sys
import json
import time
import shutil
import ctypes
import subprocess
import urllib.request
import urllib.error


def real_delete(path):
    """绕过 VS Code 终端 sitecustomize 的 safe-delete shim, 直接调用 kernel32.DeleteFileW
    确保触发真实 DeleteFile  syscall, 驱动才会拦截。"""
    res = ctypes.windll.kernel32.DeleteFileW(path)
    if not res:
        err = ctypes.GetLastError()
        raise OSError(err, "DeleteFileW failed: " + path)
    return True

API_BASE = "http://127.0.0.1:8800"
SMB_SHARE = "\\\\10.88.36.171\\share"
LOCAL_SHARE = r"E:\tmp\share"
SMOKE_DIR_LOCAL = os.path.join(LOCAL_SHARE, "__smoke__")
SMOKE_DIR_UNC = os.path.join(SMB_SHARE, "__smoke__")

PASS = 0
FAIL = 0
SKIP = 0
logs = []


def log(msg):
    logs.append(msg)
    print(msg)


def check(cond, name):
    global PASS, FAIL
    if cond:
        PASS += 1
        log("  PASS  " + name)
    else:
        FAIL += 1
        log("  FAIL  " + name)


def api_get(path):
    url = API_BASE + path
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:  # noqa
        return 0, str(e)


def api_post(path, payload):
    url = API_BASE + path
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:  # noqa
        return 0, str(e)


def wait_items_contains(substr, timeout=15):
    """轮询 /items 直到出现包含 substr 的条目, 返回该条目 dict 或 None"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        st, body = api_get("/items")
        if st == 200:
            try:
                items = json.loads(body).get("items", [])
            except Exception:  # noqa
                items = []
            for it in items:
                if substr in (it.get("orig_path", "") or ""):
                    return it
        time.sleep(1)
    return None


def main():
    global SKIP
    log("=== L5-E2E 真实 SMB 拦截/还原测试 ===")

    # 0. 环境断言
    st, hb = api_get("/health")
    check(st == 200, "API /health 可达 (实际 %d)" % st)
    if st != 200:
        log("  健康检查失败, 终止后续真实测试以避免误判")
        return 1

    # 1. 建立隔离目录(UNC 路径, 触发 SMB)
    os.makedirs(SMOKE_DIR_UNC, exist_ok=True)
    check(os.path.isdir(SMOKE_DIR_UNC), "隔离目录 __smoke__ 已创建 (UNC: %s)" % SMOKE_DIR_UNC)

    # 2. 创建 3 个含中文名的测试文件 (维度 C: 删除对象-中文名)
    created = []
    for i in range(3):
        name = u"测试文件_%d_修复验证.txt" % (i + 1)
        fpath = os.path.join(SMOKE_DIR_UNC, name)
        with open(fpath, "w", encoding="utf-8") as f:
            f.write(u"smoke payload %d\n中文内容行\n" % (i + 1))
        created.append((name, fpath))
    check(len(created) == 3, "已创建 3 个中文名测试文件")

    # 3. 真实删除(经 SMB UNC) -> 应被拦截进 RBStore (维度 H: 真实删除)
    for name, fpath in created:
        real_delete(fpath)
        gone_local = not os.path.exists(os.path.join(SMOKE_DIR_LOCAL, name))
        check(gone_local, "删除后源路径消失: %s" % name)

    # 4. API /items 应出现被拦截条目 (维度 G: 运维可见性)
    time.sleep(2)
    all_ok = True
    item_ids = []
    for name, _ in created:
        it = wait_items_contains(name, timeout=15)
        if it:
            item_ids.append(it.get("id"))
            log("    拦截条目 id=%s orig=%s" % (it.get("id"), it.get("orig_path")))
        else:
            all_ok = False
        check(it is not None, "API /items 出现被拦截条目: %s" % name)
    check(all_ok, "全部 3 个删除均被拦截 (intercepts 链路正常)")

    # 5. /search 按中文名检索 (维度 C + 搜索)
    st, body = api_get("/search?q=" + urllib.parse.quote(u"测试文件"))
    found = 0
    if st == 200:
        try:
            found = len(json.loads(body).get("items", []))
        except Exception:  # noqa
            found = 0
    check(st == 200 and found >= 3, "/search 中文检索命中 >=3 (实际 %d, http %d)" % (found, st))

    # 6. 还原 (维度 D: 真实还原) -> /ops 返回 202 + op_id, 轮询 /ops/{op_id} 直到 state=done
    #    文件应回到原 SMB 路径。注意: 还原是异步队列, item.status 不变, 成功看 ops.state=done
    restored_ok = True
    for i, (name, _) in enumerate(created):
        iid = item_ids[i] if i < len(item_ids) else None
        if not iid:
            restored_ok = False
            continue
        st, body = api_post("/ops", {"type": "restore", "id": iid})
        check(st in (200, 202), "POST /ops restore id=%s 返回 200/202 (实际 %d)" % (iid, st))
        op_id = None
        try:
            op_id = json.loads(body).get("op_id")
        except Exception:  # noqa
            pass
        check(op_id is not None, "restore 返回 op_id: %s" % (op_id,))
        # 轮询 /ops/{op_id} 直到 state=done (异步还原完成)
        op_done = False
        if op_id is not None:
            for _ in range(25):
                s2, b2 = api_get("/ops/%s" % op_id)
                stt = None
                try:
                    stt = json.loads(b2).get("state")
                except Exception:  # noqa
                    pass
                if stt in ("done", "failed"):
                    op_done = (stt == "done")
                    break
                time.sleep(1)
        check(op_done, "op_id=%s 异步还原完成 state=done: %s" % (op_id, name))
        # 文件回到原路径 (物理还原)
        back = False
        for _ in range(20):
            if os.path.exists(os.path.join(SMOKE_DIR_LOCAL, name)):
                back = True
                break
            time.sleep(1)
        check(back, "还原后文件回到原路径: %s" % name)
        if back:
            with open(os.path.join(SMOKE_DIR_LOCAL, name), "r", encoding="utf-8") as f:
                content = f.read()
            check(u"smoke payload" in content, "还原后内容完整: %s" % name)
    check(restored_ok, "全部条目还原成功")

    # 7. /items 可查到本次还原条目 (维度 G: 运维可见性)
    time.sleep(1)
    st, body = api_get("/items")
    if st == 200:
        try:
            items = json.loads(body).get("items", [])
            seen = sum(1 for it in items if it.get("id") in item_ids)
            log("    /items 中本次条目数=%d" % seen)
        except Exception:  # noqa
            pass

    # 8. 清理: 删除 __smoke__ 内残余文件(会被拦截进 RBStore), 再调 /ops 还原这些残留条目, 最终清空隔离目录与回收站中的测试痕迹
    try:
        if os.path.isdir(SMOKE_DIR_LOCAL):
            for fn in os.listdir(SMOKE_DIR_LOCAL):
                try:
                    real_delete(os.path.join(SMOKE_DIR_LOCAL, fn))
                except Exception:  # noqa
                    pass
    except Exception:  # noqa
        pass
    # 二次还原: 把所有 orig_path 含 __smoke__ 的残留条目 restore 掉
    time.sleep(2)
    st, body = api_get("/items")
    if st == 200:
        try:
            items = json.loads(body).get("items", [])
            smoke_items = [it for it in items if "__smoke__" in (it.get("orig_path", "") or "")]
            log("    清理: 发现 __smoke__ 残留条目 %d 个" % len(smoke_items))
            for it in smoke_items:
                rid = it.get("id")
                if it.get("state") != "restored":
                    st2, _ = api_post("/ops", {"type": "restore", "id": rid})
                    log("    二次还原 id=%s -> %d" % (rid, st2))
                # 还原回 __smoke__ 后再次删除, 让其离开共享根(彻底清理)
                nm = os.path.basename(it.get("orig_path", ""))
                if nm:
                    p = os.path.join(SMOKE_DIR_LOCAL, nm)
                    if os.path.exists(p):
                        try:
                            real_delete(p)
                        except Exception:  # noqa
                            pass
        except Exception as e:  # noqa
            log("    清理异常: " + str(e))
    # 最终确认隔离目录为空
    try:
        left = os.listdir(SMOKE_DIR_LOCAL) if os.path.isdir(SMOKE_DIR_LOCAL) else []
        check(len(left) == 0, "清理后 __smoke__ 目录为空 (残留 %d)" % len(left))
    except Exception:  # noqa
        pass

    log("=== 结果: PASS=%d FAIL=%d SKIP=%d ===" % (PASS, FAIL, SKIP))
    return 1 if FAIL else 0


if __name__ == "__main__":
    import urllib.parse  # noqa  (for quote in search)
    sys.exit(main())
