/*
 * rblog.c - Windows Event Log sink
 *
 * A service has no console, so stdout is useless in production. Everything
 * goes to the Windows Application event log under source "RecycleBinSvc".
 *
 * The registry entry for the event source is created on first run (requires
 * admin, which the service install path already has), and gracefully degrades
 * to OutputDebugString when it cannot be created.
 */

#include "rbsvc.h"
#include <stdio.h>
#include <stdarg.h>

static HANDLE g_EventLog = NULL;
static int    g_LogReady = 0;
/* When set, messages also go to stderr. Used by console/once modes where
   there is no event log viewer open and the operator needs instant feedback. */
static int    g_LogConsole = 0;

/* Event source registration:
   HKLM\SYSTEM\CurrentControlSet\Services\EventLog\Application\<name>
     EventMessageFile = <path to this exe>
     TypesSupported   = 7 (error|warning|information) */
static void LogRegisterSource(const WCHAR *exePath)
{
    WCHAR keyPath[512];
    HKEY key = NULL;
    DWORD types = 7;
    LONG rc;

    swprintf_s(keyPath, ARRAYSIZE(keyPath),
               L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\%s",
               RBSVC_EVENT_SOURCE);

    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                         &key, NULL);
    if (rc != ERROR_SUCCESS) return;

    RegSetValueExW(key, L"EventMessageFile", 0, REG_EXPAND_SZ,
                   (const BYTE *)exePath,
                   (DWORD)((wcslen(exePath) + 1) * sizeof(WCHAR)));
    RegSetValueExW(key, L"TypesSupported", 0, REG_DWORD,
                   (const BYTE *)&types, sizeof(types));

    RegCloseKey(key);
}

void LogInit(void)
{
    WCHAR exePath[MAX_PATH];

    if (g_LogReady) return;

    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) > 0)
        LogRegisterSource(exePath);

    g_EventLog = RegisterEventSourceW(NULL, RBSVC_EVENT_SOURCE);
    if (!g_EventLog) {
        OutputDebugStringW(L"rbservice: event source unavailable, "
                           L"falling back to debug output");
    }
    g_LogReady = 1;
}

void LogShutdown(void)
{
    if (g_EventLog) { DeregisterEventSource(g_EventLog); g_EventLog = NULL; }
    g_LogReady = 0;
    g_LogConsole = 0;
}

/* Mirrors log output to stderr. Call from console/once modes so operators and
   automated checks can see failures without opening the event viewer. */
void LogSetConsole(int enable)
{
    g_LogConsole = enable ? 1 : 0;
}

/* Core emitter. Falls back to debug output when the event log is unavailable. */
static void LogEmit(WORD type, const WCHAR *fmt, va_list args)
{
    WCHAR buf[1024];

    if (!fmt) return;

    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, args);

    if (g_EventLog) {
        LPCWSTR strings[1];
        strings[0] = buf;
        ReportEventW(g_EventLog, type, 0, 0, NULL, 1, 0, strings, NULL);
    } else {
        WCHAR dbg[1100];
        swprintf_s(dbg, ARRAYSIZE(dbg), L"rbservice: %s\n", buf);
        OutputDebugStringW(dbg);
    }

    /* Console/once modes: always visible on stderr regardless of build type */
    if (g_LogConsole) {
        const WCHAR *tag = L"info";
        if (type == EVENTLOG_WARNING_TYPE) tag = L"warn";
        else if (type == EVENTLOG_ERROR_TYPE) tag = L"error";
        fwprintf(stderr, L"rbservice: [%s] %s\n", tag, buf);
    }
}

void LogInfo(const WCHAR *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogEmit(EVENTLOG_INFORMATION_TYPE, fmt, args);
    va_end(args);
}

void LogWarn(const WCHAR *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogEmit(EVENTLOG_WARNING_TYPE, fmt, args);
    va_end(args);
}

void LogError(const WCHAR *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogEmit(EVENTLOG_ERROR_TYPE, fmt, args);
    va_end(args);
}

/* Appends the Win32 error text so operators get an actionable message. */
void LogErrorWin(DWORD err, const WCHAR *fmt, ...)
{
    va_list args;
    WCHAR base[900];
    WCHAR final[1100];
    WCHAR *sysMsg = NULL;

    va_start(args, fmt);
    _vsnwprintf_s(base, ARRAYSIZE(base), _TRUNCATE, fmt, args);
    va_end(args);

    if (err != 0) {
        DWORD len = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, err, 0, (LPWSTR)&sysMsg, 0, NULL);
        if (len && sysMsg) {
            /* Trim trailing CR/LF */
            while (len > 0 && (sysMsg[len - 1] == L'\r' ||
                               sysMsg[len - 1] == L'\n' ||
                               sysMsg[len - 1] == L'.')) {
                sysMsg[--len] = L'\0';
            }
        }
    }

    if (sysMsg)
        swprintf_s(final, ARRAYSIZE(final), L"%s [win32=%lu: %s]", base, err, sysMsg);
    else
        swprintf_s(final, ARRAYSIZE(final), L"%s [win32=%lu]", base, err);

    if (g_EventLog) {
        LPCWSTR strings[1];
        strings[0] = final;
        ReportEventW(g_EventLog, EVENTLOG_ERROR_TYPE, 0, 0, NULL, 1, 0,
                     strings, NULL);
    } else {
        WCHAR dbg[1200];
        swprintf_s(dbg, ARRAYSIZE(dbg), L"rbservice: %s\n", final);
        OutputDebugStringW(dbg);
    }

    if (g_LogConsole) {
        fwprintf(stderr, L"rbservice: [error] %s\n", final);
    }

    if (sysMsg) LocalFree(sysMsg);
}
