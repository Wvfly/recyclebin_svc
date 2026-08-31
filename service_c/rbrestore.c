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

/* ------------------------------------------------------------------ */
/* Recursive directory creation (RB-18)                                */
/* ------------------------------------------------------------------ */
/*
 * Windows deletes a directory tree one entry at a time, so restoring
 * D:\Share\proj\src\main.c also needs proj and proj\src to come back.
 * CreateDirectoryW creates a SINGLE level and fails when an ancestor is
 * missing, which made deep restores impossible: the MoveFileEx that
 * followed simply reported "path not found" and the file -- still sitting
 * safely in $Recycle.Bin -- could not be given back to the user.
 *
 * This walks the separators and creates each level in turn.
 * ERROR_ALREADY_EXISTS is treated as success, so the call is idempotent.
 */
static int EnsureDirectoryChain(const WCHAR *dir)
{
    WCHAR  buf[RBSVC_MAX_RECON_PATH];
    WCHAR *p;
    size_t len;

    if (!dir || dir[0] == L'\0') return 0;

    /* Drive-absolute only. DestIsAllowed() has already rejected UNC paths,
       device paths and ".." components before we get here. */
    if (dir[0] == L'\\' || dir[1] != L':') return 0;

    len = wcslen(dir);
    if (len < 3 || len >= ARRAYSIZE(buf)) return 0;
    if (len == 3) return 1;              /* drive root: always exists */

    memcpy(buf, dir, (len + 1) * sizeof(WCHAR));

    /* Drop trailing separators -- creating "D:\" is meaningless. */
    while (len > 3 && buf[len - 1] == L'\\')
        buf[--len] = L'\0';

    /* Create every level above the final component. */
    for (p = buf + 3; *p; p++) {
        if (*p != L'\\') continue;

        *p = L'\0';
        if (!CreateDirectoryW(buf, NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            *p = L'\\';
            return 0;
        }
        *p = L'\\';
    }

    /* Final component (already existing is fine). */
    if (!CreateDirectoryW(buf, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }
    return 1;
}
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

    /* Make sure the parent directory chain exists (RB-18). See
       EnsureDirectoryChain() above for why a single CreateDirectoryW is not
       enough here. Failure is not fatal on its own: the move below reports
       the real error, so just log it and let that be the user-visible one. */
    {
        WCHAR *slash = wcsrchr(dstDos, L'\\');
        if (slash && slash != dstDos) {
            size_t len = (size_t)(slash - dstDos);
            WCHAR *parent = (WCHAR *)malloc((len + 1) * sizeof(WCHAR));

            if (parent) {
                memcpy(parent, dstDos, len * sizeof(WCHAR));
                parent[len] = L'\0';

                if (!EnsureDirectoryChain(parent)) {
                    LogWarn(L"[restore] cannot create parent dir %s (win32=%lu)",
                            parent, GetLastError());
                }
                free(parent);
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

    /* RB-30: the $R container inside $Recycle.Bin carries HIDDEN|SYSTEM
       (recycle-bin standard bits); MoveFileExW preserves them, so a restored
       item is invisible to Explorer/SMB. Clear exactly those two bits.
       Clearing failure is a warning only - data is already back in place. */
    {
        DWORD attrs = GetFileAttributesW(dstDos);
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
            if (!SetFileAttributesW(
                    dstDos, attrs & ~(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
                LogWarn(L"[restore] cannot clear hidden/system attrs on %s (win32=%lu)",
                        dstDos, GetLastError());
            }
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
/* Tree restore                                                        */
/* ------------------------------------------------------------------ */

/*
 * Restore every live item whose original path starts with prefixDos.
 *
 * Why this exists: deleting a directory over SMB removes one entry at a time,
 * so `Project\src\main.c`, `Project\src` and `Project` each get their own
 * interception and their own row. The recycle bin then shows a flat pile of
 * scattered entries rather than the folder the user deleted, and getting the
 * tree back meant restoring them one by one (see docs/buglist.md RB-21b).
 *
 * Design notes:
 *
 *  - The prefix is validated against the protected shares with the same
 *    routine used for single restores (RB-06). This rename runs as SYSTEM, so
 *    an unvalidated prefix would be an arbitrary-write primitive, and the
 *    request reaches us through the shared `ops` table -- the queueing side is
 *    not a trusted caller.
 *  - Matching is a genuine path-prefix match, not a substring one:
 *    "D:\Share\Project" must not also pull in "D:\Share\ProjectBackup".
 *  - Partial success is reported rather than rolled back. Each entry is an
 *    independent rename; undoing the ones that already succeeded would be
 *    more surprising than leaving them and reporting what failed.
 *  - Stop is checked between entries so a long restore cannot delay shutdown.
 */
int RestoreTreeByPrefix(const WCHAR *prefixDos, WCHAR *msgBuf, DWORD cchMsg)
{
    WCHAR *prefixNt = NULL;
    RBSVC_ITEM *items = NULL;
    int n, i;
    int okCount = 0, failCount = 0;
    WCHAR firstErr[256];
    WCHAR detail[512];
    int rc;

    firstErr[0] = L'\0';

    if (!prefixDos || prefixDos[0] == L'\0') {
        if (msgBuf) swprintf_s(msgBuf, cchMsg, L"missing path prefix");
        return 0;
    }

    /* Same allow-list check as a single restore (RB-06). */
    if (!DestIsAllowed(prefixDos, detail, ARRAYSIZE(detail))) {
        LogWarn(L"[restore-tree] rejected prefix %s: %s", prefixDos, detail);
        if (msgBuf) swprintf_s(msgBuf, cchMsg, L"%s", detail);
        return 0;
    }

    /* orig_path is stored in NT form, so compare in that form. */
    prefixNt = VolDosToNt(prefixDos);
    if (!prefixNt) {
        if (msgBuf) swprintf_s(msgBuf, cchMsg,
                               L"cannot resolve volume for prefix");
        return 0;
    }

    n = DbListByOrigPathPrefix(prefixNt, &items, RBSVC_MAX_TREE_RESTORE);
    free(prefixNt);

    if (n < 0) {
        if (msgBuf) swprintf_s(msgBuf, cchMsg, L"query failed");
        return 0;
    }
    if (n == 0) {
        if (msgBuf) swprintf_s(msgBuf, cchMsg,
                               L"no restorable items under that prefix");
        return 0;
    }

    /* Report when the prefix matched more than we handle, so the caller knows
       the result is partial rather than assuming the whole tree came back. */
    if (n >= RBSVC_MAX_TREE_RESTORE) {
        LogWarn(L"[restore-tree] hit the %d item cap for %s; "
                L"narrow the prefix to restore the rest",
                RBSVC_MAX_TREE_RESTORE, prefixDos);
    }

    for (i = 0; i < n; i++) {
        WCHAR msg[256];
        int ok;

        if (WaitForSingleObject(g_StopEvent, 0) == WAIT_OBJECT_0) {
            LogWarn(L"[restore-tree] stopping early at %d/%d for %s",
                    i, n, prefixDos);
            break;
        }

        /* NULL override: every entry goes back to its own original path,
           which is what makes this reassemble the tree. */
        ok = RestoreItemById(items[i].Id, NULL, msg, ARRAYSIZE(msg));

        if (ok) {
            okCount++;
        } else {
            failCount++;
            if (firstErr[0] == L'\0') {
                swprintf_s(firstErr, ARRAYSIZE(firstErr),
                           L"id=%lld: %s", items[i].Id, msg);
            }
            LogWarn(L"[restore-tree] id=%lld failed: %s", items[i].Id, msg);
        }
    }

    DbFreeItemList(items, n);

    if (failCount == 0) {
        swprintf_s(detail, ARRAYSIZE(detail), L"restored %d/%d", okCount, n);
    } else {
        swprintf_s(detail, ARRAYSIZE(detail),
                   L"restored %d/%d; %d failed (first: %s)",
                   okCount, n, failCount, firstErr);
    }

    LogInfo(L"[restore-tree] %s under %s", detail, prefixDos);

    if (msgBuf) swprintf_s(msgBuf, cchMsg, L"%s", detail);

    rc = (failCount == 0 && okCount > 0) ? 1 : 0;
    return rc;
}

/* ------------------------------------------------------------------ */
/* ops queue drain -- commands issued by the Go REST service            */
/* ------------------------------------------------------------------ */

/* Called periodically by the ops thread. */
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
                       "missing op type (expected: restore, restore-tree)");
            continue;
        }

        if (_stricmp(ops[i].Type, "restore") == 0) {
            if (ops[i].Arg) argW = U8ToWLocal(ops[i].Arg);

            ok = RestoreItemById(ops[i].ItemId, argW, msg, ARRAYSIZE(msg));
            free(argW);

        } else if (_stricmp(ops[i].Type, "restore-tree") == 0) {
            /* The prefix lives in `arg`; item_id is unused (stored as 0). */
            if (!ops[i].Arg || ops[i].Arg[0] == '\0') {
                DbOpFinish(ops[i].Id, "failed",
                           "restore-tree requires a path prefix in arg");
                continue;
            }

            argW = U8ToWLocal(ops[i].Arg);

            ok = RestoreTreeByPrefix(argW, msg, ARRAYSIZE(msg));
            free(argW);

        } else {
            /* The `ops.type` column carries a CHECK constraint, so anything
               reaching here should already be valid. We still reject
               explicitly: a database written by an older or newer rbapi must
               not be silently ignored -- the caller deserves a reason. */
            char reason[256];
            sprintf_s(reason, sizeof(reason),
                      "unsupported op type '%s'; this build handles: "
                      "restore, restore-tree",
                      ops[i].Type);
            LogWarn(L"[ops] %S", reason);
            DbOpFinish(ops[i].Id, "failed", reason);
            continue;
        }

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
