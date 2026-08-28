"""
recyclebin_lib.py - 回收站落地、元数据与配额逻辑 (用户态)

职责:
  - 把暂存区 (\RBStore\<sid>\...) 里的文件落地到**同卷**标准回收站:
      <卷>:\\$Recycle.Bin\\<真实SID>\\$R<base36><ext>
      <卷>:\\$Recycle.Bin\\<真实SID>\\$I<base36>  (元数据: 原路径/删除时间/大小)
  - 维护 SQLite 元数据 (recycle.db, WAL + 索引 + 连接复用)
  - 配额 / 过期清理 / 磁盘水位 / 还原

规模设计 (TB 级 / 亿级文件 / 50 用户高频操作):
  - 落地目标固定为 store_path 所在卷的 $Recycle.Bin, 保证同卷 rename,
    彻底避免跨卷 copy 打爆 I/O。
  - SQLite: WAL + NORMAL + 组合索引, 配额用聚合 SQL 而非逐行全表扫描。
  - 所有 DB 访问经模块级锁串行化, 单写者模型, 避免锁风暴。

注意: 内核态直接发送请求者 Token 的**真实 SID** (S-1-5-...), 仅当拿不到
token 时才回退占位 SID "S-SESSION-<id>"; 后者由本模块用 WTS 解析为真实
SID, 再定位其 $Recycle.Bin 子目录。两种来源都统一去掉前导反斜杠。
"""
import os
import sqlite3
import time
import struct
import shutil
import string
import ctypes
import threading
from ctypes import wintypes
import config

DB_PATH = os.path.join(config.store_root(), "recycle.db")

# ============================================================
# SQLite: 单连接 + WAL + 索引, 模块级锁串行化 (单写者)
# ============================================================
_db_lock = threading.Lock()
_conn = None


def _get_conn():
    global _conn
    with _db_lock:
        if _conn is None:
            _conn = sqlite3.connect(DB_PATH, check_same_thread=False)
            _conn.execute("PRAGMA journal_mode=WAL")
            _conn.execute("PRAGMA synchronous=NORMAL")
            _conn.execute("PRAGMA busy_timeout=5000")
            _init_schema(_conn)
        return _conn


def _init_schema(conn):
    conn.execute("""
        CREATE TABLE IF NOT EXISTS items (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            orig_path    TEXT,
            store_path   TEXT,
            sid          TEXT,
            session_id   INTEGER,
            client_ip    TEXT,
            delete_time  REAL,
            file_size    INTEGER,
            is_dir       INTEGER,
            status       TEXT,            -- staged | landed | restored | purged
            recycle_path TEXT
        )
    """)
    conn.execute("CREATE INDEX IF NOT EXISTS idx_status ON items(status)")
    conn.execute("CREATE INDEX IF NOT EXISTS idx_status_sid ON items(status, sid)")
    conn.execute("CREATE INDEX IF NOT EXISTS idx_delete_time ON items(delete_time)")
    conn.commit()


def init_db():
    os.makedirs(config.store_root(), exist_ok=True)
    _get_conn()


def _commit(conn):
    try:
        conn.commit()
    except Exception:
        pass


# ============================================================
# 卷映射: NT 设备路径 <-> DOS 路径
# ============================================================
def _build_volume_map():
    mapping = {}
    try:
        kernel32 = ctypes.windll.kernel32
        buf = ctypes.create_unicode_buffer(1024)
        for letter in string.ascii_uppercase:
            drive = letter + ":"
            if kernel32.GetDriveTypeW(drive) in (2, 3, 6):  # removable/fixed/ramdisk
                if kernel32.QueryDosDeviceW(drive, buf, 1024):
                    dev = buf.value
                    if dev:
                        mapping[dev.lower()] = drive
    except Exception:
        pass
    return mapping


_VOLUME_MAP = _build_volume_map()


def nt_to_dos(nt_path):
    """\Device\HarddiskVolumeN\... -> D:\... (无匹配则原样返回)"""
    if not nt_path:
        return nt_path
    low = nt_path.lower()
    for dev, drive in sorted(_VOLUME_MAP.items(), key=lambda kv: -len(kv[0])):
        if low.startswith(dev):
            return drive + os.sep + nt_path[len(dev):].lstrip("\\")
    return nt_path


# ============================================================
# SQLite: CRUD
# ============================================================
def add_item(orig_path, store_path, sid, session_id, file_size, is_dir, client_ip=""):
    # 内核传来的 SID 带前导 '\' (作为路径分隔), 入库统一去掉, 便于 REST 查询
    if sid:
        sid = str(sid).lstrip("\\")
    with _db_lock:
        conn = _get_conn()
        cur = conn.execute(
            "INSERT INTO items(orig_path,store_path,sid,session_id,client_ip,"
            "delete_time,file_size,is_dir,status) VALUES (?,?,?,?,?,?,?,?, 'staged')",
            (orig_path, store_path, sid, session_id, client_ip,
             time.time(), file_size, int(is_dir)))
        _commit(conn)
        return cur.lastrowid


def set_landed(item_id, recycle_path):
    with _db_lock:
        conn = _get_conn()
        conn.execute("UPDATE items SET status='landed', recycle_path=? WHERE id=?",
                     (recycle_path, item_id))
        _commit(conn)


def _mark_status(item_id, status):
    with _db_lock:
        conn = _get_conn()
        conn.execute("UPDATE items SET status=? WHERE id=?", (status, item_id))
        _commit(conn)


def get_item(item_id):
    conn = _get_conn()
    row = conn.execute(
        "SELECT id,orig_path,store_path,sid,session_id,client_ip,delete_time,"
        "file_size,is_dir,status,recycle_path FROM items WHERE id=?",
        (item_id,)).fetchone()
    if not row:
        return None
    cols = ["id", "orig_path", "store_path", "sid", "session_id", "client_ip",
            "delete_time", "file_size", "is_dir", "status", "recycle_path"]
    return dict(zip(cols, row))


_ITEM_COLS = ["id", "orig_path", "store_path", "sid", "session_id", "client_ip",
              "delete_time", "file_size", "is_dir", "status", "recycle_path"]


def list_items(status=None, sid=None, limit=None, offset=0, order_by="id DESC"):
    """分页查询; limit/offset 控制返回规模, 避免亿级记录 OOM。"""
    conn = _get_conn()
    q = "SELECT id,orig_path,store_path,sid,session_id,client_ip,delete_time," \
        "file_size,is_dir,status,recycle_path FROM items"
    args = []
    where = []
    if status:
        where.append("status=?")
        args.append(status)
    if sid:
        where.append("sid=?")
        args.append(sid)
    if where:
        q += " WHERE " + " AND ".join(where)
    q += " ORDER BY " + order_by
    if limit:
        q += " LIMIT ? OFFSET ?"
        args += [int(limit), int(offset)]
    rows = conn.execute(q, args).fetchall()
    return [dict(zip(_ITEM_COLS, r)) for r in rows]


# ============================================================
# SID 解析 (session -> SID)
# ============================================================
def session_to_sid(session_id):
    """用 WTS 取会话用户 SID 并转为字符串 (二进制 SID 需 ConvertSidToStringSidW)。"""
    try:
        wtsapi32 = ctypes.windll.wtsapi32
        advapi32 = ctypes.windll.advapi32
        buf = ctypes.c_void_p()
        bytelen = wintypes.DWORD()
        if not wtsapi32.WTSQuerySessionInformationW(
                0, session_id, 16, ctypes.byref(buf), ctypes.byref(bytelen)):  # 16=WTSUserSid
            return None
        try:
            sid_buf = ctypes.c_void_p()
            if not advapi32.ConvertSidToStringSidW(buf, ctypes.byref(sid_buf)):
                return None
            try:
                return ctypes.wstring_at(sid_buf)
            finally:
                ctypes.windll.kernel32.LocalFree(sid_buf)
        finally:
            ctypes.windll.kernel32.LocalFree(buf)
    except Exception:
        return None


def _real_sid(sid):
    """统一 SID: 去掉前导 '\'; 占位 'S-SESSION-<id>' -> 真实 SID。"""
    sid = str(sid or "").lstrip("\\")
    if sid.startswith("S-SESSION-"):
        try:
            sess = int(sid.split("-")[-1])
            real = session_to_sid(sess)
            if real:
                return real.lstrip("\\")
        except Exception:
            pass
    return sid


def _recycle_root_for(store_dos):
    """落地目标所在卷的 $Recycle.Bin 根 (与 store_dos 同卷)。"""
    drive = os.path.splitdrive(store_dos)[0]  # 'D:'
    if not drive:
        drive = os.environ.get("SystemDrive", "C:")
    return os.path.join(drive + os.sep, "$Recycle.Bin")


def sid_to_recycle_dir(sid_str, store_dos):
    """返回 <store所在卷>\$Recycle.Bin\<sid> (不存在则创建, 隐藏+系统)。"""
    rb = os.path.join(_recycle_root_for(store_dos), sid_str)
    try:
        os.makedirs(rb, exist_ok=True)
        _set_hidden_system(rb)
    except Exception:
        pass
    return rb


def _set_hidden_system(path):
    try:
        FILE_ATTRIBUTE_HIDDEN = 0x2
        FILE_ATTRIBUTE_SYSTEM = 0x4
        ctypes.windll.kernel32.SetFileAttributesW(
            path, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)
    except Exception:
        pass


# ============================================================
# 落地为标准回收站条目 (同卷 rename)
# ============================================================
def _base36(n):
    chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    s = ""
    while n:
        s = chars[n % 36] + s
        n //= 36
    return s or "0"


def _build_i_file(orig_path, file_size, delete_time, is_dir):
    """构造 $Ixxxx 元数据文件内容 (Windows 标准 $I v1 格式)。

    布局 (与资源管理器/取证工具兼容):
      ULONG Version=1 | LARGE_INTEGER FileSize | LARGE_INTEGER FILETIME |
      ULONG PathLen(字符数) | WCHAR Path[] (UTF-16LE)
    """
    ft = int((delete_time + 11644473600) * 1e7)
    data = struct.pack("<IQQI", 1, file_size or 0, ft, len(orig_path))
    data += orig_path.encode("utf-16-le")
    return data


def land_item(item):
    """把暂存文件移动到**同卷** $Recycle.Bin, 写 $R + $I。

    返回 True/False。失败保持 staged 状态, 由下一轮重试。
    """
    item_id = item["id"]
    store_path = item["store_path"]
    if not store_path or not os.path.exists(store_path):
        return False

    store_dos = nt_to_dos(store_path)
    if len(os.path.splitdrive(store_dos)[0]) < 2:
        # 卷映射失败, 不能安全落地 (避免跨卷 copy), 留在暂存区
        print(f"[land] 无法解析卷: {store_path}")
        return False

    real_sid = _real_sid(item["sid"])
    rb_dir = sid_to_recycle_dir(real_sid, store_dos)

    token = _base36(int(time.time() * 1000) + item_id)
    ext = os.path.splitext(store_dos)[1]
    r_name = "$R" + token + ext
    i_name = "$I" + token + ext
    r_path = os.path.join(rb_dir, r_name)
    i_path = os.path.join(rb_dir, i_name)

    try:
        os.replace(store_dos, r_path)   # 同卷 rename, 原子且不产生拷贝
    except Exception as e:
        print(f"[land] 移动失败 {store_dos} -> {r_path}: {e}")
        return False

    orig_dos = nt_to_dos(item["orig_path"]) or item["orig_path"]
    try:
        with open(i_path, "wb") as f:
            f.write(_build_i_file(orig_dos, item["file_size"],
                                  item["delete_time"], item["is_dir"]))
        _set_hidden_system(r_path)
        _set_hidden_system(i_path)
    except Exception:
        pass

    set_landed(item_id, r_path)
    return True


# ============================================================
# 配额 / 过期清理 / 磁盘水位 (聚合 SQL, 无全表逐行扫描)
# ============================================================
def _delete_entry(item_id, recycle_path):
    """删除 $R/$I 文件并标记 purged。"""
    if recycle_path:
        try:
            if os.path.exists(recycle_path):
                os.remove(recycle_path)
            ip = recycle_path.replace("$R", "$I", 1)
            if os.path.exists(ip):
                os.remove(ip)
        except Exception:
            pass
    _mark_status(item_id, "purged")


def enforce_quota(quota_mb):
    """按 sid 聚合统计, 对超配用户按最旧优先清理。返回清理条目数。"""
    conn = _get_conn()
    quota = quota_mb * 1024 * 1024
    rows = conn.execute(
        "SELECT sid, SUM(COALESCE(file_size,0)) AS total FROM items "
        "WHERE status='landed' GROUP BY sid").fetchall()
    removed = 0
    for sid, total in rows:
        if not sid or (total or 0) <= quota:
            continue
        over = (total or 0) - quota
        old = conn.execute(
            "SELECT id, recycle_path, file_size FROM items "
            "WHERE status='landed' AND sid=? ORDER BY delete_time ASC",
            (sid,)).fetchall()
        for item_id, rp, size in old:
            if over <= 0:
                break
            _delete_entry(item_id, rp)
            over -= (size or 0)
            removed += 1
    return removed


def purge_expired(retention_days):
    """过期清理: 同时覆盖 landed(回收站) 与 staged(暂存区)。返回清理条目数。"""
    now = time.time()
    cutoff = now - retention_days * 86400
    conn = _get_conn()
    removed = 0
    rows = conn.execute(
        "SELECT id, recycle_path FROM items "
        "WHERE status='landed' AND delete_time<?", (cutoff,)).fetchall()
    for item_id, rp in rows:
        _delete_entry(item_id, rp)
        removed += 1
    rows = conn.execute(
        "SELECT id, store_path FROM items "
        "WHERE status='staged' AND delete_time<?", (cutoff,)).fetchall()
    for item_id, sp in rows:
        try:
            if sp and os.path.exists(sp):
                os.remove(sp)
        except Exception:
            pass
        _mark_status(item_id, "purged")
        removed += 1
    return removed


def _protected_volumes():
    """需要盯磁盘水位的卷: 所有受保护共享所在卷 + 元数据 StoreRoot 所在卷。"""
    vols = set()
    try:
        for p in (config.protected_paths() or []):
            d = os.path.splitdrive(str(p))[0]
            if d:
                vols.add(d + os.sep)
    except Exception:
        pass
    sr = os.path.splitdrive(config.store_root())[0]
    if sr:
        vols.add(sr + os.sep)
    return vols


def enforce_disk_watermark(min_free_mb=5120):
    """任一受保护卷(暂存区所在卷)剩余空间低于水位时, 按最旧 landed 清理。
    返回释放字节数。每个卷每轮最多处理 200 条, 避免一次删太多。"""
    min_free = min_free_mb * 1024 * 1024
    freed_total = 0
    for vol in _protected_volumes():
        try:
            usage = shutil.disk_usage(vol)
        except Exception:
            continue
        if usage.free >= min_free:
            continue
        freed = 0
        rows = list_items(status="landed", limit=200, order_by="delete_time ASC")
        for r in rows:
            if usage.free + freed >= min_free:
                break
            _delete_entry(r["id"], r["recycle_path"])
            freed += (r["file_size"] or 0)
        freed_total += freed
    return freed_total


# ============================================================
# 还原 / 健康状态
# ============================================================
def restore_item(item_id):
    """把回收站条目还原回原路径 (同卷 rename)。返回 (ok, msg)。"""
    item = get_item(item_id)
    if not item:
        return False, "not found"
    if item["status"] not in ("landed", "staged"):
        return False, "status=" + str(item["status"])
    src = item["recycle_path"] or item["store_path"]
    if not src or not os.path.exists(src):
        return False, "file missing"
    dst = nt_to_dos(item["orig_path"])
    if len(os.path.splitdrive(dst)[0]) < 2:
        return False, "cannot resolve volume: " + str(item["orig_path"])
    if os.path.exists(dst):
        return False, "target exists"
    try:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        os.replace(src, dst)
    except Exception as e:
        return False, str(e)
    _mark_status(item_id, "restored")
    return True, "ok"


def health_status():
    conn = _get_conn()
    counts = {}
    for s in ("staged", "landed", "purged", "restored"):
        try:
            counts[s] = conn.execute(
                "SELECT COUNT(*) FROM items WHERE status=?", (s,)).fetchone()[0]
        except Exception:
            counts[s] = 0
    total = conn.execute("SELECT COUNT(*) FROM items").fetchone()[0]
    try:
        usage = shutil.disk_usage(config.store_root())
        disk = {"free": usage.free, "used": usage.used, "total": usage.total}
    except Exception:
        disk = None
    return {
        "total_items": total,
        "counts": counts,
        "disk": disk,
        "db": DB_PATH,
        "ts": time.time(),
    }
