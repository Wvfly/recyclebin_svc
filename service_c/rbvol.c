/*
 * rbvol.c - NT device path <-> DOS path mapping and SID resolution
 *
 * The kernel reports paths in NT form (\Device\HarddiskVolume3\Share\a.txt)
 * because that is what FltGetFileNameInformation(NORMALIZED) produces.
 * Everything we do at the Win32 layer (MoveFileEx, CreateFile, $Recycle.Bin)
 * needs DOS form (D:\Share\a.txt).
 *
 * The mapping is built once at startup by querying every drive letter with
 * QueryDosDeviceW. Matching prefers the LONGEST device prefix so that
 * \Device\HarddiskVolume1 and \Device\HarddiskVolume10 never collide.
 */

#include "rbsvc.h"
#include <wtsapi32.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "advapi32.lib")

#define RBSVC_MAX_VOLUMES 32

typedef struct _VOL_MAP {
    WCHAR Device[64];   /* \Device\HarddiskVolume3 */
    WCHAR Drive[4];     /* D:  */
    int   DevLen;
} VOL_MAP;

static VOL_MAP  g_Vols[RBSVC_MAX_VOLUMES];
static int      g_VolCount = 0;
static int      g_VolInited = 0;

/* ------------------------------------------------------------------ */
/* Volume map                                                          */
/* ------------------------------------------------------------------ */

void VolInit(void)
{
    WCHAR letter[3];
    WCHAR dev[512];
    WCHAR c;

    if (g_VolInited) return;
    g_VolInited = 1;
    g_VolCount = 0;

    letter[1] = L':';
    letter[2] = L'\0';

    for (c = L'A'; c <= L'Z'; c++) {
        DWORD type;

        if (g_VolCount >= RBSVC_MAX_VOLUMES) break;

        letter[0] = c;
        type = GetDriveTypeW(letter);

        /* fixed / removable / ramdisk; skip CD-ROM, network, no-root */
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK)
            continue;

        if (!QueryDosDeviceW(letter, dev, ARRAYSIZE(dev)) || dev[0] == L'\0')
            continue;

        wcsncpy_s(g_Vols[g_VolCount].Device, 64, dev, _TRUNCATE);
        g_Vols[g_VolCount].Drive[0] = c;
        g_Vols[g_VolCount].Drive[1] = L':';
        g_Vols[g_VolCount].Drive[2] = L'\0';
        g_Vols[g_VolCount].DevLen  = (int)wcslen(dev);
        g_VolCount++;
    }

    LogInfo(L"volume map: %d entries", g_VolCount);
}

/* Returns allocated DOS path, or NULL if no volume matches. */
WCHAR *VolNtToDos(const WCHAR *ntPath)
{
    int i, best = -1, bestLen = 0;
    size_t ntLen, restLen;
    WCHAR *out;

    if (!ntPath || !ntPath[0]) return NULL;

    if (!g_VolInited) VolInit();

    ntLen = wcslen(ntPath);

    /* Longest-prefix match: HarddiskVolume10 before HarddiskVolume1 */
    for (i = 0; i < g_VolCount; i++) {
        if ((int)ntLen >= g_Vols[i].DevLen &&
            _wcsnicmp(ntPath, g_Vols[i].Device, (size_t)g_Vols[i].DevLen) == 0) {
            if (g_Vols[i].DevLen > bestLen) {
                bestLen = g_Vols[i].DevLen;
                best = i;
            }
        }
    }

    if (best < 0) return NULL;

    /* Skip the separator(s) after the device name */
    restLen = 0;
    {
        const WCHAR *p = ntPath + bestLen;
        while (*p == L'\\') p++;
        restLen = wcslen(p);
        out = (WCHAR *)malloc(
            (3 + 1 + restLen + 1) * sizeof(WCHAR));
        if (!out) return NULL;
        out[0] = g_Vols[best].Drive[0];
        out[1] = L':';
        out[2] = L'\\';
        if (restLen > 0) memcpy(out + 3, p, restLen * sizeof(WCHAR));
        out[3 + restLen] = L'\0';
    }

    return out;
}

/* Extracts "D:\" from a DOS path. Returns 0 when no drive letter present. */
int VolDriveOf(const WCHAR *dosPath, WCHAR *driveOut, DWORD cch)
{
    if (!dosPath || wcslen(dosPath) < 2 || dosPath[1] != L':') return 0;
    if (cch < 4) return 0;
    driveOut[0] = dosPath[0];
    driveOut[1] = L':';
    driveOut[2] = L'\\';
    driveOut[3] = L'\0';
    return 1;
}

/* ------------------------------------------------------------------ */
/* SID normalization                                                   */
/* ------------------------------------------------------------------ */

/* Resolves a session id to the logged-on user's SID string via WTS. */
static WCHAR *SidFromSession(DWORD sessionId)
{
    WTS_SESSION_INFOW dummy;   /* not used; keeps type refs honest */
    (void)dummy;

    HANDLE token = NULL;
    PTOKEN_USER ptu = NULL;
    DWORD len = 0;
    WCHAR *sidStr = NULL;
    BOOL ok;

    /* WTSQueryUserToken works for the session's primary token (SYSTEM ctx) */
    ok = WTSQueryUserToken(sessionId, &token);
    if (!ok || !token) return NULL;

    if (!GetTokenInformation(token, TokenUser, NULL, 0, &len) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(token);
        return NULL;
    }

    ptu = (PTOKEN_USER)malloc((size_t)len);
    if (!ptu) { CloseHandle(token); return NULL; }

    if (GetTokenInformation(token, TokenUser, ptu, len, &len) &&
        ConvertSidToStringSidW(ptu->User.Sid, &sidStr)) {
        /* sidStr allocated by ConvertSidToStringSidW via LocalAlloc */
        WCHAR *copy = _wcsdup(sidStr);
        LocalFree(sidStr);
        free(ptu);
        CloseHandle(token);
        return copy;
    }

    free(ptu);
    CloseHandle(token);
    return NULL;
}

/*
 * Compares a wide SID (from an item row) against a UTF-8 SID (from a GROUP BY
 * aggregate). Both should be S-1-5-21-... but casing and the leading separator
 * may differ, so compare case-insensitively after normalizing.
 */
int SidEquals(const WCHAR *wideSid, const char *utf8Sid)
{
    WCHAR wide[256];

    if (!wideSid || !utf8Sid) return 0;

    if (!MultiByteToWideChar(CP_UTF8, 0, utf8Sid, -1, wide, ARRAYSIZE(wide)))
        return 0;

    /* Normalize the item SID the same way (strip leading separators) */
    {
        const WCHAR *p = wideSid;
        while (*p == L'\\') p++;
        return (_wcsicmp(p, wide) == 0) ? 1 : 0;
    }
}

/*
 * Normalizes a SID coming from the kernel:
 *   - strips a leading '\' (the kernel emits it as a path separator)
 *   - resolves the "S-SESSION-<id>" placeholder to a real SID via WTS
 *     (only used when the kernel could not read the requestor's token)
 * Returns an allocated string; caller frees. Never returns NULL-with-content.
 */
WCHAR *SidNormalize(const WCHAR *rawSid)
{
    const WCHAR *p;
    WCHAR *out;

    if (!rawSid) return NULL;

    p = rawSid;
    while (*p == L'\\') p++;
    if (*p == L'\0') return NULL;

    /* Placeholder path: S-SESSION-<id> */
    if (_wcsnicmp(p, L"S-SESSION-", 10) == 0) {
        const WCHAR *num = p + 10;
        DWORD sess;
        WCHAR *resolved = NULL;

        if (*num == L'\\') num++;
        sess = (DWORD)_wtoi(num);

        resolved = SidFromSession(sess);
        if (resolved) {
            LogInfo(L"resolved S-SESSION-%u -> %s", sess, resolved);
            return resolved;
        }
        LogWarn(L"cannot resolve session %u to a SID", sess);
        out = _wcsdup(p);
        return out;
    }

    /* Real SID -- copy without the leading backslash */
    return _wcsdup(p);
}
