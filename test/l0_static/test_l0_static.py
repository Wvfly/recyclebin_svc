"""L0 编译期与静态契约检查

对应 test-plan 第二章 L0 层：
  L0-1  sizeof(RBF_STATS) == 68            (驱动侧 C_ASSERT)
  L0-2  rbf_protocol.h 逐字段偏移断言 + RBF_NOTIFY_MAX_SIZE <= 8192
  L0-5  schema 派生一致（schema.sql 与 service_c/schema_sql.h 关键内容一致）
  L0-3  编译零警告            -> 需 MSVC，默认跳过（--build 启用）
  L0-4  部署产物 SHA256 核对  -> 需已部署环境，默认跳过（--deployed 启用）

这些是"构建即跑"的守卫：结构体一旦漂移，编译期就该失败；
本测试确保这些守卫确实存在于源码中（防止被误删）。

Usage:  python test/l0_static/test_l0_static.py [--build] [--deployed]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from lib.common import Results, read_text  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    do_build = "--build" in sys.argv
    do_deployed = "--deployed" in sys.argv

    r = Results("L0 static / compile-time guards")

    # --------------------------------------------------------------
    r.section("L0-1 驱动侧 RBF_STATS 大小断言 (RB-29)")
    # --------------------------------------------------------------
    drv_h = os.path.join(ROOT, "driver", "rbminiflt.h")
    if not os.path.exists(drv_h):
        r.check("driver/rbminiflt.h exists", False, drv_h)
    else:
        src = read_text(drv_h)
        r.check("driver/rbminiflt.h exists", True)
        # C_ASSERT(sizeof(RBF_STATS) == 68)
        m = re.search(r"C_ASSERT\s*\(\s*sizeof\s*\(\s*RBF_STATS\s*\)\s*==\s*(\d+)\s*\)", src)
        r.check("C_ASSERT(sizeof(RBF_STATS)) present", bool(m),
                f"found == {m.group(1)}" if m else "missing: struct drift would go undetected")
        if m:
            r.check("sizeof(RBF_STATS) == 68 (7*u64 + 3*u32, packed)",
                    m.group(1) == "68", f"got {m.group(1)}")
        r.check("RBF_STATS declares ProtectedCount",
                re.search(r"ULONG\s+ProtectedCount", src) is not None,
                "RB-29 exposes the protected-prefix count")

    # --------------------------------------------------------------
    r.section("L0-2 用户态协议头镜像与偏移断言")
    # --------------------------------------------------------------
    proto_h = os.path.join(ROOT, "service_c", "rbf_protocol.h")
    if not os.path.exists(proto_h):
        r.check("service_c/rbf_protocol.h exists", False, proto_h)
    else:
        src = read_text(proto_h)
        r.check("service_c/rbf_protocol.h exists", True)

        m = re.search(r"sizeof\s*\(\s*RBF_STATS\s*\)\s*==\s*(\d+)", src)
        r.check("user-mode sizeof(RBF_STATS) assertion", bool(m),
                f"found == {m.group(1)}" if m else "missing")
        if m:
            r.check("user-mode sizeof matches driver (68)",
                    m.group(1) == "68",
                    "两侧必须一致，否则服务会把字段解析错位")

        m = re.search(r"offsetof\s*\(\s*RBF_STATS\s*,\s*ProtectedCount\s*\)\s*==\s*(\d+)", src)
        r.check("ProtectedCount offset assertion present", bool(m),
                f"offset == {m.group(1)}" if m else "missing")
        if m:
            r.check("ProtectedCount offset == 64", m.group(1) == "64",
                    f"got {m.group(1)}")

        r.check("notify max size guard (<= 8192)",
                re.search(r"RBF_NOTIFY_MAX_SIZE\s*<=\s*8192", src) is not None,
                "RB-01: 防止巨型通知结构回归")

    # --------------------------------------------------------------
    r.section("L0-5 schema 派生一致性")
    # --------------------------------------------------------------
    schema = os.path.join(ROOT, "db", "schema.sql")
    derived = os.path.join(ROOT, "service_c", "schema_sql.h")
    if not os.path.exists(schema):
        r.check("db/schema.sql exists", False, schema)
    else:
        src = read_text(schema)
        r.check("db/schema.sql exists", True)
        r.check("driver_stats.protected_count declared (RB-29)",
                re.search(r"protected_count\s+INTEGER", src) is not None,
                "0 = 驱动对一切删除放行，必须可观测")

        if not os.path.exists(derived):
            r.skip("schema_sql.h derived output present",
                   "未生成（build.cmd 会生成）；L0-5 需构建后验证")
        else:
            dsrc = read_text(derived)
            r.check("schema_sql.h derived output present", True)
            # 派生文件应包含 schema.sql 的关键 DDL 标记
            for token in ("CREATE TABLE", "protected_count", "items"):
                r.check(f"schema_sql.h contains '{token}'",
                        token in dsrc,
                        "派生输出与源不同步时说明 gen_schema.ps1 未重跑")

    # --------------------------------------------------------------
    r.section("L0-3 编译零警告（需 MSVC，默认跳过）")
    # --------------------------------------------------------------
    if not do_build:
        r.skip("compile with zero warnings",
               "默认跳过：需要 MSVC 环境。加 --build 启用（较慢）")
    else:
        build = os.path.join(ROOT, "build_all.cmd")
        if not os.path.exists(build):
            r.skip("compile with zero warnings", "build_all.cmd not found")
        else:
            import subprocess
            proc = subprocess.run(["cmd", "/c", build, "Release"],
                                  cwd=ROOT, capture_output=True, text=True,
                                  encoding="utf-8", errors="replace", timeout=900)
            out = (proc.stdout or "") + (proc.stderr or "")
            warn = [l for l in out.splitlines()
                    if re.search(r"warning\s+(C|LNK)\d+", l, re.I)]
            r.check("compile with zero warnings", proc.returncode == 0 and not warn,
                    "\n".join(warn[:4]) or f"exit={proc.returncode}")

    # --------------------------------------------------------------
    r.section("L0-4 部署产物核对（需已部署，默认跳过）")
    # --------------------------------------------------------------
    if not do_deployed:
        r.skip("deployed .sys SHA256 matches build output",
               "默认跳过：需要读取 System32\\drivers（常需管理员）。加 --deployed 启用")
    else:
        syspath = r"C:\Windows\System32\drivers\rbminiflt.sys"
        built = os.path.join(ROOT, "target", "Release", "rbminiflt.sys")
        if not (os.path.exists(syspath) and os.path.exists(built)):
            r.skip("deployed .sys SHA256 matches build output",
                   f"missing: {syspath} or {built}")
        else:
            import hashlib
            def sha(p):
                with open(p, "rb") as fh:
                    return hashlib.sha256(fh.read()).hexdigest()
            a, b = sha(syspath), sha(built)
            r.check("deployed .sys SHA256 matches build output", a == b,
                    f"deployed={a[:16]}... built={b[:16]}...\n"
                    "RB-27/28 教训：源码已修但产物未更新会伪造'已修复'")

    return r.summary()


if __name__ == "__main__":
    sys.exit(main())
