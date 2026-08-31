/*
 * rbminiflt.c - SMB remote-delete intercept Mini-Filter driver
 *
 * Behavior:
 *   PreSetInfo(FileDispositionInformation, DeleteFile=1):
 *     1. Path matches a protected share prefix? (boundary checked)
 *     2. Ensure staging dirs: <vol>\RBStore and <vol>\RBStore\<sid>
 *        (created with a DACL that lets SMB users rename files in)
 *     3. Rename to \RBStore\<Sid>\<seq>_<basename> (flat, same volume)
 *     4. Success -> COMPLETE(STATUS_SUCCESS)  (user thinks delete succeeded)
 *     5. Failure -> allow real delete (fail-open) + stats
 *     6. Async enqueue notification -> dedicated send thread -> FltSendMessage
 *        (bounded queue + 30s send timeout, never blocks the delete path)
 *
 * Note: we intentionally do NOT filter by RequestorSessionId. SMB2 deletes
 * are executed by srv2.sys which runs in session 0, so the requestor session
 * id is 0 even for remote clients; the only reliable discriminator is the
 * path policy (protected prefixes). The real client SID is resolved from the
 * requestor token (SMB impersonates the client user); placeholder fallback
 * "\S-SESSION-<id>" is only used when no token is available.
 *
 * Build: WDK 10 + VS2022, x64, Driver Type = WDM (pure Mini-Filter)
 * Linker Input: fltMgr.lib
 */
#include "rbminiflt.h"

/* This WDK install ships a minimal header/lib set: no ntseapi.h, and
   ntoskrnl.lib lacks RtlInitializeAcl / RtlAllocateAndInitializeSid /
   RtlFreeSid / RtlInitializeSecurityDescriptor / RtlSetSecurityDescriptorDacl.
   RbfCreateDirectory therefore builds a self-relative security descriptor
   by hand (pure structs, no Rtl* calls). */
#ifndef ACL_REVISION
#define ACL_REVISION (2)
#endif
#ifndef SECURITY_DESCRIPTOR_REVISION
#define SECURITY_DESCRIPTOR_REVISION (1)
#endif

/* Staging-dir DACL builder. Layout (self-relative, 44 bytes):
   SECURITY_DESCRIPTOR_RELATIVE (20) | ACL (8) | ACE_HEADER (4) | Mask (4)
   | SID bytes (8, S-1-0-0 Everyone).
   ACE grants Everyone: add-file/add-subdir/list/traverse/read-attrs/sync. */
#pragma pack(push, 1)
typedef struct _RBF_STAGING_SD {
    SECURITY_DESCRIPTOR_RELATIVE Sd;       /* off 0,  size 20 */
    ACL                          Acl;      /* off 20, size 8  */
    ACE_HEADER                   AceHeader;/* off 28, size 4  */
    ACCESS_MASK                  AceMask;  /* off 32, size 4  */
    UCHAR                        SidBytes[8]; /* off 36, S-1-0-0 */
} RBF_STAGING_SD;
#pragma pack(pop)

RBF_GLOBAL G;

/* ============================================================
 * Config: read protected paths and store root from registry
 *   HKLM\SYSTEM\CurrentControlSet\Services\rbminiflt\Parameters
 *     ProtectedPaths REG_MULTI_SZ  (NT-style, e.g. \Device\HarddiskVolume2\Share)
 *     StoreRoot      REG_SZ        (reserved; paths are derived per-volume)
 * ========================================================== */
NTSTATUS RbfLoadConfig(VOID)
{
    NTSTATUS status;
    HANDLE hKey = NULL;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING keyName;
    PKEY_VALUE_PARTIAL_INFORMATION kvpi = NULL;
    ULONG kvpiLen, resultLen;
    PWSTR multi = NULL, p;
    ULONG i = 0;

    RtlInitUnicodeString(&keyName,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\rbminiflt\\Parameters");
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwOpenKey(&hKey, KEY_QUERY_VALUE, &oa);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[RBF] No config key, using empty protected list\n");
        return STATUS_SUCCESS; /* Allow start without config */
    }

    kvpiLen = sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 16384;
    kvpi = ExAllocatePool2(POOL_FLAG_PAGED, kvpiLen, RBF_TAG);
    if (!kvpi) { ZwClose(hKey); return STATUS_SUCCESS; }

    /* StoreRoot (reserved; path is derived from source volume) */
    RtlInitUnicodeString(&keyName, L"StoreRoot");
    status = ZwQueryValueKey(hKey, &keyName, KeyValuePartialInformation,
                             kvpi, kvpiLen, &resultLen);
    UNREFERENCED_PARAMETER(status);

    /* FailClosed (REG_DWORD) -- RB-04.
       1 (default) refuses a delete we cannot stage instead of letting the
       file be destroyed. 0 restores the legacy fail-open behaviour, which
       silently loses data; only enable it deliberately as an emergency
       bypass when the staging volume is unhealthy. */
    RtlInitUnicodeString(&keyName, L"FailClosed");
    status = ZwQueryValueKey(hKey, &keyName, KeyValuePartialInformation,
                             kvpi, kvpiLen, &resultLen);
    if (NT_SUCCESS(status) && kvpi->Type == REG_DWORD &&
        kvpi->DataLength >= sizeof(ULONG)) {
        G.FailClosed = (*(PULONG)kvpi->Data) ? 1 : 0;
    }
    DbgPrint("[RBF] FailClosed=%lu\n", G.FailClosed);

    /* ProtectedPaths (REG_MULTI_SZ)
       Registry must contain NT-style prefixes (e.g.
         \Device\HarddiskVolume2\Share
       ). The deploy script (deploy.ps1) converts "D:\Share" to a volume
       device path before writing the registry. We just uppercase-store it. */
    RtlInitUnicodeString(&keyName, L"ProtectedPaths");
    status = ZwQueryValueKey(hKey, &keyName, KeyValuePartialInformation,
                             kvpi, kvpiLen, &resultLen);
    if (NT_SUCCESS(status) && kvpi->Type == REG_MULTI_SZ && kvpi->DataLength > 0) {
        multi = (PWSTR)kvpi->Data;
        for (p = multi; *p && i < RBF_MAX_PROTECTED; ) {
            SIZE_T len = 0;
            PWSTR cur = p;
            while (*p) { p++; len++; }
            p++; /* skip NULL */
            if (len < RBF_MAX_PATH) {
                RBF_PROTECTED *pe = &G.Protected[i];
                pe->Prefix.Buffer = pe->Buffer;
                pe->Prefix.MaximumLength = (USHORT)(RBF_MAX_PATH * sizeof(WCHAR));
                RtlCopyMemory(pe->Buffer, cur, len * sizeof(WCHAR));
                pe->Buffer[len] = L'\0';
                pe->Prefix.Length = (USHORT)(len * sizeof(WCHAR));
                RtlUpcaseUnicodeString(&pe->Prefix, &pe->Prefix, FALSE);
                i++;
            }
        }
    }
    G.ProtectedCount = i;

    ExFreePoolWithTag(kvpi, RBF_TAG);
    ZwClose(hKey);
    DbgPrint("[RBF] Loaded %lu protected paths\n", G.ProtectedCount);
    return STATUS_SUCCESS;
}

VOID RbfFreeConfig(VOID)
{
    ULONG i;
    for (i = 0; i < G.ProtectedCount; i++) {
        G.Protected[i].Prefix.Buffer = NULL;
        G.Protected[i].Prefix.Length = 0;
    }
    G.ProtectedCount = 0;
}

/* Check if Path matches a protected prefix. Path is NT form
   (\Device\HarddiskVolumeN\...). Cached prefixes are already uppercase.
   Boundary check: prefix must be followed by '\' or the end of the path,
   so "\Device\...\Share" does NOT match "\Device\...\ShareSecret\...". */
BOOLEAN RbfIsProtected(_In_ PCUNICODE_STRING Path)
{
    ULONG i;
    UNICODE_STRING up;
    BOOLEAN matched = FALSE;

    if (G.ProtectedCount == 0) return FALSE;

    /* Fast path: use stack buffer for short paths, avoid pool traffic. */
    {
        WCHAR stackBuf[256];
        PWSTR buf = NULL;
        if (Path->Length <= sizeof(stackBuf)) {
            buf = stackBuf;
        } else {
            buf = ExAllocatePool2(POOL_FLAG_PAGED, Path->Length, RBF_TAG);
            if (!buf) return FALSE;
        }
        up.Buffer = buf;
        up.MaximumLength = Path->Length;
        up.Length = Path->Length;
        RtlCopyMemory(buf, Path->Buffer, Path->Length);
        RtlUpcaseUnicodeString(&up, &up, FALSE);

        for (i = 0; i < G.ProtectedCount; i++) {
            ULONG plen = G.Protected[i].Prefix.Length;
            if (up.Length >= plen &&
                RtlCompareMemory(up.Buffer, G.Protected[i].Prefix.Buffer, plen) == plen) {
                /* Boundary: next char must be '\' or end of path */
                if (up.Length == plen || up.Buffer[plen / sizeof(WCHAR)] == L'\\') {
                    matched = TRUE;
                    break;
                }
            }
        }
        if (buf != stackBuf) ExFreePoolWithTag(buf, RBF_TAG);
    }
    return matched;
}

/* ============================================================
 * Session / SID
 *  Resolves the REAL client SID from the requestor token. SMB2
 *  requests are impersonated by srv2.sys with the client's token,
 *  so SeQuerySubjectContextToken -> TokenUser gives the actual user
 *  (fixes both the "session id is always 0 for SMB" problem and the
 *  per-session quota bucketing). Falls back to a placeholder
 *  "\S-SESSION-<id>" when no token is available.
 *  The returned string starts with '\' so it can be appended directly
 *  after "<vol>\RBStore". On success, SidString->Buffer must be freed
 *  by the caller.
 *  Requires PASSIVE_LEVEL (guaranteed for SET_INFORMATION dispatch).
 * ========================================================== */
NTSTATUS RbfGetRequestorSid(_In_ PFLT_CALLBACK_DATA Data,
                            _In_ ULONG SessionId,
                            _Out_ PUNICODE_STRING SidString)
{
    NTSTATUS status;
    BOOLEAN gotReal = FALSE;
    SECURITY_SUBJECT_CONTEXT ctx;

    UNREFERENCED_PARAMETER(Data);  /* subject context is taken from the
                                      current thread (SMB impersonation) */

    SidString->Buffer = NULL;
    SidString->Length = SidString->MaximumLength = 0;

    SeCaptureSubjectContext(&ctx);
    {
        PACCESS_TOKEN token = SeQuerySubjectContextToken(&ctx);
        PTOKEN_USER tokenUser = NULL;

        if (token != NULL &&
            NT_SUCCESS(SeQueryInformationToken(token, TokenUser, &tokenUser)) &&
            tokenUser != NULL && tokenUser->User.Sid != NULL) {

            UNICODE_STRING sidNoSlash = {0};
            status = RtlConvertSidToUnicodeString(&sidNoSlash,
                                                  tokenUser->User.Sid, TRUE);
            if (NT_SUCCESS(status) &&
                sidNoSlash.Length > 0 &&
                sidNoSlash.Length + sizeof(WCHAR) < RBF_MAX_NAME * sizeof(WCHAR)) {

                PWSTR buf = ExAllocatePool2(POOL_FLAG_PAGED,
                                            RBF_MAX_NAME * sizeof(WCHAR), RBF_TAG);
                if (buf != NULL) {
                    buf[0] = L'\\';
                    RtlCopyMemory(buf + 1, sidNoSlash.Buffer, sidNoSlash.Length);
                    buf[sidNoSlash.Length / sizeof(WCHAR) + 1] = L'\0';
                    SidString->Buffer = buf;
                    SidString->Length = (USHORT)(sidNoSlash.Length + sizeof(WCHAR));
                    SidString->MaximumLength = (USHORT)(RBF_MAX_NAME * sizeof(WCHAR));
                    gotReal = TRUE;
                }
                ExFreePoolWithTag(sidNoSlash.Buffer, RBF_TAG);
            } else {
                DbgPrint("[RBF] SID conversion failed 0x%X, fallback\n", status);
            }
        }
        if (tokenUser != NULL) ExFreePool(tokenUser);
    }
    SeReleaseSubjectContext(&ctx);

    if (gotReal) return STATUS_SUCCESS;

    /* Fallback: placeholder L"\S-SESSION-<id>" */
    SidString->MaximumLength = (USHORT)(RBF_MAX_NAME * sizeof(WCHAR));
    SidString->Buffer = ExAllocatePool2(POOL_FLAG_PAGED,
                                        SidString->MaximumLength, RBF_TAG);
    if (!SidString->Buffer) return STATUS_INSUFFICIENT_RESOURCES;

    {
        PCWSTR prefix = L"\\S-SESSION-";
        SIZE_T pi = 0;
        ULONG n = SessionId;
        WCHAR digits[16];
        INT di = 0, k;

        while (prefix[pi]) { SidString->Buffer[pi] = prefix[pi]; pi++; }
        if (n == 0) {
            SidString->Buffer[pi++] = L'0';
        } else {
            while (n > 0) { digits[di++] = (WCHAR)(L'0' + (n % 10)); n /= 10; }
            for (k = di - 1; k >= 0; k--) { SidString->Buffer[pi++] = digits[k]; }
        }
        SidString->Buffer[pi] = L'\0';
        SidString->Length = (USHORT)(pi * sizeof(WCHAR));
    }
    return STATUS_SUCCESS;
}

/* ============================================================
 * Move to staging: rename source to StorePath.
 *   StorePath must already be constructed (same volume, outside the
 *   protected prefix). We do NOT recurse into ourselves because the
 *   target path won't match the protected prefix.
 * ========================================================== */
NTSTATUS RbfMoveToStore(_In_ PCFLT_RELATED_OBJECTS FltObjects,
                        _In_ PFILE_OBJECT FileObject,
                        _In_ PCUNICODE_STRING StorePath)
{
    NTSTATUS status;
    PFILE_RENAME_INFORMATION renameInfo;
    ULONG allocLen;

    allocLen = sizeof(FILE_RENAME_INFORMATION) +
               StorePath->Length - sizeof(WCHAR);
    renameInfo = ExAllocatePool2(POOL_FLAG_PAGED, allocLen, RBF_TAG);
    if (!renameInfo) return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(renameInfo, allocLen);
    renameInfo->ReplaceIfExists = FALSE;
    renameInfo->RootDirectory = NULL;
    renameInfo->FileNameLength = StorePath->Length;
    RtlCopyMemory(renameInfo->FileName, StorePath->Buffer, StorePath->Length);

    status = FltSetInformationFile(
        FltObjects->Instance,
        FileObject,
        renameInfo,
        allocLen,
        FileRenameInformation);

    ExFreePoolWithTag(renameInfo, RBF_TAG);
    return status;
}

/* ============================================================
 * Create a directory (open-if-exists). Used to ensure
 *   <vol>\RBStore and <vol>\RBStore\<Sid>
 * exist before any rename lands in them. ZwCreateFile is safe here:
 * our filter only hooks IRP_MJ_SET_INFORMATION (delete/rename), so
 * these IRP_MJ_CREATE calls never re-enter this callback.
 *
 * CRITICAL: the rename into RBStore is executed with the REQUESTOR's
 * token (SMB client user). Volume roots are usually NOT writable by
 * share users, so the staging dirs must carry an explicit DACL granting
 * Everyone the rights needed to add entries (but NOT delete/modify other
 * people's staged items). Staged FILES keep their original ACLs, so
 * content is not exposed to users who lacked access before deletion.
 * ========================================================== */
NTSTATUS RbfCreateDirectory(_In_ PCUNICODE_STRING DirPath)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES oa;
    HANDLE hDir = NULL;
    IO_STATUS_BLOCK iosb;
    RBF_STAGING_SD sd;
    PSECURITY_DESCRIPTOR psd = NULL;

    RtlZeroMemory(&sd, sizeof(sd));

    /* Build self-relative SD: DACL = 1 ACE (Everyone, add-only rights). */
    sd.Sd.Revision = SECURITY_DESCRIPTOR_REVISION;
    sd.Sd.Sbz1 = 0;
    sd.Sd.Control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
    sd.Sd.Owner = 0;
    sd.Sd.Group = 0;
    sd.Sd.Sacl = 0;
    sd.Sd.Dacl = 20; /* fixed layout: SD header (20) precedes the ACL */

    sd.Acl.AclRevision = ACL_REVISION;
    sd.Acl.Sbz1 = 0;
    sd.Acl.AclSize = (USHORT)(sizeof(ACL) + 4 + 4 + 8); /* ACL + ACE = 24 */
    sd.Acl.AceCount = 1;
    sd.Acl.Sbz2 = 0;

    sd.AceHeader.AceType = ACCESS_ALLOWED_ACE_TYPE;
    sd.AceHeader.AceFlags = 0;
    sd.AceHeader.AceSize = (USHORT)(4 + 4 + 8); /* header+mask+sid = 16 */
    sd.AceMask = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_LIST_DIRECTORY |
                 FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    /* SID = S-1-0-0 (Everyone): rev=1, subauth=0, authority=0x000000000001 */
    sd.SidBytes[0] = 1;
    sd.SidBytes[1] = 0;
    sd.SidBytes[2] = 0;
    sd.SidBytes[3] = 0;
    sd.SidBytes[4] = 0;
    sd.SidBytes[5] = 0;
    sd.SidBytes[6] = 0;
    sd.SidBytes[7] = 1;

    psd = (PSECURITY_DESCRIPTOR)&sd;

    InitializeObjectAttributes(&oa, (PUNICODE_STRING)DirPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, psd);
    status = ZwCreateFile(&hDir,
        FILE_LIST_DIRECTORY | SYNCHRONIZE,
        &oa, &iosb, NULL,
        FILE_ATTRIBUTE_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN_IF,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT,
        NULL, 0);
    if (NT_SUCCESS(status) && hDir != NULL) {
        ZwClose(hDir);
        status = STATUS_SUCCESS;
    }
    return status;
}

/* Ensure <VolumeName>\RBStore and <VolumeName>\RBStore\<Sid> exist. */
NTSTATUS RbfEnsureStoreDir(_In_ PCUNICODE_STRING VolumeName,
                           _In_ PCUNICODE_STRING SidString)
{
    NTSTATUS status;
    UNICODE_STRING dirPath;
    UNICODE_STRING storeDir;

    RtlInitUnicodeString(&storeDir, L"\\RBStore");

    dirPath.Buffer = ExAllocatePool2(POOL_FLAG_PAGED, RBF_MAX_PATH * sizeof(WCHAR), RBF_TAG);
    if (!dirPath.Buffer) return STATUS_INSUFFICIENT_RESOURCES;
    dirPath.MaximumLength = (USHORT)(RBF_MAX_PATH * sizeof(WCHAR));
    dirPath.Length = 0;

    status = RtlAppendUnicodeStringToString(&dirPath, VolumeName);
    if (NT_SUCCESS(status))
        status = RtlAppendUnicodeStringToString(&dirPath, &storeDir);
    if (NT_SUCCESS(status))
        status = RbfCreateDirectory(&dirPath);

    if (NT_SUCCESS(status)) {
        status = RtlAppendUnicodeStringToString(&dirPath, SidString);
        if (NT_SUCCESS(status))
            status = RbfCreateDirectory(&dirPath);
    }

    ExFreePoolWithTag(dirPath.Buffer, RBF_TAG);
    return status;
}

/* ============================================================
 * Build staging target path (same volume, avoids cross-volume rename):
 *   <source volume>\RBStore\<Sid>\<seq>_<basename>
 *  Example: \Device\HarddiskVolume2\Share\sub\a.txt
 *        -> \Device\HarddiskVolume2\RBStore\S-1-5-21-...\0000000001_a.txt
 *
 * FLAT layout (NOT path-preserving). The relative directory tree is
 * intentionally not recreated under RBStore: that would require creating
 * every intermediate directory before each rename (N create calls per
 * delete, and any missing level fails the rename -> fail-open real
 * delete). Flat names only need the already-created <vol>\RBStore\<sid>
 * dir; the original path travels in the notification so user-mode can
 * restore exactly.
 * ========================================================== */
NTSTATUS RbfBuildStorePath(_In_ PCUNICODE_STRING SidString,
                           _In_ PCUNICODE_STRING SrcPath,
                           _Out_ PUNICODE_STRING StorePath)
{
    NTSTATUS status;
    USHORT volLen = 0;
    USHORT baseStart = 0;
    UNICODE_STRING volPart, rbDir, seqPart, baseName;
    USHORT i, slashes = 0;
    ULONG64 seq;
    WCHAR seqBuf[64];

    StorePath->Buffer = ExAllocatePool2(POOL_FLAG_PAGED,
                                       RBF_MAX_PATH * sizeof(WCHAR), RBF_TAG);
    if (!StorePath->Buffer) return STATUS_INSUFFICIENT_RESOURCES;
    StorePath->MaximumLength = (USHORT)(RBF_MAX_PATH * sizeof(WCHAR));
    StorePath->Length = 0;

    /* Find the end of the volume component: \Device\HarddiskVolumeN */
    for (i = 0; i < SrcPath->Length / sizeof(WCHAR); i++) {
        if (SrcPath->Buffer[i] == L'\\') {
            slashes++;
            if (slashes == 3) break;  /* \Device\HarddiskVolumeN\... */
        }
    }
    volLen = i; /* index of the third '\' (exclusive: volPart excludes it) */
    if (volLen == 0 || volLen >= SrcPath->Length / sizeof(WCHAR)) {
        ExFreePoolWithTag(StorePath->Buffer, RBF_TAG);
        StorePath->Buffer = NULL;
        return STATUS_INVALID_PARAMETER;
    }

    /* Base name = component after the last '\' */
    baseStart = volLen;
    for (i = volLen; i < SrcPath->Length / sizeof(WCHAR); i++) {
        if (SrcPath->Buffer[i] == L'\\') baseStart = i + 1;
    }
    if (baseStart >= SrcPath->Length / sizeof(WCHAR)) baseStart = volLen;

    volPart.Buffer = SrcPath->Buffer;
    volPart.Length = volPart.MaximumLength = (USHORT)(volLen * sizeof(WCHAR));
    baseName.Buffer = SrcPath->Buffer + baseStart;
    baseName.Length = baseName.MaximumLength =
        (USHORT)(SrcPath->Length - baseStart * sizeof(WCHAR));

    /* seq part: L"\<seq>_" (monotonic -> unique per boot) */
    seq = (ULONG64)InterlockedIncrement64((volatile LONG64 *)&G.StageSeq);
    {
        INT di = 0, k = 0;
        ULONG64 n = seq;
        WCHAR tmp[32];

        seqBuf[di++] = L'\\';
        if (n == 0) {
            seqBuf[di++] = L'0';
        } else {
            while (n > 0) { tmp[k++] = (WCHAR)(L'0' + (n % 10)); n /= 10; }
            while (k > 0) seqBuf[di++] = tmp[--k];
        }
        seqBuf[di++] = L'_';
        seqBuf[di] = L'\0';
    }
    RtlInitUnicodeString(&seqPart, seqBuf);
    RtlInitUnicodeString(&rbDir, L"\\RBStore");

    status = RtlAppendUnicodeStringToString(StorePath, &volPart);
    if (NT_SUCCESS(status))
        status = RtlAppendUnicodeStringToString(StorePath, &rbDir);
    if (NT_SUCCESS(status))
        status = RtlAppendUnicodeStringToString(StorePath, SidString);
    if (NT_SUCCESS(status))
        status = RtlAppendUnicodeStringToString(StorePath, &seqPart);
    if (NT_SUCCESS(status))
        status = RtlAppendUnicodeStringToString(StorePath, &baseName);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(StorePath->Buffer, RBF_TAG);
        StorePath->Buffer = NULL;
    }
    return status;
}

/* ============================================================
 * Variable-length notification allocation  (RB-01 / RB-07)
 *
 * A fixed RBF_NOTIFICATION used to be ~64 KB, which cannot live on the 24 KB
 * kernel stack -- declaring one as a local in the pre-callback bugchecked on
 * the first intercepted delete. Callers now allocate exactly the bytes they
 * need from paged pool and keep only a pointer on the stack, so stack usage
 * no longer scales with RBF_MAX_PATH and a queued entry costs what the
 * request actually needs instead of the worst case.
 * ========================================================== */

/* Copy Src into the payload slot at Offset and return its length in bytes,
   excluding the terminating NUL. CapacityBytes must include room for that
   NUL. The result is always NUL terminated and WCHAR aligned. */
static ULONG
RbfNotifySetStr(
    _Inout_        PRBF_NOTIFICATION Note,
    _In_           ULONG             Offset,
    _In_           ULONG             CapacityBytes,
    _In_opt_       PCUNICODE_STRING  Src)
{
    PWCH  dst   = (PWCH)((PUCHAR)Note + Offset);
    ULONG bytes = 0;
    ULONG max;

    if (!Note || CapacityBytes < sizeof(WCHAR)) return 0;
    max = CapacityBytes - sizeof(WCHAR);

    if (Src && Src->Buffer && Src->Length >= sizeof(WCHAR)) {
        bytes = Src->Length;
        if (bytes > max) bytes = max;
        bytes -= (bytes % sizeof(WCHAR));
        RtlCopyMemory(dst, Src->Buffer, bytes);
    }
    dst[bytes / sizeof(WCHAR)] = L'\0';
    return bytes;
}

/* Allocate a notification sized for the three payloads. Each *Bytes
   argument is a byte count that must include room for the NUL.
   Free with RbfNotifyFree(), or hand it to RbfQueueNotify() which takes
   ownership. */
NTSTATUS
RbfAllocNotify(
    _In_      ULONG                PathBytes,
    _In_      ULONG                StoreBytes,
    _In_      ULONG                SidBytes,
    _Outptr_  PRBF_NOTIFICATION   *OutNote)
{
    ULONG pathBytes, storeBytes, sidBytes;
    ULONG total;
    PRBF_NOTIFICATION note;

    *OutNote = NULL;

    /* Clamp to the configured maxima. This runs after the rename succeeded,
       so the file is already staged: dropping the notification would turn it
       into an invisible orphan, hence truncate rather than fail. */
    pathBytes  = min(PathBytes,  (ULONG)RBF_MAX_PATH * sizeof(WCHAR));
    storeBytes = min(StoreBytes, (ULONG)RBF_MAX_PATH * sizeof(WCHAR));
    sidBytes   = min(SidBytes,   (ULONG)RBF_MAX_NAME * sizeof(WCHAR));

    if (pathBytes  < sizeof(WCHAR)) pathBytes  = sizeof(WCHAR);
    if (storeBytes < sizeof(WCHAR)) storeBytes = sizeof(WCHAR);
    if (sidBytes   < sizeof(WCHAR)) sidBytes   = sizeof(WCHAR);

    pathBytes  -= (pathBytes  % sizeof(WCHAR));
    storeBytes -= (storeBytes % sizeof(WCHAR));
    sidBytes   -= (sidBytes   % sizeof(WCHAR));

    total = (ULONG)sizeof(RBF_NOTIFICATION) + pathBytes + storeBytes + sidBytes;
    if (total > RBF_NOTIFY_MAX_SIZE) return STATUS_BUFFER_OVERFLOW;

    note = (PRBF_NOTIFICATION)ExAllocatePool2(POOL_FLAG_PAGED, total, RBF_TAG);
    if (!note) return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(note, total);

    note->Magic           = RBF_NOTIFY_MAGIC;
    note->TotalSize       = total;
    note->PathOffset      = (ULONG)sizeof(RBF_NOTIFICATION);
    note->StorePathOffset = note->PathOffset + pathBytes;
    note->SidOffset       = note->StorePathOffset + storeBytes;

    *OutNote = note;
    return STATUS_SUCCESS;
}

/* Each setter derives its own capacity from the neighbouring offset, so it
   can never write past the end of the allocation. */
VOID RbfNotifySetPath(_Inout_ PRBF_NOTIFICATION Note, _In_opt_ PCUNICODE_STRING Src)
{
    if (!Note) return;
    Note->PathLength = RbfNotifySetStr(
        Note, Note->PathOffset,
        Note->StorePathOffset - Note->PathOffset, Src);
}

VOID RbfNotifySetStorePath(_Inout_ PRBF_NOTIFICATION Note, _In_opt_ PCUNICODE_STRING Src)
{
    if (!Note) return;
    Note->StorePathLength = RbfNotifySetStr(
        Note, Note->StorePathOffset,
        Note->SidOffset - Note->StorePathOffset, Src);
}

VOID RbfNotifySetSid(_Inout_ PRBF_NOTIFICATION Note, _In_opt_ PCUNICODE_STRING Src)
{
    if (!Note) return;
    Note->SidLength = RbfNotifySetStr(
        Note, Note->SidOffset,
        Note->TotalSize - Note->SidOffset, Src);
}

VOID RbfNotifyFree(_In_ PRBF_NOTIFICATION Note)
{
    if (Note) ExFreePoolWithTag(Note, RBF_TAG);
}

/* ============================================================
 * Async notification queue (bounded) + async send worker
 * ========================================================== */

/* ============================================================
 * Queue slot reservation  (RB-08)
 *
 * A burst of deletes -- `rd /s`, a cleanup job, a backup rotation -- produces
 * one notification per file. When the bounded queue fills, the old code
 * dropped the notification AFTER the file had already been renamed into
 * staging, so every dropped notification silently became an orphan: a staged
 * file with no database row, invisible to restore, outside quota accounting,
 * and never purged. Those accumulate until the shared volume fills up.
 *
 * Reserving inverts the order. The callback takes a queue slot before it
 * stages anything, so "queue is full" is discovered while the file is still
 * untouched and the delete can simply be refused (fail-closed). The staged
 * file can then never outlive its notification.
 * ========================================================== */
BOOLEAN RbfReserveQueueSlot(VOID)
{
    KIRQL   irql;
    BOOLEAN ok = FALSE;

    KeAcquireSpinLock(&G.QueueLock, &irql);
    if (G.Reserved < RBF_QUEUE_MAX) {
        G.Reserved++;
        ok = TRUE;
    }
    KeReleaseSpinLock(&G.QueueLock, irql);

    return ok;
}

/* Release a reservation that was never converted into a queued entry. */
VOID RbfReleaseQueueSlot(VOID)
{
    KIRQL irql;

    KeAcquireSpinLock(&G.QueueLock, &irql);
    if (G.Reserved > 0) G.Reserved--;
    KeReleaseSpinLock(&G.QueueLock, irql);
}

/* Convert a reservation into a queued entry. Caller holds no lock. */
static VOID RbfCommitQueueSlot(VOID)
{
    KIRQL irql;

    KeAcquireSpinLock(&G.QueueLock, &irql);
    if (G.Reserved > 0) G.Reserved--;
    G.QueueDepth++;
    if (G.QueueDepth > G.MaxQueueDepth) G.MaxQueueDepth = G.QueueDepth;
    KeReleaseSpinLock(&G.QueueLock, irql);
}

/* Takes ownership of Note and frees it on every failure path.
   Assumes a slot was reserved via RbfReserveQueueSlot(); the reservation is
   always released here, on both success and failure. */
NTSTATUS RbfQueueNotify(_In_ PRBF_NOTIFICATION Note)
{
    PRBF_NOTIFY_NODE node;
    KIRQL irql;
    PFLT_PORT port;

    /* Reject anything that is not a well-formed notification rather than
       walking off its offsets. */
    if (!Note ||
        Note->Magic != RBF_NOTIFY_MAGIC ||
        Note->TotalSize < sizeof(RBF_NOTIFICATION) ||
        Note->TotalSize > RBF_NOTIFY_MAX_SIZE) {
        RbfReleaseQueueSlot();
        return STATUS_INVALID_PARAMETER;
    }

    if (!G.QueueActive) {
        RbfNotifyFree(Note);
        RbfReleaseQueueSlot();
        InterlockedIncrement64((volatile LONG64 *)&G.Stats.NotifyDropped);
        return STATUS_DEVICE_NOT_READY;
    }

    /* Snapshot the client port under the queue lock. */
    KeAcquireSpinLock(&G.QueueLock, &irql);
    port = G.ClientPort;
    KeReleaseSpinLock(&G.QueueLock, irql);
    if (port == NULL) {
        RbfNotifyFree(Note);
        RbfReleaseQueueSlot();
        InterlockedIncrement64((volatile LONG64 *)&G.Stats.NotifyDropped);
        return STATUS_PORT_DISCONNECTED;
    }

    node = ExAllocatePool2(POOL_FLAG_PAGED, sizeof(RBF_NOTIFY_NODE), RBF_TAG);
    if (!node) {
        RbfNotifyFree(Note);
        RbfReleaseQueueSlot();
        InterlockedIncrement64((volatile LONG64 *)&G.Stats.NotifyDropped);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    node->Notification = Note;
    node->Size         = Note->TotalSize;
    node->Port         = port;

    /* The slot was reserved before staging, so the queue cannot be over
       capacity here -- this insert is the reservation being honoured. */
    KeAcquireSpinLock(&G.QueueLock, &irql);
    InsertTailList(&G.NotifyQueue, &node->Entry);
    if (G.Reserved > 0) G.Reserved--;
    G.QueueDepth++;
    if (G.QueueDepth > G.MaxQueueDepth) G.MaxQueueDepth = G.QueueDepth;
    G.Stats.QueueDepth    = G.QueueDepth;
    G.Stats.MaxQueueDepth = G.MaxQueueDepth;
    KeReleaseSpinLock(&G.QueueLock, irql);

    /* Wake the send thread. */
    KeSetEvent(&G.NotifyEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

/* Dedicated system thread that drains the queue via FltSendMessage.
   FltSendMessage may block while user-mode is slow; a bounded queue
   upstream protects us, and this thread never touches the I/O path. */
VOID RbfSendThread(_In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    while (G.QueueActive) {
        KeWaitForSingleObject(&G.NotifyEvent, Executive, KernelMode,
                              FALSE, NULL);
        if (!G.QueueActive)
            break;

        while (1) {
            PRBF_NOTIFY_NODE node;
            PFLT_PORT port;
            NTSTATUS status;
            KIRQL irql;

            KeAcquireSpinLock(&G.QueueLock, &irql);
            if (IsListEmpty(&G.NotifyQueue)) {
                KeReleaseSpinLock(&G.QueueLock, irql);
                break;
            }
            node = CONTAINING_RECORD(RemoveHeadList(&G.NotifyQueue),
                                     RBF_NOTIFY_NODE, Entry);
            G.QueueDepth--;
            G.Stats.QueueDepth = G.QueueDepth;
            port = node->Port;
            KeReleaseSpinLock(&G.QueueLock, irql);

            {
                /* 30 s relative timeout: prevents the send thread from
                   blocking forever if user-mode stops reading but the
                   port is still open. */
                LARGE_INTEGER timeout;
                timeout.QuadPart = -30LL * 10 * 1000 * 1000; /* 100ns units */
                /* Send only the bytes this notification actually uses. */
                status = FltSendMessage(G.Filter, &port,
                                        node->Notification, node->Size,
                                        NULL, NULL, &timeout);
            }
            if (NT_SUCCESS(status)) {
                InterlockedIncrement64((volatile LONG64 *)&G.Stats.NotifySent);
            } else {
                InterlockedIncrement64((volatile LONG64 *)&G.Stats.NotifyDropped);
            }
            RbfNotifyFree(node->Notification);
            ExFreePoolWithTag(node, RBF_TAG);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID RbfFlushQueue(VOID)
{
    PRBF_NOTIFY_NODE node;
    KIRQL irql;
    while (1) {
        KeAcquireSpinLock(&G.QueueLock, &irql);
        if (IsListEmpty(&G.NotifyQueue)) {
            KeReleaseSpinLock(&G.QueueLock, irql);
            break;
        }
        node = CONTAINING_RECORD(RemoveHeadList(&G.NotifyQueue),
                                 RBF_NOTIFY_NODE, Entry);
        G.QueueDepth--;
        KeReleaseSpinLock(&G.QueueLock, irql);
        RbfNotifyFree(node->Notification);
        ExFreePoolWithTag(node, RBF_TAG);
    }
}

/* ============================================================
 * Instance / unload
 * ========================================================== */
NTSTATUS RbfInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS  FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE            VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE    VolumeFilesystemType)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(VolumeDeviceType);
    if (VolumeFilesystemType != FLT_FSTYPE_NTFS)
        return STATUS_FLT_DO_NOT_ATTACH;
    return STATUS_SUCCESS;
}

NTSTATUS RbfInstanceQueryTeardown(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
    return STATUS_SUCCESS;
}

NTSTATUS RbfUnload(_In_ FLT_FILTER_UNLOAD_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Flags);
    G.QueueActive = FALSE;

    /* Wake and wait for the send thread to exit. */
    KeSetEvent(&G.NotifyEvent, IO_NO_INCREMENT, FALSE);
    if (G.SendThreadHandle) {
        KeWaitForSingleObject(G.SendThreadHandle, Executive, KernelMode,
                              FALSE, NULL);
        ZwClose(G.SendThreadHandle);
        G.SendThreadHandle = NULL;
    }

    RbfFlushQueue();
    if (G.ServerPort) FltCloseCommunicationPort(G.ServerPort);
    if (G.Filter)     FltUnregisterFilter(G.Filter);
    RbfFreeConfig();
    if (G.StoreRoot.Buffer) ExFreePoolWithTag(G.StoreRoot.Buffer, RBF_TAG);
    return STATUS_SUCCESS;
}

/* ============================================================
 * Communication port
 * ========================================================== */
NTSTATUS RbfPortConnect(
    _In_     PFLT_PORT ClientPort,
    _In_opt_ PVOID     ServerPortCookie,
    _In_     PVOID     ConnectionContext,
    _In_     ULONG     SizeOfContext,
    _Out_    PVOID*    ConnectionPortCookie)
{
    UNREFERENCED_PARAMETER(ServerPortCookie);
    UNREFERENCED_PARAMETER(ConnectionContext);
    UNREFERENCED_PARAMETER(SizeOfContext);
    UNREFERENCED_PARAMETER(ConnectionPortCookie);
    {
        KIRQL irql;
        KeAcquireSpinLock(&G.QueueLock, &irql);
        G.ClientPort = ClientPort;
        KeReleaseSpinLock(&G.QueueLock, irql);
    }
    DbgPrint("[RBF] User-mode service connected\n");
    return STATUS_SUCCESS;
}

VOID RbfPortDisconnect(_In_opt_ PVOID ConnectionCookie)
{
    UNREFERENCED_PARAMETER(ConnectionCookie);
    {
        KIRQL irql;
        KeAcquireSpinLock(&G.QueueLock, &irql);
        G.ClientPort = NULL;
        KeReleaseSpinLock(&G.QueueLock, irql);
    }
    DbgPrint("[RBF] User-mode service disconnected\n");
}

/* Handle user-mode -> driver commands (RBF_CMD_QUERY_STATS). */
NTSTATUS RbfPortMessage(
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_opt_(OutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength)
{
    PRBF_REPLY reply;
    UNREFERENCED_PARAMETER(ClientPort);
    UNREFERENCED_PARAMETER(ServerPortCookie);

    if (ReturnOutputBufferLength) *ReturnOutputBufferLength = 0;
    if (!InputBuffer || InputBufferLength < sizeof(RBF_REPLY))
        return STATUS_INVALID_PARAMETER;

    reply = (PRBF_REPLY)InputBuffer;
    if (reply->Ack == RBF_CMD_QUERY_STATS) {
        if (OutputBufferLength < sizeof(RBF_STATS))
            return STATUS_BUFFER_TOO_SMALL;
        RtlCopyMemory(OutputBuffer, &G.Stats, sizeof(RBF_STATS));
        if (ReturnOutputBufferLength) *ReturnOutputBufferLength = sizeof(RBF_STATS);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ============================================================
 * Fail-open / fail-closed decision  (RB-04)
 *
 * Every point where the driver cannot stage a delete used to fall back to
 * "allow the real delete", silently and permanently destroying the very file
 * this product exists to protect. Those failures now all route through here:
 *
 *   fail-closed (1, default) -- complete the request with STATUS_ACCESS_DENIED.
 *       The data survives and the user simply retries once staging is healthy.
 *   fail-open   (0, legacy)  -- let the delete proceed and lose the file.
 *
 * Both branches are counted, so a sick staging area is visible in the
 * statistics instead of hiding behind a string of invisible data losses.
 * ========================================================== */
static FLT_PREOP_CALLBACK_STATUS
RbfFailDelete(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_    PCWSTR             Reason,
    _In_    NTSTATUS           FailureStatus)
{
    if (!G.FailClosed) {
        DbgPrint("[RBF] %ws failed 0x%X, allowing real delete (fail-open)\n",
                 Reason, FailureStatus);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    InterlockedIncrement64((volatile LONG64 *)&G.Stats.DeleteDenied);
    DbgPrint("[RBF] %ws failed 0x%X, denying delete (fail-closed)\n",
             Reason, FailureStatus);

    Data->IoStatus.Status      = STATUS_ACCESS_DENIED;
    Data->IoStatus.Information = 0;
    return FLT_PREOP_COMPLETE;
}

/* ============================================================
 * Core: Pre delete callback
 * ========================================================== */
FLT_PREOP_CALLBACK_STATUS
RbfPreSetInfo(
    _Inout_ PFLT_CALLBACK_DATA    Data,
    _In_    PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID* CompletionContext)
{
    PFILE_DISPOSITION_INFORMATION dispInfo;
    PFLT_FILE_NAME_INFORMATION    nameInfo = NULL;
    FILE_STANDARD_INFORMATION     stdInfo;
    UNICODE_STRING                storePath = {0};
    PRBF_NOTIFICATION             note = NULL;   /* paged pool, never on stack */
    NTSTATUS                      status;
    ULONG                         sessionId = 0;

    UNREFERENCED_PARAMETER(CompletionContext);

    /* 1. Only care about delete mark */
    if (Data->Iopb->Parameters.SetFileInformation.FileInformationClass
            != FileDispositionInformation)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    dispInfo = (PFILE_DISPOSITION_INFORMATION)
               Data->Iopb->Parameters.SetFileInformation.InfoBuffer;
    if (!dispInfo->DeleteFile)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    /* 2. Path-based policy: intercept ANY delete hitting a protected
       prefix (local or remote). SMB2 deletes are executed by srv2.sys in
       session 0, so RequestorSessionId is 0 even for remote clients and
       cannot be used as a discriminator; we only record it for audit. */
    status = FltGetRequestorSessionId(Data, &sessionId);
    if (!NT_SUCCESS(status))
        sessionId = 0;

    /* 3. Get path */
    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInfo);
    if (!NT_SUCCESS(status))
        return RbfFailDelete(Data, L"get file name", status);

    FltParseFileNameInformation(nameInfo);

    /* 4. Path matches protected prefix? otherwise allow */
    if (!RbfIsProtected(&nameInfo->Name)) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    InterlockedIncrement64((volatile LONG64 *)&G.Stats.Intercepts);

    /* 5. Get requestor SID: real client SID from the requestor token
       (SMB impersonates the client user), placeholder fallback. */
    UNICODE_STRING sidStr = {0};
    status = RbfGetRequestorSid(Data, sessionId, &sidStr);
    if (!NT_SUCCESS(status)) {
        FltReleaseFileNameInformation(nameInfo);
        return RbfFailDelete(Data, L"get requestor sid", status);
    }

    /* 5.2 Reserve a notification slot BEFORE staging (RB-08).
          A burst of deletes (rd /s, cleanup jobs) can outrun the queue; if we
          only discovered that after the rename, every dropped notification
          would leave an unrecoverable orphan in staging. Checking here means
          a full queue refuses the delete while the data is still intact. */
    if (!RbfReserveQueueSlot()) {
        InterlockedIncrement64((volatile LONG64 *)&G.Stats.NotifyQueueFull);
        ExFreePoolWithTag(sidStr.Buffer, RBF_TAG);
        FltReleaseFileNameInformation(nameInfo);
        return RbfFailDelete(Data, L"notification queue full",
                             STATUS_INSUFFICIENT_RESOURCES);
    }

    /* 5.5 Ensure staging directories exist: <vol>\RBStore and <vol>\RBStore\<sid> */
    status = RbfEnsureStoreDir(&nameInfo->Volume, &sidStr);
    if (!NT_SUCCESS(status)) {
        RbfReleaseQueueSlot();
        ExFreePoolWithTag(sidStr.Buffer, RBF_TAG);
        FltReleaseFileNameInformation(nameInfo);
        return RbfFailDelete(Data, L"ensure store dir", status);
    }

    /* 6. Build staging target path: <Vol>\RBStore\<Sid>\<rel> */
    status = RbfBuildStorePath(&sidStr, &nameInfo->Name, &storePath);
    if (!NT_SUCCESS(status)) {
        RbfReleaseQueueSlot();
        if (sidStr.Buffer) ExFreePoolWithTag(sidStr.Buffer, RBF_TAG);
        FltReleaseFileNameInformation(nameInfo);
        return RbfFailDelete(Data, L"build store path", status);
    }

    /* 7. Execute rename to staging */
    status = RbfMoveToStore(FltObjects, FltObjects->FileObject, &storePath);
    if (NT_SUCCESS(status)) {
        /* Success: complete the original DELETE as success */
        InterlockedIncrement64((volatile LONG64 *)&G.Stats.RenameOk);
        Data->IoStatus.Status      = STATUS_SUCCESS;
        Data->IoStatus.Information = 0;

        /* 8. Build a variable-length notification and enqueue it (RB-01).
              Allocated from paged pool, so the pre-callback's stack frame
              stays at a few dozen bytes instead of ~64 KB. */
        note = NULL;
        status = RbfAllocNotify(nameInfo->Name.Length + sizeof(WCHAR),
                                storePath.Length      + sizeof(WCHAR),
                                sidStr.Length         + sizeof(WCHAR),
                                &note);
        if (!NT_SUCCESS(status)) {
            /* The rename already succeeded, so the file sits in staging with
               no database row: an orphan. Count the drop so it shows up in
               the statistics instead of failing silently. */
            InterlockedIncrement64((volatile LONG64 *)&G.Stats.NotifyDropped);
            DbgPrint("[RBF] alloc notify failed 0x%X, staged file orphaned\n",
                     status);
        } else {
            RbfNotifySetPath(note, &nameInfo->Name);
            RbfNotifySetStorePath(note, &storePath);
            RbfNotifySetSid(note, &sidStr);
            note->FileSize    = 0;
            note->SessionId   = sessionId;
            note->IsDirectory = 0;
            if (NT_SUCCESS(FltQueryInformationFile(
                    FltObjects->Instance, FltObjects->FileObject,
                    &stdInfo, sizeof(stdInfo), FileStandardInformation, NULL))) {
                note->FileSize    = stdInfo.EndOfFile.QuadPart;
                note->IsDirectory = stdInfo.Directory;
            }
            RbfQueueNotify(note);   /* takes ownership, frees on failure */
            note = NULL;
        }

        ExFreePoolWithTag(storePath.Buffer, RBF_TAG);
        if (sidStr.Buffer) ExFreePoolWithTag(sidStr.Buffer, RBF_TAG);
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_COMPLETE;
    }

    /* Staging failed. The file is untouched so far -- decide whether the
       delete may proceed and destroy it, per the fail-closed policy. */
    InterlockedIncrement64((volatile LONG64 *)&G.Stats.RenameFail);
    RbfReleaseQueueSlot();
    ExFreePoolWithTag(storePath.Buffer, RBF_TAG);
    if (sidStr.Buffer) ExFreePoolWithTag(sidStr.Buffer, RBF_TAG);
    FltReleaseFileNameInformation(nameInfo);
    return RbfFailDelete(Data, L"rename to staging", status);
}

/* ============================================================
 * Registration / callback table
 * ========================================================== */
const FLT_OPERATION_REGISTRATION G_Callbacks[] = {
    { IRP_MJ_SET_INFORMATION, 0, RbfPreSetInfo, NULL },
    { IRP_MJ_OPERATION_END }
};

const FLT_REGISTRATION G_FilterReg = {
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    NULL,
    G_Callbacks,
    RbfUnload,
    RbfInstanceSetup,
    RbfInstanceQueryTeardown,
    NULL, NULL, NULL, NULL, NULL
};

/* ============================================================
 * DriverEntry
 * ========================================================== */
NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS          status;
    UNICODE_STRING    portName;
    UNICODE_STRING    altitude;
    OBJECT_ATTRIBUTES oa;
    PSECURITY_DESCRIPTOR sd = NULL;

    UNREFERENCED_PARAMETER(RegistryPath);

    /* Global init */
    RtlZeroMemory(&G, sizeof(G));
    KeInitializeSpinLock(&G.QueueLock);
    InitializeListHead(&G.NotifyQueue);
    KeInitializeEvent(&G.NotifyEvent, SynchronizationEvent, FALSE);
    G.QueueActive = TRUE;
    G.ClientPort = NULL;
    G.SendThreadHandle = NULL;

    /* Default StoreRoot (reserved, paths derived from source volume) */
    RtlInitUnicodeString(&G.StoreRoot, L"\\??\\C:\\RBStore");

    /* RB-04: never destroy data silently. RbfLoadConfig() may still turn this
       off explicitly via Parameters\FailClosed = 0. */
    G.FailClosed = 1;

    RbfLoadConfig();

    status = FltRegisterFilter(DriverObject, &G_FilterReg, &G.Filter);
    if (!NT_SUCCESS(status)) { RbfFreeConfig(); return status; }

    /* Communication port */
    RtlInitUnicodeString(&portName, RBF_PORT_NAME);
    status = FltBuildDefaultSecurityDescriptor(&sd, FLT_PORT_ALL_ACCESS);
    if (!NT_SUCCESS(status)) { FltUnregisterFilter(G.Filter); RbfFreeConfig(); return status; }

    InitializeObjectAttributes(&oa, &portName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, sd);
    status = FltCreateCommunicationPort(
        G.Filter, &G.ServerPort, &oa, NULL,
        RbfPortConnect, RbfPortDisconnect,
        (PFLT_MESSAGE_NOTIFY)RbfPortMessage, 1);
    FltFreeSecurityDescriptor(sd);
    if (!NT_SUCCESS(status)) {
        FltUnregisterFilter(G.Filter);
        RbfFreeConfig();
        return status;
    }

    /* Dedicated async notification sender thread (single consumer). */
    status = PsCreateSystemThread(&G.SendThreadHandle, THREAD_ALL_ACCESS,
                                  NULL, NULL, NULL, RbfSendThread, NULL);
    if (!NT_SUCCESS(status)) {
        FltCloseCommunicationPort(G.ServerPort);
        FltUnregisterFilter(G.Filter);
        RbfFreeConfig();
        return status;
    }

    /* Altitude string (some WDK versions require it in registry; INF already sets it) */
    RtlInitUnicodeString(&altitude, RBF_ALTITUDE);
    UNREFERENCED_PARAMETER(altitude);

    status = FltStartFiltering(G.Filter);
    if (!NT_SUCCESS(status)) {
        G.QueueActive = FALSE;
        KeSetEvent(&G.NotifyEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(G.SendThreadHandle, Executive, KernelMode,
                              FALSE, NULL);
        ZwClose(G.SendThreadHandle);
        G.SendThreadHandle = NULL;
        FltCloseCommunicationPort(G.ServerPort);
        FltUnregisterFilter(G.Filter);
        RbfFreeConfig();
    }
    return status;
}
