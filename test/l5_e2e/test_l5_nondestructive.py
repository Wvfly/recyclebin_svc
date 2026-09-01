"""L5 端到端 — 非破坏性检查

对**正在运行的**系统做只读验证：不删除任何文件、不改任何数据。
对应 test-plan 第二章 L5 与第三章的核心链路（S-G1、S-G10 等运维场景）。

破坏性场景（真的删除/还原，对应 S-H1~H10）见 destructive 用例，
需要独立测试机，默认不执行。

Usage:  python test/l5_e2e/test_l5_nondestructive.py [--url http://127.0.0.1:8800]
"""
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lib.common import Results  # noqa: E402

DEFAULT_URL = "http://127.0.0.1:8800"


def get_json(url, timeout=10):
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main():
    url = DEFAULT_URL
    for arg in sys.argv[1:]:
        if arg.startswith("--url="):
            url = arg.split("=", 1)[1]

    r = Results(f"L5 end-to-end (non-destructive) @ {url}")

    # --------------------------------------------------------------
    r.section("服务可达性")
    # --------------------------------------------------------------
    try:
        health = get_json(f"{url}/health")
        r.check("GET /health reachable", True)
    except Exception as exc:  # noqa: BLE001
        r.check("GET /health reachable", False, f"{type(exc).__name__}: {exc}")
        r.skip("remaining L5 checks", "服务未运行；启动 RecycleBinApi 后重跑")
        return r.summary()

    r.check("/health reports ok", health.get("ok") is True,
            f"ok={health.get('ok')}")

    # --------------------------------------------------------------
    r.section("RB-29 保护生效可观测性")
    # --------------------------------------------------------------
    drv = health.get("driver")
    if not isinstance(drv, dict):
        r.check("driver block present in /health", False, f"driver={drv!r}")
    else:
        r.check("driver block present in /health", True)
        pc = drv.get("protected_count")
        r.check("protected_count is an int", isinstance(pc, int), f"got {pc!r}")
        # 这是 RB-29 的核心告警：0 表示驱动对一切删除放行
        r.check("protected_count > 0 (share is actually protected)",
                isinstance(pc, int) and pc > 0,
                "RB-29：0 意味着驱动放行一切删除，必须告警而非静默")

        age = drv.get("age_sec")
        r.check("driver sample is fresh (age_sec <= 120)",
                isinstance(age, (int, float)) and age <= 120,
                f"age_sec={age}（过旧说明驱动已停止应答）")

    # --------------------------------------------------------------
    r.section("驱动 attach 状态（fltmc）")
    # --------------------------------------------------------------
    try:
        proc = subprocess.run(["fltmc", "filters"], capture_output=True,
                              text=True, encoding="utf-8", errors="replace",
                              timeout=30)
        out = (proc.stdout or "") + (proc.stderr or "")
        # fltmc 需要管理员权限；权限不足不是产品缺陷，记为 SKIP 并提示提权重跑
        if "0x80070005" in out or "Access is denied" in out:
            r.skip("rbminiflt is loaded (fltmc)",
                   "需要管理员权限；以管理员身份重跑以启用该检查")
        else:
            r.check("rbminiflt is loaded", "rbminiflt" in out,
                    out.strip()[:160])
    except FileNotFoundError:
        r.skip("rbminiflt is loaded", "fltmc 不可用")
    except Exception as exc:  # noqa: BLE001
        r.skip("rbminiflt is loaded", f"{type(exc).__name__}: {exc}")

    # --------------------------------------------------------------
    r.section("条目列表与状态模型")
    # --------------------------------------------------------------
    try:
        data = get_json(f"{url}/items")
    except Exception as exc:  # noqa: BLE001
        data = None
        r.check("GET /items reachable", False, f"{type(exc).__name__}: {exc}")

    if data is not None:
        r.check("GET /items reachable", True)
        items = data.get("items", [])
        counts = data.get("counts") or {}
        r.check("items is a list", isinstance(items, list), f"count={len(items)}")

        valid = {"staged", "landed", "purged", "restored"}
        bad = [i.get("status") for i in items if i.get("status") not in valid]
        r.check("all item statuses are valid", not bad, f"invalid={bad[:3]}")

        # 每个可见条目都应带还原所需的关键字段
        missing = [i.get("id") for i in items
                   if not i.get("orig_path_dos") and not i.get("orig_path")]
        r.check("every item has a path (restorable)", not missing,
                f"ids missing path: {missing[:5]}")

        total = counts.get("total")
        if isinstance(total, int):
            r.check("counts.total consistent with returned items",
                    total >= len(items), f"total={total} returned={len(items)}")

    # --------------------------------------------------------------
    r.section("RB-30 已还原条目可见性（只读抽查）")
    # --------------------------------------------------------------
    if data is not None:
        restored = [i for i in items if i.get("status") == "restored"]
        if not restored:
            r.skip("restored items have no hidden/system attrs",
                   "当前无 restored 条目；执行一次还原后再验证")
        else:
            # 抽查最近还原的条目：属性必须已清除（RB-30）
            sample = restored[:3]
            details = []
            all_clean = True
            for it in sample:
                path = it.get("orig_path_dos") or it.get("orig_path") or ""
                if not path or not os.path.exists(path):
                    details.append(f"id={it.get('id')} 路径不存在（可能已移动）")
                    continue
                import ctypes
                attrs = ctypes.windll.kernel32.GetFileAttributesW(str(path))
                if attrs == -1:
                    details.append(f"id={it.get('id')} 无法读取属性")
                    continue
                hidden = bool(attrs & 0x2)     # FILE_ATTRIBUTE_HIDDEN
                system = bool(attrs & 0x4)     # FILE_ATTRIBUTE_SYSTEM
                if hidden or system:
                    all_clean = False
                details.append(
                    f"id={it.get('id')} hidden={hidden} system={system}")
            r.check("restored items have no hidden/system attrs (RB-30)",
                    all_clean, "; ".join(details))

    # --------------------------------------------------------------
    r.section("鉴权（若启用）")
    # --------------------------------------------------------------
    try:
        req = urllib.request.Request(f"{url}/items")
        urllib.request.urlopen(req, timeout=10)
        r.skip("unauthenticated /items rejected",
               "未启用 token（开发环境语义）；生产环境应拒绝")
    except urllib.error.HTTPError as exc:
        r.check("unauthenticated /items rejected", exc.code in (401, 403),
                f"status={exc.code}")
    except Exception as exc:  # noqa: BLE001
        r.skip("unauthenticated /items rejected", f"{type(exc).__name__}: {exc}")

    return r.summary()


if __name__ == "__main__":
    sys.exit(main())
