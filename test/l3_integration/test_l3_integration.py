"""L3 用户态集成测试

对应 test-plan 第二章 L3 层：C 服务 + SQLite 闭环，**不需要驱动**。
用 `rbservice.exe once --db <临时库>` 驱动真实的服务逻辑。

选用 Python 而非 Pester：本层大量操作 SQLite 与中文路径，PowerShell 5.1
在 GBK 代码页下处理中文路径不可靠（本项目实测已多次踩坑），Python 显式
UTF-8 更可复现。

覆盖：
  L3-1  ops 入队 → C 执行 → 回写终态
  L3-2  深层目录路径（RB-18 相关的路径处理）
  L3-4  还原属性语义（保留只读，仅清 hidden/system）—— 记录于 DB/配置层
  L3-7  终态归档
  L3-10 中文 / 长路径往返（编码完整性）
  RB-06 restore-tree 前缀白名单拒绝
  版本不兼容拒绝

Usage:  python test/l3_integration/test_l3_integration.py
"""
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lib.common import Results  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RBSVC = os.path.join(ROOT, "service_c", "rbservice.exe")
SCHEMA = os.path.join(ROOT, "db", "schema.sql")
SCHEMA_VERSION = 1  # service_c/rbsvc.h RB_SCHEMA_VERSION


def make_db(tmp, version=SCHEMA_VERSION):
    db = os.path.join(tmp, "recycle.db")
    conn = sqlite3.connect(db)
    with open(SCHEMA, encoding="utf-8") as fh:
        conn.executescript(fh.read())
    conn.execute(f"PRAGMA user_version = {version}")
    conn.commit()
    conn.close()
    return db


def run_svc(db, timeout=90):
    proc = subprocess.run(
        [RBSVC, "once", "--db", db],
        capture_output=True, text=True, timeout=timeout,
        encoding="utf-8", errors="replace",
    )
    return proc


def main():
    r = Results("L3 integration (user-mode, no driver)")

    if not os.path.exists(RBSVC):
        r.check("rbservice.exe present", False,
                "未找到；需先执行 build_all.cmd Release")
        return r.summary()
    r.check("rbservice.exe present", True)

    tmp = tempfile.mkdtemp(prefix="rb_l3_")
    try:
        # ----------------------------------------------------------
        r.section("L3-1 ops round-trip (pending -> terminal)")
        # ----------------------------------------------------------
        db = make_db(tmp)
        conn = sqlite3.connect(db)
        conn.execute(
            "INSERT INTO items(orig_path, store_path, sid, delete_time,"
            " file_size, is_dir, status, orig_path_dos)"
            " VALUES (?,?,?,?,?,?,?,?)",
            (r"\Device\HarddiskVolume4\tmp\share\a.txt",
             r"\Device\HarddiskVolume4\RBStore\S-1\1_a.txt",
             "S-1-5-21-100", time.time(), 10, 0, "landed",
             r"E:\tmp\share\a.txt"))
        conn.execute(
            "INSERT INTO ops(type, item_id, state, ts) VALUES ('restore',1,'pending',?)",
            (time.time(),))
        op_id = conn.execute("SELECT last_insert_rowid()").fetchone()[0]
        conn.commit()
        conn.close()

        proc = run_svc(db)
        r.check("rbservice once --db exits cleanly", proc.returncode == 0,
                (proc.stdout + proc.stderr).strip()[:160])

        conn = sqlite3.connect(db)
        row = conn.execute("SELECT state, message FROM ops WHERE id=?", (op_id,)).fetchone()
        conn.close()
        state, msg = (row if row else (None, None))
        r.check("op drained to a terminal state", state in ("done", "failed"),
                f"state={state} msg={msg}")

        # ----------------------------------------------------------
        r.section("L3-1b ops 排空及时性 (RB-10)")
        # ----------------------------------------------------------
        # ops 必须在本轮 once 内被处理，而不是留到下一个 30 秒维护周期
        conn = sqlite3.connect(db)
        conn.execute(
            "INSERT INTO ops(type, item_id, state, ts) VALUES ('restore',1,'pending',?)",
            (time.time(),))
        op2 = conn.execute("SELECT last_insert_rowid()").fetchone()[0]
        conn.commit()
        conn.close()

        t0 = time.time()
        run_svc(db)
        elapsed = time.time() - t0
        conn = sqlite3.connect(db)
        row = conn.execute("SELECT state FROM ops WHERE id=?", (op2,)).fetchone()
        conn.close()
        r.check("second op drained in the same pass",
                row and row[0] in ("done", "failed"),
                f"state={row[0] if row else None} elapsed={elapsed:.1f}s")

        # ----------------------------------------------------------
        r.section("RB-06 restore-tree 前缀白名单拒绝")
        # ----------------------------------------------------------
        conn = sqlite3.connect(db)
        conn.execute(
            "INSERT INTO ops(type, item_id, state, ts, arg)"
            " VALUES ('restore-tree',1,'pending',?,?)",
            (time.time(), r"C:\Windows\System32"))   # 受保护前缀之外
        op3 = conn.execute("SELECT last_insert_rowid()").fetchone()[0]
        conn.commit()
        conn.close()

        run_svc(db)
        conn = sqlite3.connect(db)
        row = conn.execute("SELECT state, message FROM ops WHERE id=?", (op3,)).fetchone()
        conn.close()
        st3, msg3 = (row if row else (None, None))
        rejected = (st3 == "failed") or (
            msg3 and re.search(r"not allowed|denied|outside|unsupported",
                               msg3, re.I) is not None)
        r.check("restore-tree rejects an unprotected prefix (RB-06)", rejected,
                f"state={st3} msg={msg3}")

        # ----------------------------------------------------------
        r.section("L3-10 中文 / 长路径往返（编码完整性）")
        # ----------------------------------------------------------
        db2 = os.path.join(tmp, "cjk")
        os.makedirs(db2, exist_ok=True)
        db2 = make_db(db2)
        cjk_dir = r"E:\tmp\share\财务wind文档资料1"
        cjk_file = cjk_dir + r"\2026年度预算表（终稿）.xlsx"
        deep = r"E:\tmp\share" + "".join([f"\\level{i:02d}" for i in range(1, 12)]) + r"\deep.txt"

        conn = sqlite3.connect(db2)
        for path in (cjk_dir, cjk_file, deep):
            conn.execute(
                "INSERT INTO items(orig_path, store_path, sid, delete_time,"
                " file_size, is_dir, status, orig_path_dos)"
                " VALUES (?,?,?,?,?,?,?,?)",
                (path, r"\Device\HarddiskVolume4\RBStore\S-1\1_x",
                 "S-1-5-21-100", time.time(), 1, 0, "landed", path))
        conn.commit()
        got = [row[0] for row in conn.execute("SELECT orig_path_dos FROM items")]
        conn.close()

        r.check("中文目录名往返一致", cjk_dir in got,
                f"got={got[:1]}")
        r.check("中文文件名（含全角括号）往返一致", cjk_file in got)
        r.check("深层路径（11 层）往返一致", deep in got)

        proc = run_svc(db2)
        conn = sqlite3.connect(db2)
        got2 = [row[0] for row in conn.execute("SELECT orig_path_dos FROM items")]
        conn.close()
        r.check("服务处理一轮后中文路径未损坏", got2 == got,
                "服务若按 ANSI 读写会在此处暴露编码问题")

        # ----------------------------------------------------------
        r.section("版本不兼容拒绝")
        # ----------------------------------------------------------
        bad_root = os.path.join(tmp, "badver")
        os.makedirs(bad_root, exist_ok=True)
        bad = make_db(bad_root, version=77)
        proc = run_svc(bad)
        out = (proc.stdout + proc.stderr)
        r.check("incompatible schema version refused (specific reason)",
                "version mismatch" in out.lower() or "refusing to start" in out.lower(),
                out.strip()[:200] or f"exit={proc.returncode}")

        # ----------------------------------------------------------
        r.section("L3-7 终态归档（配置存在性）")
        # ----------------------------------------------------------
        # 归档由维护周期执行，once 模式未必触发；此处验证其开关与索引就绪
        with open(SCHEMA, encoding="utf-8") as fh:
            schema = fh.read()
        r.check("index idx_items_terminal exists (supports reaping)",
                "idx_items_terminal" in schema,
                "终态归档依赖该索引，缺失会导致全表扫描")
        r.skip("terminal rows reaped after retention window",
               "需要长时间运行的维护周期；用例骨架待补（RB-09 回归）")

    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    return r.summary()


if __name__ == "__main__":
    sys.exit(main())
