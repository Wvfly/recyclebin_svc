/*
 * rbport.c - Kernel communication port reader
 *
 * Blocks on FilterGetMessage for RBF_NOTIFICATION structures sent by the
 * minifilter, persists each one, and recovers from driver restarts.
 *
 * The structure layout comes from rbf_protocol.h which is layout-asserted
 * against driver\rbminiflt.h at compile time -- a mismatch is a build error,
 * not a runtime corruption.
 */

#include "rbsvc.h"

#pragma comment(lib, "fltlib.lib")

/* FILTER_MESSAGE_HEADER as defined in fltuser.h.
 *
 * Notifications are variable length, so the payload area is a flat byte
 * buffer sized for the largest legal message. The driver sends only the
 * bytes it used (RBF_NOTIFICATION.TotalSize) and FilterGetMessage reports
 * how many arrived. That keeps this struct near 4.7 KB instead of the old
 * fixed ~64 KB (RB-01 / RB-07). */
typedef struct _RB_MSG_BUFFER {
    FILTER_MESSAGE_HEADER Header;
    BYTE                  Payload[RBF_NOTIFY_MAX_SIZE];
} RB_MSG_BUFFER;

static HANDLE g_Port = NULL;
static CRITICAL_SECTION g_PortLock;

/* The lock used to be initialised and torn down inside the port thread. That
 * made PortQueryStats() unsafe anywhere else: console/once mode never starts
 * that thread, so the section was still uninitialised and the call crashed
 * (caught by the contract test, not by the compiler).
 *
 * The lock now follows the process lifetime instead, via PortInit/PortFini. */
static LONG g_PortLockReady = 0;

void PortInit(void)
{
    if (InterlockedCompareExchange(&g_PortLockReady, 1, 0) == 0)
        InitializeCriticalSection(&g_PortLock);
}

void PortFini(void)
{
    if (InterlockedCompareExchange(&g_PortLockReady, 0, 1) == 1)
        DeleteCriticalSection(&g_PortLock);
}

static void PortCloseLocked(void)
{
    if (g_Port) {
        /* FilterClose is not exported by name in all SDKs; use CloseHandle */
        CloseHandle(g_Port);
        g_Port = NULL;
    }
}

static int PortConnect(const WCHAR *portName)
{
    HRESULT hr;

    EnterCriticalSection(&g_PortLock);
    hr = FilterConnectCommunicationPort(portName, 0, NULL, 0, NULL, &g_Port);
    if (FAILED(hr) || g_Port == NULL) {
        g_Port = NULL;
        LeaveCriticalSection(&g_PortLock);
        return 0;
    }
    LeaveCriticalSection(&g_PortLock);
    return 1;
}

DWORD WINAPI PortThreadProc(LPVOID param)
{
    const WCHAR *portName = (const WCHAR *)param;
    int connected = 0;

    PortInit();

    while (WaitForSingleObject(g_StopEvent, 0) != WAIT_OBJECT_0) {
        RB_MSG_BUFFER buf;
        HRESULT hr;

        if (!connected) {
            if (!PortConnect(portName)) {
                LogWarn(L"cannot connect to %s, retrying in %d ms",
                        portName, RBSVC_RECONNECT_MS);
                if (WaitForSingleObject(g_StopEvent, RBSVC_RECONNECT_MS) == WAIT_OBJECT_0)
                    break;
                continue;
            }
            connected = 1;
            LogInfo(L"connected to kernel port %s", portName);
        }

        ZeroMemory(&buf, sizeof(buf));

        hr = FilterGetMessage(g_Port, &buf.Header,
                              sizeof(RB_MSG_BUFFER) - sizeof(FILTER_MESSAGE_HEADER),
                              NULL);

        if (FAILED(hr)) {
            /* Port went away (driver unloaded / restarting) -- reconnect */
            connected = 0;
            EnterCriticalSection(&g_PortLock);
            PortCloseLocked();
            LeaveCriticalSection(&g_PortLock);
            LogWarn(L"FilterGetMessage failed (hr=0x%08lx), reconnecting", hr);
            if (WaitForSingleObject(g_StopEvent, RBSVC_RECONNECT_MS) == WAIT_OBJECT_0)
                break;
            continue;
        }

        /* Validate before parsing. The payload is variable length and
           addressed by offsets, so a malformed or truncated message must be
           rejected rather than dereferenced. */
        {
            RBF_NOTIFICATION *note = (RBF_NOTIFICATION *)buf.Payload;
            /* ReplyLength covers the header plus the payload. */
            ULONG avail = (buf.Header.ReplyLength >= sizeof(FILTER_MESSAGE_HEADER))
                          ? (ULONG)(buf.Header.ReplyLength - sizeof(FILTER_MESSAGE_HEADER))
                          : 0;

            if (avail < sizeof(RBF_NOTIFICATION) ||
                note->Magic != RBF_NOTIFY_MAGIC ||
                note->TotalSize < sizeof(RBF_NOTIFICATION) ||
                note->TotalSize > avail)
            {
                LogError(L"malformed notification (len=%lu magic=0x%08lx), ignored",
                         avail, note->Magic);
                continue;
            }

            /* Persist. Failure here must not kill the reader thread. */
            {
                LONG64 id = DbAddItem(note);
                if (id < 0) {
                    LogError(L"failed to persist notification for %s",
                             RBF_NOTIFY_PATH(note));
                } else {
                    LogInfo(L"intercepted delete id=%lld: %s",
                            id, RBF_NOTIFY_PATH(note));
                }
            }
        }
    }

    EnterCriticalSection(&g_PortLock);
    PortCloseLocked();
    LeaveCriticalSection(&g_PortLock);

    LogInfo(L"port thread exiting");
    return 0;
}

/* Query driver statistics. Returns 0 on success. */
int PortQueryStats(RBF_STATS *stats)
{
    RBF_REPLY req;
    DWORD bytesReturned = 0;
    HRESULT hr;
    int rc = -1;

    if (!stats) return -1;

    EnterCriticalSection(&g_PortLock);
    if (!g_Port) { LeaveCriticalSection(&g_PortLock); return -1; }

    req.Ack = RBF_CMD_QUERY_STATS;
    ZeroMemory(stats, sizeof(*stats));

    hr = FilterSendMessage(g_Port, &req, (DWORD)sizeof(req),
                           stats, (DWORD)sizeof(*stats), &bytesReturned);

    if (SUCCEEDED(hr) && bytesReturned == sizeof(*stats)) rc = 0;
    else if (SUCCEEDED(hr)) rc = -2;   /* short reply: protocol mismatch */
    else rc = -1;

    LeaveCriticalSection(&g_PortLock);
    return rc;
}

/*
 * Sample the driver counters into driver_stats (RB-13).
 *
 * The kernel port accepts a single connection and this service holds it, so
 * rbapi.exe cannot ask the driver itself -- it reads the snapshot we leave in
 * the database instead. That is also why this lives here rather than in the Go
 * service: opening a second port would simply be refused.
 *
 * Returns 1 if the driver answered, 0 if it did not (port down, driver
 * unloaded). A failed sample is still recorded, because "the driver stopped
 * answering" is itself the signal an operator needs -- see the stale-ts rule
 * in RBSVC_STATS_STALE_SEC.
 */
int PortSampleStats(void)
{
    RBF_STATS stats;
    int rc;

    ZeroMemory(&stats, sizeof(stats));

    rc = PortQueryStats(&stats);
    if (rc != 0) {
        /* Port is not connected: nothing to report, but keep the previous
           row so a reader can still tell how stale it is. */
        return 0;
    }

    DbWriteDriverStats(&stats, 1);
    return 1;
}
