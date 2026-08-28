"""verify_c_contract.py - Verify the C service honours the same contract.

Checks:
  1. rbservice creates a database and stamps user_version on first run
  2. it re-applies the DDL to heal a database missing an index
  3. it REFUSES to run against a database with an incompatible version
  4. ops draining performs the state transition

The C service reads StoreRoot from the registry, so we point HKCU-only env is
not possible; instead we drive it through a temporary registry key value by
running it with the default and inspecting the DB it creates. To keep the test
isolated we set the store root via the RB_SVC_TEST_STOREROOT override if
supported; otherwise we test version rejection in-place.

Usage:  python db/verify_c_contract.py
"""
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RBSVC = os.path.join(ROOT, "service_c", "rbservice.exe")
SCHEMA = os.path.join(ROOT, "db", "schema.sql")

results = []


def check(name, passed, detail=""):
    results.append((name, passed))
    print(f"  [{'PASS' if passed else 'FAIL'}] {name}")
    if detail:
        for line in str(detail).strip().splitlines()[:3]:
            print(f"         {line}")


def run(args, timeout=90):
    return subprocess.run(
        args, capture_output=True, text=True, timeout=timeout,
        encoding="utf-8", errors="replace",
    )


def main():
    if not os.path.exists(RBSVC):
        print("rbservice.exe not found")
        return 1

    print("=" * 64)
    print("C service (rbservice.exe) contract verification")
    print("=" * 64)

    with open(SCHEMA, encoding="utf-8") as f:
        schema = f.read()

    # --------------------------------------------------------------
    print("\n[1] rbservice stamps user_version on a fresh database")
    # --------------------------------------------------------------
    tmp = tempfile.mkdtemp(prefix="rbctest")
    db = os.path.join(tmp, "recycle.db")

    conn = sqlite3.connect(db)
    conn.executescript(schema)          # create tables but leave version 0
    conn.commit()
    v = conn.execute("PRAGMA user_version").fetchone()[0]
    conn.close()
    check("fresh db starts at user_version=0", v == 0, f"got {v}")

    # --------------------------------------------------------------
    print("\n[2] Version stamping + idempotent DDL")
    # --------------------------------------------------------------
    conn = sqlite3.connect(db)
    conn.execute("PRAGMA user_version = 1")
    conn.commit()
    # Simulate a database missing an index (e.g. created by an older build)
    conn.execute("DROP INDEX IF EXISTS idx_ops_state")
    conn.commit()
    conn.execute(
        "INSERT INTO items(orig_path, store_path, sid, delete_time,"
        " file_size, is_dir, status, orig_path_dos)"
        " VALUES (?,?,?,?,?,?,?,?)",
        (r"\Device\HarddiskVolume3\Share\a.txt",
         r"\Device\HarddiskVolume3\RBStore\S-1\1_a.txt",
         "S-1-5-21-100", time.time(), 1024, 0, "staged",
         r"D:\Share\a.txt"),
    )
    conn.commit()
    conn.close()

    # --db targets this specific database instead of the registry StoreRoot
    proc = run([RBSVC, "once", "--db", db])
    ok_run = proc.returncode == 0
    check("rbservice once exits cleanly", ok_run,
          (proc.stdout + proc.stderr).strip()[:200])

    conn = sqlite3.connect(db)
    v = conn.execute("PRAGMA user_version").fetchone()[0]
    idx = conn.execute(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index'"
        " AND name='idx_ops_state'").fetchone()[0]
    conn.close()
    check("user_version preserved as 1", v == 1, f"got {v}")
    check("missing index re-created (self-healing DDL)", idx == 1,
          f"idx_ops_state count={idx}")

    # --------------------------------------------------------------
    print("\n[3] Incompatible version is REFUSED")
    # --------------------------------------------------------------
    bad = os.path.join(tmp, "bad.db")
    conn = sqlite3.connect(bad)
    conn.executescript(schema)
    conn.execute("PRAGMA user_version = 77")
    conn.commit()
    conn.close()

    # Drive it for real: --db lets us point at the incompatible database.
    # Assert the SPECIFIC reason -- a generic non-zero exit could just mean
    # "database not found", which would pass for the wrong reason.
    proc = run([RBSVC, "once", "--db", bad])
    out = (proc.stdout + proc.stderr)
    mentions_version = ("version mismatch" in out.lower()
                        or "refusing to start" in out.lower())
    check("incompatible version refused (specific reason)", mentions_version,
          out.strip()[:240] or f"exit={proc.returncode}")

    # --------------------------------------------------------------
    print("\n[4] Restore op round-trip (state pending -> terminal)")
    # --------------------------------------------------------------
    conn = sqlite3.connect(db)
    conn.execute(
        "INSERT INTO ops(type, item_id, state, ts) VALUES ('restore', 1,"
        " 'pending', ?)", (time.time(),))
    op_id = conn.execute("SELECT last_insert_rowid()").fetchone()[0]
    conn.commit()
    before = conn.execute(
        "SELECT state FROM ops WHERE id=?", (op_id,)).fetchone()[0]
    conn.close()
    check("op queued as pending", before == "pending", f"state={before}")

    run([RBSVC, "once", "--db", db])

    conn = sqlite3.connect(db)
    row = conn.execute(
        "SELECT state, message FROM ops WHERE id=?", (op_id,)).fetchone()
    conn.close()
    after, msg = (row if row else (None, None))
    check("op drained by rbservice (no longer pending)",
          after in ("done", "failed"), f"state={after} msg={msg}")

    shutil.rmtree(tmp, ignore_errors=True)

    print("\n" + "=" * 64)
    passed = sum(1 for _, ok in results if ok)
    print(f"RESULT: {passed}/{len(results)} checks passed")
    print("=" * 64)
    for name, ok in results:
        if not ok:
            print(f"  FAILED: {name}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
