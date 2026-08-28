/*
 * rbservice.c - Service entry point, worker threads, graceful shutdown
 *
 *   RecycleBinSvc (SYSTEM, auto-start)
 *     thread 1: PortThreadProc   -- kernel notifications -> DB
 *     thread 2: MaintainThread   -- land / expire / watermark / quota / ops
 *
 * Graceful shutdown is the main reason this exists as a real Windows service
 * rather than `sc create` wrapping a script. On SERVICE_CONTROL_STOP we:
 *     1. signal g_StopEvent
 *     2. let the port thread drain and exit
 *     3. run ONE final maintenance pass so nothing is left half-landed
 *     4. checkpoint + close the DB
 *     5. report SERVICE_STOPPED
 *
 * That last maintenance pass is what prevents "orphan" files -- items staged
 * just before shutdown that would otherwise never reach $Recycle.Bin.
 */

#include "rbsvc.h"

#pragma comment(lib, "fltlib.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

HANDLE       g_StopEvent = NULL;
static SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
static SERVICE_STATUS        g_Status;
static HANDLE g_PortThread  = NULL;
static HANDLE g_MaintThread = NULL;

/* Declared in rbpolicy.c / rbrestore.c */
int RestoreDrainOps(void);

/* ------------------------------------------------------------------ */
/* SCM status reporting                                                */
/* ------------------------------------------------------------------ */

void ReportServiceStatus(DWORD state, DWORD exitCode, DWORD waitHint)
{
    static DWORD checkpoint = 1;

    g_Status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_Status.dwCurrentState            = state;
    g_Status.dwWin32ExitCode           = exitCode;
    g_Status.dwServiceSpecificExitCode = 0;
    g_Status.dwWaitHint                = waitHint;
    g_Status.dwControlsAccepted =
        (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;

    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
        g_Status.dwCheckPoint = checkpoint++;
    else
        g_Status.dwCheckPoint = 0;

    if (g_StatusHandle)
        SetServiceStatus(g_StatusHandle, &g_Status);
}

/* ------------------------------------------------------------------ */
/* Maintenance thread                                                  */
/* ------------------------------------------------------------------ */

/* One full pass. Called on a timer AND once more during shutdown. */
static void MaintenancePass(void)
{
    RBSVC_ITEM *items = NULL;
    int n, i, landed = 0;
    int batch;

    /* 1) Land staged items, in bounded batches until the backlog is drained */
    batch = (int)g_Config.StagedBatch;
    if (batch <= 0) batch = DEF_STAGED_BATCH;

    for (;;) {
        n = DbListStaged(&items, batch);
        if (n <= 0) break;

        for (i = 0; i < n; i++) {
            if (WaitForSingleObject(g_StopEvent, 0) == WAIT_OBJECT_0) {
                DbFreeItemList(items, n);
                return;
            }
            if (StoreLandItem(&items[i])) landed++;
        }
        DbFreeItemList(items, n);
        items = NULL;

        /* Fewer than a full batch means we drained it */
        if (n < batch) break;
    }

    if (landed > 0) LogInfo(L"[maint] landed %d entries", landed);

    /* 2) Execute restore commands issued by the Go REST service */
    RestoreDrainOps();

    /* 3) Retention */
    PolicyPurgeExpired(g_Config.RetentionDays);

    /* 4) Multi-volume disk watermark */
    PolicyDiskWatermark(g_Config.DiskFreeMinMB);

    /* 5) Per-user quota */
    PolicyEnforceQuota(g_Config.QuotaMB);
}

static DWORD WINAPI MaintainThreadProc(LPVOID param)
{
    (void)param;

    /* Short initial delay so the port thread is up first */
    if (WaitForSingleObject(g_StopEvent, 3000) == WAIT_OBJECT_0)
        return 0;

    while (WaitForSingleObject(g_StopEvent, RBSVC_MAINTAIN_MS) != WAIT_OBJECT_0) {
        __try {
            MaintenancePass();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogError(L"[maint] unhandled exception in maintenance pass");
        }
    }

    LogInfo(L"maintenance thread exiting");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Control handler                                                     */
/* ------------------------------------------------------------------ */

DWORD WINAPI ServiceCtrlHandlerEx(DWORD ctrl, DWORD eventType,
                                  LPVOID eventData, LPVOID context)
{
    (void)eventType; (void)eventData; (void)context;

    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 30000);

        /* Signal every worker to wind down */
        if (g_StopEvent) SetEvent(g_StopEvent);

        /* Wait for the port thread (bounded by its reconnect sleep) */
        if (g_PortThread) {
            if (WaitForSingleObject(g_PortThread, 15000) != WAIT_OBJECT_0)
                LogWarn(L"port thread did not exit within 15s");
        }
        if (g_MaintThread) {
            if (WaitForSingleObject(g_MaintThread, 20000) != WAIT_OBJECT_0)
                LogWarn(L"maintenance thread did not exit within 20s");
        }

        /* Final pass: land anything staged just before shutdown so we do not
           leave orphans behind that no DB row will ever claim. */
        __try {
            MaintenancePass();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogError(L"exception during shutdown maintenance pass");
        }

        DbClose();
        LogInfo(L"service stopped");
        LogShutdown();

        ReportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        ReportServiceStatus(g_Status.dwCurrentState, NO_ERROR, 0);
        return NO_ERROR;

    default:
        return NO_ERROR;
    }
}

/* ------------------------------------------------------------------ */
/* Service main                                                        */
/* ------------------------------------------------------------------ */

void WINAPI ServiceMain(DWORD argc, LPWSTR *argv)
{
    (void)argc; (void)argv;

    LogInit();

    g_StatusHandle = RegisterServiceCtrlHandlerExW(
        RBSVC_SERVICE_NAME, ServiceCtrlHandlerEx, NULL);
    if (!g_StatusHandle) {
        LogErrorWin(GetLastError(), L"RegisterServiceCtrlHandlerEx failed");
        return;
    }

    ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 10000);

    /* Load configuration before anything touches disk */
    ConfigLoad(&g_Config);
    VolInit();

    if (!DbOpen(g_Config.StoreRoot)) {
        LogError(L"cannot open database under %s -- aborting", g_Config.StoreRoot);
        ReportServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
        LogShutdown();
        return;
    }

    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_StopEvent) {
        LogErrorWin(GetLastError(), L"CreateEvent failed");
        DbClose();
        ReportServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
        LogShutdown();
        return;
    }

    g_PortThread = CreateThread(NULL, 0, PortThreadProc,
                                g_Config.PortName, 0, NULL);
    g_MaintThread = CreateThread(NULL, 0, MaintainThreadProc, NULL, 0, NULL);
    (void)g_MaintThread; /* referenced in the check below */

    if (!g_PortThread || !g_MaintThread) {
        LogErrorWin(GetLastError(), L"worker thread creation failed");
        SetEvent(g_StopEvent);
        DbClose();
        ReportServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
        LogShutdown();
        return;
    }

    LogInfo(L"service started (store=%s, port=%s)",
            g_Config.StoreRoot, g_Config.PortName);
    ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);

    /* Block until stopped. The control handler does the teardown. */
    WaitForSingleObject(g_StopEvent, INFINITE);

    CloseHandle(g_PortThread);
    CloseHandle(g_MaintThread);
    CloseHandle(g_StopEvent);
    g_StopEvent = NULL;
}

/* ------------------------------------------------------------------ */
/* Console mode (debugging / manual ops)                                */
/* ------------------------------------------------------------------ */

static void PrintUsage(void)
{
    fwprintf(stderr,
        L"rbservice.exe - RecycleBin for SMB user-mode service\n"
        L"\n"
        L"Usage:\n"
        L"  rbservice.exe                 run as a Windows service (SCM)\n"
        L"  rbservice.exe console         run in the foreground (Ctrl+C to stop)\n"
        L"  rbservice.exe once            perform a single maintenance pass and exit\n"
        L"\n"
        L"Options:\n"
        L"  --db <path>                   use an explicit recycle.db instead of\n"
        L"                                <StoreRoot>\\recycle.db from the registry.\n"
        L"                                Intended for maintenance/testing.\n"
        L"\n"
        L"The Go REST service (rbapi.exe) reads the same database; this\n"
        L"executable owns all filesystem mutations.\n");
}

/* Scans argv for --db <path>. Returns 1 on success, 0 on a malformed switch. */
static int ParseDbOverride(int argc, WCHAR *argv[])
{
    int i;
    for (i = 0; i < argc - 1; i++) {
        if (_wcsicmp(argv[i], L"--db") == 0) {
            if (!argv[i + 1][0]) {
                fwprintf(stderr, L"--db requires a path\n");
                return 0;
            }
            g_DbPathOverride = argv[i + 1];
            return 1;
        }
    }
    /* Tolerate a trailing --db with no value only if it was never given */
    for (i = 0; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--db") == 0) {
            fwprintf(stderr, L"--db requires a path\n");
            return 0;
        }
    }
    return 1;
}

/* Runs one pass and exits -- handy for draining a backlog by hand. */
static int RunOnce(void)
{
    LogInit();
    LogSetConsole(1);
    ConfigLoad(&g_Config);
    VolInit();

    if (!DbOpen(g_Config.StoreRoot)) {
        fwprintf(stderr, L"cannot open database: %s\n",
                 g_DbPathOverride ? g_DbPathOverride : g_Config.StoreRoot);
        LogShutdown();
        return 1;
    }

    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_StopEvent) { DbClose(); LogShutdown(); return 1; }

    MaintenancePass();

    CloseHandle(g_StopEvent);
    g_StopEvent = NULL;
    DbClose();
    LogShutdown();
    return 0;
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl)
{
    switch (ctrl) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_StopEvent) SetEvent(g_StopEvent);
        return TRUE;
    default:
        return FALSE;
    }
}

static int RunConsole(void)
{
    LogInit();
    LogSetConsole(1);
    ConfigLoad(&g_Config);
    VolInit();

    if (!DbOpen(g_Config.StoreRoot)) {
        fwprintf(stderr, L"cannot open database: %s\n",
                 g_DbPathOverride ? g_DbPathOverride : g_Config.StoreRoot);
        LogShutdown();
        return 1;
    }

    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_StopEvent) { DbClose(); LogShutdown(); return 1; }

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    g_PortThread  = CreateThread(NULL, 0, PortThreadProc,
                                 g_Config.PortName, 0, NULL);
    g_MaintThread = CreateThread(NULL, 0, MaintainThreadProc, NULL, 0, NULL);

    fwprintf(stderr, L"running in console mode; Ctrl+C to stop\n");

    WaitForSingleObject(g_StopEvent, INFINITE);

    WaitForSingleObject(g_PortThread, 10000);
    WaitForSingleObject(g_MaintThread, 15000);

    MaintenancePass();

    CloseHandle(g_PortThread);
    CloseHandle(g_MaintThread);
    CloseHandle(g_StopEvent);
    g_StopEvent = NULL;

    DbClose();
    LogShutdown();
    return 0;
}

int wmain(int argc, WCHAR *argv[])
{
    /* Console/once modes are explicit; everything else goes through the SCM */
    /* Accept --db in any position, for console/once modes. */
    if (!ParseDbOverride(argc, argv)) return 1;

    if (argc >= 2) {
        if (_wcsicmp(argv[1], L"console") == 0) return RunConsole();
        if (_wcsicmp(argv[1], L"once") == 0)    return RunOnce();
        if (_wcsicmp(argv[1], L"/?") == 0 ||
            _wcsicmp(argv[1], L"-h") == 0 ||
            _wcsicmp(argv[1], L"--help") == 0) {
            PrintUsage();
            return 0;
        }
    }

    if (argc >= 2) PrintUsage();

    {
        SERVICE_TABLE_ENTRYW table[2];
        table[0].lpServiceName = (LPWSTR)RBSVC_SERVICE_NAME;
        table[0].lpServiceProc = ServiceMain;
        table[1].lpServiceName = NULL;
        table[1].lpServiceProc = NULL;

        if (!StartServiceCtrlDispatcherW(table)) {
            /* Not launched by the SCM */
            DWORD err = GetLastError();
            if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                fwprintf(stderr,
                    L"This executable is a Windows service.\n"
                    L"Run 'rbservice.exe console' for foreground testing,\n"
                    L"or install it with: sc create RecycleBinSvc binPath= \"<path>\"\n");
            } else {
                fwprintf(stderr, L"StartServiceCtrlDispatcher failed: %lu\n", err);
            }
            return 1;
        }
    }

    return 0;
}
