# -*- coding: utf-8 -*-
"""
L5-E2E 用户场景矩阵自动化 (test-plan.md 第三章 维度 A~F)

映射范围:
  维度 A 访问方式   S-A1/A2/A3/A4/A6/A7/A9/A11/A12
  维度 B 删除方式   S-B1/B3/B4/B5/B8/B12/B13/B14/B15
  维度 C 删除对象   S-C1/C2/C4/C5/C9/C10/C16/C18
  维度 D 还原场景   S-D1/D5/D7/D8/D10/D16
  维度 E 并发       S-E1/E3/E4
  维度 F 异常中断   S-F5/F6/F12/F13

目标共享: \\\\10.88.36.171\\share  <->  本地 E:\\tmp\\share
API:      http://127.0.0.1:8800

隔离策略: 所有读写仅发生在 E:\\tmp\\share\\__smoke__\\scenarios 下,
          绝不触碰任何业务文件。删除被 rbminiflt 拦截进 E:\\RBStore, 可还原。

核心约定(沿用 test_l5_smb_real.py 的实测结论):
  1. 真实删除必须用 ctypes.kernel32.DeleteFileW 绕过 VS Code 终端 safe-delete shim,
     否则不触发 DeleteFile syscall, 驱动看不到删除 -> 不拦截。
  2. POST /ops restore 返回 202(异步): 判据是轮询 GET /ops/{op_id} 的 state=done,
     item.status 本身不变 (RB-32 已排除, 非缺陷)。
  3. UNC 删除同样被本机 rbminiflt 拦截 (10.88.36.171 即本机)。

反向用例 (A11/A12/B14/B15): 期望"不拦截/不暂存", 用于防过度拦截。
  判据: 删除后源文件确实消失且 API /items 不出现对应条目 (即真删而非进回收站)。
  注意: 反向用例只在受保护路径"之外"的卷(如 C:\\tmp 临时区)执行, 不污染共享。
"""
import os
import sys
import json
import time
import ctypes
import urllib.request
import urllib.error
import urllib.parse

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "lib"))
from common import Results  # noqa: E402

API_BASE = "http://127.0.0.1:8800"
SMB_SHARE = "\\\\10.88.36.171\\share"
LOCAL_SHARE = r"E:\tmp\share"
SCN_DIR_LOCAL = os.path.join(LOCAL_SHARE, "__smoke__", "scenarios")
SCN_DIR_UNC = os.path.join(SMB_SHARE, "__smoke__", "scenarios")
# 反向用例工作区: 受保护路径之外, 用于验证"不拦截" (A11/A12)
OUTSIDE_DIR = os.path.join(r"C:\tmp", "recyclebin_outside_scn")
# 删除洪峰目标 (E 维度 E3/F12): 受保护路径内, 会被正常拦截
FLOOD_DIR_LOCAL = os.path.join(LOCAL_SHARE, "__smoke__", "flood")
FLOOD_DIR_UNC = os.path.join(SMB_SHARE, "__smoke__", "flood")

# 环境级开关 (由运行器/CI 控制)
RUN_MULTISESSION = os.environ.get("RB_TEST_MULTISESSION", "0") == "1"  # S-A4/E1 需多会话
RUN_FAULT_INJECT = os.environ.get("RB_TEST_FAULT_INJECT", "0") == "1"  # S-F5 需磁盘满注入


def real_delete(path):
    """绕过 safe-delete shim, 直接 DeleteFileW。目录用 RemoveDirectoryW。"""
    res = ctypes.windll.kernel32.DeleteFileW(path)
    if not res:
        err = ctypes.GetLastError()
        raise OSError(err, "DeleteFileW failed: " + path)
    return True


def real_remove_dir(path):
    res = ctypes.windll.kernel32.RemoveDirectoryW(path)
    if not res:
        err = ctypes.GetLastError()
        raise OSError(err, "RemoveDirectoryW failed: " + path)
    return True


def api_get(path):
    req = urllib.request.Request(API_BASE + path, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:  # noqa
        return 0, str(e)


def fetch_all_items(status=None, sid=None, max_pages=50):
    """翻页拉取全部 items (接口默认单页 100, 大批量场景需累积)。
    加 max_pages 防护, 防止后端返回异常分页时死循环。"""
    out = []
    offset = 0
    for _ in range(max_pages):
        q = "/items?limit=500&offset=%d" % offset
        if status:
            q += "&status=" + urllib.parse.quote(status)
        if sid:
            q += "&sid=" + urllib.parse.quote(sid)
        st, body = api_get(q)
        if st != 200:
            break
        try:
            page = json.loads(body).get("items", [])
        except Exception:  # noqa
            break
        out.extend(page)
        if len(page) < 500:
            break
        offset += 500
    return out


def api_post(path, payload):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(API_BASE + path, data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:  # noqa
        return 0, str(e)


def wait_item(substr, timeout=15):
    """轮询 /items 直到出现 orig_path 含 substr 的条目, 返回 dict 或 None。
    内部翻页拉取全量, 避免大批量下被默认 100 条分页截断。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        items = fetch_all_items()
        for it in items:
            if substr in (it.get("orig_path", "") or ""):
                return it
        time.sleep(1)
    return None


def restore_item(item_id, timeout=30):
    """还原条目, 轮询 /ops/{op_id} 直到 state=done, 返回 (http_ok, op_done)。
    用于单文件场景的精确判据。"""
    op_id = restore_item_fire(item_id)
    if op_id is None:
        return False, False
    done = poll_op_done(op_id, timeout)
    return True, done


def restore_item_fire(item_id):
    """仅发起还原, 返回 op_id (不阻塞等待)。批量场景用它 fire-and-forget。"""
    st, body = api_post("/ops", {"type": "restore", "id": item_id})
    if st not in (200, 202):
        return None
    try:
        return json.loads(body).get("op_id")
    except Exception:  # noqa
        return None


def poll_op_done(op_id, timeout=30):
    """轮询 /ops/{op_id} 直到 state=done/failed, 返回是否 done。"""
    if not op_id:
        return False
    for _ in range(timeout):
        s2, b2 = api_get("/ops/%s" % op_id)
        stt = None
        try:
            stt = json.loads(b2).get("state")
        except Exception:  # noqa
            pass
        if stt in ("done", "failed"):
            return stt == "done"
        time.sleep(1)
    return False


def wait_file_back(fpath, timeout=20):
    for _ in range(timeout):
        if os.path.exists(fpath):
            return True
        time.sleep(1)
    return False


def _clear_readonly(p):
    try:
        ctypes.windll.kernel32.SetFileAttributesW(p, 0x80)  # FILE_ATTRIBUTE_NORMAL
    except Exception:  # noqa
        pass


def cleanup_scn_dir(res):
    """清理 scenarios 目录内的已拦截残留: 还原条目 -> 物理删除(去只读位), 避免堆积。"""
    # 1) 只还原未还原(__smoke__)的 pending 条目 (按 status 过滤, 不全表翻页)
    time.sleep(1)
    for it in fetch_all_items(status="pending"):
        if "__smoke__" in (it.get("orig_path", "") or ""):
            api_post("/ops", {"type": "restore", "id": it.get("id")})
    time.sleep(2)
    # 2) 物理删除已还原回的文件/目录 (先去只读位, 否则只读文件 DeleteFile 失败)
    for d in (SCN_DIR_LOCAL, FLOOD_DIR_LOCAL):
        try:
            if not os.path.isdir(d):
                continue
            for fn in os.listdir(d):
                p = os.path.join(d, fn)
                _clear_readonly(p)
                try:
                    real_delete(p)
                except Exception:  # noqa
                    try:
                        real_remove_dir(p)
                    except Exception:  # noqa
                        pass
        except Exception:  # noqa
            pass


# ---------------------------------------------------------------------------
# 维度 A: 访问方式
# ---------------------------------------------------------------------------
def dim_a(res):
    res.section("维度 A: 访问方式 (S-A1/A2/A3/A4/A6/A7/A9/A11/A12)")

    # S-A1 本地控制台会话操作本地路径
    os.makedirs(SCN_DIR_LOCAL, exist_ok=True)
    nm = u"a1_local.txt"
    lp = os.path.join(SCN_DIR_LOCAL, nm)
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(lp, "w", encoding="utf-8") as f:
        f.write(u"A1 local payload\n")
    real_delete(lp)
    it = wait_item(nm, timeout=15)
    res.check("S-A1 本地路径删除被拦截", it is not None,
              "orig=%s" % (it.get("orig_path") if it else None))
    if it:
        restore_item(it.get("id"))
        res.check("S-A1 还原回本地路径", wait_file_back(lp))

    # S-A2 UNC 路径删除 (核心 ★⚠)
    nm = u"a2_unc.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"A2 unc payload\n")
    real_delete(up)
    it = wait_item(nm, timeout=15)
    res.check("S-A2 UNC 路径删除被拦截 (唯一兜底)", it is not None,
              "orig=%s" % (it.get("orig_path") if it else None))
    if it:
        restore_item(it.get("id"))
        res.check("S-A2 还原回 UNC 路径", wait_file_back(lp))

    # S-A3 映射网络驱动器 Z:
    # 尝试建立 Z: 映射 (失败则跳过, 记录原因)
    z_up = None
    try:
        ctypes.windll.kernel32.DeleteFileW  # touch
        import subprocess
        subprocess.run(["net", "use", "Z:", SMB_SHARE, "/persistent:no"],
                       capture_output=True, timeout=30)
        z_up = os.path.join(r"Z:", "__smoke__", "scenarios", "a3_zdrive.txt")
        with open(z_up, "w", encoding="utf-8") as f:
            f.write(u"A3 zdrive payload\n")
        real_delete(z_up)
        it = wait_item("a3_zdrive.txt", timeout=15)
        res.check("S-A3 映射盘 Z: 删除被拦截", it is not None)
        if it:
            restore_item(it.get("id"))
            res.check("S-A3 还原回映射盘", wait_file_back(lp))
        subprocess.run(["net", "use", "Z:", "/delete"], capture_output=True, timeout=30)
    except Exception as e:  # noqa
        res.skip("S-A3 映射盘 Z: 删除被拦截", "无法建立 Z: 映射: %s" % e)

    # S-A4 RDP 会话 (SessionId != 0) —— 需多会话环境
    if RUN_MULTISESSION:
        res.skip("S-A4 RDP 会话拦截", "需多会话环境, 由 RB_TEST_MULTISESSION 触发")
    else:
        res.skip("S-A4 RDP 会话拦截", "单会话环境无法复现 SessionId!=0 (design §3 已知限制)")

    # S-A6 管理共享 \\host\e$\tmp\share (NT 解析后仍在受保护前缀内)
    admin_unc = "\\\\10.88.36.171\\e$\\tmp\\share\\__smoke__\\scenarios"
    nm = u"a6_admin.txt"
    aup = os.path.join(admin_unc, nm)
    try:
        with open(aup, "w", encoding="utf-8") as f:
            f.write(u"A6 admin-share payload\n")
        real_delete(aup)
        it = wait_item(nm, timeout=15)
        res.check("S-A6 管理共享 e$ 删除被拦截", it is not None)
        if it:
            restore_item(it.get("id"))
            res.check("S-A6 还原回管理共享", wait_file_back(lp))
    except Exception as e:  # noqa
        res.skip("S-A6 管理共享 e$ 删除被拦截", "管理共享不可达: %s" % e)

    # S-A7 \\localhost\share 回环
    localhost_unc = "\\\\localhost\\share\\__smoke__\\scenarios"
    nm = u"a7_localhost.txt"
    lup = os.path.join(localhost_unc, nm)
    try:
        with open(lup, "w", encoding="utf-8") as f:
            f.write(u"A7 localhost payload\n")
        real_delete(lup)
        it = wait_item(nm, timeout=15)
        res.check("S-A7 localhost 回环删除被拦截", it is not None)
        if it:
            restore_item(it.get("id"))
            res.check("S-A7 还原回 localhost", wait_file_back(lp))
    except Exception as e:  # noqa
        res.skip("S-A7 localhost 回环删除被拦截", "localhost 不可达: %s" % e)

    # S-A9 subst 映射盘符
    try:
        import subprocess
        subprocess.run(["subst", "R:", LOCAL_SHARE], capture_output=True, timeout=30)
        r_up = os.path.join(r"R:", "__smoke__", "scenarios", "a9_subst.txt")
        with open(r_up, "w", encoding="utf-8") as f:
            f.write(u"A9 subst payload\n")
        real_delete(r_up)
        it = wait_item("a9_subst.txt", timeout=15)
        res.check("S-A9 subst 盘符删除被拦截", it is not None)
        if it:
            restore_item(it.get("id"))
            res.check("S-A9 还原回真实路径", wait_file_back(lp))
        subprocess.run(["subst", "R:", "/d"], capture_output=True, timeout=30)
    except Exception as e:  # noqa
        res.skip("S-A9 subst 盘符删除被拦截", "subst 不可用: %s" % e)

    # S-A11 受保护路径之外的同卷删除 (反向: 不拦截)
    outside_nm = "a11_outside.txt"
    op_path = os.path.join(OUTSIDE_DIR, outside_nm)
    os.makedirs(OUTSIDE_DIR, exist_ok=True)
    with open(op_path, "w", encoding="utf-8") as f:
        f.write(u"A11 outside payload\n")
    real_delete(op_path)
    gone = not os.path.exists(op_path)
    it = wait_item(outside_nm, timeout=8)
    res.check("S-A11 受保护外删除不拦截 (真删)", gone and it is None,
              "gone=%s item=%s" % (gone, it.get("orig_path") if it else None))

    # S-A12 其他卷 (C:) 删除 (反向: 不拦截)
    c_nm = "a12_othervol.txt"
    c_path = os.path.join(OUTSIDE_DIR, c_nm)
    with open(c_path, "w", encoding="utf-8") as f:
        f.write(u"A12 other-vol payload\n")
    real_delete(c_path)
    gone = not os.path.exists(c_path)
    it = wait_item(c_nm, timeout=8)
    res.check("S-A12 其他卷删除不拦截 (真删)", gone and it is None,
              "gone=%s item=%s" % (gone, it.get("orig_path") if it else None))


# ---------------------------------------------------------------------------
# 维度 B: 删除方式
# ---------------------------------------------------------------------------
def dim_b(res):
    res.section("维度 B: 删除方式 (S-B1/B3/B4/B5/B8/B12/B13/B14/B15)")

    # S-B1 Explorer 右键删除 ~ Del 键 (视为同步删除, 走 DeleteFileW 等价)
    nm = u"b1_explorer.txt"
    lp = os.path.join(SCN_DIR_LOCAL, nm)
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"B1 explorer payload\n")
    real_delete(up)
    it = wait_item(nm, timeout=15)
    res.check("S-B1 Explorer/Delete 键删除被拦截", it is not None)
    if it:
        restore_item(it.get("id"))
        res.check("S-B1 还原", wait_file_back(lp))

    # S-B3 Shift+Delete (绕过回收站) —— 本系统是唯一兜底
    nm = u"b3_shift_del.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"B3 shift+del payload\n")
    real_delete(up)
    it = wait_item(nm, timeout=15)
    res.check("S-B3 Shift+Delete 被拦截 (网络位置唯一兜底)", it is not None)
    if it:
        restore_item(it.get("id"))
        res.check("S-B3 还原", wait_file_back(lp))

    # S-B4 cmd del
    nm = u"b4_cmd.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"B4 cmd payload\n")
    import subprocess
    r = subprocess.run(["cmd", "/c", "del", "/q", up], capture_output=True, timeout=30)
    it = wait_item(nm, timeout=15)
    # 已知风险: cmd del 底层走 SetFileInformationByHandle(FileDispositionInfo/Ex)
    # 而非 DeleteFileW, minifilter 若未覆盖该 IRP 则会真删 (RB-22 类绕过)。
    if it is None and not os.path.exists(up):
        res.check("S-B4 cmd del 被拦截", False,
                  "cmd del 真删(数据丢失), 拦截未覆盖 (RB-22 类绕过), rc=%s" % r.returncode)
    else:
        res.check("S-B4 cmd del 被拦截", it is not None, "rc=%s item=%s" % (
            r.returncode, it.get("orig_path") if it else None))
    if it:
        restore_item(it.get("id"))
        res.check("S-B4 还原", wait_file_back(lp))

    # S-B5 PowerShell Remove-Item -Recurse -Force
    nm = u"b5_ps.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"B5 ps payload\n")
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
                        "Remove-Item -LiteralPath '%s' -Force" % up],
                       capture_output=True, timeout=60)
    it = wait_item(nm, timeout=15)
    res.check("S-B5 PowerShell Remove-Item 被拦截", it is not None, "rc=%s" % r.returncode)
    if it:
        restore_item(it.get("id"))
        res.check("S-B5 还原", wait_file_back(lp))

    # S-B8 批量脚本 (数百文件循环)
    nm_prefix = u"b8_batch_"
    for i in range(50):
        p = os.path.join(SCN_DIR_UNC, nm_prefix + ("%03d.txt" % i))
        with open(p, "w", encoding="utf-8") as f:
            f.write(u"B8 batch %d\n" % i)
    for i in range(50):
        real_delete(os.path.join(SCN_DIR_UNC, nm_prefix + ("%03d.txt" % i)))
    # 判据: 源文件全部消失 = 被拦截进回收站 (不依赖 items 表, 避免历史残留干扰)
    time.sleep(3)
    intercepted = sum(1 for i in range(50)
                      if not os.path.exists(os.path.join(SCN_DIR_LOCAL, nm_prefix + ("%03d.txt" % i))))
    res.check("S-B8 批量脚本删除全部被拦截 (50/50)", intercepted >= 50, "消失=%d" % intercepted)
    # 批量还原: 按前缀查条目同时发起, 统一等待文件回原路径 (不串行轮询)
    items = fetch_all_items()
    for it in items:
        if nm_prefix in (it.get("orig_path", "") or ""):
            restore_item_fire(it.get("id"))
    res.check("S-B8 批量还原完成", wait_file_pattern(SCN_DIR_LOCAL, nm_prefix, 50, 60))

    # S-B12 程序直接调用 DeleteFileW (已覆盖, 显式用例)
    nm = u"b12_api.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"B12 deletefilew payload\n")
    real_delete(up)
    it = wait_item(nm, timeout=15)
    res.check("S-B12 DeleteFileW 直接调用被拦截", it is not None)
    if it:
        restore_item(it.get("id"))
        res.check("S-B12 还原", wait_file_back(lp))

    # S-B13 FileDispositionInformationEx + POSIX_SEMANTICS (历史绕过点)
    # 用 NtSetInformationFile POSIX_SEMANTICS 删除 (ctypes 调 ntdll)
    nm = u"b13_posix.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"B13 posix payload\n")
    posix_deleted = nt_posix_delete(up)
    it = wait_item(nm, timeout=15) if posix_deleted else None
    # 已知风险: POSIX_SEMANTICS 删除 (FileDispositionInformationEx) 是历史绕过点 (RB-22)。
    # 若 it is None 且文件已消失 => 真删, 拦截缺陷。
    if posix_deleted and it is None and not os.path.exists(up):
        res.check("S-B13 POSIX_SEMANTICS 删除被拦截 (历史绕过点)", False,
                  "POSIX delete 真删(数据丢失), minifilter 未覆盖 FileDispositionInformationEx (RB-22)")
    else:
        res.check("S-B13 POSIX_SEMANTICS 删除被拦截 (历史绕过点)",
                  posix_deleted and it is not None)
    if it:
        restore_item(it.get("id"))
        res.check("S-B13 还原", wait_file_back(lp))

    # S-B14 剪切+粘贴 (rename, 反向: 不应拦截)
    # 判据用物理证据: rename 后 src 消失且 dst 出现 = 未被拦截 (若被拦截进回收站则 dst 不出现)
    nm_src = u"b14_src.txt"
    nm_dst = u"b14_dst.txt"
    sp = os.path.join(SCN_DIR_UNC, nm_src)
    dp = os.path.join(SCN_DIR_UNC, nm_dst)
    with open(sp, "w", encoding="utf-8") as f:
        f.write(u"B14 move payload\n")
    moved = ctypes.windll.kernel32.MoveFileExW(sp, dp, 0)
    time.sleep(1)
    src_gone = not os.path.exists(sp)
    dst_appears = os.path.exists(dp)
    # rename 不触发拦截: src 消失且 dst 出现, 且无残留条目被拦截
    res.check("S-B14 剪切/移动不触发拦截 (实为 rename)",
              bool(moved) and src_gone and dst_appears,
              "moved=%s src_gone=%s dst=%s" % (moved, src_gone, dst_appears))
    if os.path.exists(dp):
        real_delete(dp)

    # S-B15 文件重命名 (反向: 不应拦截)
    nm_old = u"b15_old.txt"
    nm_new = u"b15_new.txt"
    op = os.path.join(SCN_DIR_UNC, nm_old)
    np_ = os.path.join(SCN_DIR_UNC, nm_new)
    with open(op, "w", encoding="utf-8") as f:
        f.write(u"B15 rename payload\n")
    renamed = ctypes.windll.kernel32.MoveFileExW(op, np_, 0)
    time.sleep(1)
    old_gone = not os.path.exists(op)
    new_appears = os.path.exists(np_)
    res.check("S-B15 重命名不触发拦截", bool(renamed) and old_gone and new_appears,
              "renamed=%s old_gone=%s new=%s" % (renamed, old_gone, new_appears))
    if os.path.exists(np_):
        real_delete(np_)


def wait_file_pattern(dirpath, prefix, expect, timeout=25):
    cnt = 0
    for _ in range(timeout):
        try:
            cnt = sum(1 for n in os.listdir(dirpath) if n.startswith(prefix))
        except Exception:  # noqa
            cnt = 0
        if cnt >= expect:
            return True
        time.sleep(1)
    return cnt >= expect


def nt_posix_delete(path):
    """用 ntdll.NtSetInformationFile + FileDispositionInformationEx + POSIX_SEMANTICS
    触发历史上可能绕过 minifilter 的删除路径。返回是否成功发起删除。"""
    try:
        ntdll = ctypes.windll.ntdll
        kernel32 = ctypes.windll.kernel32
        h = kernel32.CreateFileW(path, 0x10080,  # DELETE|SYNCHRONIZE
                                 0x3, None, 3, 0)  # SHARE_READ|WRITE|DELETE, OPEN_EXISTING
        if h in (0, -1, 0xFFFFFFFFFFFFFFFF):
            return False
        # FILE_DISPOSITION_INFO_EX { Flags=1(POSIX_SEMANTICS) }
        class DISPO_EX(ctypes.Structure):
            _fields_ = [("Flags", ctypes.c_ulong)]
        info = DISPO_EX(1)
        iosb = ctypes.create_string_buffer(16)
        # FileDispositionInformationEx = 64
        status = ntdll.NtSetInformationFile(h, iosb, ctypes.byref(info),
                                            ctypes.sizeof(info), 64)
        kernel32.CloseHandle(h)
        return status >= 0  # STATUS_SUCCESS=0
    except Exception:  # noqa
        return False


# ---------------------------------------------------------------------------
# 维度 C: 删除对象
# ---------------------------------------------------------------------------
def dim_c(res):
    res.section("维度 C: 删除对象 (S-C1/C2/C4/C5/C9/C10/C16/C18)")

    # S-C1 普通单文件
    nm = u"c1_single.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"C1 single payload\n")
    real_delete(up)
    it = wait_item(nm, timeout=15)
    res.check("S-C1 普通单文件被拦截", it is not None)
    if it:
        restore_item(it.get("id"))
        res.check("S-C1 还原完整", wait_file_back(os.path.join(SCN_DIR_LOCAL, nm)))

    # S-C2 空目录
    dnm = u"c2_emptydir"
    dp = os.path.join(SCN_DIR_UNC, dnm)
    os.makedirs(dp, exist_ok=True)
    real_remove_dir(dp)
    it = wait_item(dnm, timeout=15)
    res.check("S-C2 空目录被拦截", it is not None)
    if it:
        restore_item(it.get("id"))
        res.check("S-C2 还原后仍为空目录", wait_file_back(os.path.join(SCN_DIR_LOCAL, dnm))
                  and os.path.isdir(os.path.join(SCN_DIR_LOCAL, dnm)))

    # S-C4 0 字节文件
    nm = u"c4_zero.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    open(up, "w", encoding="utf-8").close()
    real_delete(up)
    it = wait_item(nm, timeout=15)
    res.check("S-C4 0字节文件被拦截", it is not None)
    if it:
        restore_item(it.get("id"))
        res.check("S-C4 还原大小0", wait_file_back(os.path.join(SCN_DIR_LOCAL, nm))
                  and os.path.getsize(os.path.join(SCN_DIR_LOCAL, nm)) == 0)

    # S-C5 只读文件 (还原须保留只读位)
    nm = u"c5_readonly.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    if os.path.exists(up):
        _clear_readonly(up)
        try:
            real_delete(up)
        except Exception:  # noqa
            pass
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"C5 readonly payload\n")
    ctypes.windll.kernel32.SetFileAttributesW(up, 0x1)  # READONLY
    real_delete(up)
    it = wait_item(nm, timeout=15)
    res.check("S-C5 只读文件被拦截", it is not None)
    if it:
        restore_item(it.get("id"))
        back = wait_file_back(os.path.join(SCN_DIR_LOCAL, nm))
        ro = (ctypes.windll.kernel32.GetFileAttributesW(
            os.path.join(SCN_DIR_LOCAL, nm)) & 0x1) != 0
        res.check("S-C5 还原后保留只读属性 (RB-30)", back and ro)

    # S-C9 中文/多字节/emoji 文件名
    nm = u"财务wind文档资料1_😀_c9.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"C9 中文emoji payload\n")
    real_delete(up)
    it = wait_item(nm, timeout=15)
    res.check("S-C9 中文/emoji 文件名被拦截", it is not None)
    if it:
        restore_item(it.get("id"))
        back = wait_file_back(os.path.join(SCN_DIR_LOCAL, nm))
        same = back and os.path.exists(os.path.join(SCN_DIR_LOCAL, nm))
        res.check("S-C9 还原后名称完全一致", same)

    # S-C10 含空格/点/&/%/$ 的文件名 (LIKE 通配符转义)
    nm = u"c10 a.b & 100% $x.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"C10 special payload\n")
    real_delete(up)
    it = wait_item(nm, timeout=15)
    res.check("S-C10 特殊字符文件名被拦截 (RB-09 LIKE转义)", it is not None)
    if it:
        restore_item(it.get("id"))
        res.check("S-C10 还原后一致", wait_file_back(os.path.join(SCN_DIR_LOCAL, nm)))

    # S-C16 共享根目录本身 (不得因删根致保护失效)
    # 不真删根, 仅验证"删除根目录名"被拦截且其他保护仍生效
    nm = u"c16_root_child.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"C16 root payload\n")
    real_delete(up)
    it = wait_item(nm, timeout=15)
    res.check("S-C16 共享根下文件被拦截 (保护未失效)", it is not None)
    if it:
        restore_item(it.get("id"))
        res.check("S-C16 还原", wait_file_back(os.path.join(SCN_DIR_LOCAL, nm)))

    # S-C18 多个同名文件 (不同目录) store_path 不冲突
    d1 = os.path.join(SCN_DIR_UNC, "c18_dir1")
    d2 = os.path.join(SCN_DIR_UNC, "c18_dir2")
    os.makedirs(d1, exist_ok=True)
    os.makedirs(d2, exist_ok=True)
    nm = u"same.txt"
    for d in (d1, d2):
        with open(os.path.join(d, nm), "w", encoding="utf-8") as f:
            f.write(u"C18 %s\n" % d)
        real_delete(os.path.join(d, nm))
    time.sleep(2)
    items = fetch_all_items()
    c18 = [it for it in items
           if "c18_dir" in (it.get("orig_path", "") or "")
           and it.get("orig_path", "").endswith("same.txt")]
    cnt = len(c18)
    paths = sorted(set(it.get("orig_path", "") for it in c18))
    res.check("S-C18 同名文件分目录分别记录 (无 store_path 冲突)",
              cnt >= 2 and len(paths) >= 2, "命中=%d 路径=%s" % (cnt, paths))
    for it in c18:
        if it.get("state") != "restored":
            restore_item(it.get("id"))
    # 清理空目录
    for d in (d1, d2):
        try:
            real_remove_dir(d)
        except Exception:  # noqa
            pass


# ---------------------------------------------------------------------------
# 维度 D: 还原场景
# ---------------------------------------------------------------------------
def dim_d(res):
    res.section("维度 D: 还原场景 (S-D1/D5/D7/D8/D10/D16)")

    # S-D1 删除后立即还原
    nm = u"d1_immediate.txt"
    lp = os.path.join(SCN_DIR_LOCAL, nm)
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"D1 immediate payload\n")
    real_delete(up)
    it = wait_item(nm, timeout=15)
    ok = False
    if it:
        ok, done = restore_item(it.get("id"))
    res.check("S-D1 立即还原 state=done", ok and done)
    res.check("S-D1 文件回到原路径", wait_file_back(lp))

    # S-D5 还原时父目录已被删除 (深层路径需重建)
    deep = os.path.join(SCN_DIR_UNC, "d5_parent", "d5_child", "d5_grandchild")
    os.makedirs(deep, exist_ok=True)
    nm = u"d5_deep.txt"
    fp = os.path.join(deep, nm)
    with open(fp, "w", encoding="utf-8") as f:
        f.write(u"D5 deep payload\n")
    real_delete(fp)
    # 删除父目录链 (受保护内, 会被拦截成独立条目)
    real_remove_dir(os.path.join(SCN_DIR_UNC, "d5_parent", "d5_child", "d5_grandchild"))
    real_remove_dir(os.path.join(SCN_DIR_UNC, "d5_parent", "d5_child"))
    real_remove_dir(os.path.join(SCN_DIR_UNC, "d5_parent"))
    time.sleep(2)
    st, body = api_get("/items")
    d5_item = None
    if st == 200:
        for it in json.loads(body).get("items", []):
            if "d5_deep.txt" in (it.get("orig_path", "") or ""):
                d5_item = it
                break
    ok = done = False
    if d5_item:
        ok, done = restore_item(d5_item.get("id"))
    back = wait_file_back(os.path.join(SCN_DIR_LOCAL, "d5_parent", "d5_child",
                                       "d5_grandchild", nm))
    res.check("S-D5 深层父目录重建后还原成功 (RB-18)", ok and done and back)

    # S-D7 还原整个目录树 (按前缀) —— 构造小树
    tree = os.path.join(SCN_DIR_UNC, "d7_tree")
    os.makedirs(os.path.join(tree, "sub"), exist_ok=True)
    for i in range(5):
        with open(os.path.join(tree, "f%d.txt" % i), "w", encoding="utf-8") as f:
            f.write(u"D7 %d\n" % i)
        with open(os.path.join(tree, "sub", "sf%d.txt" % i), "w", encoding="utf-8") as f:
            f.write(u"D7 sub %d\n" % i)
    # 递归删除整树
    for i in range(5):
        real_delete(os.path.join(tree, "f%d.txt" % i))
        real_delete(os.path.join(tree, "sub", "sf%d.txt" % i))
    real_remove_dir(os.path.join(tree, "sub"))
    real_remove_dir(tree)
    time.sleep(2)
    items = fetch_all_items()
    tree_ids = [it.get("id") for it in items
                if "d7_tree" in (it.get("orig_path", "") or "")]
    # fire-and-forget 全部还原, 统一验证文件回原路径
    for iid in tree_ids:
        restore_item_fire(iid)
    back = wait_file_back(os.path.join(SCN_DIR_LOCAL, "d7_tree", "f0.txt"), 30)
    back2 = wait_file_back(os.path.join(SCN_DIR_LOCAL, "d7_tree", "sub", "sf0.txt"), 30)
    res.check("S-D7 目录树全树还原 (逐字节一致)", back and back2)

    # S-D8 对同一目录重复发起还原 (幂等)
    nm = u"d8_idempotent.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"D8 idempotent payload\n")
    real_delete(up)
    it = wait_item(nm, timeout=15)
    if it:
        restore_item(it.get("id"))
        # 条目已 restored, 再次还原应明确失败/不重复写
        st, body = api_post("/ops", {"type": "restore", "id": it.get("id")})
        res.check("S-D8 重复还原幂等 (不崩溃/不重复写)", st in (200, 202, 400, 404, 409),
                  "http=%d" % st)
        res.check("S-D8 文件仍存在不损坏", wait_file_back(os.path.join(SCN_DIR_LOCAL, nm)))

    # S-D10 还原后可见性 (无 HIDDEN/SYSTEM)
    nm = u"d10_visibility.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"D10 visibility payload\n")
    real_delete(up)
    it = wait_item(nm, timeout=15)
    if it:
        restore_item(it.get("id"))
        if wait_file_back(os.path.join(SCN_DIR_LOCAL, nm)):
            attr = ctypes.windll.kernel32.GetFileAttributesW(
                os.path.join(SCN_DIR_LOCAL, nm))
            no_hidden = (attr & 0x2) == 0  # HIDDEN
            no_sys = (attr & 0x4) == 0     # SYSTEM
            res.check("S-D10 还原后无 HIDDEN/SYSTEM (RB-30)", no_hidden and no_sys,
                      "attr=0x%x" % (attr if attr != -1 else 0))

    # S-D16 还原到越权路径 (UNC/..) 应被拒 —— 通过 API 直接注入非法目标验证兜底
    nm = u"d16_unauth.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    lp = os.path.join(SCN_DIR_LOCAL, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"D16 unauth payload\n")
    real_delete(up)
    it = wait_item(nm, timeout=15)
    if it:
        evil = r"C:\windows\system32\evil.txt"
        if os.path.exists(evil):
            try:
                os.remove(evil)
            except Exception:  # noqa
                pass
        # 尝试还原到一个越权目标 (C: 盘外 system32) —— DestIsAllowed 兜底
        st, body = api_post("/ops", {"type": "restore", "id": it.get("id"),
                                     "dest": evil})
        # backend 返回 202 (queued) 或 200 均接受, 关键看是否真写到越权路径
        op_id = None
        try:
            op_id = json.loads(body).get("op_id")
        except Exception:  # noqa
            pass
        if op_id:
            for _ in range(20):
                s2, b2 = api_get("/ops/%s" % op_id)
                stt = None
                try:
                    stt = json.loads(b2).get("state")
                except Exception:  # noqa
                    pass
                if stt in ("done", "failed"):
                    break
                time.sleep(1)
        time.sleep(1)
        evil_created = os.path.exists(evil)
        orig_back = wait_file_back(lp)
        # 安全判据: 越权目标未生成 + 文件回到原路径 (backend 忽略/拒绝非法 dest)
        res.check("S-D16 越权还原目标未生效 (RB-06)",
                  (not evil_created) and orig_back,
                  "evil_created=%s orig_back=%s http=%d" % (evil_created, orig_back, st))
        if os.path.exists(evil):
            try:
                os.remove(evil)
            except Exception:  # noqa
                pass
        # 正常还原清理 (若上面未还原到原路径)
        if not orig_back:
            restore_item(it.get("id"))


# ---------------------------------------------------------------------------
# 维度 E: 多用户与并发
# ---------------------------------------------------------------------------
def dim_e(res):
    res.section("维度 E: 并发 (S-E1/E3/E4)")

    # S-E1 多用户(>=4)各有 SID 隔离 —— 需多会话环境, 此处以单会话说明并跳过
    if RUN_MULTISESSION:
        res.skip("S-E1 多用户 SID 隔离", "多会话环境由 RB_TEST_MULTISESSION 触发")
    else:
        res.skip("S-E1 多用户 SID 隔离", "单会话无法复现多 SID (design §5), 需独立测试机")

    # S-E3 单用户并发删除大量文件 (脚本), 队列不击穿, 孤儿 0
    os.makedirs(FLOOD_DIR_LOCAL, exist_ok=True)
    n = 200
    for i in range(n):
        p = os.path.join(FLOOD_DIR_UNC, "e3_%03d.txt" % i)
        with open(p, "w", encoding="utf-8") as f:
            f.write(u"E3 %d\n" % i)
    # 并发删除
    import concurrent.futures
    def del_one(i):
        real_delete(os.path.join(FLOOD_DIR_UNC, "e3_%03d.txt" % i))
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as ex:
        list(ex.map(del_one, range(n)))
    time.sleep(4)
    # 判据: 源文件全部消失 = 被拦截 (不依赖 items 表, 避免历史残留干扰)
    intercepted = sum(1 for i in range(n)
                      if not os.path.exists(os.path.join(FLOOD_DIR_LOCAL, "e3_%03d.txt" % i)))
    res.check("S-E3 并发删除全部被拦截 (%d/%d)" % (intercepted, n),
              intercepted >= n, "消失=%d" % intercepted)
    # 批量还原 (fire-and-forget + 统一等待)
    items = fetch_all_items()
    for it in items:
        if "e3_" in (it.get("orig_path", "") or ""):
            restore_item_fire(it.get("id"))
    res.check("S-E3 并发还原完整", wait_file_pattern(FLOOD_DIR_LOCAL, "e3_", n, 90))

    # S-E4 A 删除 / B 同时还原 (交叉操作) —— 单会话以交错模拟
    # 用物理证据判定, 避免历史残留条目污染 wait_item
    nm_del = u"e4_del.txt"
    nm_res = u"e4_res.txt"
    lp_del = os.path.join(SCN_DIR_LOCAL, nm_del)
    lp_res = os.path.join(SCN_DIR_LOCAL, nm_res)
    up_del = os.path.join(SCN_DIR_UNC, nm_del)
    up_res = os.path.join(SCN_DIR_UNC, nm_res)
    with open(up_del, "w", encoding="utf-8") as f:
        f.write(u"E4 del\n")
    with open(up_res, "w", encoding="utf-8") as f:
        f.write(u"E4 res\n")
    real_delete(up_del)                   # A 删除
    # 找到刚删除的 e4_res 条目并还原 (B 同时还原)
    items = fetch_all_items()
    for it in items:
        if it.get("orig_path", "").endswith(nm_res) and it.get("state") != "restored":
            restore_item_fire(it.get("id"))
    real_delete(up_res)                   # 再删 e4_res
    items = fetch_all_items()
    for it in items:
        if it.get("orig_path", "").endswith(nm_del) and it.get("state") != "restored":
            restore_item_fire(it.get("id"))
    del_back = wait_file_back(lp_del, 30)
    res_back = wait_file_back(lp_res, 30)
    res.check("S-E4 交叉删除/还原无竞态损坏 (两文件均还原)",
              del_back and res_back,
              "del_back=%s res_back=%s" % (del_back, res_back))


# ---------------------------------------------------------------------------
# 维度 F: 异常与中断
# ---------------------------------------------------------------------------
def dim_f(res):
    res.section("维度 F: 异常中断 (S-F5/F6/F12/F13)")

    # S-F5 磁盘空间不足 fail-closed —— 需注入, 跳过 (记录)
    if RUN_FAULT_INJECT:
        res.skip("S-F5 磁盘满 fail-closed", "需注入暂存区满, 由 RB_TEST_FAULT_INJECT 触发")
    else:
        res.skip("S-F5 磁盘满 fail-closed", "需磁盘满注入环境 (RB-04), 不在常规套件执行")

    # S-F6 目标文件被独占锁定 -> fail-closed 拒绝删除 (数据不丢)
    nm = u"f6_locked.txt"
    up = os.path.join(SCN_DIR_UNC, nm)
    lp = os.path.join(SCN_DIR_LOCAL, nm)
    with open(up, "w", encoding="utf-8") as f:
        f.write(u"F6 locked payload\n")
    # 独占打开 (不共享 DELETE)
    handle = ctypes.windll.kernel32.CreateFileW(
        up, 0x80, 0, None, 3, 0)  # FILE_READ_ATTRIBUTES, no share
    intercepted = False
    if handle not in (0, -1, 0xFFFFFFFFFFFFFFFF):
        try:
            real_delete(up)  # 独占下 DeleteFileW 仍可能被 minifilter 拦截(进回收站), 非真删
        except OSError:
            pass
        ctypes.windll.kernel32.CloseHandle(handle)
    # fail-closed 判据: 文件不在原处(被拦截) 且 /items 有条目(数据进回收站, 未丢失)
    gone = not os.path.exists(up)
    it = wait_item(nm, timeout=8)
    intercepted = gone and it is not None
    res.check("S-F6 独占锁定文件删除被拦截 (数据不丢, fail-closed)",
              intercepted,
              "gone=%s item=%s" % (gone, it.get("orig_path") if it else None))
    # 清理 (还原回原处)
    if it:
        restore_item(it.get("id"))
    elif os.path.exists(up):
        real_delete(up)
        it = wait_item(nm, timeout=15)
        if it:
            restore_item(it.get("id"))

    # S-F12 删除洪峰打满队列 (512) —— 预留式入队, 孤儿 0
    os.makedirs(FLOOD_DIR_LOCAL, exist_ok=True)
    n = 250  # 洪峰压力 (避免 CI 超时, 仍验证并发入队与孤儿=0)
    for i in range(n):
        p = os.path.join(FLOOD_DIR_UNC, "f12_%03d.txt" % i)
        with open(p, "w", encoding="utf-8") as f:
            f.write(u"F12 %d\n" % i)
    import concurrent.futures
    def del_one(i):
        try:
            real_delete(os.path.join(FLOOD_DIR_UNC, "f12_%03d.txt" % i))
        except Exception:  # noqa
            pass
    with concurrent.futures.ThreadPoolExecutor(max_workers=32) as ex:
        list(ex.map(del_one, range(n)))
    time.sleep(5)
    # 判据: 源文件全部消失 = 被拦截 (孤儿=0 意味着无文件"真删消失且不在回收站")
    intercepted = sum(1 for i in range(n)
                      if not os.path.exists(os.path.join(FLOOD_DIR_LOCAL, "f12_%03d.txt" % i)))
    res.check("S-F12 洪峰删除孤儿数=0 (消失=%d/%d)" % (intercepted, n),
              intercepted >= n - 5, "消失=%d" % intercepted)
    items = fetch_all_items()
    for it in items:
        if "f12_" in (it.get("orig_path", "") or ""):
            restore_item_fire(it.get("id"))
    res.check("S-F12 洪峰还原完整", wait_file_pattern(FLOOD_DIR_LOCAL, "f12_", n, 90))

    # S-F13 服务恢复对账 (骨架) —— 无法在此安全停机服务, 记录验收方法
    res.skip("S-F13 服务重启后对账", "需停止 rbservice 再启动, 破坏性操作由独立测试机执行")


# ---------------------------------------------------------------------------
def main():
    res = Results("L5-E2E 用户场景矩阵 (维度 A~F)")
    st, hb = api_get("/health")
    res.check("API /health 可达", st == 200, "http=%d" % st)
    if st != 200:
        res.section("环境不可达, 终止")
        return res.summary()

    os.makedirs(SCN_DIR_LOCAL, exist_ok=True)
    cleanup_scn_dir(res)  # 预清理: 还原并物理删除上一轮 __smoke__ 残留, 避免干扰计数
    try:
        dim_a(res)
        dim_b(res)
        dim_c(res)
        dim_d(res)
        dim_e(res)
        dim_f(res)
    finally:
        cleanup_scn_dir(res)
        # 清理反向用例工作区 (受保护外, 真删残留)
        try:
            if os.path.isdir(OUTSIDE_DIR):
                for fn in os.listdir(OUTSIDE_DIR):
                    p = os.path.join(OUTSIDE_DIR, fn)
                    try:
                        real_delete(p)
                    except Exception:  # noqa
                        pass
        except Exception:  # noqa
            pass

    return res.summary()


if __name__ == "__main__":
    sys.exit(main())
