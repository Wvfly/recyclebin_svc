"""verify_contract.py - Prove the C <-> Go decoupling guards actually work.

Each check deliberately breaks the shared contract and asserts that the
service REFUSES TO START instead of silently mis-reading data.

Usage:  python db/verify_contract.py
Requires: rbservice.exe and rbapi.exe already built.
"""
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RBAPI = os.path.join(ROOT, "service_go", "rbapi.exe")
RBSVC = os.path.join(ROOT, "service_c", "rbservice.exe")

PORT = 18901
TOKEN = "verify-token"

results = []


def run(args, timeout=25):
    """Run to completion (use for cases expected to EXIT, i.e. rejections)."""
    return subprocess.run(
        args, capture_output=True, text=True, timeout=timeout,
        encoding="utf-8", errors="replace",
    )


def run_until_listening(args, wait=3.0):
    """Start a server and report whether it stayed up (accepted) or exited.

    Returns (still_running, output). A rejected startup exits quickly with an
    error on stderr; a healthy one binds the port and keeps running.
    """
    import time
    proc = subprocess.Popen(
        args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding="utf-8", errors="replace",
    )
    time.sleep(wait)
    if proc.poll() is None:
        proc.terminate()
        try:
            out, err = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate()
        return True, (out or "") + (err or "")
    out, err = proc.communicate()
    return False, (out or "") + (err or "")


def check(name, passed, detail=""):
    results.append((name, passed, detail))
    mark = "PASS" if passed else "FAIL"
    print(f"  [{mark}] {name}")
    if detail:
        for line in detail.strip().splitlines()[:4]:
            print(f"         {line}")


def make_db(tmp):
    """Let rbservice create a real database, then return its path."""
    db = os.path.join(tmp, "recycle.db")
    # `once` opens the DB (creating + stamping schema), runs one pass, exits.
    run([RBSVC, "once"], timeout=60)
    return db


def main():
    if not os.path.exists(RBAPI):
        print("rbapi.exe not found; build service_go first")
        return 1
    if not os.path.exists(RBSVC):
        print("rbservice.exe not found; build service_c first")
        return 1

    print("=" * 64)
    print("C <-> Go contract verification")
    print("=" * 64)

    # --------------------------------------------------------------
    print("\n[1] Normal case: valid database is accepted")
    # --------------------------------------------------------------
    tmp = tempfile.mkdtemp(prefix="rbverify")
    # rbservice uses the registry StoreRoot, so we cannot redirect it easily.
    # Instead build the DB the same way it does, then test rbapi against it.
    db = os.path.join(tmp, "recycle.db")
    with open(os.path.join(ROOT, "db", "schema.sql"), encoding="utf-8") as f:
        schema = f.read()
    conn = sqlite3.connect(db)
    conn.executescript(schema)
    conn.execute("PRAGMA user_version = 1")
    conn.commit()
    conn.close()

    alive, out = run_until_listening(
        [RBAPI, "--db", db, "--addr", f"127.0.0.1:{PORT}", "--token", TOKEN])
    check("valid schema (v1) accepted", alive, out.strip()[:200])

    # --------------------------------------------------------------
    print("\n[2] P0-2: wrong user_version must be REJECTED")
    # --------------------------------------------------------------
    db2 = os.path.join(tmp, "badver.db")
    conn = sqlite3.connect(db2)
    conn.executescript(schema)
    conn.execute("PRAGMA user_version = 99")   # simulate a newer/older build
    conn.commit()
    conn.close()

    proc = run([RBAPI, "--db", db2, "--addr", f"127.0.0.1:{PORT + 1}",
                "--token", TOKEN], timeout=15)
    out = proc.stdout + proc.stderr
    rejected = ("version mismatch" in out) or ("schema version" in out.lower())
    check("schema version mismatch rejected", rejected, out.strip()[:200])

    # --------------------------------------------------------------
    print("\n[3] P0-1: renamed column must be REJECTED")
    # --------------------------------------------------------------
    db3 = os.path.join(tmp, "badcol.db")
    conn = sqlite3.connect(db3)
    conn.executescript(schema)
    # Rename a column the way a careless migration would
    conn.execute("ALTER TABLE items RENAME COLUMN recycle_path TO recycle_path_v2")
    conn.execute("PRAGMA user_version = 1")
    conn.commit()
    conn.close()

    proc = run([RBAPI, "--db", db3, "--addr", f"127.0.0.1:{PORT + 2}",
                "--token", TOKEN], timeout=15)
    out = proc.stdout + proc.stderr
    check("renamed column rejected", "schema mismatch" in out.lower(),
          out.strip()[:200])

    # --------------------------------------------------------------
    print("\n[4] P0-2: uninitialized database (user_version=0) rejected")
    # --------------------------------------------------------------
    db4 = os.path.join(tmp, "empty.db")
    conn = sqlite3.connect(db4)
    conn.executescript(schema)          # tables exist but version never set
    conn.commit()
    conn.close()

    proc = run([RBAPI, "--db", db4, "--addr", f"127.0.0.1:{PORT + 3}",
                "--token", TOKEN], timeout=15)
    out = proc.stdout + proc.stderr
    check("uninitialized db rejected", "uninitialized" in out.lower(),
          out.strip()[:200])

    # --------------------------------------------------------------
    print("\n[5] P1-2: ops.type CHECK rejects unsupported commands")
    # --------------------------------------------------------------
    db5 = os.path.join(tmp, "ops.db")
    conn = sqlite3.connect(db5)
    conn.executescript(schema)
    conn.execute("PRAGMA user_version = 1")
    conn.commit()
    try:
        conn.execute(
            "INSERT INTO ops(type, item_id, state, ts) VALUES (?,?,?,?)",
            ("drop-everything", 1, "pending", 0.0),
        )
        conn.commit()
        check("ops.type CHECK rejects unknown type", False,
              "insert succeeded -- constraint missing!")
    except sqlite3.IntegrityError as e:
        check("ops.type CHECK rejects unknown type", True, str(e))

    # --------------------------------------------------------------
    print("\n[6] status CHECK rejects invalid lifecycle values")
    # --------------------------------------------------------------
    try:
        conn.execute(
            "INSERT INTO items(orig_path, status) VALUES (?,?)",
            ("x", "half-deleted"),
        )
        conn.commit()
        check("items.status CHECK rejects invalid state", False,
              "insert succeeded -- constraint missing!")
    except sqlite3.IntegrityError as e:
        check("items.status CHECK rejects invalid state", True, str(e))

    conn.close()

    # --------------------------------------------------------------
    print("\n[7] P2-1: orig_path_dos column exists for readable paths")
    # --------------------------------------------------------------
    conn = sqlite3.connect(db)
    cols = [r[1] for r in conn.execute("PRAGMA table_info(items)")]
    conn.close()
    check("orig_path_dos column present", "orig_path_dos" in cols,
          f"columns: {', '.join(cols)}")

    # --------------------------------------------------------------
    print("\n[8] Integration: a database created by C is readable by Go")
    # --------------------------------------------------------------
    integ = os.path.join(tmp, "integ.db")
    proc = run([RBSVC, "once", "--db", integ], timeout=120)
    if proc.returncode != 0:
        check("C service creates database", False,
              (proc.stdout + proc.stderr)[:200])
    else:
        conn = sqlite3.connect(integ)
        conn.execute(
            "INSERT INTO items(orig_path, orig_path_dos, store_path, sid,"
            " delete_time, file_size, is_dir, status)"
            " VALUES (?,?,?,?,?,?,?,?)",
            (r"\Device\HarddiskVolume3\Share\q3.docx",
             r"D:\Share\q3.docx",
             r"\Device\HarddiskVolume3\RBStore\S-1\9_q3.docx",
             "S-1-5-21-777", time.time(), 8888, 0, "landed"),
        )
        conn.commit()
        conn.close()

        port = PORT + 10
        srv = subprocess.Popen(
            [RBAPI, "--db", integ, "--addr", f"127.0.0.1:{port}",
             "--token", TOKEN],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            encoding="utf-8", errors="replace",
        )
        time.sleep(3.0)
        payload = None
        try:
            if srv.poll() is None:
                req = urllib.request.Request(
                    f"http://127.0.0.1:{port}/items?limit=5",
                    headers={"X-Auth-Token": TOKEN})
                with urllib.request.urlopen(req, timeout=8) as resp:
                    payload = json.loads(resp.read().decode("utf-8"))
        except Exception as e:      # noqa: BLE001 - report any failure
            check("Go reads C-created database", False, f"{type(e).__name__}: {e}")
        finally:
            srv.terminate()
            try:
                srv.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                srv.kill()
                srv.communicate()

        if payload is not None:
            items = payload.get("items", [])
            got = items[0]["display_path"] if items else None
            check("Go reads C-created database", len(items) == 1,
                  f"{len(items)} item(s)")
            check("display_path prefers the DOS form",
                  got == r"D:\Share\q3.docx", f"display_path={got!r}")

    shutil.rmtree(tmp, ignore_errors=True)

    # --------------------------------------------------------------
    print("\n" + "=" * 64)
    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    print(f"RESULT: {passed}/{total} checks passed")
    print("=" * 64)

    for name, ok, _ in results:
        if not ok:
            print(f"  FAILED: {name}")

    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
