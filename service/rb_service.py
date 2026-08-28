"""
rb_service.py - RecycleBin for SMB 用户态服务主程序

功能:
  1. 连接内核通信端口 \\RecycleBinPort, 循环读取驱动异步通知
     (断线自动重连; 协议结构与驱动 rbminiflt.h 完全一致, 偏移由
      ctypes 计算, 杜绝手工算偏移错位)
  2. 收到通知 -> 写 SQLite 元数据
  3. 维护线程: 暂存区 -> 同卷 $Recycle.Bin (落地) + 配额 + 过期清理 + 磁盘水位
  4. 可选 REST API (EnableRestApi=1, Token 认证, 分页)

运行:
  python rb_service.py run      # 前台运行 (调试)
  python rb_service.py install  # 打印 sc 注册命令
"""

import os
import sys
import threading
import time
import config
import recyclebin_lib as rb

import ctypes
from ctypes import wintypes

# ============================================================
# 通信端口 (fltlib.dll) 与协议结构
# 结构字段与 driver/rbminiflt.h 完全一致; 偏移由 ctypes 计算
# ============================================================
FLT_PORT_NAME = config.CONFIG.get("PortName", "\\RecycleBinPort")
RBF_CMD_QUERY_STATS = 1


class FILTER_MESSAGE_HEADER(ctypes.Structure):
    _fields_ = [("ReplyLength", wintypes.ULONG), ("MessageId", wintypes.ULONG)]


class RBF_NOTIFICATION(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("FilePath", ctypes.c_wchar * 16383),
        ("PathLength", wintypes.ULONG),
        ("StorePath", ctypes.c_wchar * 16383),
        ("StorePathLength", wintypes.ULONG),
        ("FileSize", ctypes.c_uint64),
        ("SessionId", wintypes.ULONG),
        ("IsDirectory", wintypes.ULONG),
        ("SidString", ctypes.c_wchar * 256),
    ]


class RBF_REPLY(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("Ack", wintypes.ULONG)]


class RBF_STATS(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("Intercepts", ctypes.c_uint64),
        ("RenameOk", ctypes.c_uint64),
        ("RenameFail", ctypes.c_uint64),
        ("NotifySent", ctypes.c_uint64),
        ("NotifyDropped", ctypes.c_uint64),
        ("NotifyQueueFull", ctypes.c_uint64),
        ("QueueDepth", wintypes.ULONG),
        ("MaxQueueDepth", wintypes.ULONG),
    ]


try:
    _flt = ctypes.windll.fltlib
    _flt.FilterConnectCommunicationPort.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD,
        ctypes.c_void_p, wintypes.WORD, ctypes.c_void_p,
        ctypes.POINTER(wintypes.HANDLE)]
    _flt.FilterConnectCommunicationPort.restype = wintypes.DWORD
    _flt.FilterGetMessage.argtypes = [
        wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p]
    _flt.FilterGetMessage.restype = wintypes.DWORD
    _flt.FilterSendMessage.argtypes = [
        wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
        ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    _flt.FilterSendMessage.restype = wintypes.DWORD
    _flt.FilterClose.argtypes = [wintypes.HANDLE]
    _flt.FilterClose.restype = wintypes.DWORD
except Exception:
    _flt = None

NOTIFY_BUFFER = ctypes.sizeof(FILTER_MESSAGE_HEADER) + ctypes.sizeof(RBF_NOTIFICATION)

_port_lock = threading.Lock()
_PORT_HANDLE = None


def _connect_port():
    port_handle = wintypes.HANDLE()
    status = _flt.FilterConnectCommunicationPort(
        FLT_PORT_NAME, 0, None, 0, None, ctypes.byref(port_handle))
    if status != 0:
        return None
    return port_handle


def _read_notification(buf):
    """从 FilterGetMessage 缓冲区解析通知 (跳过 FILTER_MESSAGE_HEADER)。"""
    msg_addr = ctypes.addressof(buf) + ctypes.sizeof(FILTER_MESSAGE_HEADER)
    note = ctypes.cast(msg_addr, ctypes.POINTER(RBF_NOTIFICATION)).contents
    return {
        "orig_path": ctypes.wstring_at(ctypes.addressof(note.FilePath)),
        "store_path": ctypes.wstring_at(ctypes.addressof(note.StorePath)),
        "file_size": int(note.FileSize),
        "session_id": int(note.SessionId),
        "is_dir": int(note.IsDirectory),
        "sid": ctypes.wstring_at(ctypes.addressof(note.SidString)),
    }


def read_port_loop(stop_event):
    """连接通信端口并循环读取通知; 断线自动重连。"""
    if _flt is None:
        print("[svc] 无法加载 fltlib.dll, 端口读取不可用")
        return
    global _PORT_HANDLE
    port_handle = None

    while not stop_event.is_set():
        if port_handle is None:
            port_handle = _connect_port()
            if port_handle is None:
                print("[svc] 连接端口失败, 5s 后重试 (驱动加载中?)")
                stop_event.wait(5)
                continue
            print("[svc] 已连接内核通信端口")
            with _port_lock:
                _PORT_HANDLE = port_handle

        buf = ctypes.create_string_buffer(NOTIFY_BUFFER)
        res = _flt.FilterGetMessage(port_handle, ctypes.byref(buf),
                                    NOTIFY_BUFFER, None)
        if res != 0:
            if stop_event.is_set():
                break
            print(f"[svc] FilterGetMessage 错误 {res:#x}, 重连...")
            try:
                _flt.FilterClose(port_handle)
            except Exception:
                pass
            with _port_lock:
                _PORT_HANDLE = None
            port_handle = None
            stop_event.wait(2)
            continue

        try:
            n = _read_notification(buf)
            rb.add_item(n["orig_path"], n["store_path"], n["sid"],
                        n["session_id"], n["file_size"], n["is_dir"])
            print(f"[svc] 拦截删除: {n['orig_path']} -> {n['store_path']} "
                  f"(session {n['session_id']})")
        except Exception as e:
            print(f"[svc] 处理通知异常: {e}")

    if port_handle is not None:
        try:
            _flt.FilterClose(port_handle)
        except Exception:
            pass
        with _port_lock:
            _PORT_HANDLE = None


def query_stats():
    """向驱动查询统计 (RBF_CMD_QUERY_STATS), 失败返回 None。"""
    with _port_lock:
        h = _PORT_HANDLE
    if not h or _flt is None:
        return None
    req = RBF_REPLY()
    req.Ack = RBF_CMD_QUERY_STATS
    out = RBF_STATS()
    ret_len = wintypes.DWORD(0)
    res = _flt.FilterSendMessage(h, ctypes.byref(req), ctypes.sizeof(RBF_REPLY),
                                 ctypes.byref(out), ctypes.sizeof(RBF_STATS),
                                 ctypes.byref(ret_len))
    if res != 0:
        return None
    return {
        "intercepts": out.Intercepts,
        "rename_ok": out.RenameOk,
        "rename_fail": out.RenameFail,
        "notify_sent": out.NotifySent,
        "notify_dropped": out.NotifyDropped,
        "notify_queue_full": out.NotifyQueueFull,
        "queue_depth": out.QueueDepth,
        "max_queue_depth": out.MaxQueueDepth,
    }


def maintain_loop(stop_event):
    """周期整理: 落地(分页) + 过期 + 磁盘水位 + 配额(聚合)。"""
    while not stop_event.is_set():
        try:
            batch = max(1, int(config.CONFIG.get("StagedBatch", 500)))
            # 1) 落地暂存区条目 (分页处理, 避免一次全表)
            while True:
                staged = rb.list_items(status="staged", limit=batch,
                                       order_by="delete_time ASC")
                if not staged:
                    break
                for it in staged:
                    rb.land_item(it)
                if len(staged) < batch:
                    break
            # 2) 过期清理 (landed + staged)
            rb.purge_expired(config.CONFIG.get("RetentionDays", 30))
            # 3) 磁盘水位
            rb.enforce_disk_watermark(config.CONFIG.get("DiskFreeMinMB", 5120))
            # 4) 配额 (聚合 SQL)
            rb.enforce_quota(config.CONFIG.get("QuotaMB", 5120))
        except Exception as e:
            print(f"[svc] 维护线程异常: {e}")
        stop_event.wait(30)


# ============================================================
# REST 管理 API (可选): Token 认证 + 分页 + 健康/统计/还原
# ============================================================
def start_rest_api():
    from http.server import BaseHTTPRequestHandler, HTTPServer
    from urllib.parse import urlparse, parse_qs
    import json

    class H(BaseHTTPRequestHandler):
        def _auth(self):
            # 每次动态读取: /config/reload 后新 token 立即生效
            token = config.CONFIG.get("RestApiToken", "")
            if not token:
                return True
            return self.headers.get("X-Auth-Token", "") == token

        def _json(self, obj, code=200):
            body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _read_body(self):
            try:
                ln = int(self.headers.get("Content-Length", 0))
            except ValueError:
                return {}
            if ln <= 0:
                return {}
            try:
                return json.loads(self.rfile.read(ln).decode("utf-8"))
            except Exception:
                return {}

        def do_GET(self):
            if not self._auth():
                self._json({"error": "unauthorized"}, 401)
                return
            parsed = urlparse(self.path)
            path = parsed.path
            qs = parse_qs(parsed.query)

            if path == "/items":
                limit = int(qs.get("limit", ["100"])[0])
                offset = int(qs.get("offset", ["0"])[0])
                status = qs.get("status", [None])[0]
                sid = qs.get("sid", [None])[0]
                items = rb.list_items(status=status, sid=sid,
                                      limit=min(max(limit, 1), 1000), offset=offset)
                self._json({"count": len(items), "items": items})
            elif path.startswith("/items/") and len(path) > 7:
                try:
                    item_id = int(path.rsplit("/", 1)[-1])
                except ValueError:
                    self._json({"error": "bad id"}, 400)
                    return
                it = rb.get_item(item_id)
                if not it:
                    self._json({"error": "not found"}, 404)
                else:
                    self._json(it)
            elif path == "/health":
                st = rb.health_status()
                st["driver"] = query_stats()
                self._json(st)
            elif path == "/stats":
                st = query_stats()
                self._json(st if st is not None else {"error": "driver offline"}, 503 if st is None else 200)
            else:
                self._json({"error": "unknown"}, 404)

        def do_POST(self):
            if not self._auth():
                self._json({"error": "unauthorized"}, 401)
                return
            path = urlparse(self.path).path
            if path == "/restore":
                body = self._read_body()
                item_id = body.get("id")
                if not item_id:
                    self._json({"error": "id required"}, 400)
                    return
                ok, msg = rb.restore_item(int(item_id))
                self._json({"ok": ok, "msg": msg}, 200 if ok else 409)
            elif path == "/config/reload":
                config.CONFIG.update(config.load_config())
                self._json({"ok": True})
            else:
                self._json({"error": "unknown"}, 404)

        def log_message(self, *a):
            pass

    port = int(config.CONFIG.get("RestApiPort", 8800))
    srv = HTTPServer(("127.0.0.1", port), H)
    print(f"[svc] REST API 监听 127.0.0.1:{port}")
    srv.serve_forever()


def main():
    rb.init_db()
    if len(sys.argv) > 1 and sys.argv[1] == "install":
        print("[svc] 注册为 SYSTEM 服务 (管理员):")
        print(f"  sc create RecycleBinSvc binPath= \"{os.path.abspath(sys.executable)}\" "
              f"\"{os.path.abspath(__file__)}\" run type= own start= auto obj= LocalSystem")
        return

    stop = threading.Event()
    t_port = threading.Thread(target=read_port_loop, args=(stop,), daemon=True)
    t_maint = threading.Thread(target=maintain_loop, args=(stop,), daemon=True)
    t_port.start()
    t_maint.start()

    if config.CONFIG.get("EnableRestApi", 0):
        threading.Thread(target=start_rest_api, daemon=True).start()

    print("[svc] RecycleBin 服务运行中 (Ctrl+C 退出)")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        stop.set()
        print("[svc] 正在停止...")


if __name__ == "__main__":
    main()
