/*
 * rbsvc.h - RecycleBin for SMB user-mode service (C port)
 *
 * Architecture (per decision: shared SQLite + independent services + C executes restore):
 *
 *   rbminiflt.sys (kernel)
 *        |  \RecycleBinPort  (RBF_NOTIFICATION)
 *        v
 *   rbservice.exe  (this, SYSTEM, sole WRITER of recycle.db)
 *        |  writes items / performs landing / quota / purge / restore
 *        v
 *   <StoreRoot>\recycle.db   (SQLite, WAL)
 *        ^
 *        |  read-only SELECT + INSERT into ops table (commands)
 *   rbapi.exe      (Go REST, independent service, READER + command issuer)
 *
 * Divide of responsibility:
 *   - rbservice.exe owns ALL filesystem mutations (rename/delete of $R/$I/staging).
 *   - rbapi.exe NEVER touches the filesystem. It only reads the DB and inserts
 *     rows into `ops` to request actions. rbservice polls `ops` and executes.
 *   - This keeps a single-writer model: no cross-process file races.
 *
 * The driver header is included directly so RBF_NOTIFICATION/RBF_STATS layouts
 * are checked by the compiler -- no hand-mirrored ctypes definitions.
 */
#ifndef _RBSVC_H_
#define _RBSVC_H_

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <fltuser.h>      /* FilterConnectCommunicationPort, Flt* helpers */
#include <sddl.h>         /* ConvertSidToStringSidW */
#include <wtsapi32.h>     /* WTSQuerySessionInformationW */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Driver-shared structures. Must match rbminiflt.h EXACTLY.
   We pull the pieces we need without pulling in kernel headers. */
#include "rbf_protocol.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define RBSVC_SERVICE_NAME   L"RecycleBinSvc"
#define RBSVC_DISPLAY_NAME   L"RecycleBin for SMB - Delete Interception Service"
#define RBSVC_EVENT_SOURCE   L"RecycleBinSvc"

#define RBSVC_REG_KEY        L"SOFTWARE\\RecycleBin"

/* Defaults (used when registry value absent) */
#define DEF_STORE_ROOT       L"C:\\RBStore"
#define DEF_QUOTA_MB         5120
#define DEF_RETENTION_DAYS   30
#define DEF_DISKFREE_MIN_MB  5120
#define DEF_STAGED_BATCH     500
/* Days an untracked staging file is kept before the reconciliation sweep
   reclaims it (RB-05). Long enough to survive a service outage. */
#define DEF_ORPHAN_GRACE_DAYS 7
/* Days a terminal ('restored' / 'purged') item row is kept for audit before
   the reaper deletes it (RB-09). The row is the only audit trail of what was
   deleted, so this is deliberately longer than the file retention itself. */
#define DEF_TERMINAL_KEEP_DAYS 90
/* Upper bound on rows removed per maintenance pass, so a huge backlog after a
   long outage is drained over several passes instead of blocking the writer. */
#define RBSVC_MAX_REAP_PER_PASS 50000
/* RB-11: database backup cadence and retention. Daily by default; the backup
   is the only thing that can reconstruct where a recycled file belongs. */
#define RBSVC_BACKUP_INTERVAL   86400
#define RBSVC_BACKUP_KEEP       7

/* RB-12: custom service control code that re-reads the user-mode registry
   configuration in place, so changing retention/quota/watermark no longer
   needs a service restart (and therefore a change window).
   Send it with:  sc control RecycleBinSvc 128
   Note: this covers only the HKLM\SOFTWARE\RecycleBin settings. The driver's
   own values (ProtectedPaths, FailClosed) live under the driver's Parameters
   key and are read once at load time -- those still require a reboot. */
#define RBSVC_CTRL_RELOAD_CONFIG 128
/* Minimum interval between reconciliation sweeps (seconds). A full tree walk
   is too expensive to run on every 30 s maintenance pass. */
#define RBSVC_RECON_INTERVAL  3600
/* How long after start-up the first sweep runs (seconds) -- soon enough to
   surface orphans left behind by a previous run or a service outage. */
#define RBSVC_RECON_STARTUP_DELAY 120
/* Path bound for the staging tree walk. MAX_PATH (260) is too small for
   <StoreRoot>\<Sid>\<seq>_<name>; entries that would overflow are counted as
   errors and skipped rather than truncated. */
#define RBSVC_MAX_RECON_PATH  1024

/* RB-13: how often the driver counters are sampled into driver_stats.
   Sampling happens on the port thread (rbport.c PortThreadProc) by timing out
   the overlapped FilterGetMessage wait. These are cumulative counters, so a
   few seconds of lag costs nothing; the high-water queue depth in the driver
   covers the in-between peaks. */
#define RBSVC_STATS_INTERVAL     5
/* RB-13: a snapshot older than this is treated as "driver offline" rather
   than "idle", so a dead driver is never reported as healthy zeros. */
#define RBSVC_STATS_STALE_SEC    30

/* Restore-tree (see rbrestore.c): bounds a single request so one bad prefix
   cannot queue an unbounded amount of work behind the ops thread. */
#define RBSVC_MAX_TREE_RESTORE 5000
#define RBSVC_MAX_TREE_PREFIX  512
#define DEF_PORT_NAME        L"\\RecycleBinPort"
#define DEF_PROTECTED        L"D:\\Share"

/* Schema version of recycle.db.
 *
 * Stored in SQLite's PRAGMA user_version. Both rbservice.exe and rbapi.exe
 * check it at startup and REFUSE TO RUN on a mismatch, so a code/database
 * skew fails loudly instead of silently mis-reading columns.
 *
 * Bump this whenever db/schema.sql changes incompatibly (rename/drop column,
 * change semantics). Additive changes that old code can still read may keep
 * the same number, but when in doubt bump it.
 */
#define RB_SCHEMA_VERSION    1

/* Maintenance loop period (ms) */
#define RBSVC_MAINTAIN_MS    30000
/* ops table poll period (ms) -- commands from Go REST */
#define RBSVC_OPS_POLL_MS    2000
/* Reconnect backoff (ms) */
#define RBSVC_RECONNECT_MS   5000

/* WTS info class not in all SDK headers */
#ifndef WTSUserSid
#define WTSUserSid 16
#endif

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

typedef struct _RBSVC_CONFIG {
    WCHAR  StoreRoot[MAX_PATH];
    WCHAR  PortName[128];
    WCHAR **ProtectedPaths;      /* DOS form, array */
    DWORD   ProtectedCount;
    DWORD   QuotaMB;
    DWORD   RetentionDays;
    DWORD   DiskFreeMinMB;
    DWORD   StagedBatch;
    DWORD   OrphanGraceDays;  /* RB-05: reclaim untracked staging files after N days */
    DWORD   TerminalKeepDays; /* RB-09: keep restored/purged rows this long, then reap */
} RBSVC_CONFIG;

/* ------------------------------------------------------------------ */
/* Item row (mirrors SQLite `items` table)                             */
/* ------------------------------------------------------------------ */

#define RBSVC_STATUS_MAX 16

typedef struct _RBSVC_ITEM {
    LONG64 Id;
    WCHAR *OrigPath;        /* NT form from kernel */
    WCHAR *OrigPathDos;     /* DOS form, resolved at landing time */
    WCHAR *StorePath;       /* NT form */
    WCHAR *Sid;
    DWORD  SessionId;
    WCHAR *ClientIp;
    double DeleteTime;      /* unix epoch seconds */
    LONG64 FileSize;
    DWORD  IsDir;
    CHAR   Status[RBSVC_STATUS_MAX];  /* staged|landed|restored|purged */
    WCHAR *RecyclePath;     /* DOS form, $R file */
} RBSVC_ITEM;

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */
void ConfigLoad(RBSVC_CONFIG *cfg);

/* RB-12: re-read the registry configuration in place. 1 = adopted,
   0 = rejected (the previous configuration stays active). */
int  ReloadConfig(void);
void ConfigFree(RBSVC_CONFIG *cfg);
const WCHAR *ConfigStoreRoot(void);   /* live pointer to g_Config */

/* ------------------------------------------------------------------ */
/* Logging (Windows Event Log + debug output)                          */
/* ------------------------------------------------------------------ */
void LogInit(void);
void LogShutdown(void);
/* Mirror log output to stderr (console/once modes). */
void LogSetConsole(int enable);
void LogInfo(const WCHAR *fmt, ...);
void LogWarn(const WCHAR *fmt, ...);
void LogError(const WCHAR *fmt, ...);
/* Last-error aware variant: appends GetLastError() text */
void LogErrorWin(DWORD err, const WCHAR *fmt, ...);

/* ------------------------------------------------------------------ */
/* Database (SQLite amalgamation, single-writer)                        */
/* ------------------------------------------------------------------ */

/* Set to an explicit .db file path to override <StoreRoot>\recycle.db.
   Driven by the --db switch for maintenance/testing. NULL = use registry. */
extern const WCHAR *g_DbPathOverride;

int  DbOpen(const WCHAR *storeRoot);   /* opens <storeRoot>\recycle.db, creates schema */
void DbClose(void);

/* 1 = store_path present in items, 0 = orphan, -1 = error (RB-05) */
int    DbStorePathExists(const WCHAR *storePathNt);

/* Items whose orig_path (NT form) starts with prefixNt, oldest first, limited
   to live ('landed','staged') rows. Returns count, or -1 on error.
   Backs the restore-tree op. */
int    DbListByOrigPathPrefix(const WCHAR *prefixNt, RBSVC_ITEM **out, int limit);

/* Delete terminal ('restored'/'purged') rows older than keepDays (RB-09).
   Returns rows deleted, or -1 on error. The backing file is already gone by
   then -- this only bounds the table so queries stay fast. */
int    DbReapTerminalRows(DWORD keepDays);

/* RB-11: online backup via SQLite's backup API -- never needs to stop writes.
   Returns 0 on success, -1 on failure. */
int    DbBackupTo(const WCHAR *destPath);

/* RB-11: PRAGMA integrity_check. 1 = healthy, 0 = corrupt, -1 = cannot run. */
int    DbCheckIntegrity(void);

LONG64 DbAddItem(const RBF_NOTIFICATION *note);
int    DbSetLanded(LONG64 id, const WCHAR *recyclePath, const WCHAR *origPathDos);
int    DbSetStatus(LONG64 id, const char *status);
int    DbGetItem(LONG64 id, RBSVC_ITEM *out);
void   DbFreeItem(RBSVC_ITEM *it);

/* Query staged items, oldest first. Caller frees with DbFreeItemList. */
int    DbListStaged(RBSVC_ITEM **out, int limit);
int    DbListLandedOldest(RBSVC_ITEM **out, int limit);
int    DbListExpired(RBSVC_ITEM **out, double cutoff);
int    DbListExpiredStaged(RBSVC_ITEM **out, double cutoff);
void   DbFreeItemList(RBSVC_ITEM *list, int count);

/* Per-SID aggregate of landed bytes. Returns count; caller frees. */
typedef struct _RBSVC_QUOTA_ROW { char *Sid; LONG64 Total; } RBSVC_QUOTA_ROW;
int    DbQuotaTotals(RBSVC_QUOTA_ROW **out);
void   DbFreeQuotaRows(RBSVC_QUOTA_ROW *rows, int count);

/* Command queue (written by Go REST, executed here) */
typedef struct _RBSVC_OP {
    LONG64 Id;
    CHAR  *Type;      /* "restore" */
    LONG64 ItemId;
    CHAR  *Arg;       /* optional target path for restore */
    CHAR   State[16]; /* pending|done|failed */
    CHAR  *Message;
} RBSVC_OP;

int    DbOpsPending(RBSVC_OP **out, int limit);
void   DbFreeOps(RBSVC_OP *ops, int count);
int    DbOpFinish(LONG64 opId, const char *state, const char *message);

/* ------------------------------------------------------------------ */
/* Volume / path helpers                                               */
/* ------------------------------------------------------------------ */
/* Builds NT-device -> drive-letter map once. */
void VolInit(void);
/* \Device\HarddiskVolume3\Share\a.txt -> D:\Share\a.txt (allocates; caller free) */
WCHAR *VolNtToDos(const WCHAR *ntPath);
/* Returns drive root for a DOS path, e.g. L"D:\\" into caller buffer */
int  VolDriveOf(const WCHAR *dosPath, WCHAR *driveOut, DWORD cch);
/* D:\RBStore\... -> \Device\HarddiskVolumeN\RBStore\... (allocates; caller free) */
WCHAR *VolDosToNt(const WCHAR *dosPath);

/* ------------------------------------------------------------------ */
/* SID                                                                 */
/* ------------------------------------------------------------------ */
/* Normalizes: strips leading '\\'; resolves "S-SESSION-<id>" via WTS.
   Returns allocated string; caller free(). */
WCHAR *SidNormalize(const WCHAR *rawSid);

/* Case-insensitive compare of a wide SID against a UTF-8 SID (SQL aggregate). */
int SidEquals(const WCHAR *wideSid, const char *utf8Sid);

/* ------------------------------------------------------------------ */
/* Store / landing                                                     */
/* ------------------------------------------------------------------ */
/* Moves staging file into <same volume>\$Recycle.Bin\<sid>\$R/$I pair. */
int  StoreLandItem(const RBSVC_ITEM *item);
/* Deletes $R/$I (or staging file) for an item. */
void StoreDeleteEntry(const RBSVC_ITEM *item);

/* ------------------------------------------------------------------ */
/* Policy (quota / retention / watermark)                              */
/* ------------------------------------------------------------------ */
int  PolicyEnforceQuota(DWORD quotaMB);
int  PolicyPurgeExpired(DWORD retentionDays);
int  PolicyDiskWatermark(DWORD minFreeMB);

/* ------------------------------------------------------------------ */
/* Staging reconciliation (RB-05)                                      */
/* ------------------------------------------------------------------ */
/* Walks <StoreRoot> and reclaims files that no items row points at -- the
   footprint of a notification that never reached the service. Returns the
   number of files reclaimed (>=0), or -1 if the sweep could not run. */
int  ReconcileStaging(DWORD graceDays);

/* RB-13: persist the latest driver counter snapshot into driver_stats. */
int  DbWriteDriverStats(const RBF_STATS *stats, int driverResponded);

/* ------------------------------------------------------------------ */
/* Restore (executed here on behalf of Go REST)                        */
/* ------------------------------------------------------------------ */
/* Returns 1 ok, 0 fail. Message written into buf. */
int  RestoreItemById(LONG64 itemId, const WCHAR *argOverride,
                     WCHAR *msgBuf, DWORD cchMsg);

/* Restore every live item whose original path begins with prefixDos, e.g.
   D:\Share\Project. Returns 1 if all succeeded, 0 otherwise; msgBuf carries a
   summary ("restored 41/42; 1 failed: ...").

   Deleting a directory over SMB removes one entry at a time, so a tree
   arrives in the store as many scattered rows (see docs/buglist.md RB-21b).
   This reassembles them from a single request instead of requiring one
   restore per entry. */
int  RestoreTreeByPrefix(const WCHAR *prefixDos, WCHAR *msgBuf, DWORD cchMsg);

/* ------------------------------------------------------------------ */
/* Kernel port reader                                                  */
/* ------------------------------------------------------------------ */
DWORD WINAPI PortThreadProc(LPVOID param);
/* Initialise / tear down the port subsystem. PortInit() must run before any
   other Port* call -- it is idempotent, and PortQueryStats() is safe in
   console/once mode once it has run. */
void PortInit(void);
void PortFini(void);

/* Query driver stats; returns 0 on success. */
int  PortQueryStats(RBF_STATS *stats);

/* Sample the driver counters into driver_stats so rbapi.exe can read them
   (RB-13). Returns 1 if the driver answered, 0 if it did not. */
int  PortSampleStats(void);

/* ------------------------------------------------------------------ */
/* Service lifecycle                                                   */
/* ------------------------------------------------------------------ */
void WINAPI ServiceMain(DWORD argc, LPWSTR *argv);
DWORD WINAPI ServiceCtrlHandlerEx(DWORD ctrl, DWORD eventType,
                                  LPVOID eventData, LPVOID context);
void ReportServiceStatus(DWORD state, DWORD exitCode, DWORD waitHint);

/* Global stop signal shared by all worker threads */
extern HANDLE g_StopEvent;

/* Last time the staging reconciliation sweep ran (unix epoch seconds).
   Owned by the maintenance thread; see RBSVC_RECON_INTERVAL (RB-05). */
extern time_t g_LastReconcile;

/* Last time the database was backed up (unix epoch seconds).
   Owned by the maintenance thread; see RBSVC_BACKUP_INTERVAL (RB-11). */
extern time_t g_LastBackup;

/* Live configuration, loaded once in ServiceMain (defined in rbconfig.c) */
extern RBSVC_CONFIG g_Config;

/* Drain pending restore commands queued by the Go REST service.
   Implemented in rbrestore.c; called by the maintenance thread. */
int RestoreDrainOps(void);

#endif /* _RBSVC_H_ */
