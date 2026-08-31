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
