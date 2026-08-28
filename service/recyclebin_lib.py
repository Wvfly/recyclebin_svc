"""
recyclebin_lib.py - 回收站落地与配额逻辑 (用户态)

职责:
  - 把暂存区 (\RBStore\Sid\...) 里的文件落地为标准 Windows 回收站条目:
      C:\\$Recycle.Bin\\<真实SID>\\$R<base36><ext>
      C:\\$Recycle.Bin\\<真实SID>\\$I<base36>  (元数据: 原路径/删除时间/大小)
  - 维护 SQLite 元数据 (recycle.db)
  - 配额 / 过期清理

注意: 内核态传来的是占位 SID "S-SESSION-<id>", 本模块用 WTS 把
session id 解析为真实用户 SID, 再定位其 $Recycle.Bin 子目录。
"""
import os
import sqlite3
import time
import base64
import struct
import ctypes
from ctypes import wintypes
import config

DB_PATH = os.path.join(config.store_root(), "recycle.db")

# ---------- SQLite ----------
def init_db():
    os.makedirs(config.store_root(), exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS items (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            orig_path   TEXT,
            store_path  TEXT,
            sid         TEXT,
            session_id  INTEGER,
            client_ip   TEXT,
            delete_time REAL,
            file_size   INTEGER,
            is_dir      INTEGER,
            status      TEXT,            -- staged | landed | restored | purged
            recycle_path TEXT
        )
    """)
    conn.commit()
    conn.close()

def add_item(orig_path, store_path, sid, session_id, file_size, is_dir, client_ip=""):
    conn = sqlite3.connect(DB_PATH)
    cur = conn.execute(
        "INSERT INTO items(orig_path,store_path,sid,session_id,client_ip,"
        "delete_time,file_size,is_dir,status) VALUES (?,?,?,?,?,?,?,?, 'staged')",
        (orig_path, store_path, sid, session_id, client_ip,
         time.time(), file_size, int(is_dir)))
    conn.commit()
    item_id = cur.lastrowid
    conn.close()
    return item_id

def set_landed(item_id, recycle_path):
    conn = sqlite3.connect(DB_PATH)
    conn.execute("UPDATE items SET status='landed', recycle_path=? WHERE id=?",
                 (recycle_path, item_id))
    conn.commit()
    conn.close()

def list_items(status=None, sid=None):
    conn = sqlite3.connect(DB_PATH)
    q = "SELECT id,orig_path,store_path,sid,session_id,client_ip," \
        "delete_time,file_size,is_dir,status,recycle_path FROM items"
    args = []
    where = []
    if status:
        where.append("status=?");
        args.append(status)
    if sid:
        where.append("sid=?");
        args.append(sid)
    if where:
        q += " WHERE " + " AND ".join(where)
    rows = conn.execute(q, args).fetchall()
    conn.close()
    cols = ["id","orig_path","store_path","sid","session_id","client_ip",
            "delete_time","file_size","is_dir","status","recycle_path"]
    return [dict(zip(cols, r)) for r in rows]

# ---------- SID 解析 (session -> SID) ----------
def session_to_sid(session_id):
    """用 WTSQuerySessionInformation 取会话用户 SID 字符串。"""
    try:
        wtsapi32 = ctypes.windll.wtsapi32
        buf = ctypes.c_void_p()
        bytelen = wintypes.DWORD()
        if wtsapi32.WTSQuerySessionInformationW(
                0, session_id, 16, ctypes.byref(buf), ctypes.byref(bytelen)):  # 16=WTSUserSid
            sid_str = ctypes.wstring_at(buf, bytelen.value // 2)
            ctypes.windll.kernel32.LocalFree(buf)
            return sid_str
    except Exception:
        pass
    return None

def sid_to_recycle_dir(sid_str):
    """返回 C:\\$Recycle.Bin\\<sid> 路径 (若不存在则创建)。"""
    drive = os.environ.get("SystemDrive", "C:")
    rb = os.path.join(drive, "$Recycle.Bin", sid_str)
    try:
        os.makedirs(rb, exist_ok=True)
    except Exception:
        pass
    return rb

# ---------- 落地为标准回收站条目 ----------
def _base36(n):
    chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    s = ""
    while n:
        s = chars[n % 36] + s
        n //= 36
    return s or "0"

def _build_i_file(orig_path, file_size, delete_time, is_dir):
    """构造 $Ixxxx 元数据文件内容 (Windows $I 格式, 二进制)。"""
    # 头部: 8 字节固定 + 4 标记 + ... 简化实现标准字段
    version = 1
    flags = 0
    # 删除时间 (FILETIME, 100ns since 1601)
    ft = int((delete_time + 11644473600) * 1e7)
    data = struct.pack("<Q", 1)               # 版本/头
    data += struct.pack("<Q", file_size)      # 文件大小
    data += struct.pack("<Q", ft)             # 删除时间 FILETIME
    data += struct.pack("<I", len(orig_path)) # 原路径长度(字符)
    data += orig_path.encode("utf-16-le")
    return data

def land_item(item):
    """把暂存文件移动到该用户的 $Recycle.Bin, 写 $R + $I。"""
    item_id = item["id"]
    store_path = item["store_path"]
    sid = item["sid"]
    if not os.path.exists(store_path):
        return False

    # 解析真实 SID (内核传的是 S-SESSION-x 占位, 需转换)
    real_sid = sid
    if sid.startswith("S-SESSION-"):
        sess = int(sid.split("-")[-1])
        real = session_to_sid(sess)
        if real:
            real_sid = real
    rb_dir = sid_to_recycle_dir(real_sid)

    # 生成 $R / $I 名称
    token = _base36(int(time.time() * 1000) + item_id)
    ext = os.path.splitext(store_path)[1]
    r_name = "$R" + token + ext
    i_name = "$I" + token + ext
    r_path = os.path.join(rb_dir, r_name)
    i_path = os.path.join(rb_dir, i_name)

    try:
        os.makedirs(os.path.dirname(r_path), exist_ok=True)
        os.replace(store_path, r_path)
    except Exception as e:
        print(f"[land] 移动失败 {store_path} -> {r_path}: {e}")
        return False

    # 写 $I 元数据 (把 NT 路径转回 DOS 形式用于展示)
    orig_dos = item["orig_path"]
    try:
        with open(i_path, "wb") as f:
            f.write(_build_i_file(orig_dos, item["file_size"],
                                  item["delete_time"], item["is_dir"]))
    except Exception:
        pass

    set_landed(item_id, r_path)
    return True

# ---------- 配额 / 清理 ----------
def enforce_quota(sid, quota_mb):
    rows = list_items(status="landed", sid=sid)
    total = sum((r["file_size"] or 0) for r in rows)
    if total > quota_mb * 1024 * 1024:
        # 超出: 按时间最旧优先清理
        rows.sort(key=lambda r: r["delete_time"])
        for r in rows:
            try:
                if r["recycle_path"] and os.path.exists(r["recycle_path"]):
                    os.remove(r["recycle_path"])
                ip = r["recycle_path"].replace("$R", "$I", 1)
                if os.path.exists(ip):
                    os.remove(ip)
            except Exception:
                pass
            _mark_purged(r["id"])
            total -= (r["file_size"] or 0)
            if total <= quota_mb * 1024 * 1024:
                break

def purge_expired(retention_days):
    now = time.time()
    cutoff = now - retention_days * 86400
    conn = sqlite3.connect(DB_PATH)
    rows = conn.execute(
        "SELECT id,recycle_path FROM items WHERE status='landed' AND delete_time<?",
        (cutoff,)).fetchall()
    for item_id, rp in rows:
        try:
            if rp and os.path.exists(rp):
                os.remove(rp)
            ip = rp.replace("$R", "$I", 1) if rp else None
            if ip and os.path.exists(ip):
                os.remove(ip)
        except Exception:
            pass
        conn.execute("UPDATE items SET status='purged' WHERE id=?", (item_id,))
    conn.commit()
    conn.close()

def _mark_purged(item_id):
    conn = sqlite3.connect(DB_PATH)
    conn.execute("UPDATE items SET status='purged' WHERE id=?", (item_id,))
    conn.commit()
    conn.close()
