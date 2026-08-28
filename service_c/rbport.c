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

/* FILTER_MESSAGE_HEADER as defined in fltuser.h */
typedef struct _RB_MSG_BUFFER {
    FILTER_MESSAGE_HEADER Header;
    RBF_NOTIFICATION      Note;
} RB_MSG_BUFFER;

static HANDLE g_Port = NULL;
static CRITICAL_SECTION g_PortLock;

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

    InitializeCriticalSection(&g_PortLock);

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

        /* Persist. Failure here must not kill the reader thread. */
        {
            LONG64 id = DbAddItem(&buf.Note);
            if (id < 0) {
                LogError(L"failed to persist notification for %s",
                         buf.Note.FilePath);
            } else {
                LogInfo(L"intercepted delete id=%lld: %s",
                        id, buf.Note.FilePath);
            }
        }
    }

    EnterCriticalSection(&g_PortLock);
    PortCloseLocked();
    LeaveCriticalSection(&g_PortLock);
    DeleteCriticalSection(&g_PortLock);

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
