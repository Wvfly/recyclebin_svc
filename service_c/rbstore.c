/*
 * rbstore.c - Staging -> $Recycle.Bin landing, $I metadata, entry deletion
 *
 * Landing is a SAME-VOLUME rename (MoveFileEx without MOVEFILE_COPY_ALLOWED):
 * atomic, zero-copy, instant. This is why StoreRoot must live on the same
 * volume as the protected share -- the kernel's rename has the same constraint.
 *
 * Output format is the standard Windows Recycle Bin layout so Explorer can
 * enumerate and restore natively:
 *
 *   <vol>\$Recycle.Bin\<SID>\$R<base36><ext>    file data
 *   <vol>\$Recycle.Bin\<SID>\$I<base36><ext>    metadata (v2 format)
 *
 * $I v2 layout (per the libyal Windows Recycle.Bin spec; verified against a
 * native Explorer-generated $I byte-for-byte -- see RB-39 in docs/buglist.md):
 *   offset 0x00  INT64   Version     = 2               (8 bytes, NOT 4)
 *   offset 0x08  INT64   FileSize
 *   offset 0x10  INT64   DeleteTime  (FILETIME, 100ns since 1601-01-01)
 *   offset 0x18  ULONG   PathLen     (WCHAR count, INCLUDING the terminating
 *                                     NUL -- Explorer's own writer counts it)
 *   offset 0x1C  WCHAR[] Original path, UTF-16LE, NUL-terminated
 *
 * Version was previously written as a 4-byte ULONG (RB-17/RB-39): that shifts
 * every field after it 4 bytes early, so Explorer reads garbage for FileSize/
 * DeleteTime/PathLen and silently drops the entry -- the file and its $I both
 * exist on disk, the DB says 'landed', but the entry never appears in the
 * desktop Recycle Bin. Restore via this service still worked throughout,
 * since restore reads the DB, not the $I file.
 */

#include "rbsvc.h"

/* ------------------------------------------------------------------ */
/* Base36 token, mirrors the Python implementation                     */
/* ------------------------------------------------------------------ */

static void Base36(unsigned long long v, WCHAR *out, size_t cch)
{
    static const WCHAR digits[] = L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    WCHAR tmp[32];
    int n = 0, i = 0;

    if (v == 0) tmp[n++] = L'0';
    while (v) { tmp[n++] = digits[v % 36]; v /= 36; }

    /* reverse */
    if (n >= (int)cch) n = (int)cch - 1;
    while (n > 0) out[i++] = tmp[--n];
    out[i] = L'\0';
}

/* ------------------------------------------------------------------ */
/* Hidden + system attributes                                          */
/* ------------------------------------------------------------------ */

static void SetHiddenSystem(const WCHAR *path)
{
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    SetFileAttributesW(path, attr | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
}

/* ------------------------------------------------------------------ */
/* $Recycle.Bin directory for a SID                                    */
/* ------------------------------------------------------------------ */

/* Builds <drive>\$Recycle.Bin\<sid> and creates it if missing.
   driveOfStore: DOS path of the staging file (used to pick the volume). */
static int BuildRecycleDir(const WCHAR *sid, const WCHAR *storeDos,
                           WCHAR *out, size_t cch)
{
    WCHAR drive[8];

    if (!VolDriveOf(storeDos, drive, ARRAYSIZE(drive))) {
        /* Fall back to system drive when the volume cannot be resolved */
        wcsncpy_s(drive, ARRAYSIZE(drive), L"C:\\", _TRUNCATE);
    }

    if (swprintf_s(out, cch, L"%s$Recycle.Bin\\%s", drive, sid) < 0)
        return 0;

    CreateDirectoryW(out, NULL);
    SetHiddenSystem(out);
    return 1;
}

/* ------------------------------------------------------------------ */
/* $I metadata                                                         */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)
typedef struct _RB_I_HEADER {
    LONG64  Version;      /* 2 -- MUST be 8 bytes, see file header comment */
    LONG64  FileSize;
    LONG64  DeleteTime;   /* FILETIME */
    ULONG   PathLen;      /* WCHAR count, INCLUDING the terminating NUL */
} RB_I_HEADER;
#pragma pack(pop)

/* unix seconds -> FILETIME (100ns ticks since 1601-01-01) */
static LONG64 UnixToFileTime(double unixSeconds)
{
    /* seconds between 1601-01-01 and 1970-01-01 */
    const LONG64 kEpochOffset = 11644473600LL;
    return (LONG64)((unixSeconds + (double)kEpochOffset) * 1e7);
}

static int WriteIFile(const WCHAR *iPath, const WCHAR *origDos,
                      LONG64 fileSize, double deleteTime)
{
    HANDLE h;
    RB_I_HEADER hdr;
    DWORD written;
    DWORD charsNoNul;
    DWORD charsWithNul; /* PathLen per spec: character count INCLUDING NUL */
    BOOL ok = TRUE;

    h = CreateFileW(iPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    charsNoNul   = (DWORD)wcslen(origDos);
    charsWithNul = charsNoNul + 1;

    hdr.Version    = 2;
    hdr.FileSize   = fileSize;
    hdr.DeleteTime = UnixToFileTime(deleteTime);
    hdr.PathLen    = charsWithNul;

    ok &= WriteFile(h, &hdr, sizeof(hdr), &written, NULL);
    /* Write the path AND its terminating NUL -- charsWithNul WCHARs total,
       matching the PathLen we just wrote. Explorer's own $I writer does the
       same; a mismatch here is exactly what makes RB-17/RB-39 recur. */
    ok &= WriteFile(h, origDos, charsWithNul * sizeof(WCHAR), &written, NULL);

    CloseHandle(h);
    if (!ok) { DeleteFileW(iPath); return 0; }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Landing                                                             */
/* ------------------------------------------------------------------ */

int StoreLandItem(const RBSVC_ITEM *item)
{
    WCHAR *storeDos = NULL;
    WCHAR *origDos  = NULL;
    WCHAR *realSid  = NULL;
    WCHAR  rbDir[MAX_PATH];
    WCHAR  rPath[MAX_PATH];
    WCHAR  iPath[MAX_PATH];
    WCHAR  token[32];
    WCHAR  drive[8];
    const WCHAR *ext;
    int result = 0;

    if (!item || !item->StorePath) return 0;

    /* NT -> DOS; bail out if unresolvable (never risk a cross-volume copy) */
    storeDos = VolNtToDos(item->StorePath);
    if (!storeDos) {
        LogWarn(L"[land] cannot resolve volume for %s", item->StorePath);
        goto cleanup;
    }
    if (!VolDriveOf(storeDos, drive, ARRAYSIZE(drive))) {
        LogWarn(L"[land] no drive letter for %s", storeDos);
        goto cleanup;
    }
    if (GetFileAttributesW(storeDos) == INVALID_FILE_ATTRIBUTES) {
        /* Staging file gone (already handled?) -- drop the record */
        LogWarn(L"[land] staging file missing: %s", storeDos);
        goto cleanup;
    }

    realSid = SidNormalize(item->Sid);
    if (!realSid || realSid[0] == L'\0') {
        LogWarn(L"[land] cannot resolve SID for item %lld", item->Id);
        goto cleanup;
    }

    if (!BuildRecycleDir(realSid, storeDos, rbDir, ARRAYSIZE(rbDir))) {
        LogError(L"[land] cannot create recycle dir for %s", realSid);
        goto cleanup;
    }

    /* Token: milliseconds + row id, base36. Unique across restarts. */
    {
        FILETIME ft;
        ULARGE_INTEGER uli;
        GetSystemTimeAsFileTime(&ft);
        uli.LowPart  = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        Base36(uli.QuadPart / 10000ULL + (unsigned long long)item->Id,
               token, ARRAYSIZE(token));
    }

    /* Preserve the extension so double-click associations still work */
    ext = wcsrchr(storeDos, L'.');
    if (!ext) ext = L"";

    if (swprintf_s(rPath, ARRAYSIZE(rPath), L"%s\\$R%s%s",
                   rbDir, token, ext) < 0) goto cleanup;
    if (swprintf_s(iPath, ARRAYSIZE(iPath), L"%s\\$I%s%s",
                   rbDir, token, ext) < 0) goto cleanup;

    /* Same-volume rename. No MOVEFILE_COPY_ALLOWED: fail rather than copy. */
    if (!MoveFileExW(storeDos, rPath,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LogErrorWin(GetLastError(), L"[land] move failed %s -> %s",
                    storeDos, rPath);
        goto cleanup;
    }

    /* Resolve the DOS form once and persist it: the $I metadata needs it, and
       storing it lets the Go API render readable paths without re-deriving the
       NT->DOS volume mapping. */
    origDos = VolNtToDos(item->OrigPath);
    if (!WriteIFile(iPath, origDos ? origDos : item->OrigPath,
                    item->FileSize, item->DeleteTime)) {
        LogError(L"[land] cannot write metadata %s", iPath);
    }

    SetHiddenSystem(rPath);
    SetHiddenSystem(iPath);

    DbSetLanded(item->Id, rPath, origDos ? origDos : L"");
    LogInfo(L"[land] id=%lld -> %s", item->Id, rPath);
    result = 1;

cleanup:
    free(storeDos);
    free(origDos);
    free(realSid);
    return result;
}

/* ------------------------------------------------------------------ */
/* Migration: rebuild $I files that were written with the pre-fix       */
/* 4-byte Version field (RB-17/RB-39). $R is untouched -- only the      */
/* $I sidecar is regenerated, from the DB row, in the corrected format. */
/* Idempotent: WriteIFile always overwrites (CREATE_ALWAYS), so this    */
/* is safe to run more than once (e.g. after a partial failure).        */
/* ------------------------------------------------------------------ */

static int RebuildIFileFor(const RBSVC_ITEM *item)
{
    WCHAR iPath[MAX_PATH];
    WCHAR *found;
    const WCHAR *origDos;

    if (!item || !item->RecyclePath || !item->RecyclePath[0]) return 0;

    /* $I counterpart of the $R file: same name, $R -> $I (first hit only) */
    wcsncpy_s(iPath, ARRAYSIZE(iPath), item->RecyclePath, _TRUNCATE);
    found = wcsrchr(iPath, L'\\');
    if (!found || found[1] != L'$' || found[2] != L'R') {
        LogWarn(L"[rebuild-i] id=%lld recycle_path not in $R form: %s",
                item->Id, item->RecyclePath);
        return 0;
    }
    found[2] = L'I';

    /* Prefer the DOS form resolved at landing time; fall back to the raw
       NT path rather than skip the item outright. */
    origDos = (item->OrigPathDos && item->OrigPathDos[0])
                  ? item->OrigPathDos
                  : (item->OrigPath ? item->OrigPath : L"");

    if (!WriteIFile(iPath, origDos, item->FileSize, item->DeleteTime)) {
        LogError(L"[rebuild-i] id=%lld cannot write %s", item->Id, iPath);
        return 0;
    }
    SetHiddenSystem(iPath);
    return 1;
}

/* Rewrites every currently-landed item's $I file. Returns the number that
   were successfully rewritten; logs a warning per item that was skipped
   (e.g. already purged, or a malformed recycle_path). */
int StoreRebuildAllIFiles(void)
{
    RBSVC_ITEM *items = NULL;
    int count, ok = 0, i;

    /* DbListExpired(cutoff) returns every 'landed' row with delete_time <
       cutoff; a far-future cutoff makes that "every landed row", with no
       LIMIT truncation (unlike DbListLandedOldest). */
    count = DbListExpired(&items, 253402300800.0 /* ~year 9999 */);

    for (i = 0; i < count; i++) {
        if (RebuildIFileFor(&items[i])) ok++;
    }

    LogInfo(L"[rebuild-i] rewrote %d/%d $I files", ok, count);
    if (items) DbFreeItemList(items, count);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Deletion                                                            */
/* ------------------------------------------------------------------ */

void StoreDeleteEntry(const RBSVC_ITEM *item)
{
    WCHAR *dos = NULL;
    const WCHAR *primary;
    WCHAR iPath[MAX_PATH];
    WCHAR *found;

    if (!item) return;

    /* Prefer the landed $R file; fall back to the staging path */
    primary = (item->RecyclePath && item->RecyclePath[0])
                  ? item->RecyclePath : item->StorePath;
    if (!primary || !primary[0]) return;

    dos = (item->RecyclePath && item->RecyclePath[0])
              ? _wcsdup(item->RecyclePath)          /* already DOS form */
              : VolNtToDos(primary);
    if (!dos) return;

    /* RB-08: a directory delete is staged as a whole subtree, so reclaiming
       it needs RemoveDirectoryW -- DeleteFileW always fails on a directory
       and would leave the tree in staging forever. Try the directory form
       first; it fails harmlessly on files. */
    if (!RemoveDirectoryW(dos)) {
        DWORD err = GetLastError();

        if (err == ERROR_DIRECTORY_NOT_SUPPORTED ||
            err == ERROR_FILE_NOT_FOUND ||
            err == ERROR_PATH_NOT_FOUND ||
            err == ERROR_DIRECTORY) {
            /* Not a directory, or already gone: fall through to file delete. */
        } else {
            LogErrorWin(err, L"[purge] remove directory failed %s", dos);
        }
    }

    if (!DeleteFileW(dos)) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND &&
            err != ERROR_ACCESS_DENIED) {
            LogErrorWin(err, L"[purge] delete failed %s", dos);
        }
    }

    /* $I counterpart: same name with $R -> $I (first occurrence only) */
    wcsncpy_s(iPath, ARRAYSIZE(iPath), dos, _TRUNCATE);
    found = wcsrchr(iPath, L'\\');
    if (found && (found[1] == L'$') && (found[2] == L'R')) {
        found[2] = L'I';
        DeleteFileW(iPath);
    }

    free(dos);
}
