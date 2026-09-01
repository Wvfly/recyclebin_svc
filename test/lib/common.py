"""共用断言框架 (test/lib/common.py)

统一结果收集与汇总格式，供所有 Python 用例复用。
输出统一为 UTF-8：Windows 控制台默认 GBK，直接 print 中文会抛
UnicodeEncodeError（本项目实测已踩），因此入口处强制重配 stdout。
"""
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except AttributeError:
    pass  # Python < 3.7


class Results:
    """收集 pass/fail/skip，输出统一汇总。"""

    def __init__(self, title):
        self.title = title
        self.rows = []  # (name, state, detail)  state in PASS/FAIL/SKIP
        print("=" * 70)
        print(title)
        print("=" * 70)

    def check(self, name, passed, detail=""):
        state = "PASS" if passed else "FAIL"
        self._add(name, state, detail)
        return passed

    def skip(self, name, reason=""):
        """已知未修缺陷 / 环境不满足 —— 不计入失败，但必须显式记录原因。"""
        self._add(name, "SKIP", reason)

    def expect_fail(self, name, passed, bug, detail=""):
        """对应已知未修缺陷：断言"当前确实未通过"，用于固化待修项。

        若它意外通过（PASS），说明缺陷已修复，提示更新清单。
        """
        if passed:
            self._add(name, "PASS", f"{bug} 已修复? 请更新 buglist 与用例: {detail}")
        else:
            self._add(name, "SKIP", f"{bug} 未修（预期失败）: {detail}")

    def _add(self, name, state, detail):
        self.rows.append((name, state, detail))
        print(f"  [{state}] {name}")
        if detail:
            text = str(detail).strip()
            for line in text.splitlines()[:4]:
                print(f"         {line}")

    def section(self, title):
        print(f"\n--- {title} ---")

    def summary(self):
        p = sum(1 for _, s, _ in self.rows if s == "PASS")
        f = sum(1 for _, s, _ in self.rows if s == "FAIL")
        s = sum(1 for _, s, _ in self.rows if s == "SKIP")
        print("\n" + "-" * 70)
        print(f"RESULT {self.title}: {p} passed, {f} failed, {s} skipped "
              f"(total {len(self.rows)})")
        if self.rows and f == 0 and p > 0:
            print("STATUS: GREEN")
        elif f:
            print("STATUS: RED")
        else:
            print("STATUS: NO-RUN")
        print("-" * 70)
        return 1 if f else 0


def read_text(path):
    """读文本文件，容忍 BOM 与编码差异。"""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()
