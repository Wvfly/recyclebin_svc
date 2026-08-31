/*
 * rbconfig.c - Registry configuration (HKLM\SOFTWARE\RecycleBin)
 *
 * Kept byte-compatible with the Python config.py so existing deployments and
 * the Go REST service read the same values.
 *
 * ProtectedPaths is REG_MULTI_SZ in DOS form here (the service's own copy);
 * the driver keeps a separate NT-form copy under its service Parameters key.
 * deploy.ps1 writes both.
 */

#include "rbsvc.h"

RBSVC_CONFIG g_Config;

static DWORD RegReadDword(HKEY key, const WCHAR *name, DWORD def)
{
    DWORD val = 0, type = 0, size = sizeof(val);
    LONG rc = RegQueryValueExW(key, name, NULL, &type,
                               (LPBYTE)&val, &size);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return def;
    return val;
}

static void RegReadString(HKEY key, const WCHAR *name,
                          WCHAR *out, DWORD cch, const WCHAR *def)
{
    DWORD type = 0, size = cch * sizeof(WCHAR);
    LONG rc;

    wcsncpy_s(out, cch, def, _TRUNCATE);

    rc = RegQueryValueExW(key, name, NULL, &type, (LPBYTE)out, &size);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        wcsncpy_s(out, cch, def, _TRUNCATE);
        return;
    }
    out[cch - 1] = L'\0';
}

/* Reads REG_MULTI_SZ into a freshly allocated array of strings. */
static WCHAR **RegReadMultiSz(HKEY key, const WCHAR *name, DWORD *countOut)
{
    WCHAR **result = NULL;
    DWORD type = 0, size = 0;
    LONG rc;
    WCHAR *blob = NULL;
    DWORD count = 0, i;
    const WCHAR *p;

    *countOut = 0;

    rc = RegQueryValueExW(key, name, NULL, &type, NULL, &size);
    if (rc != ERROR_SUCCESS || type != REG_MULTI_SZ || size == 0) return NULL;

    blob = (WCHAR *)malloc((size_t)size + sizeof(WCHAR));
    if (!blob) return NULL;

    rc = RegQueryValueExW(key, name, NULL, &type, (LPBYTE)blob, &size);
    if (rc != ERROR_SUCCESS) { free(blob); return NULL; }

    /* Count strings in the double-NUL-terminated block */
    p = blob;
    while (*p) { count++; p += wcslen(p) + 1; }
    if (count == 0) { free(blob); return NULL; }

    result = (WCHAR **)calloc((size_t)count, sizeof(WCHAR *));
    if (!result) { free(blob); return NULL; }

    p = blob;
    for (i = 0; i < count; i++) {
        size_t len = wcslen(p);
        result[i] = (WCHAR *)malloc((len + 1) * sizeof(WCHAR));
        if (result[i]) memcpy(result[i], p, (len + 1) * sizeof(WCHAR));
        p += len + 1;
    }

    free(blob);
    *countOut = count;
    return result;
}

void ConfigLoad(RBSVC_CONFIG *cfg)
{
    HKEY key = NULL;
    LONG rc;

    ZeroMemory(cfg, sizeof(*cfg));

    /* Defaults first -- registry absent is not an error */
    wcsncpy_s(cfg->StoreRoot, MAX_PATH, DEF_STORE_ROOT, _TRUNCATE);
    wcsncpy_s(cfg->PortName, ARRAYSIZE(cfg->PortName), DEF_PORT_NAME, _TRUNCATE);
    cfg->QuotaMB       = DEF_QUOTA_MB;
    cfg->RetentionDays = DEF_RETENTION_DAYS;
    cfg->DiskFreeMinMB = DEF_DISKFREE_MIN_MB;
    cfg->StagedBatch   = DEF_STAGED_BATCH;
    cfg->OrphanGraceDays = DEF_ORPHAN_GRACE_DAYS;
    cfg->TerminalKeepDays = DEF_TERMINAL_KEEP_DAYS;
    cfg->ProtectedCount = 0;
    cfg->ProtectedPaths = NULL;

    rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, RBSVC_REG_KEY, 0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        LogWarn(L"config key %s not found (win32=%ld); using defaults",
                RBSVC_REG_KEY, rc);
        /* Fall back to a single default protected path so we still do something */
        cfg->ProtectedPaths = (WCHAR **)calloc(1, sizeof(WCHAR *));
        if (cfg->ProtectedPaths) {
            cfg->ProtectedPaths[0] = _wcsdup(DEF_PROTECTED);
            cfg->ProtectedCount = 1;
        }
        return;
    }

    RegReadString(key, L"StoreRoot", cfg->StoreRoot, MAX_PATH, DEF_STORE_ROOT);
    RegReadString(key, L"PortName",  cfg->PortName, ARRAYSIZE(cfg->PortName),
                  DEF_PORT_NAME);

    cfg->QuotaMB       = RegReadDword(key, L"QuotaMB",       DEF_QUOTA_MB);
    cfg->RetentionDays = RegReadDword(key, L"RetentionDays", DEF_RETENTION_DAYS);
    cfg->DiskFreeMinMB = RegReadDword(key, L"DiskFreeMinMB", DEF_DISKFREE_MIN_MB);
    cfg->StagedBatch   = RegReadDword(key, L"StagedBatch",   DEF_STAGED_BATCH);
    cfg->OrphanGraceDays = RegReadDword(key, L"OrphanGraceDays",
                                        DEF_ORPHAN_GRACE_DAYS);
    cfg->TerminalKeepDays = RegReadDword(key, L"TerminalKeepDays",
                                         DEF_TERMINAL_KEEP_DAYS);

    cfg->ProtectedPaths = RegReadMultiSz(key, L"ProtectedPaths",
                                         &cfg->ProtectedCount);
    if (!cfg->ProtectedPaths || cfg->ProtectedCount == 0) {
        ConfigFree(cfg);
        cfg->ProtectedPaths = (WCHAR **)calloc(1, sizeof(WCHAR *));
        if (cfg->ProtectedPaths) {
            cfg->ProtectedPaths[0] = _wcsdup(DEF_PROTECTED);
            cfg->ProtectedCount = 1;
        }
    }

    RegCloseKey(key);

    LogInfo(L"config loaded: store=%s quota=%lu MB retention=%lu days "
            L"watermark=%lu MB protected=%lu",
            cfg->StoreRoot, cfg->QuotaMB, cfg->RetentionDays,
            cfg->DiskFreeMinMB, cfg->ProtectedCount);
}

/* RB-12: re-read the configuration while the service is running.
 *
 * Returns 1 if the new configuration was adopted, 0 if it was rejected.
 *
 * The new values are decoded into a scratch struct and only swapped in once
 * they look sane. Reading straight into g_Config would leave the service
 * running with half-applied settings if the registry were edited mid-write,
 * or with a pathological value such as RetentionDays = 0 that would start
 * deleting files the moment the next maintenance pass ran.
 */
int ReloadConfig(void)
{
    RBSVC_CONFIG next;
    RBSVC_CONFIG prev = g_Config;
    DWORD i;

    ConfigLoad(&next);

    /* Retention of 0 would purge everything on the next pass. */
    if (next.RetentionDays == 0) {
        LogError(L"[reload] rejected: RetentionDays must be >= 1");
        return 0;
    }

    /* A store root that does not exist yet is fine -- we create it -- but an
       empty value would put the database in the wrong place. */
    if (!next.StoreRoot || next.StoreRoot[0] == L'\0') {
        LogError(L"[reload] rejected: StoreRoot is empty");
        return 0;
    }

    /* Swap first, then release the old path list: ConfigLoad allocates a
       fresh array, so `next` never aliases `prev`'s pointers. */
    g_Config = next;
    if (prev.ProtectedPaths) {
        for (i = 0; i < prev.ProtectedCount; i++) free(prev.ProtectedPaths[i]);
        free(prev.ProtectedPaths);
    }

    LogInfo(L"[reload] configuration reloaded "
            L"(store=%s, retention=%lu d, quota=%lu MB, "
            L"orphan_grace=%lu d, terminal_keep=%lu d)",
            g_Config.StoreRoot, g_Config.RetentionDays, g_Config.QuotaMB,
            g_Config.OrphanGraceDays, g_Config.TerminalKeepDays);

    if (_wcsicmp(prev.StoreRoot, g_Config.StoreRoot) != 0) {
        /* Everything else takes effect on the next pass, but the database is
           opened once at startup, so moving it needs a restart. Say so rather
           than letting the operator believe the move succeeded. */
        LogWarn(L"[reload] StoreRoot changed but the database path is fixed at "
                L"startup; restart the service to move the store");
    }

    return 1;
}

void ConfigFree(RBSVC_CONFIG *cfg)
{
    DWORD i;
    if (!cfg) return;
    if (cfg->ProtectedPaths) {
        for (i = 0; i < cfg->ProtectedCount; i++) free(cfg->ProtectedPaths[i]);
        free(cfg->ProtectedPaths);
        cfg->ProtectedPaths = NULL;
    }
    cfg->ProtectedCount = 0;
}

/* Live pointer for modules that just need the store root */
const WCHAR *ConfigStoreRoot(void)
{
    return g_Config.StoreRoot;
}
