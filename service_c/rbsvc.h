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
/* Restore (executed here on behalf of Go REST)                        */
/* ------------------------------------------------------------------ */
/* Returns 1 ok, 0 fail. Message written into buf. */
int  RestoreItemById(LONG64 itemId, const WCHAR *argOverride,
                     WCHAR *msgBuf, DWORD cchMsg);

/* ------------------------------------------------------------------ */
/* Kernel port reader                                                  */
/* ------------------------------------------------------------------ */
DWORD WINAPI PortThreadProc(LPVOID param);
/* Query driver stats; returns 0 on success. */
int  PortQueryStats(RBF_STATS *stats);

/* ------------------------------------------------------------------ */
/* Service lifecycle                                                   */
/* ------------------------------------------------------------------ */
void WINAPI ServiceMain(DWORD argc, LPWSTR *argv);
DWORD WINAPI ServiceCtrlHandlerEx(DWORD ctrl, DWORD eventType,
                                  LPVOID eventData, LPVOID context);
void ReportServiceStatus(DWORD state, DWORD exitCode, DWORD waitHint);

/* Global stop signal shared by all worker threads */
extern HANDLE g_StopEvent;

/* Live configuration, loaded once in ServiceMain (defined in rbconfig.c) */
extern RBSVC_CONFIG g_Config;

/* Drain pending restore commands queued by the Go REST service.
   Implemented in rbrestore.c; called by the maintenance thread. */
int RestoreDrainOps(void);

#endif /* _RBSVC_H_ */
