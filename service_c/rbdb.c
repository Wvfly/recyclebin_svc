/*
 * rbdb.c - SQLite persistence layer (sole writer in the process architecture)
 *
 * Single-connection, WAL, one writer. The Go REST service (rbapi.exe) opens the
 * same file READ-ONLY for queries and INSERTs rows into `ops` to request work.
 * WAL makes this safe: readers never block the writer and vice versa.
 *
 * Cross-process coordination:
 *   - recycle.db-wal / -shm handle MVCC between rbservice.exe and rbapi.exe.
 *   - busy_timeout guards against transient lock contention.
 *   - ONLY this process mutates `items`; rbapi only reads it and writes `ops`.
 */
#include "rbsvc.h"
#include "sqlite3.h"
#include <stdarg.h>

static sqlite3 *g_Db = NULL;
static CRITICAL_SECTION g_DbLock;

/* ------------------------------------------------------------------ */
/* Schema                                                              */
/* ------------------------------------------------------------------ */
/* kSchema comes from schema_sql.h, which is GENERATED from the shared
   db\schema.sql by db\gen_schema.ps1. Never edit it by hand -- the SQL lives
   in exactly one place so the C writer and the Go reader cannot drift apart
   silently. */

#include "schema_sql.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void DbLogError(const char *where)
{
    LogErrorWin(GetLastError(), L"sqlite: %S failed: %S",
                where, sqlite3_errmsg(g_Db) ? sqlite3_errmsg(g_Db) : "?");
}

static int DbExec(const char *sql)
{
    char *err = NULL;
    int rc;
    if (!g_Db) return SQLITE_ERROR;
    rc = sqlite3_exec(g_Db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LogError(L"sqlite exec failed [%S]: %S", sql, err ? err : "?");
        sqlite3_free(err);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */

/* Reads PRAGMA user_version. Returns 0 when the database is brand new. */
static int DbReadVersion(void)
{
    sqlite3_stmt *st = NULL;
    int ver = 0;

    if (sqlite3_prepare_v2(g_Db, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) ver = sqlite3_column_int(st, 0);
    }
    if (st) sqlite3_finalize(st);
    return ver;
}

static int DbWriteVersion(int ver)
{
    char sql[64];
    sprintf_s(sql, sizeof(sql), "PRAGMA user_version = %d", ver);
    return DbExec(sql);
}

/*
 * Verifies the on-disk schema against RB_SCHEMA_VERSION.
 *
 * Rules:
 *   version == 0                  -> brand new database; create and stamp it
 *   version == RB_SCHEMA_VERSION  -> compatible; re-apply DDL to heal any
 *                                    missing table/index (all statements are
 *                                    IF NOT EXISTS, so this is a no-op on a
 *                                    healthy database)
 *   anything else                 -> REFUSE TO START. Silently reading a
 *                                    schema we do not understand would
 *                                    corrupt data or mis-place columns.
 */
static int DbEnsureSchema(void)
{
    int ver = DbReadVersion();

    if (ver != 0 && ver != RB_SCHEMA_VERSION) {
        LogError(L"schema version mismatch: database is %d, this build expects %d. "
                 L"Refusing to start -- migrate the database or delete it.",
                 ver, RB_SCHEMA_VERSION);
        return 0;
    }

    if (DbExec(kSchema) != SQLITE_OK) {
        LogError(L"failed to apply embedded schema");
        return 0;
    }

    if (ver == 0) {
        if (DbWriteVersion(RB_SCHEMA_VERSION) != SQLITE_OK) return 0;
        LogInfo(L"created new database, schema version %d", RB_SCHEMA_VERSION);
    }

    return 1;
}

/* Forward declaration: WToU8 is static and defined below, but the backup
   helpers earlier in this file need it. */
static char *WToU8(const WCHAR *w);

/* When set, overrides <StoreRoot>\recycle.db with an explicit file path.
   Supplied by the --db switch; NULL in normal service operation. */
const WCHAR *g_DbPathOverride = NULL;

/* Creates the parent directory of a file path (...\dir\file -> ...\dir). */
static void EnsureParentDir(const WCHAR *filePath)
{
    WCHAR parent[MAX_PATH];
    const WCHAR *slash;

    slash = wcsrchr(filePath, L'\\');
    if (!slash || slash == filePath) return;

    {
        size_t len = (size_t)(slash - filePath);
        if (len >= MAX_PATH) return;
        memcpy(parent, filePath, len * sizeof(WCHAR));
        parent[len] = L'\0';
    }
    CreateDirectoryW(parent, NULL);
}

int DbOpen(const WCHAR *storeRoot)
{
    WCHAR dbPath[MAX_PATH];
    char  dbUtf8[MAX_PATH * 4];
    int   rc;

    if (g_Db) return 1;

    InitializeCriticalSection(&g_DbLock);

    if (g_DbPathOverride && g_DbPathOverride[0]) {
        /* Explicit path (--db): useful for maintenance against a specific
           database without touching the registry. */
        wcsncpy_s(dbPath, MAX_PATH, g_DbPathOverride, _TRUNCATE);
        EnsureParentDir(dbPath);
    } else {
        /* Normal operation: <StoreRoot>\recycle.db */
        CreateDirectoryW(storeRoot, NULL);
        swprintf_s(dbPath, MAX_PATH, L"%s\\recycle.db", storeRoot);
    }

    if (!WideCharToMultiByte(CP_UTF8, 0, dbPath, -1,
                             dbUtf8, sizeof(dbUtf8), NULL, NULL)) {
        LogError(L"DbOpen: path conversion failed");
        return 0;
    }

    rc = sqlite3_open_v2(dbUtf8, &g_Db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        LogError(L"DbOpen: cannot open %s: %S", dbPath,
                 sqlite3_errmsg(g_Db));
        sqlite3_close(g_Db);
        g_Db = NULL;
        return 0;
    }

    /* WAL so the Go REST reader never blocks us and never sees torn state */
    DbExec("PRAGMA journal_mode=WAL");
    DbExec("PRAGMA synchronous=NORMAL");
    DbExec("PRAGMA busy_timeout=5000");
    DbExec("PRAGMA foreign_keys=ON");

    if (!DbEnsureSchema()) {
        sqlite3_close(g_Db);
        g_Db = NULL;
        return 0;
    }

    LogInfo(L"database opened: %s (WAL, schema v%d)", dbPath, RB_SCHEMA_VERSION);
    return 1;
}

/* ================================================================== */
/* Backup and integrity  (RB-11)                                       */
/* ================================================================== */

/*
 * Copy the live database to another file using SQLite's online backup API.
 *
 * The database is the ONLY thing that knows where a recycled file originally
 * lived. Lose it and every staged file becomes an unlabelled blob: present on
 * disk, invisible to restore, and impossible to clean up. Nothing else in the
 * system can reconstruct those rows, so this is the difference between an
 * incident and an outage.
 *
 * The backup API takes a consistent snapshot while the service keeps writing,
 * so this never needs to stop the delete path.
 */
int DbBackupTo(const WCHAR *destPath)
{
    sqlite3        *dst = NULL;
    sqlite3_backup *bk  = NULL;
    char           *destUtf8 = NULL;
    int             rc;

    if (!g_Db || !destPath || !destPath[0]) return -1;

    destUtf8 = WToU8(destPath);
    if (!destUtf8) {
        LogError(L"[backup] path conversion failed: %s", destPath);
        return -1;
    }

    rc = sqlite3_open_v2(destUtf8, &dst,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        LogError(L"[backup] cannot create %s: %S", destPath,
                 sqlite3_errmsg(dst));
        if (dst) sqlite3_close(dst);
        free(destUtf8);
        return -1;
    }

    bk = sqlite3_backup_init(dst, "main", g_Db, "main");
    if (!bk) {
        LogError(L"[backup] backup_init failed: %S", sqlite3_errmsg(dst));
        sqlite3_close(dst);
        free(destUtf8);
        return -1;
    }

    /* Copy everything in one call; the database is small enough that
       step(-1) completes without holding anything for long. */
    rc = sqlite3_backup_step(bk, -1);
    sqlite3_backup_finish(bk);

    if (rc != SQLITE_DONE) {
        LogError(L"[backup] incomplete (rc=%d): %S", rc, sqlite3_errmsg(dst));
        sqlite3_close(dst);
        free(destUtf8);
        return -1;
    }

    sqlite3_close(dst);
    free(destUtf8);
    LogInfo(L"[backup] wrote %s", destPath);
    return 0;
}

/*
 * Run PRAGMA integrity_check. A database that fails this is already
 * corrupt -- WAL recovery only replays committed frames, so the moment we
 * detect it we must stop trusting the file and keep the last known-good
 * backup rather than keep writing into a damaged image.
 */
int DbCheckIntegrity(void)
{
    sqlite3_stmt *st = NULL;
    int  ok = 0;
    int  rc;

    if (!g_Db) return -1;

    EnterCriticalSection(&g_DbLock);

    rc = sqlite3_prepare_v2(g_Db, "PRAGMA integrity_check", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        DbLogError("prepare integrity_check");
        LeaveCriticalSection(&g_DbLock);
        return -1;
    }

    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *res = sqlite3_column_text(st, 0);
        ok = (res && strcmp((const char *)res, "ok") == 0);
        if (!ok) {
            LogError(L"[integrity] database reports: %S", res ? res : (const unsigned char *)"unknown");
        }
    }
    sqlite3_finalize(st);

    LeaveCriticalSection(&g_DbLock);
    return ok ? 1 : 0;
}

void DbClose(void)
{
    if (g_Db) {
        /* Truncate WAL on clean shutdown for a tidy file */
        sqlite3_exec(g_Db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);
        sqlite3_close(g_Db);
        g_Db = NULL;
    }
    DeleteCriticalSection(&g_DbLock);
}

/* ------------------------------------------------------------------ */
/* UTF-8 <-> UTF-16 helpers                                            */
/* ------------------------------------------------------------------ */

static char *WToU8(const WCHAR *w)
{
    int n;
    char *buf;
    if (!w) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, n, NULL, NULL);
    buf[n] = '\0';
    return buf;
}

static WCHAR *U8ToW(const char *s)
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

/* ------------------------------------------------------------------ */
/* Item binding                                                        */
/* ------------------------------------------------------------------ */

void DbFreeItem(RBSVC_ITEM *it)
{
    if (!it) return;
    free(it->OrigPath);
    free(it->OrigPathDos);
    free(it->StorePath);
    free(it->Sid);
    free(it->ClientIp);
    free(it->RecyclePath);
    memset(it, 0, sizeof(*it));
}

/* Canonical column order shared by every SELECT below.
 *
 * This list mirrors db/schema.sql. If the two drift, RB_SCHEMA_VERSION is
 * bumped and the Go service refuses to start -- see db/db.go verifySchema.
 */
#define ITEM_SELECT_COLS \
    "id,orig_path,orig_path_dos,store_path,sid,session_id,client_ip," \
    "delete_time,file_size,is_dir,status,recycle_path"

/* Column indices into ITEM_SELECT_COLS -- keep in sync with the macro above. */
enum {
    IT_COL_ID            = 0,
    IT_COL_ORIG_PATH     = 1,
    IT_COL_ORIG_PATH_DOS = 2,
    IT_COL_STORE_PATH    = 3,
    IT_COL_SID           = 4,
    IT_COL_SESSION_ID    = 5,
    IT_COL_CLIENT_IP     = 6,
    IT_COL_DELETE_TIME   = 7,
    IT_COL_FILE_SIZE     = 8,
    IT_COL_IS_DIR        = 9,
    IT_COL_STATUS        = 10,
    IT_COL_RECYCLE_PATH  = 11
};

static void BindItem(sqlite3_stmt *st, RBSVC_ITEM *out)
{
    const unsigned char *t;

    memset(out, 0, sizeof(*out));

    out->Id          = sqlite3_column_int64(st, IT_COL_ID);
    out->OrigPath    = U8ToW((const char *)sqlite3_column_text(st, IT_COL_ORIG_PATH));
    out->OrigPathDos = U8ToW((const char *)sqlite3_column_text(st, IT_COL_ORIG_PATH_DOS));
    out->StorePath   = U8ToW((const char *)sqlite3_column_text(st, IT_COL_STORE_PATH));
    out->Sid         = U8ToW((const char *)sqlite3_column_text(st, IT_COL_SID));
    out->SessionId   = (DWORD)sqlite3_column_int(st, IT_COL_SESSION_ID);
    out->ClientIp    = U8ToW((const char *)sqlite3_column_text(st, IT_COL_CLIENT_IP));
    out->DeleteTime  = sqlite3_column_double(st, IT_COL_DELETE_TIME);
    out->FileSize    = sqlite3_column_int64(st, IT_COL_FILE_SIZE);
    out->IsDir       = (DWORD)sqlite3_column_int(st, IT_COL_IS_DIR);

    t = sqlite3_column_text(st, IT_COL_STATUS);
    if (t) strncpy_s(out->Status, RBSVC_STATUS_MAX, (const char *)t, _TRUNCATE);

    out->RecyclePath = U8ToW((const char *)sqlite3_column_text(st, IT_COL_RECYCLE_PATH));
}

/* Generic list fetch: sql must return ITEM_SELECT_COLS in order.
 *
 * bindText, when not NULL, is bound to the statement's first parameter. That
 * exists so caller-supplied text (a LIKE pattern for tree restore) never has
 * to be spliced into the SQL string, where quoting or length would be a
 * problem. */
static int DbFetchItemsEx(const char *sql, const char *bindText,
                          RBSVC_ITEM **out, int limit)
{
    sqlite3_stmt *st = NULL;
    RBSVC_ITEM *list = NULL;
    int count = 0, cap = 0, rc;

    *out = NULL;
    if (!g_Db) return 0;

    EnterCriticalSection(&g_DbLock);

    rc = sqlite3_prepare_v2(g_Db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) { DbLogError("prepare"); goto done; }

    if (bindText) {
        if (sqlite3_bind_text(st, 1, bindText, -1, SQLITE_TRANSIENT)
                != SQLITE_OK) {
            DbLogError("bind");
            goto done;
        }
    }

    cap = (limit > 0 && limit < 4096) ? limit : 256;
    list = (RBSVC_ITEM *)calloc((size_t)cap, sizeof(RBSVC_ITEM));
    if (!list) goto done;

    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (count >= cap) {
            int ncap = cap * 2;
            RBSVC_ITEM *nl = (RBSVC_ITEM *)realloc(
                list, (size_t)ncap * sizeof(RBSVC_ITEM));
            if (!nl) break;
            memset(nl + cap, 0, (size_t)(ncap - cap) * sizeof(RBSVC_ITEM));
            list = nl; cap = ncap;
        }
        BindItem(st, &list[count]);
        count++;
        if (limit > 0 && count >= limit) break;
    }

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) DbLogError("step");

done:
    if (st) sqlite3_finalize(st);
    LeaveCriticalSection(&g_DbLock);

    if (count == 0) { free(list); list = NULL; }
    *out = list;
    return count;
}

static int DbFetchItems(const char *sql, RBSVC_ITEM **out, int limit)
{
    return DbFetchItemsEx(sql, NULL, out, limit);
}

/* Escape the LIKE metacharacters in user-supplied text so it matches
   literally: without this a prefix of "D:\Share\100%" would silently match
   anything, and "_" would match any single character.
   `out` must have room for 2*wcslen(in)+1 WCHARs. */
static void SqlLikeEscape(const WCHAR *in, WCHAR *out)
{
    WCHAR *p = out;

    for (; in && *in; in++) {
        if (*in == L'%' || *in == L'_' || *in == L'\\') *p++ = L'\\';
        *p++ = *in;
    }
    *p = L'\0';
}

/* Items whose orig_path starts with `prefixNt` (NT form), oldest first.
 *
 * Powers the restore-tree op (see rbrestore.c). Deleting a directory over SMB
 * removes one entry at a time, so its contents land in the store as separate
 * rows; this is what reassembles them for a single restore request.
 *
 * Only live rows are returned -- 'restored' / 'purged' entries have already
 * left (or been discarded from) the store, so asking to restore them again
 * would only produce confusing "not found" failures.
 *
 * There is deliberately no index on orig_path. Every intercepted delete
 * inserts a row, so an index there would tax the hot path continuously to
 * speed up an operation an administrator runs occasionally; the reaper keeps
 * the table small enough for the scan.
 */
int DbListByOrigPathPrefix(const WCHAR *prefixNt, RBSVC_ITEM **out, int limit)
{
    const char *sql =
        "SELECT " ITEM_SELECT_COLS
        " FROM items"
        " WHERE orig_path LIKE ? ESCAPE '\\'"
        "   AND status IN ('landed','staged')"
        " ORDER BY orig_path ASC"
        " LIMIT ?";
    sqlite3_stmt *st = NULL;
    char *patU8 = NULL;
    WCHAR *esc = NULL;
    size_t len;
    int count = 0;

    *out = NULL;
    if (!g_Db || !prefixNt || !prefixNt[0]) return 0;

    len = wcslen(prefixNt);
    if (len > RBSVC_MAX_TREE_PREFIX) return -1;   /* refuse absurd input */

    esc = (WCHAR *)malloc((2 * len + 2) * sizeof(WCHAR));
    if (!esc) return -1;
    SqlLikeEscape(prefixNt, esc);

    /* Trailing wildcard is intentional: everything under the prefix. */
    wcscat_s(esc, 2 * len + 2, L"%");

    patU8 = WToU8(esc);
    free(esc);
    if (!patU8) return -1;

    EnterCriticalSection(&g_DbLock);

    if (sqlite3_prepare_v2(g_Db, sql, -1, &st, NULL) == SQLITE_OK) {
        RBSVC_ITEM *list = NULL;
        int cap = (limit > 0 && limit < 4096) ? limit : 256;

        sqlite3_bind_text(st, 1, patU8, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, limit > 0 ? limit : RBSVC_MAX_TREE_RESTORE);

        list = (RBSVC_ITEM *)calloc((size_t)cap, sizeof(RBSVC_ITEM));
        if (list) {
            int rc;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                if (count >= cap) {
                    int ncap = cap * 2;
                    RBSVC_ITEM *nl = (RBSVC_ITEM *)realloc(
                        list, (size_t)ncap * sizeof(RBSVC_ITEM));
                    if (!nl) break;
                    memset(nl + cap, 0,
                           (size_t)(ncap - cap) * sizeof(RBSVC_ITEM));
                    list = nl; cap = ncap;
                }
                BindItem(st, &list[count]);
                count++;
                if (limit > 0 && count >= limit) break;
            }
            if (rc != SQLITE_DONE && rc != SQLITE_ROW) DbLogError("step");

            if (count == 0) { free(list); list = NULL; }
            *out = list;
        } else {
            count = -1;
        }
    } else {
        DbLogError("prepare orig_path prefix");
        count = -1;
    }

    if (st) sqlite3_finalize(st);
    LeaveCriticalSection(&g_DbLock);

    free(patU8);
    return count;
}

void DbFreeItemList(RBSVC_ITEM *list, int count)
{
    int i;
    if (!list) return;
    for (i = 0; i < count; i++) DbFreeItem(&list[i]);
    free(list);
}

/* ------------------------------------------------------------------ */
/* Writes                                                             */
/* ------------------------------------------------------------------ */

/* 1 = store_path known to the database, 0 = unknown (orphan), -1 = error.
   Used by the staging reconciliation sweep (RB-05). */
int DbStorePathExists(const WCHAR *storePathNt)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT 1 FROM items WHERE store_path=? LIMIT 1";
    char *sp = NULL;
    int found = 0;

    if (!g_Db || !storePathNt) return -1;

    sp = WToU8(storePathNt);
    if (!sp) return -1;

    EnterCriticalSection(&g_DbLock);

    if (sqlite3_prepare_v2(g_Db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sp, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) found = 1;
    } else {
        DbLogError("prepare store_path exists");
        found = -1;
    }

    if (st) sqlite3_finalize(st);
    LeaveCriticalSection(&g_DbLock);

    free(sp);
    return found;
}

/* RB-09: bound the size of `items`.
 *
 * Rows in a terminal state ('restored', 'purged') are the only remaining
 * audit trail of a deletion -- the data they describe has already left the
 * store -- but they never stop accumulating. At a few million deletions a day
 * the table reaches hundreds of millions of rows, which slows every query
 * (the substring search in particular) and grows the database file without
 * limit.
 *
 * Deleting in batches keeps each transaction short, so the sweeper cannot
 * hold the write lock long enough to stall the delete path that shares this
 * database.
 */
int DbReapTerminalRows(DWORD keepDays)
{
    const char *sql =
        "DELETE FROM items WHERE id IN ("
        "  SELECT id FROM items"
        "  WHERE status IN ('restored','purged')"
        "    AND delete_time < ?"
        "  LIMIT 1000)";
    int total = 0;
    int batch;

    if (!g_Db) return -1;

    /* Guard against a misconfiguration wiping the audit trail: 0 would mean
       "delete every terminal row immediately". */
    if (keepDays == 0) keepDays = DEF_TERMINAL_KEEP_DAYS;

    EnterCriticalSection(&g_DbLock);

    do {
        sqlite3_stmt *st = NULL;
        double cutoff = (double)time(NULL) - ((double)keepDays * 86400.0);

        batch = 0;
        if (sqlite3_prepare_v2(g_Db, sql, -1, &st, NULL) != SQLITE_OK) {
            DbLogError("prepare terminal reaper");
            if (st) sqlite3_finalize(st);
            LeaveCriticalSection(&g_DbLock);
            return -1;
        }

        sqlite3_bind_double(st, 1, cutoff);
        if (sqlite3_step(st) != SQLITE_DONE) {
            DbLogError("step terminal reaper");
            sqlite3_finalize(st);
            LeaveCriticalSection(&g_DbLock);
            return -1;
        }

        batch = sqlite3_changes(g_Db);
        sqlite3_finalize(st);
        total += batch;

    } while (batch > 0 && total < RBSVC_MAX_REAP_PER_PASS);

    LeaveCriticalSection(&g_DbLock);

    if (total > 0) {
        LogInfo(L"[reap] removed %d terminal item row(s) older than %lu day(s)",
                total, keepDays);
    }
    return total;
}

LONG64 DbAddItem(const RBF_NOTIFICATION *note)
{
    sqlite3_stmt *st = NULL;
    LONG64 id = -1;
    char *orig = NULL, *store = NULL, *sid = NULL;
    const char *sql =
        "INSERT INTO items(orig_path,store_path,sid,session_id,client_ip,"
        "delete_time,file_size,is_dir,status) VALUES (?,?,?,?,?,?,?,?,'staged')";
    double now;

    if (!g_Db) return -1;

    /* Kernel sends SID with a leading '\\' as a path separator; strip it so
       REST lookups and $Recycle.Bin directory names match. */
    WCHAR sidBuf[RBF_MAX_NAME];
    const WCHAR *raw = RBF_NOTIFY_SID(note);
    if (*raw == L'\\') raw++;
    wcsncpy_s(sidBuf, RBF_MAX_NAME, raw, _TRUNCATE);

    /* Payloads live after the variable-length header -- see rbf_protocol.h. */
    orig  = WToU8(RBF_NOTIFY_PATH(note));
    store = WToU8(RBF_NOTIFY_STOREPATH(note));
    sid   = WToU8(sidBuf);

    now = (double)time(NULL);

    EnterCriticalSection(&g_DbLock);

    if (sqlite3_prepare_v2(g_Db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, orig,  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, sid,   -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (st, 4, (int)note->SessionId);
        sqlite3_bind_text(st, 5, "",    -1, SQLITE_STATIC);
        sqlite3_bind_double(st, 6, now);
        sqlite3_bind_int64(st, 7, (sqlite3_int64)note->FileSize);
        sqlite3_bind_int (st, 8, (int)note->IsDirectory);

        if (sqlite3_step(st) == SQLITE_DONE)
            id = sqlite3_last_insert_rowid(g_Db);
        else
            DbLogError("insert item");
    } else {
        DbLogError("prepare insert item");
    }

    if (st) sqlite3_finalize(st);
    LeaveCriticalSection(&g_DbLock);

    free(orig); free(store); free(sid);
    return id;
}

static int DbUpdateStatusInternal(LONG64 id, const char *status)
{
    sqlite3_stmt *st = NULL;
    int ok = 0;
    const char *sql = "UPDATE items SET status=? WHERE id=?";

    if (!g_Db) return 0;

    EnterCriticalSection(&g_DbLock);
    if (sqlite3_prepare_v2(g_Db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, status, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)id);
        ok = (sqlite3_step(st) == SQLITE_DONE);
    } else {
        DbLogError("prepare update status");
    }

    if (!ok) DbLogError("update status");
    if (st) sqlite3_finalize(st);
    LeaveCriticalSection(&g_DbLock);
    return ok;
}

/*
 * Marks an item landed AND stores the DOS-form paths.
 *
 * origPathDos is written here (not at insert time) because the NT->DOS volume
 * mapping is only known once we resolve the volume during landing. Storing it
 * means the Go API can show readable paths without duplicating the volume
 * mapping logic.
 */
int DbSetLanded(LONG64 id, const WCHAR *recyclePath, const WCHAR *origPathDos)
{
    sqlite3_stmt *st = NULL;
    int ok = 0;
    char *rp = NULL, *op = NULL;
    const char *sql =
        "UPDATE items SET status='landed', recycle_path=?, orig_path_dos=? "
        "WHERE id=?";

    if (!g_Db) return 0;

    rp = WToU8(recyclePath);
    op = WToU8(origPathDos);

    EnterCriticalSection(&g_DbLock);
    if (sqlite3_prepare_v2(g_Db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, rp ? rp : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, op ? op : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)id);
        ok = (sqlite3_step(st) == SQLITE_DONE);
    } else {
        DbLogError("prepare set landed");
    }
    if (!ok) DbLogError("set landed");
    if (st) sqlite3_finalize(st);
    LeaveCriticalSection(&g_DbLock);

    free(rp);
    free(op);
    return ok;
}

int DbSetStatus(LONG64 id, const char *status)
{
    return DbUpdateStatusInternal(id, status);
}

/* ------------------------------------------------------------------ */
/* Reads                                                              */
/* ------------------------------------------------------------------ */

int DbGetItem(LONG64 id, RBSVC_ITEM *out)
{
    sqlite3_stmt *st = NULL;
    int found = 0;
    const char *sql = "SELECT " ITEM_SELECT_COLS " FROM items WHERE id=?";

    if (!g_Db || !out) return 0;
    memset(out, 0, sizeof(*out));

    EnterCriticalSection(&g_DbLock);
    if (sqlite3_prepare_v2(g_Db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        if (sqlite3_step(st) == SQLITE_ROW) { BindItem(st, out); found = 1; }
    } else {
        DbLogError("prepare get item");
    }
    if (st) sqlite3_finalize(st);
    LeaveCriticalSection(&g_DbLock);
    return found;
}

int DbListStaged(RBSVC_ITEM **out, int limit)
{
    char sql[512];
    sprintf_s(sql, sizeof(sql),
              "SELECT " ITEM_SELECT_COLS " FROM items WHERE status='staged' "
              "ORDER BY delete_time ASC LIMIT %d", limit > 0 ? limit : 500);
    return DbFetchItems(sql, out, limit);
}

int DbListLandedOldest(RBSVC_ITEM **out, int limit)
{
    char sql[512];
    sprintf_s(sql, sizeof(sql),
              "SELECT " ITEM_SELECT_COLS " FROM items WHERE status='landed' "
              "ORDER BY delete_time ASC LIMIT %d", limit > 0 ? limit : 200);
    return DbFetchItems(sql, out, limit);
}

int DbListExpired(RBSVC_ITEM **out, double cutoff)
{
    char sql[512];
    sprintf_s(sql, sizeof(sql),
              "SELECT " ITEM_SELECT_COLS " FROM items "
              "WHERE status='landed' AND delete_time < %.6f", cutoff);
    return DbFetchItems(sql, out, 0);
}

int DbListExpiredStaged(RBSVC_ITEM **out, double cutoff)
{
    char sql[512];
    sprintf_s(sql, sizeof(sql),
              "SELECT " ITEM_SELECT_COLS " FROM items "
              "WHERE status='staged' AND delete_time < %.6f", cutoff);
    return DbFetchItems(sql, out, 0);
}

/* ------------------------------------------------------------------ */
/* Quota aggregates                                                    */
/* ------------------------------------------------------------------ */

int DbQuotaTotals(RBSVC_QUOTA_ROW **out)
{
    sqlite3_stmt *st = NULL;
    RBSVC_QUOTA_ROW *rows = NULL;
    int count = 0, cap = 0, rc;
    const char *sql =
        "SELECT sid, SUM(COALESCE(file_size,0)) AS total FROM items "
        "WHERE status='landed' GROUP BY sid";

    *out = NULL;
    if (!g_Db) return 0;

    EnterCriticalSection(&g_DbLock);
    rc = sqlite3_prepare_v2(g_Db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) { DbLogError("prepare quota"); goto done; }

    cap = 64;
    rows = (RBSVC_QUOTA_ROW *)calloc((size_t)cap, sizeof(RBSVC_QUOTA_ROW));
    if (!rows) goto done;

    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (count >= cap) {
            int ncap = cap * 2;
            RBSVC_QUOTA_ROW *nr = (RBSVC_QUOTA_ROW *)realloc(
                rows, (size_t)ncap * sizeof(RBSVC_QUOTA_ROW));
            if (!nr) break;
            memset(nr + cap, 0, (size_t)(ncap - cap) * sizeof(RBSVC_QUOTA_ROW));
            rows = nr; cap = ncap;
        }
        {
            const char *s = (const char *)sqlite3_column_text(st, 0);
            size_t len = s ? strlen(s) : 0;
            rows[count].Sid = (char *)malloc(len + 1);
            if (rows[count].Sid) {
                memcpy(rows[count].Sid, s ? s : "", len + 1);
            }
            rows[count].Total = sqlite3_column_int64(st, 1);
            count++;
        }
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) DbLogError("step quota");

done:
    if (st) sqlite3_finalize(st);
    LeaveCriticalSection(&g_DbLock);

    if (count == 0) { free(rows); rows = NULL; }
    *out = rows;
    return count;
}

void DbFreeQuotaRows(RBSVC_QUOTA_ROW *rows, int count)
{
    int i;
    if (!rows) return;
    for (i = 0; i < count; i++) free(rows[i].Sid);
    free(rows);
}

/* ------------------------------------------------------------------ */
/* Command queue (ops)                                                 */
/* ------------------------------------------------------------------ */

int DbOpsPending(RBSVC_OP **out, int limit)
{
    sqlite3_stmt *st = NULL;
    RBSVC_OP *list = NULL;
    int count = 0, cap = 0, rc;
    char sql[256];
    const char *text;

    *out = NULL;
    if (!g_Db) return 0;

    sprintf_s(sql, sizeof(sql),
              "SELECT id,type,item_id,arg,state,message FROM ops "
              "WHERE state='pending' ORDER BY id ASC LIMIT %d",
              limit > 0 ? limit : 64);

    EnterCriticalSection(&g_DbLock);
    rc = sqlite3_prepare_v2(g_Db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) { DbLogError("prepare ops"); goto done; }

    cap = 32;
    list = (RBSVC_OP *)calloc((size_t)cap, sizeof(RBSVC_OP));
    if (!list) goto done;

    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (count >= cap) {
            int ncap = cap * 2;
            RBSVC_OP *nl = (RBSVC_OP *)realloc(list, (size_t)ncap * sizeof(RBSVC_OP));
            if (!nl) break;
            memset(nl + cap, 0, (size_t)(ncap - cap) * sizeof(RBSVC_OP));
            list = nl; cap = ncap;
        }
        list[count].Id     = sqlite3_column_int64(st, 0);
        text = (const char *)sqlite3_column_text(st, 1);
        if (text) list[count].Type = _strdup(text);
        list[count].ItemId = sqlite3_column_int64(st, 2);
        text = (const char *)sqlite3_column_text(st, 3);
        if (text) list[count].Arg = _strdup(text);
        text = (const char *)sqlite3_column_text(st, 4);
        if (text) strncpy_s(list[count].State, 16, text, _TRUNCATE);
        text = (const char *)sqlite3_column_text(st, 5);
        if (text) list[count].Message = _strdup(text);
        count++;
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) DbLogError("step ops");

done:
    if (st) sqlite3_finalize(st);
    LeaveCriticalSection(&g_DbLock);

    if (count == 0) { free(list); list = NULL; }
    *out = list;
    return count;
}

void DbFreeOps(RBSVC_OP *ops, int count)
{
    int i;
    if (!ops) return;
    for (i = 0; i < count; i++) {
        free(ops[i].Type);
        free(ops[i].Arg);
        free(ops[i].Message);
    }
    free(ops);
}

int DbOpFinish(LONG64 opId, const char *state, const char *message)
{
    sqlite3_stmt *st = NULL;
    int ok = 0;
    const char *sql = "UPDATE ops SET state=?, message=?, result=? WHERE id=?";

    if (!g_Db) return 0;

    EnterCriticalSection(&g_DbLock);
    if (sqlite3_prepare_v2(g_Db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, state, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, message ? message : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, message ? message : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)opId);
        ok = (sqlite3_step(st) == SQLITE_DONE);
    } else {
        DbLogError("prepare op finish");
    }
    if (st) sqlite3_finalize(st);
    LeaveCriticalSection(&g_DbLock);
    return ok;
}
