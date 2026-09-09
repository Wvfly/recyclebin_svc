/*
 * rbconcile.c - Staging orphan reconciliation  (RB-05)
 *
 * The data path is asynchronous:
 *
 *   kernel renames the file into staging  ->  notification  ->  service writes DB
 *
 * Every step after the rename can drop the ball: the service is down, the
 * bounded queue is full, or the notification allocation fails. When that
 * happens the file is sitting in staging but no `items` row points at it, so
 * it is:
 *
 *   - invisible to every REST query (no original path -> not restorable)
 *   - outside quota accounting (quota only sums `landed` rows)
 *   - never cleaned up (purge walks DB rows, not the filesystem)
 *
 * Those files accumulate silently until the shared volume fills up and the
 * business breaks. This sweep is the safety net: it walks the staging tree,
 * asks the database whether each file is known, and reclaims the ones that
 * are not after a grace period.
 *
 * The grace period matters. A file that was renamed a second ago has not been
 * written to the database yet, so "not in DB" alone is not proof of an
 * orphan. Only age plus absence makes it safe to delete.
 */

#include "rbsvc.h"

typedef struct _RECON_STATS {
    DWORD Scanned;    /* files inspected */
    DWORD Orphans;    /* not present in the database */
    DWORD Reclaimed;  /* deleted (orphan older than the grace period) */
    DWORD InGrace;    /* orphan still inside the grace period, left alone */
    DWORD Errors;     /* lookup or delete failures */
} RECON_STATS;

/* Age of a file in whole days, computed from its last-write time. */
static int DaysSince(const FILETIME *ft)
{
    ULARGE_INTEGER then, now;
    LONGLONG diff;

    if (!ft) return 0;

    then.LowPart  = ft->dwLowDateTime;
    then.HighPart = ft->dwHighDateTime;

    GetSystemTimeAsFileTime((LPFILETIME)&now);

    diff = (LONGLONG)now.QuadPart - (LONGLONG)then.QuadPart;
    if (diff <= 0) return 0;

    /* 100 ns units -> days */
    return (int)(diff / (10LL * 1000 * 1000 * 60 * 60 * 24));
}

/* The database and its SQLite sidecars live under StoreRoot and must never be
   mistaken for staged content. */
static int IsDbFile(const WCHAR *name)
{
    if (_wcsicmp(name, L"recycle.db") == 0)          return 1;
    if (_wcsicmp(name, L"recycle.db-wal") == 0)      return 1;
    if (_wcsicmp(name, L"recycle.db-shm") == 0)      return 1;
    if (_wcsicmp(name, L"recycle.db-journal") == 0)  return 1;
    return 0;
}

static void ExamineFile(const WCHAR *dosPath,
                        const FILETIME *mtime,
                        DWORD graceDays,
                        RECON_STATS *st)
{
    WCHAR *nt = NULL;
    int known;
    int age;

    /* items.store_path holds the NT form the kernel reported. */
    nt = VolDosToNt(dosPath);
    if (!nt) {
        /* Drive is not mounted/mapped -- not an orphan, just unreachable. */
        st->Errors++;
        return;
    }

    known = DbStorePathExists(nt);
    free(nt);

    if (known < 0) { st->Errors++;   return; }   /* query failed, try later */
    if (known > 0) { return; }                   /* tracked: nothing to do */

    st->Orphans++;

    age = DaysSince(mtime);
    if (age < (int)graceDays) {
        /* Recently staged, so it may still be in flight. Leave it. */
        st->InGrace++;
        return;
    }

    if (DeleteFileW(dosPath)) {
        st->Reclaimed++;
        LogWarn(L"[reconcile] reclaimed orphan staging file (age %d d): %s",
                age, dosPath);
    } else {
        st->Errors++;
        LogErrorWin(GetLastError(),
                    L"[reconcile] cannot reclaim orphan: %s", dosPath);
    }
}

static void WalkDir(const WCHAR *dir, DWORD graceDays, RECON_STATS *st)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    WCHAR pattern[RBSVC_MAX_RECON_PATH];

    if (_snwprintf_s(pattern, ARRAYSIZE(pattern), _TRUNCATE,
                     L"%s\\*", dir) < 0) {
        st->Errors++;
        return;
    }

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;   /* empty or gone: nothing to do */

    do {
        WCHAR child[RBSVC_MAX_RECON_PATH];

        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        if (_snwprintf_s(child, ARRAYSIZE(child), _TRUNCATE,
                         L"%s\\%s", dir, fd.cFileName) < 0) {
            st->Errors++;
            continue;
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* One level per SID below StoreRoot; recurse to be safe. */
            WalkDir(child, graceDays, st);
            continue;
        }

        if (IsDbFile(fd.cFileName)) continue;

        st->Scanned++;
        ExamineFile(child, &fd.ftLastWriteTime, graceDays, st);

    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

/* Reclaim staged files that no items row references.
   Returns the number of files deleted, or -1 if the sweep could not run. */
int ReconcileStaging(DWORD graceDays)
{
    RECON_STATS st;
    const WCHAR *root;

    ZeroMemory(&st, sizeof(st));

    root = ConfigStoreRoot();
    if (!root || root[0] == L'\0') {
        LogError(L"[reconcile] no StoreRoot configured; sweep skipped");
        return -1;
    }

    /* Nothing to sweep if staging does not exist yet. */
    if (GetFileAttributesW(root) == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }

    WalkDir(root, graceDays, &st);

    if (st.Orphans > 0 || st.Errors > 0) {
        /* Always visible: orphans mean notifications were lost, which is a
           real defect signal, not routine noise. */
        LogWarn(L"[reconcile] scanned=%lu orphans=%lu reclaimed=%lu "
                L"in_grace=%lu errors=%lu (grace=%lu d)",
                st.Scanned, st.Orphans, st.Reclaimed,
                st.InGrace, st.Errors, graceDays);
    } else {
        LogInfo(L"[reconcile] clean: %lu file(s) scanned, no orphans",
                st.Scanned);
    }

    return (int)st.Reclaimed;
}

/* ------------------------------------------------------------------ */
/* Landed-row reconciliation (RB-43)                                   */
/* ------------------------------------------------------------------ */
/*
 * A 'landed' row can go stale a way the sections above do not cover: an
 * admin who opens the DESKTOP Recycle Bin directly (not this service's web
 * console) and restores or Shift+Deletes a file there. Native Explorer never
 * touches items/ops -- it just moves or deletes $R/$I itself -- so the row
 * keeps saying 'landed' and pointing at a $R file that is no longer there.
 *
 * Unlike ReconcileStaging above, this is not on a timer. A stale 'landed'
 * row costs nothing but an inaccurate status and slightly-off quota
 * accounting, not disk space, so there is no 30-second urgency, and at this
 * project's file-count scale a periodic full scan of $Recycle.Bin is not
 * something to run automatically (see the scale discussion in
 * docs/buglist.md). This runs only when asked: ops type 'reconcile', wired
 * to the web console's "刷新对账" button.
 *
 * Cost, chosen to stay cheap even with millions of landed rows:
 *   1. One DB query for every 'landed' row (the same DbListExpired idiom
 *      RB-39's rebuild-i migration uses, with a cutoff far enough in the
 *      future that it means "all of them").
 *   2. qsort by the directory portion of recycle_path, so every row that
 *      shares a $Recycle.Bin\<SID>\ folder becomes contiguous.
 *   3. ONE FindFirstFile/FindNextFile enumeration per directory (not per
 *      row) snapshots the $R/$I names actually present; qsort'd so each
 *      row's own filename is a bsearch, not a GetFileAttributesW syscall.
 *   4. Only rows whose $R turned out missing -- the minority -- pay a
 *      single follow-up GetFileAttributesW, on orig_path_dos, to tell
 *      "restored" (reappeared at the original location) from "purged"
 *      (gone for good).
 *
 * If a directory cannot be enumerated at all (SID folder missing, access
 * denied, ...), its rows are left untouched and counted separately rather
 * than guessed at -- "cannot verify" is not the same as "confirmed gone".
 */

typedef struct _RECON2_STATS {
    DWORD Landed;    /* $R still present: no change */
    DWORD Restored;  /* $R gone, reappeared at orig_path_dos */
    DWORD Purged;    /* $R gone, not at orig_path_dos either */
    DWORD Skipped;   /* directory unreadable, or no recycle_path to check */
} RECON2_STATS;

static int CompareWStrNoCase(const void *a, const void *b)
{
    WCHAR *const *sa = (WCHAR *const *)a;
    WCHAR *const *sb = (WCHAR *const *)b;
    return _wcsicmp(*sa, *sb);
}

static void FreeNameArray(WCHAR **names, int count)
{
    int i;
    if (!names) return;
    for (i = 0; i < count; i++) free(names[i]);
    free(names);
}

/* Enumerates dir\* into a qsort'd array of heap-duplicated filenames (both
   $R* / $I* and anything else present -- callers only ever look up $R names,
   so the rest is harmless noise). Returns 1 if the directory could be
   enumerated (possibly empty), 0 if it could not be opened at all. */
static int SnapshotDir(const WCHAR *dir, WCHAR ***outNames, int *outCount)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    WCHAR pattern[RBSVC_MAX_RECON_PATH];
    WCHAR **names = NULL;
    int cap = 0, n = 0;

    *outNames = NULL;
    *outCount = 0;

    if (_snwprintf_s(pattern, ARRAYSIZE(pattern), _TRUNCATE,
                     L"%s\\*", dir) < 0) {
        return 0;
    }

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        if (n == cap) {
            int newCap = cap ? cap * 2 : 64;
            WCHAR **grown = (WCHAR **)realloc(names, (size_t)newCap * sizeof(WCHAR *));
            if (!grown) break;   /* best effort: keep what we already have */
            names = grown;
            cap = newCap;
        }

        names[n] = _wcsdup(fd.cFileName);
        if (names[n]) n++;
    } while (FindNextFileW(h, &fd));

    FindClose(h);

    if (n > 1) qsort(names, (size_t)n, sizeof(WCHAR *), CompareWStrNoCase);

    *outNames = names;
    *outCount = n;
    return 1;
}

static int NameArrayContains(WCHAR **names, int count, const WCHAR *name)
{
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = _wcsicmp(names[mid], name);
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* Directory portion of path (everything before the last '\'), truncated
   into buf. Empty string if path has no separator. */
static void DirOf(const WCHAR *path, WCHAR *buf, size_t bufChars)
{
    const WCHAR *slash = path ? wcsrchr(path, L'\\') : NULL;
    size_t len = slash ? (size_t)(slash - path) : 0;

    if (len >= bufChars) len = bufChars - 1;
    if (len > 0) memcpy(buf, path, len * sizeof(WCHAR));
    buf[len] = L'\0';
}

/* Orders items so that every row sharing a recycle_path directory becomes
   contiguous. Only the directory portion is compared -- not a general path
   sort -- so grouping is stable regardless of what else differs. */
static int CompareItemsByRecycleDir(const void *pa, const void *pb)
{
    const RBSVC_ITEM *a = (const RBSVC_ITEM *)pa;
    const RBSVC_ITEM *b = (const RBSVC_ITEM *)pb;
    const WCHAR *sa = (a->RecyclePath && a->RecyclePath[0]) ? a->RecyclePath : L"";
    const WCHAR *sb = (b->RecyclePath && b->RecyclePath[0]) ? b->RecyclePath : L"";
    const WCHAR *la = wcsrchr(sa, L'\\');
    const WCHAR *lb = wcsrchr(sb, L'\\');
    size_t lena = la ? (size_t)(la - sa) : wcslen(sa);
    size_t lenb = lb ? (size_t)(lb - sb) : wcslen(sb);
    size_t minLen = lena < lenb ? lena : lenb;
    int c = wcsncmp(sa, sb, minLen);

    if (c != 0) return c;
    if (lena != lenb) return (lena < lenb) ? -1 : 1;
    return 0;
}

int ReconcileLanded(void)
{
    RBSVC_ITEM *items = NULL;
    RECON2_STATS st;
    WCHAR curDir[RBSVC_MAX_RECON_PATH];
    WCHAR **names = NULL;
    int nameCount = 0;
    int dirOk = 0, haveDir = 0;
    int count, i;

    ZeroMemory(&st, sizeof(st));
    curDir[0] = L'\0';

    /* Same "no cutoff" idiom RB-39's rebuild-i migration uses: every row
       currently 'landed', regardless of age. */
    count = DbListExpired(&items, 253402300800.0 /* ~year 9999 */);
    if (count <= 0) {
        if (items) DbFreeItemList(items, count);
        LogInfo(L"[reconcile-landed] nothing landed, nothing to check");
        return 0;
    }

    qsort(items, (size_t)count, sizeof(RBSVC_ITEM), CompareItemsByRecycleDir);

    for (i = 0; i < count; i++) {
        RBSVC_ITEM *it = &items[i];
        WCHAR thisDir[RBSVC_MAX_RECON_PATH];
        const WCHAR *base;

        if (!it->RecyclePath || !it->RecyclePath[0]) {
            st.Skipped++;
            continue;
        }

        DirOf(it->RecyclePath, thisDir, ARRAYSIZE(thisDir));

        if (!haveDir || wcscmp(thisDir, curDir) != 0) {
            FreeNameArray(names, nameCount);
            names = NULL;
            nameCount = 0;
            wcsncpy_s(curDir, ARRAYSIZE(curDir), thisDir, _TRUNCATE);
            dirOk = SnapshotDir(curDir, &names, &nameCount);
            haveDir = 1;
            if (!dirOk) {
                LogWarn(L"[reconcile-landed] cannot read %s (win32=%lu); "
                        L"leaving its landed rows unchanged",
                        curDir, GetLastError());
            }
        }

        if (!dirOk) { st.Skipped++; continue; }

        base = wcsrchr(it->RecyclePath, L'\\');
        base = base ? base + 1 : it->RecyclePath;

        if (NameArrayContains(names, nameCount, base)) {
            st.Landed++;   /* still there: nothing to change */
            continue;
        }

        /* $R is gone. Back at its original path -> something put it there
           (almost certainly the native Recycle Bin's own restore). Not
           there either -> gone for good (Shift+Delete from inside the
           native bin, or the whole $Recycle.Bin\<SID>\ folder was cleared
           some other way). */
        if (it->OrigPathDos && it->OrigPathDos[0] &&
            GetFileAttributesW(it->OrigPathDos) != INVALID_FILE_ATTRIBUTES) {
            DbSetStatus(it->Id, "restored");
            st.Restored++;
            LogInfo(L"[reconcile-landed] id=%lld restored externally -> %s",
                    it->Id, it->OrigPathDos);
        } else {
            DbSetStatus(it->Id, "purged");
            st.Purged++;
            LogInfo(L"[reconcile-landed] id=%lld purged externally (was %s)",
                    it->Id, it->RecyclePath);
        }
    }

    FreeNameArray(names, nameCount);
    DbFreeItemList(items, count);

    LogInfo(L"[reconcile-landed] landed=%lu restored=%lu purged=%lu skipped=%lu",
            st.Landed, st.Restored, st.Purged, st.Skipped);

    return (int)(st.Restored + st.Purged);
}
