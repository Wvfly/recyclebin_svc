"""
config.py - 配置加载 (注册表优先, 回退到内置默认值)
所有路径/阈值集中在此，方便运维调整而无需重编驱动。
"""
import winreg
import os

REG_KEY = r"SOFTWARE\RecycleBin"

DEFAULTS = {
    "StoreRoot": r"C:\RBStore",
    "QuotaMB": 5120,            # 每用户配额(MB)
    "RetentionDays": 30,        # 保留天数
    "EnableRestApi": 0,         # 是否开启管理 API
    "RestApiPort": 8800,        # 管理 API 端口
    "PortName": r"\RecycleBinPort",
    # 受保护共享根 (NT 风格或 DOS 风格均可, 驱动内部归一)
    "ProtectedPaths": [r"D:\Share", r"E:\Public"],
}

def _read_reg_multi(key, name):
    try:
        val, typ = winreg.QueryValueEx(key, name)
        if typ == winreg.REG_MULTI_SZ:
            return [v for v in val if v]
    except OSError:
        pass
    return None

def _read_reg_str(key, name):
    try:
        val, _ = winreg.QueryValueEx(key, name)
        return str(val)
    except OSError:
        return None

def _read_reg_dword(key, name):
    try:
        val, _ = winreg.QueryValueEx(key, name)
        return int(val)
    except OSError:
        return None

def load_config():
    cfg = dict(DEFAULTS)
    try:
        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, REG_KEY, 0, winreg.KEY_READ)
    except OSError:
        return cfg

    pp = _read_reg_multi(key, "ProtectedPaths")
    if pp:
        cfg["ProtectedPaths"] = pp
    v = _read_reg_str(key, "StoreRoot")
    if v:
        cfg["StoreRoot"] = v
    for name in ("QuotaMB", "RetentionDays", "EnableRestApi", "RestApiPort"):
        d = _read_reg_dword(key, name)
        if d is not None:
            cfg[name] = d
    return cfg

# 供其它模块直接 import
CONFIG = load_config()

def protected_paths():
    return CONFIG.get("ProtectedPaths", [])

def store_root():
    return CONFIG.get("StoreRoot", r"C:\RBStore")
