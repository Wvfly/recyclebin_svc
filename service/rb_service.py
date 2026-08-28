"""
rb_service.py - RecycleBin for SMB 用户态服务主程序

功能:
  1. 连接内核通信端口 \\RecycleBinPort, 循环读取驱动异步通知
  2. 收到通知 -> 写 SQLite 元数据
  3. 整理线程: 暂存区 -> $Recycle.Bin (落地) + 配额 + 过期清理
  4. 可选 REST API (配置 EnableRestApi=1)

通信协议 (与驱动 rbminiflt.h 一致):
  RBF_NOTIFICATION (pragma pack 1):
    WCHAR  FilePath[32768]   off 0
    ULONG  PathLength        off 65536
    WCHAR  StorePath[32768]  off 65540
    ULONG  StorePathLength   off 131076
    ULONG64 FileSize         off 131080
    ULONG  SessionId         off 131088
    ULONG  IsDirectory       off 131092
    WCHAR  SidString[256]    off 131096
    total                     131096 + 512 = 131608

运行:
  python rb_service.py run      # 前台运行 (调试)
  python rb_service.py install  # 注册为 SYSTEM 服务 (需 pywin32 或 nssm)
  python rb_service.py start
"""
import os
import sys
import struct
import threading
import time
import config
import recyclebin_lib as rb

import ctypes
from ctypes import wintypes

# ---------- 通信端口 (fltlib.dll) ----------
FLT_PORT_NAME = config.CONFIG.get("PortName", "\\RecycleBinPort")
# 用 fltlib 的 FilterConnectCommunicationPort / FilterGetMessage
try:
    _flt = ctypes.windll.fltlib
    # 设置原型, 避免 32/64 位参数推断错误
    _flt.FilterConnectCommunicationPort.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD,
        ctypes.c_void_p, wintypes.WORD, ctypes.c_void_p,
        ctypes.POINTER(wintypes.HANDLE)]
    _flt.FilterConnectCommunicationPort.restype = wintypes.DWORD
    _flt.FilterGetMessage.argtypes = [
        wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p]
    _flt.FilterGetMessage.restype = wintypes.DWORD
    _flt.FilterClose.argtypes = [wintypes.HANDLE]
    _flt.FilterClose.restype = wintypes.DWORD
except Exception:
    _flt = None

NOTIFY_SIZE = 131608  # 见上方协议计算

def _decode_wchar(buf, offset, max_chars):
    data = buf[offset:offset + max_chars * 2]
    s = data.decode("utf-16-le", errors="ignore")
    return s.split("\x00", 1)[0]

def read_port_loop(stop_event):
    """连接通信端口并循环读取通知, 写入元数据。"""
    if _flt is None:
        print("[svc] 无法加载 fltlib.dll, 端口读取不可用")
        return
    port_handle = wintypes.HANDLE()
    status = _flt.FilterConnectCommunicationPort(
        FLT_PORT_NAME, 0, None, 0, None, ctypes.byref(port_handle))
    if status != 0:
        print(f"[svc] 连接端口失败, 状态码 {status:#x} (驱动是否已加载?)")
        return
    print("[svc] 已连接内核通信端口")

    buf = ctypes.create_string_buffer(NOTIFY_SIZE)
    while not stop_event.is_set():
        res = _flt.FilterGetMessage(port_handle, ctypes.byref(buf), NOTIFY_SIZE, None)
        if res != 0:
            if stop_event.is_set():
                break
            time.sleep(0.5)
            continue
        b = buf.raw
        orig_path = _decode_wchar(b, 0, 32768)
        store_path = _decode_wchar(b, 65540, 32768)
        file_size, = struct.unpack_from("<Q", b, 131080)
        session_id, = struct.unpack_from("<I", b, 131088)
        is_dir, = struct.unpack_from("<I", b, 131092)
        sid = _decode_wchar(b, 131096, 256)
        rb.add_item(orig_path, store_path, sid, session_id, file_size, is_dir)
        print(f"[svc] 拦截删除: {orig_path} -> {store_path} (session {session_id})")

    try:
        _flt.FilterClose(port_handle)
    except Exception:
        pass

def maintain_loop(stop_event):
    """周期整理: 落地 + 配额 + 过期清理。"""
    while not stop_event.is_set():
        try:
            # 1) 落地暂存区条目
            staged = rb.list_items(status="staged")
            for it in staged:
                rb.land_item(it)
            # 2) 配额与过期清理
            rb.purge_expired(config.CONFIG.get("RetentionDays", 30))
            for it in rb.list_items(status="landed"):
                rb.enforce_quota(it["sid"], config.CONFIG.get("QuotaMB", 5120))
        except Exception as e:
            print(f"[svc] 维护线程异常: {e}")
        stop_event.wait(30)  # 每 30s 一轮

# ---------- REST 管理 API (可选) ----------
def start_rest_api():
    from http.server import BaseHTTPRequestHandler, HTTPServer
    import json

    class H(BaseHTTPRequestHandler):
        def _json(self, obj, code=200):
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(obj).encode("utf-8"))

        def do_GET(self):
            if self.path == "/items":
                self._json(rb.list_items())
            elif self.path.startswith("/items/"):
                sid = self.path.split("/")[-1]
                self._json(rb.list_items(sid=sid))
            else:
                self._json({"status": "ok"}, 200)

        def do_POST(self):
            if self.path == "/config/reload":
                config.CONFIG.update(config.load_config())
                self._json({"ok": True})
            else:
                self._json({"error": "unknown"}, 404)

        def log_message(self, *a):
            pass

    port = config.CONFIG.get("RestApiPort", 8800)
    srv = HTTPServer(("127.0.0.1", port), H)
    print(f"[svc] REST API 监听 127.0.0.1:{port}")
    srv.serve_forever()

def main():
    rb.init_db()
    if len(sys.argv) > 1 and sys.argv[1] == "install":
        print("[svc] 请使用 nssm 或 sc 注册为 SYSTEM 服务, 例如:")
        print(f"  sc create RecycleBinSvc binPath= {os.path.abspath(sys.executable)} "
              f"{os.path.abspath(__file__)} run type= own start= auto obj= LocalSystem")
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
