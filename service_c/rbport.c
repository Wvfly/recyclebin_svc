/*
 * rbport.c - Kernel communication port reader
 *
 * Blocks on FilterGetMessage for RBF_NOTIFICATION structures sent by the
 * minifilter, persists each one, and recovers from driver restarts.
 *
 * Design (post RB-34/RB-34b fix, confirmed by 2026-09-02 dump):
 *
 *   The minifilter communication port is treated as STRICTLY ONE-WAY: the
 *   driver pushes notifications and this thread only reads them, overlapped.
 *   The thread NEVER calls FilterSendMessage() to probe the driver from inside
 *   the read loop -- that call is synchronous with no timeout, and if the
 *   driver is unloaded mid-flight it never returns, wedging whatever thread
 *   issued it forever (see worker 0x5adc in the dump). Instead, "driver alive"
 *   is derived purely from g_LastMsgTick, the time of the most recent
 *   notification actually received.
 *
 *   On a read timeout we treat the port as dead and CloseHandle() it outright.
 *   Closing the handle -- NOT CancelIoEx -- is what lets any still-pending
 *   FilterSendMessage() in a stats worker return (the kernel cancels the IRP
 *   when the port object is torn down), so the worker thread drains and exits.
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

/* RB-34/RB-34b: time (unix epoch seconds) of the most recent notification
 * actually received from the driver. "Driver alive" is derived from this, NOT
 * from any active FilterSendMessage() probe -- see file header. Updated only
 * while holding g_PortLock to guard against torn reads from /health. */
static time_t g_LastMsgTick = 0;

/* RB-34b: a stats worker may be inside FilterSendMessage() when the port is
 * torn down. Non-zero means one is outstanding. Used only to avoid a double
 * CloseHandle of g_Port (the worker closes it via its own context copy). */
static volatile LONG g_SendInFlight = 0;

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

/* Close g_Port under the lock. If a stats worker is still inside
 * FilterSendMessage() it holds its OWN copy of the handle (see
 * PortSendWorker), so we must not CloseHandle() the same handle twice. The
 * worker's copy is closed by the worker when its call returns -- which,
 * because we close g_Port here, it will, promptly (kernel cancels the IRP). */
static void PortCloseLocked(void)
{
    if (g_Port) {
        HANDLE h = g_Port;
        g_Port = NULL;
        /* FilterClose is not exported by name in all SDKs; use CloseHandle */
        CloseHandle(h);
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

/* Validate and persist one notification. The payload is variable length and
 * addressed by offsets, so a malformed or truncated message must be rejected
 * rather than dereferenced. Failure to persist must not kill the reader. */
static void PortHandleMessage(const RB_MSG_BUFFER *buf, ULONG avail)
{
    const RBF_NOTIFICATION *note = (const RBF_NOTIFICATION *)buf->Payload;
    /* Payload size comes from the actual transfer size, not from the header:
     * FILTER_MESSAGE_HEADER only reports ReplyLength (0 for a fire-and-forget
     * notification) and has no payload-length field in WDK 26100. The async
     * path passes bytes-16 from GetOverlappedResult; the sync path passes
     * RBF_NOTIFY_MAX_SIZE and relies on TotalSize + Magic self validation. */

    if (avail < sizeof(RBF_NOTIFICATION) ||
        note->Magic != RBF_NOTIFY_MAGIC ||
        note->TotalSize < sizeof(RBF_NOTIFICATION) ||
        note->TotalSize > avail)
    {
        LogError(L"malformed notification (len=%lu magic=0x%08lx), ignored",
                 avail, note->Magic);
        return;
    }

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

DWORD WINAPI PortThreadProc(LPVOID param)
{
    const WCHAR *portName = (const WCHAR *)param;
    HANDLE hMsgEvent;
    int connected = 0;
    time_t lastSample = 0;

    PortInit();

    /* Completion event for the overlapped FilterGetMessage (manual reset). */
    hMsgEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!hMsgEvent) {
        LogErrorWin(GetLastError(), L"CreateEvent failed in port thread");
        return 1;
    }

    while (WaitForSingleObject(g_StopEvent, 0) != WAIT_OBJECT_0) {
        RB_MSG_BUFFER buf;
        OVERLAPPED ov;
        DWORD bytes = 0;
        DWORD wait;
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
            lastSample = 0;   /* snapshot the counters right after (re)connect */
            LogInfo(L"connected to kernel port %s", portName);
            /* A (re)connect counts as liveness -- the driver just answered. */
            EnterCriticalSection(&g_PortLock);
            g_LastMsgTick = time(NULL);
            LeaveCriticalSection(&g_PortLock);
        }

        ZeroMemory(&buf, sizeof(buf));
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = hMsgEvent;
        ResetEvent(hMsgEvent);

        /* Overlapped read: the kernel port carries ONE in-flight message per
         * connection. If the driver is up it sends notifications; if it is
         * unloaded the wait simply times out and we tear the port down below.
         * We never issue FilterSendMessage() from this thread, so a dead driver
         * can never wedge the reader (the exact RB-34/RB-34b failure mode). */
        hr = FilterGetMessage(g_Port, &buf.Header,
                              sizeof(RB_MSG_BUFFER) - sizeof(FILTER_MESSAGE_HEADER),
                              &ov);

        if (hr == HRESULT_FROM_WIN32(ERROR_IO_PENDING)) {
            wait = WaitForSingleObject(hMsgEvent, RBSVC_PORT_READ_MS);

            if (wait == WAIT_OBJECT_0) {
                /* Message arrived. */
                if (!GetOverlappedResult(g_Port, &ov, &bytes, FALSE)) {
                    DWORD err = GetLastError();
                    if (err == ERROR_OPERATION_ABORTED)
                        continue;            /* cancelled: loop back around */
                    /* Port died while we were parked. */
                    connected = 0;
                    EnterCriticalSection(&g_PortLock);
                    PortCloseLocked();
                    LeaveCriticalSection(&g_PortLock);
                    LogWarn(L"FilterGetMessage failed (err=%lu), reconnecting", err);
                    if (WaitForSingleObject(g_StopEvent, RBSVC_RECONNECT_MS) == WAIT_OBJECT_0)
                        break;
                    continue;
                }
                PortHandleMessage(&buf,
                                  (bytes > (DWORD)sizeof(FILTER_MESSAGE_HEADER))
                                      ? bytes - (DWORD)sizeof(FILTER_MESSAGE_HEADER)
                                      : 0);
                /* A real notification proves the driver is alive right now. */
                EnterCriticalSection(&g_PortLock);
                g_LastMsgTick = time(NULL);
                LeaveCriticalSection(&g_PortLock);
                continue;
            }

            /* Timeout: no notification within the read interval. This is NOT
             * necessarily a dead driver -- the driver may simply be idle. We do
             * NOT close the port on a quiet read; we just take a stats sample
             * (fire-and-forget) and loop. The port is only torn down when
             * FilterGetMessage itself fails (genuine failure branch below) or
             * when a stats worker reports the underlying handle is gone. */
            if (WaitForSingleObject(g_StopEvent, 0) == WAIT_OBJECT_0)
                break;

            if (time(NULL) - lastSample >= (time_t)RBSVC_STATS_INTERVAL) {
                lastSample = time(NULL);
                PortSampleStats();   /* async, never blocks this thread */
            }
            continue;
        }

        if (SUCCEEDED(hr)) {
            /* Message was available immediately; the transfer size is not
             * reported on the synchronous path, so rely on TotalSize + Magic
             * validation against the fixed buffer bound. */
            PortHandleMessage(&buf, RBF_NOTIFY_MAX_SIZE);
            EnterCriticalSection(&g_PortLock);
            g_LastMsgTick = time(NULL);
            LeaveCriticalSection(&g_PortLock);
            continue;
        }

        /* Genuine failure: port went away (driver unloaded / restarting). */
        connected = 0;
        EnterCriticalSection(&g_PortLock);
        PortCloseLocked();
        LeaveCriticalSection(&g_PortLock);
        LogWarn(L"FilterGetMessage failed (hr=0x%08lx), reconnecting", hr);
        if (WaitForSingleObject(g_StopEvent, RBSVC_RECONNECT_MS) == WAIT_OBJECT_0)
            break;
    }

    EnterCriticalSection(&g_PortLock);
    PortCloseLocked();
    LeaveCriticalSection(&g_PortLock);
    CloseHandle(hMsgEvent);

    LogInfo(L"port thread exiting");
    return 0;
}

/* RB-34b: context for one QUERY_STATS round-trip.
 *
 * FilterSendMessage() has no timeout, so it is issued on a worker thread. The
 * context is heap-allocated because the worker may outlive the caller's
 * patience: whoever finishes last frees it. CRITICAL: the port thread never
 * waits on c->Done -- it fires this and moves on. Only the stats sampler waits,
 * and it does so on a throwaway worker of its own that it abandons on timeout.
 *
 * If the port is torn down (CloseHandle) while the worker is inside
 * FilterSendMessage(), the kernel cancels the IRP and the call returns with
 * ERROR_OPERATION_ABORTED, so the worker always drains -- it cannot wedge. */
typedef struct _PORT_SEND_CTX {
    HANDLE  Port;          /* the worker's OWN copy of the handle */
    RBF_REPLY Req;
    RBF_STATS Stats;
    DWORD   BytesReturned;
    HRESULT Hr;
    HANDLE  Done;
    volatile LONG Refs;    /* 2 at start: caller + worker */
} PORT_SEND_CTX;

static void PortSendCtxRelease(PORT_SEND_CTX *c)
{
    if (!c) return;
    if (InterlockedDecrement(&c->Refs) == 0) {
        if (c->Done) CloseHandle(c->Done);
        HeapFree(GetProcessHeap(), 0, c);
    }
}

static DWORD WINAPI PortSendWorker(LPVOID param)
{
    PORT_SEND_CTX *c = (PORT_SEND_CTX *)param;

    c->Hr = FilterSendMessage(c->Port, &c->Req, (DWORD)sizeof(c->Req),
                              &c->Stats, (DWORD)sizeof(c->Stats),
                              &c->BytesReturned);

    /* Wake the caller if it is still waiting. */
    SetEvent(c->Done);
    InterlockedExchange(&g_SendInFlight, 0);

    /* The worker owns its copy of the port handle and closes it here. This is
     * safe even if the port thread already closed g_Port: they are distinct
     * handle values (DuplicateHandle-style duplication is NOT used -- the
     * worker captured g_Port at send time, which is the live handle value). */
    CloseHandle(c->Port);
    PortSendCtxRelease(c);   /* drop the worker's reference */
    return 0;
}

/* Query driver statistics. Returns 0 on success.
 *
 * This is FIRE-AND-FORGET-safe: it issues FilterSendMessage() on a worker,
 * waits at most RBSVC_PORT_READ_MS, and on timeout abandons the sample (the
 * worker keeps the context alive until the call returns, then frees it). It
 * must NOT be called from the port thread's read loop in a way that blocks that
 * loop -- the port thread calls PortSampleStats() which calls this but does NOT
 * wait on the result (see PortSampleStats). */
int PortQueryStats(RBF_STATS *stats)
{
    PORT_SEND_CTX *c;
    HANDLE hThread;
    DWORD  w;
    int    rc = -1;

    if (!stats) return -1;

    /* Only one round-trip at a time. If a previous sender is still stalled
     * inside FilterSendMessage(), skip this sample rather than pile up. */
    if (InterlockedCompareExchange(&g_SendInFlight, 1, 0) != 0)
        return -1;

    EnterCriticalSection(&g_PortLock);
    if (!g_Port) {
        LeaveCriticalSection(&g_PortLock);
        InterlockedExchange(&g_SendInFlight, 0);
        return -1;
    }

    c = (PORT_SEND_CTX *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   sizeof(*c));
    if (!c) {
        LeaveCriticalSection(&g_PortLock);
        InterlockedExchange(&g_SendInFlight, 0);
        return -1;
    }

    /* Capture the live handle value; the worker closes THIS copy. */
    c->Port = g_Port;
    c->Req.Ack = RBF_CMD_QUERY_STATS;
    c->Refs = 2;                     /* caller + worker */
    c->Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    LeaveCriticalSection(&g_PortLock);

    if (!c->Done) {
        InterlockedExchange(&g_SendInFlight, 0);
        PortSendCtxRelease(c);       /* caller */
        PortSendCtxRelease(c);       /* worker, which never started */
        return -1;
    }

    hThread = CreateThread(NULL, 0, PortSendWorker, c, 0, NULL);
    if (!hThread) {
        InterlockedExchange(&g_SendInFlight, 0);
        PortSendCtxRelease(c);       /* caller */
        PortSendCtxRelease(c);       /* worker, which never started */
        return -1;
    }
    CloseHandle(hThread);            /* worker owns the context now */

    ZeroMemory(stats, sizeof(*stats));

    /* Bounded wait. An unanswered QUERY_STATS (driver unloaded) used to wedge
     * the port thread forever; here the worst case is one leaked worker that
     * drains when the port is closed -- never a hang. */
    w = WaitForSingleObject(c->Done, RBSVC_PORT_READ_MS);
    if (w != WAIT_OBJECT_0) {
        LogWarn(L"QUERY_STATS did not answer within %lu ms; "
                L"abandoning this sample",
                (unsigned long)RBSVC_PORT_READ_MS);
        /* Drop our reference only -- the worker keeps the context alive
         * until FilterSendMessage() returns, then frees it. */
        PortSendCtxRelease(c);
        return -1;
    }

    *stats = c->Stats;
    if (SUCCEEDED(c->Hr) && c->BytesReturned == sizeof(*stats)) rc = 0;
    else if (SUCCEEDED(c->Hr)) rc = -2;   /* short reply: protocol mismatch */
    else rc = -1;

    PortSendCtxRelease(c);
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
 *
 * NB: this function is safe to call from the port thread's read loop ONLY
 * because PortQueryStats() runs its blocking call on a worker and never blocks
 * the caller beyond RBSVC_PORT_READ_MS. The port thread does NOT depend on the
 * result for its liveness judgement -- that comes from g_LastMsgTick.
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
