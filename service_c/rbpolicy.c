/*
 * rbpolicy.c - Quota, retention, and multi-volume disk watermark
 *
 * All three policies run on the maintenance thread. Each is deliberately
 * bounded (per-pass limits) so a huge backlog cannot monopolize the service
 * or delete more than necessary in one go.
 *
 * Multi-volume watermark (bug B6): the original implementation only watched
 * the StoreRoot volume, so a protected share on a different volume could fill
 * up unnoticed. We now check every protected volume PLUS the StoreRoot volume.
 */

#include "rbsvc.h"

/* Per-pass safety caps */
#define POLICY_QUOTA_MAX_PER_SID   4096
#define POLICY_WATERMARK_MAX_PASS  200
#define POLICY_EXPIRED_MAX_PASS    8192

/* ------------------------------------------------------------------ */
/* Quota: per-SID, oldest first                                        */
/* ------------------------------------------------------------------ */

int PolicyEnforceQuota(DWORD quotaMB)
{
    RBSVC_QUOTA_ROW *rows = NULL;
    int rowCount, total = 0, i;
    unsigned long long quotaBytes;

    if (quotaMB == 0) return 0;

    quotaBytes = (unsigned long long)quotaMB * 1024ULL * 1024ULL;

    rowCount = DbQuotaTotals(&rows);
    if (rowCount <= 0) return 0;

    for (i = 0; i < rowCount; i++) {
        RBSVC_ITEM *items = NULL;
        int n, k;
        long long over;

        if (!rows[i].Sid || rows[i].Total <= (LONG64)quotaBytes) continue;

        over = rows[i].Total - (LONG64)quotaBytes;

        /* Fetch this SID's landed items oldest-first, then trim until under. */
        n = DbListLandedOldest(&items, POLICY_QUOTA_MAX_PER_SID);
        for (k = 0; k < n && over > 0; k++) {
            if (items[k].Sid && !SidEquals(items[k].Sid, rows[i].Sid)) {
                continue;
            }
            StoreDeleteEntry(&items[k]);
            DbSetStatus(items[k].Id, "purged");
            over -= items[k].FileSize;
            total++;
        }
        DbFreeItemList(items, n);
    }

    DbFreeQuotaRows(rows, rowCount);
    if (total > 0) LogInfo(L"[quota] purged %d entries", total);
    return total;
}

/* ------------------------------------------------------------------ */
/* Retention: landed ($R/$I) and staged (staging file)                  */
/* ------------------------------------------------------------------ */

int PolicyPurgeExpired(DWORD retentionDays)
{
    double cutoff;
    RBSVC_ITEM *items = NULL;
    int n, i, removed = 0;

    if (retentionDays == 0) return 0;

    cutoff = (double)time(NULL) - (double)retentionDays * 86400.0;

    /* landed -> remove $R + $I */
    n = DbListExpired(&items, cutoff);
    if (n > POLICY_EXPIRED_MAX_PASS) n = POLICY_EXPIRED_MAX_PASS;
    for (i = 0; i < n; i++) {
        StoreDeleteEntry(&items[i]);
        DbSetStatus(items[i].Id, "purged");
        removed++;
    }
    DbFreeItemList(items, n);

    /* staged -> remove the staging file (never landed) */
    items = NULL;
    n = DbListExpiredStaged(&items, cutoff);
    if (n > POLICY_EXPIRED_MAX_PASS) n = POLICY_EXPIRED_MAX_PASS;
    for (i = 0; i < n; i++) {
        if (items[i].StorePath) {
            WCHAR *dos = VolNtToDos(items[i].StorePath);
            if (dos) { DeleteFileW(dos); free(dos); }
        }
        DbSetStatus(items[i].Id, "purged");
        removed++;
    }
    DbFreeItemList(items, n);

    if (removed > 0) LogInfo(L"[expire] purged %d entries", removed);
    return removed;
}

/* ------------------------------------------------------------------ */
/* Disk watermark: every protected volume + StoreRoot volume            */
/* ------------------------------------------------------------------ */

int PolicyDiskWatermark(DWORD minFreeMB)
{
    unsigned long long minFree;
    int totalFreed = 0;
    int i;

    /* Collect volumes to watch */
    WCHAR vols[32][4];
    int volCount = 0;

    if (minFreeMB == 0) return 0;
    minFree = (unsigned long long)minFreeMB * 1024ULL * 1024ULL;

    /* StoreRoot volume */
    {
        RBSVC_CONFIG *cfg = NULL;
        WCHAR drive[8];
        const WCHAR *root = ConfigStoreRoot();
        if (root && VolDriveOf(root, drive, ARRAYSIZE(drive))) {
            wcsncpy_s(vols[volCount], 4, drive, _TRUNCATE);
            volCount++;
        }
        (void)cfg;
    }

    /* Protected share volumes (config is already DOS form) */
    {
        /* Access protected paths through the live config */
        extern RBSVC_CONFIG g_Config;
        for (i = 0; i < (int)g_Config.ProtectedCount && volCount < 32; i++) {
            WCHAR drive[8];
            if (VolDriveOf(g_Config.ProtectedPaths[i], drive, ARRAYSIZE(drive))) {
                int dup = 0, j;
                for (j = 0; j < volCount; j++) {
                    if (_wcsicmp(vols[j], drive) == 0) { dup = 1; break; }
                }
                if (!dup) {
                    wcsncpy_s(vols[volCount], 4, drive, _TRUNCATE);
                    volCount++;
                }
            }
        }
    }

    /* Check each volume; free oldest landed entries when below the watermark */
    for (i = 0; i < volCount; i++) {
        ULARGE_INTEGER freeBytes, totalBytes, totalFree;
        RBSVC_ITEM *items = NULL;
        int n, k;
        unsigned long long freed = 0;

        if (!GetDiskFreeSpaceExW(vols[i], &freeBytes, &totalBytes, &totalFree))
            continue;

        if (freeBytes.QuadPart >= minFree) continue;

        LogWarn(L"[watermark] volume %s below %llu MB (free=%llu MB)",
                vols[i], (unsigned long long)minFreeMB,
                freeBytes.QuadPart / (1024 * 1024));

        n = DbListLandedOldest(&items, POLICY_WATERMARK_MAX_PASS);
        for (k = 0; k < n; k++) {
            if (freeBytes.QuadPart + freed >= minFree) break;
            StoreDeleteEntry(&items[k]);
            DbSetStatus(items[k].Id, "purged");
            freed += (unsigned long long)items[k].FileSize;
            totalFreed++;
        }
        DbFreeItemList(items, n);

        if (freed > 0) {
            LogInfo(L"[watermark] volume %s freed %llu MB",
                    vols[i], freed / (1024 * 1024));
        }
    }

    return totalFreed;
}
