/*
 * rbsession.c - Best-effort "who deleted this, from where" for SMB clients
 *
 * Why this exists:
 *   The kernel cannot tell us the client address. An SMB delete is executed by
 *   srv2.sys in session 0 (see the note at the top of driver/rbminiflt.c), so
 *   the minifilter only ever sees a system context -- there is no socket and no
 *   session to ask. That is why items.client_ip used to be written as a
 *   hard-coded empty string (see DbAddItem) and the UI showed "-".
 *
 * Approach:
 *   Ask the SMB server itself. NetSessionEnum(502) enumerates the live SMB
 *   sessions and gives us (username, client) pairs. We snapshot that table
 *   periodically and, at insert time, resolve the item's SID -> account name
 *   and look it up in the snapshot.
 *
 * This is deliberately best effort:
 *   - a client that already disconnected has no session -> empty
 *   - one account logged in from several machines -> we take the first match
 *   - a local (non-SMB) delete has no session at all -> empty
 *   - NetSessionEnum can fail outright (server service off, permissions) -> the
 *     snapshot is cleared and lookups return empty
 * An empty value must never be presented as "no client"; it means "unknown".
 *
 * Refresh cadence matters: NetSessionEnum is an RPC into the server service and
 * is far too slow to run on the delete path. The maintenance thread refreshes
 * the snapshot; DbAddItem only reads the cached table.
 */

#include "rbsvc.h"

#include <lm.h>        /* NetSessionEnum, SESSION_INFO_502 */
#include <sddl.h>      /* ConvertStringSidToSidW */

#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "advapi32.lib")

/* One live session we care about: an account name and where it came from. */
typedef struct _RB_SESSION {
    WCHAR User[256];    /* account name only, no domain */
    WCHAR Client[256];  /* client name or address, leading \\ stripped */
} RB_SESSION;

static CRITICAL_SECTION g_SessLock;
static RB_SESSION      *g_Sessions = NULL;
static int              g_SessCount = 0;
static int              g_SessCap   = 0;
static int              g_SessReady = 0;   /* 1 once Init succeeded */

/* Copies src into dst, stripping a leading "\\" (NetSessionEnum returns
   cname in UNC form, e.g. \\192.168.1.50 or \\DESKTOP-ABC). */
static void CopyClientName(WCHAR *dst, size_t cch, const WCHAR *src)
{
    if (!src) { dst[0] = L'\0'; return; }
    if (src[0] == L'\\' && src[1] == L'\\') src += 2;
    wcsncpy_s(dst, cch, src, _TRUNCATE);
}

void SessionInit(void)
{
    if (g_SessReady) return;
    InitializeCriticalSection(&g_SessLock);
    g_SessReady = 1;
}

void SessionShutdown(void)
{
    if (!g_SessReady) return;
    EnterCriticalSection(&g_SessLock);
    free(g_Sessions);
    g_Sessions = NULL;
    g_SessCount = 0;
    g_SessCap   = 0;
    LeaveCriticalSection(&g_SessLock);

    DeleteCriticalSection(&g_SessLock);
    g_SessReady = 0;
}

/* Replaces the cached table with the current SMB session list.
   Called from the maintenance thread; safe to call from anywhere. */
void SessionRefresh(void)
{
    LPSESSION_INFO_502 buf = NULL;
    NET_API_STATUS     st;
    DWORD              read = 0, total = 0, resume = 0;
    RB_SESSION        *fresh = NULL;
    int                freshCount = 0, freshCap = 0;
    DWORD              i;

    if (!g_SessReady) return;

    st = NetSessionEnum(NULL, NULL, NULL, 502,
                        (LPBYTE *)&buf, MAX_PREFERRED_LENGTH,
                        &read, &total, &resume);
    if (st != NERR_Success && st != ERROR_MORE_DATA) {
        /* Cannot enumerate (server service stopped, or not a server SKU).
           Drop the old snapshot: keeping it would attribute clients to
           sessions that no longer exist. */
        EnterCriticalSection(&g_SessLock);
        free(g_Sessions);
        g_Sessions = NULL;
        g_SessCount = 0;
        g_SessCap   = 0;
        LeaveCriticalSection(&g_SessLock);
        return;
    }

    if (!buf) return;

    freshCap = (read > 0) ? (int)read : 1;
    fresh = (RB_SESSION *)calloc((size_t)freshCap, sizeof(RB_SESSION));
    if (!fresh) { NetApiBufferFree(buf); return; }

    for (i = 0; i < read; i++) {
        int dup = 0;
        int k;

        if (!buf[i].sesi502_username || !buf[i].sesi502_cname) continue;

        /* Same account from several machines: keep the first. Any choice is a
           guess, and pretending otherwise would be worse than admitting it. */
        for (k = 0; k < freshCount; k++) {
            if (_wcsicmp(fresh[k].User, buf[i].sesi502_username) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;

        if (freshCount >= freshCap) {
            int   newCap = freshCap * 2;
            RB_SESSION *tmp = (RB_SESSION *)realloc(fresh, (size_t)newCap * sizeof(RB_SESSION));
            if (!tmp) break;
            fresh = tmp;
            freshCap = newCap;
        }

        wcsncpy_s(fresh[freshCount].User, ARRAYSIZE(fresh[freshCount].User),
                  buf[i].sesi502_username, _TRUNCATE);
        CopyClientName(fresh[freshCount].Client,
                       ARRAYSIZE(fresh[freshCount].Client),
                       buf[i].sesi502_cname);
        freshCount++;
    }

    NetApiBufferFree(buf);

    EnterCriticalSection(&g_SessLock);
    free(g_Sessions);
    g_Sessions = fresh;
    g_SessCount = freshCount;
    g_SessCap   = freshCap;
    LeaveCriticalSection(&g_SessLock);
}

/* Looks up the cached client for an account name. Returns a copy the caller
   frees, or NULL when unknown. Caller must not hold the lock. */
static WCHAR *SessionLookupByUser(const WCHAR *user)
{
    WCHAR *out = NULL;
    int i;

    if (!g_SessReady || !user || user[0] == L'\0') return NULL;

    EnterCriticalSection(&g_SessLock);
    for (i = 0; i < g_SessCount; i++) {
        if (_wcsicmp(g_Sessions[i].User, user) == 0) {
            out = _wcsdup(g_Sessions[i].Client);
            break;
        }
    }
    LeaveCriticalSection(&g_SessLock);
    return out;
}

/* SID -> account name via LookupAccountSidW. Returns allocated "name" only
   (no domain, which is what NetSessionEnum reports). Caller frees. */
static WCHAR *SidToAccountName(const WCHAR *sid)
{
    PSID   psid = NULL;
    WCHAR  name[256], domain[256];
    DWORD  nameLen = ARRAYSIZE(name), domLen = ARRAYSIZE(domain);
    SID_NAME_USE use;
    WCHAR *out = NULL;

    if (!sid || sid[0] == L'\0') return NULL;
    if (!ConvertStringSidToSidW(sid, &psid)) return NULL;

    if (LookupAccountSidW(NULL, psid, name, &nameLen,
                          domain, &domLen, &use) && name[0] != L'\0') {
        out = _wcsdup(name);
    }
    LocalFree(psid);
    return out;
}

/* Best-effort client address for a SID. Returns NULL when unknown; the caller
   stores an empty string in that case, never a fabricated value. */
WCHAR *SessionLookupClientBySid(const WCHAR *sid)
{
    WCHAR *user, *client;

    if (!g_SessReady || !sid || sid[0] == L'\0') return NULL;
    /* Kernel session placeholders have no SMB session behind them. */
    if (wcsncmp(sid, L"S-SESSION-", 10) == 0) return NULL;

    user = SidToAccountName(sid);
    if (!user) return NULL;

    client = SessionLookupByUser(user);
    free(user);
    return client;
}
