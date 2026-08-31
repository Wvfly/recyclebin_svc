/*
 * rbrestore.c - Restore execution (this service performs ALL filesystem work)
 *
 * The Go REST service never touches the filesystem. It inserts a row into the
 * `ops` table; this module picks it up and performs the actual rename.
 *
 * Rationale:
 *   - Restore is a same-volume rename with several safety preconditions
 *     (status check, source existence, target collision, volume resolvability).
 *   - Keeping those checks in ONE process avoids duplicate logic and races
 *     between two processes mutating the same files.
 *   - The DB stays the single source of truth for "what happened".
 */

#include "rbsvc.h"

/* UTF-8 -> UTF-16 helper (local copy; rbdb.c keeps its own static one) */
static WCHAR *U8ToWLocal(const char *s)
{
    int n;
    WCHAR *buf;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    buf = (WCHAR *)malloc(((size_t)n + 1) * sizeof(WCHAR));
    if (!buf) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, n);
    return buf;
}

/*
 * RB-06: is this destination allowed to receive a restored file?
 *
 * This service runs as SYSTEM and performs the rename itself, so an
 * unvalidated destination is an arbitrary-write primitive: anyone able to
 * queue an op (that is, anyone holding the REST token) could restore a file
 * into C:\Windows\System32 or anywhere else on the box.
 *
 * The request does not arrive as a trusted in-process call -- it comes through
 * the shared `ops` table -- so the check has to live here, next to the rename,
 * and not in the caller that queued it.
 */
static int DestIsAllowed(const WCHAR *dstDos, WCHAR *reasonBuf, DWORD cch)
{
    DWORD i;
    int configured;

    if (!dstDos || dstDos[0] == L'\0') return 0;

    /* Drive-absolute only. This rejects UNC (\\server\share), device paths
       (\\?\, \\.\) and anything relative before we compare prefixes. */
    if (dstDos[0] == L'\\' || dstDos[1] != L':') {
        if (reasonBuf) swprintf_s(reasonBuf, cch,
            L"rejected: destination must be a drive-absolute path");
        return 0;
    }

    /* Reject '..' components: "D:\Share\..\..\Windows" passes a naive prefix
       test but escapes the share entirely. */
    {
        const WCHAR *p = dstDos;
        while (*p) {
            if (p[0] == L'.' && p[1] == L'.' && (p[2] == L'\\' || p[2] == L'\0')) {
                if (reasonBuf) swprintf_s(reasonBuf, cch,
                    L"rejected: destination must not contain '..'");
                return 0;
            }
            p++;
        }
    }

    configured = (g_Config.ProtectedPaths != NULL && g_Config.ProtectedCount > 0);
    if (!configured) {
        /* Fail closed: with no allow-list there is nothing to validate
           against, so refuse rather than perform a SYSTEM-level rename. */
        if (reasonBuf) swprintf_s(reasonBuf, cch,
            L"rejected: no protected paths configured");
        return 0;
    }

    for (i = 0; i < g_Config.ProtectedCount; i++) {
        const WCHAR *prefix = g_Config.ProtectedPaths[i];
        size_t plen;

        if (!prefix || prefix[0] == L'\0') continue;
        plen = wcslen(prefix);

        if (_wcsnicmp(dstDos, prefix, plen) != 0) continue;

        /* Must be the share itself or something beneath it, so that
           "D:\Share" does not also authorise "D:\ShareSecret\...". */
        if (dstDos[plen] == L'\0' || dstDos[plen] == L'\\') return 1;
    }

    if (reasonBuf) swprintf_s(reasonBuf, cch,
        L"rejected: destination is outside the protected shares");
    return 0;
}

/*
 * Restores item `itemId` back to its original path (or `argOverride` if given).
 * Returns 1 on success, 0 on failure; `msgBuf` receives a human-readable reason.
 */
int RestoreItemById(LONG64 itemId, const WCHAR *argOverride,
                    WCHAR *msgBuf, DWORD cchMsg)
{
    RBSVC_ITEM item;
    WCHAR *srcDos = NULL;
    WCHAR *dstDos = NULL;
    int ok = 0;

    if (msgBuf && cchMsg > 0) msgBuf[0] = L'\0';

    if (!DbGetItem(itemId, &item)) {
        if (msgBuf) swprintf_s(msgBuf, cchMsg, L"item %lld not found", itemId);
        return 0;
    }

    /* Only landed / staged items can be restored */
    if (_stricmp(item.Status, "landed") != 0 &&
        _stricmp(item.Status, "staged") != 0) {
        if (msgBuf) swprintf_s(msgBuf, cchMsg,
                               L"cannot restore: status=%S", item.Status);
        goto cleanup;
    }

    /* Source: $R file when landed, staging file when still staged */
    if (item.RecyclePath && item.RecyclePath[0]) {
        srcDos = _wcsdup(item.RecyclePath);       /* already DOS form */
    } else if (item.StorePath && item.StorePath[0]) {
        srcDos = VolNtToDos(item.StorePath);
    }
    if (!srcDos) {
        if (msgBuf) swprintf_s(msgBuf, cchMsg, L"cannot resolve source volume");
        goto cleanup;
    }

    if (GetFileAttributesW(srcDos) == INVALID_FILE_ATTRIBUTES) {
        if (msgBuf) swprintf_s(msgBuf, cchMsg, L"source file missing");
        goto cleanup;
    }

    /* Destination: override, else the recorded original path */
    if (argOverride && argOverride[0]) {
        dstDos = _wcsdup(argOverride);
    } else {
        dstDos = VolNtToDos(item.OrigPath);
    }
    if (!dstDos) {
        if (msgBuf) swprintf_s(msgBuf, cchMsg,
                               L"cannot resolve destination volume");
        goto cleanup;
    }

    /* RB-06: validate before ANY filesystem work. This rename runs as SYSTEM,
       so a caller-supplied destination outside the protected shares would be
       an arbitrary write. */
    if (!DestIsAllowed(dstDos, msgBuf, cchMsg)) {
        LogWarn(L"[restore] rejected destination for id=%lld: %s",
                itemId, dstDos);
        goto cleanup;
    }

    /* Refuse to clobber an existing file */
    if (GetFileAttributesW(dstDos) != INVALID_FILE_ATTRIBUTES) {
        if (msgBuf) swprintf_s(msgBuf, cchMsg, L"target already exists");
        goto cleanup;
    }

    /* Make sure the parent directory exists */
    {
        WCHAR *slash = wcsrchr(dstDos, L'\\');
        if (slash && slash != dstDos) {
            WCHAR parent[MAX_PATH];
            size_t len = (size_t)(slash - dstDos);
            if (len < MAX_PATH) {
                memcpy(parent, dstDos, len * sizeof(WCHAR));
                parent[len] = L'\0';
                /* CreateDirectoryW is recursive-ish only if parents exist;
                   use SHCreateDirectoryExW-equivalent via CreateDirectory loop */
                CreateDirectoryW(parent, NULL);
            }
        }
    }

    /* Same-volume rename; no copy allowed */
    if (!MoveFileExW(srcDos, dstDos,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        /* ERROR_NOT_SAME_DEVICE is the cross-volume case we explicitly refuse */
        if (msgBuf) {
            swprintf_s(msgBuf, cchMsg, L"move failed (win32=%lu)", err);
        }
        LogErrorWin(err, L"[restore] move failed %s -> %s", srcDos, dstDos);
        goto cleanup;
    }

    /* Remove the $I metadata counterpart when we restored a landed entry */
    if (item.RecyclePath && item.RecyclePath[0]) {
        WCHAR iPath[MAX_PATH];
        WCHAR *marker;
        wcsncpy_s(iPath, ARRAYSIZE(iPath), srcDos, _TRUNCATE);
        marker = wcsrchr(iPath, L'\\');
        if (marker && marker[1] == L'$' && marker[2] == L'R') {
            marker[2] = L'I';
            DeleteFileW(iPath);
        }
    }

    DbSetStatus(itemId, "restored");
    if (msgBuf) swprintf_s(msgBuf, cchMsg, L"ok");
    LogInfo(L"[restore] id=%lld -> %s", itemId, dstDos);
    ok = 1;

cleanup:
    free(srcDos);
    free(dstDos);
    DbFreeItem(&item);
    return ok;
}

/* ------------------------------------------------------------------ */
/* ops queue drain -- commands issued by the Go REST service            */
/* ------------------------------------------------------------------ */

/* Called periodically by the maintenance thread. */
int RestoreDrainOps(void)
{
    RBSVC_OP *ops = NULL;
    int n, i, handled = 0;

    n = DbOpsPending(&ops, 64);
    for (i = 0; i < n; i++) {
        WCHAR msg[512];
        char msgU8[1024];
        int ok;
        WCHAR *argW = NULL;

        if (!ops[i].Type || ops[i].Type[0] == '\0') {
            DbOpFinish(ops[i].Id, "failed",
                       "missing op type (expected one of: restore)");
            continue;
        }

        /* The `ops.type` column carries a CHECK constraint, so anything
           reaching here should already be valid. We still reject explicitly:
           a database written by an older/newer rbapi must not be silently
           ignored -- the caller deserves a reason. */
        if (_stricmp(ops[i].Type, "restore") != 0) {
            char reason[256];
            sprintf_s(reason, sizeof(reason),
                      "unsupported op type '%s'; this build handles: restore",
                      ops[i].Type);
            LogWarn(L"[ops] %S", reason);
            DbOpFinish(ops[i].Id, "failed", reason);
            continue;
        }

        if (ops[i].Arg) argW = U8ToWLocal(ops[i].Arg);

        ok = RestoreItemById(ops[i].ItemId, argW, msg, ARRAYSIZE(msg));
        free(argW);

        if (!WideCharToMultiByte(CP_UTF8, 0, msg, -1,
                                 msgU8, sizeof(msgU8), NULL, NULL)) {
            msgU8[0] = '\0';
        }

        DbOpFinish(ops[i].Id, ok ? "done" : "failed", msgU8);
        handled++;
    }

    DbFreeOps(ops, n);
    return handled;
}
