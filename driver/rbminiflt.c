/*
 * rbminiflt.c - SMB remote-delete intercept Mini-Filter driver
 *
 * Behavior:
 *   PreSetInfo(FileDispositionInformation, DeleteFile=1):
 *     1. Only intercept remote sessions (RequestorSessionId != 0)
 *     2. Path matches a protected share prefix?
 *     3. Rename to \RBStore\<VolGuid>\<Sid>\<relative path>
 *     4. Success -> COMPLETE(STATUS_SUCCESS)  (user thinks the delete succeeded)
 *     5. Failure -> allow real delete + event log
 *     6. Async enqueue notification -> communication port (non-blocking)
 *
 * Build: WDK 10 + VS2022, x64, Driver Type = WDM (pure Mini-Filter)
 * Linker Input: fltMgr.lib
 */
#include "rbminiflt.h"

RBF_GLOBAL G;

/* ============================================================
 * Config: read protected paths and store root from registry
 *   HKLM\SYSTEM\CurrentControlSet\Services\rbminiflt\Parameters
 *     ProtectedPaths REG_MULTI_SZ  (NT-style, e.g. \Device\HarddiskVolume2\Share)
 *     StoreRoot      REG_SZ        (e.g. C:\RBStore) -- reserved, paths derived
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
   (\Device\HarddiskVolumeN\...). Cached prefixes are already uppercase. */
BOOLEAN RbfIsProtected(_In_ PCUNICODE_STRING Path)
{
    ULONG i;
    UNICODE_STRING up;
    if (G.ProtectedCount == 0) return FALSE;
    up.Buffer = ExAllocatePool2(POOL_FLAG_PAGED, Path->Length, RBF_TAG);
    if (!up.Buffer) return FALSE;
    up.MaximumLength = Path->Length;
    up.Length = Path->Length;
    RtlCopyMemory(up.Buffer, Path->Buffer, Path->Length);
    RtlUpcaseUnicodeString(&up, &up, FALSE);

    for (i = 0; i < G.ProtectedCount; i++) {
        if (up.Length >= G.Protected[i].Prefix.Length &&
            RtlCompareMemory(up.Buffer,
                             G.Protected[i].Prefix.Buffer,
                             G.Protected[i].Prefix.Length) == G.Protected[i].Prefix.Length) {
            ExFreePoolWithTag(up.Buffer, RBF_TAG);
            return TRUE;
        }
    }
    ExFreePoolWithTag(up.Buffer, RBF_TAG);
    return FALSE;
}

/* ============================================================
 * Session / SID
 *  Resolving a real SID string in kernel mode is expensive and the
 *  relevant APIs are unstable across WDK versions. We instead return
 *  a placeholder SID derived from the SessionId; the user-mode
 *  service resolves the real SID using WTS APIs.
 *  On success, SidString->Buffer must be freed by the caller.
 * ========================================================== */
NTSTATUS RbfGetRequestorSid(_In_ PFLT_CALLBACK_DATA Data,
                            _In_ ULONG SessionId,
                            _Out_ PUNICODE_STRING SidString)
{
    UNREFERENCED_PARAMETER(Data);
    SidString->Buffer = NULL;
    SidString->Length = SidString->MaximumLength = 0;
    SidString->MaximumLength = (USHORT)(RBF_MAX_NAME * sizeof(WCHAR));
    SidString->Buffer = ExAllocatePool2(POOL_FLAG_PAGED,
                                        SidString->MaximumLength, RBF_TAG);
    if (!SidString->Buffer) return STATUS_INSUFFICIENT_RESOURCES;

    /* Manual build of L"S-SESSION-<id>" without CRT/ntstrsafe dependency. */
    {
        PCWSTR prefix = L"S-SESSION-";
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
 *   protected prefix). We do NOT recurse-into ourselves because the
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
 * Build staging target path (same volume, avoids cross-volume rename):
 *   <source volume>\RBStore\<Sid>\<relative path>
 *  Example: \Device\HarddiskVolume2\Share\a.txt
 *        -> \Device\HarddiskVolume2\RBStore\S-SESSION-1\Share\a.txt
 *  This puts the target outside the protected prefix
 *  (\Device\HarddiskVolume2\Share) so the Pre callback won't recurse.
 * ========================================================== */
NTSTATUS RbfBuildStorePath(_In_ PCUNICODE_STRING SidString,
                           _In_ PCUNICODE_STRING SrcPath,
                           _Out_ PUNICODE_STRING StorePath)
{
    NTSTATUS status;
    USHORT volLen = 0;
    UNICODE_STRING volPart, restPart, rbDir;
    USHORT i, slashes = 0;

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
    volLen = i; /* index of the third '\' (inclusive) */
    if (volLen == 0 || volLen >= SrcPath->Length / sizeof(WCHAR)) {
        ExFreePoolWithTag(StorePath->Buffer, RBF_TAG);
        StorePath->Buffer = NULL;
        return STATUS_INVALID_PARAMETER;
    }

    volPart.Buffer = SrcPath->Buffer;
    volPart.Length = volPart.MaximumLength = (USHORT)(volLen * sizeof(WCHAR));
    restPart.Buffer = SrcPath->Buffer + volLen;  /* includes leading '\' */
    restPart.Length = restPart.MaximumLength =
        (USHORT)(SrcPath->Length - volPart.Length);

    RtlInitUnicodeString(&rbDir, L"\\RBStore");

    status = RtlAppendUnicodeStringToString(StorePath, &volPart);
    if (NT_SUCCESS(status))
        status = RtlAppendUnicodeStringToString(StorePath, &rbDir);
    if (NT_SUCCESS(status))
        status = RtlAppendUnicodeStringToString(StorePath, SidString);
    if (NT_SUCCESS(status))
        status = RtlAppendUnicodeStringToString(StorePath, &restPart);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(StorePath->Buffer, RBF_TAG);
        StorePath->Buffer = NULL;
    }
    return status;
}

/* ============================================================
 * Async notification queue
 * ========================================================== */
NTSTATUS RbfQueueNotify(_In_ PRBF_NOTIFICATION Note)
{
    PRBF_NOTIFY_NODE node;
    KIRQL irql;

    if (!G.QueueActive) return STATUS_DEVICE_NOT_READY;

    node = ExAllocatePool2(POOL_FLAG_PAGED, sizeof(RBF_NOTIFY_NODE), RBF_TAG);
    if (!node) return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(&node->Notification, Note, sizeof(RBF_NOTIFICATION));
    KeAcquireSpinLock(&G.QueueLock, &irql);
    InsertTailList(&G.NotifyQueue, &node->Entry);
    KeReleaseSpinLock(&G.QueueLock, irql);

    KeSetEvent(&G.NotifyEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
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
        KeReleaseSpinLock(&G.QueueLock, irql);
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
    G.ClientPort = ClientPort;
    DbgPrint("[RBF] User-mode service connected\n");
    return STATUS_SUCCESS;
}

VOID RbfPortDisconnect(_In_opt_ PVOID ConnectionCookie)
{
    UNREFERENCED_PARAMETER(ConnectionCookie);
    G.ClientPort = NULL;
    DbgPrint("[RBF] User-mode service disconnected\n");
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
    RBF_NOTIFICATION              note = {0};
    NTSTATUS                      status;
    ULONG                         sessionId = 0;
    BOOLEAN                       isRemote = FALSE;

    UNREFERENCED_PARAMETER(CompletionContext);

    /* 1. Only care about delete mark */
    if (Data->Iopb->Parameters.SetFileInformation.FileInformationClass
            != FileDispositionInformation)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    dispInfo = (PFILE_DISPOSITION_INFORMATION)
               Data->Iopb->Parameters.SetFileInformation.InfoBuffer;
    if (!dispInfo->DeleteFile)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    /* 2. Only intercept remote sessions (SessionId != 0) */
    status = FltGetRequestorSessionId(Data, &sessionId);
    if (NT_SUCCESS(status) && sessionId != 0) {
        isRemote = TRUE;
    }
    if (!isRemote)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    /* 3. Get path */
    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInfo);
    if (!NT_SUCCESS(status))
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    FltParseFileNameInformation(nameInfo);

    /* 4. Path matches protected prefix? otherwise allow */
    if (!RbfIsProtected(&nameInfo->Name)) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    /* 5. Get requestor SID placeholder (SessionId-based, user-mode resolves) */
    UNICODE_STRING sidStr = {0};
    status = RbfGetRequestorSid(Data, sessionId, &sidStr);
    if (!NT_SUCCESS(status)) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    /* 6. Build staging target path: <Vol>\RBStore\<Sid>\<rel> */
    status = RbfBuildStorePath(&sidStr, &nameInfo->Name, &storePath);
    if (!NT_SUCCESS(status)) {
        if (sidStr.Buffer) ExFreePoolWithTag(sidStr.Buffer, RBF_TAG);
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    /* 7. Execute rename to staging */
    status = RbfMoveToStore(FltObjects, FltObjects->FileObject, &storePath);
    if (NT_SUCCESS(status)) {
        /* Success: complete the original DELETE as success */
        Data->IoStatus.Status      = STATUS_SUCCESS;
        Data->IoStatus.Information = 0;

        /* 8. Fill notification and async enqueue (non-blocking) */
        note.PathLength      = nameInfo->Name.Length;
        note.StorePathLength = storePath.Length;
        note.FileSize        = 0;
        note.SessionId       = sessionId;
        note.IsDirectory     = 0;
        if (NT_SUCCESS(FltQueryInformationFile(
                FltObjects->Instance, FltObjects->FileObject,
                &stdInfo, sizeof(stdInfo), FileStandardInformation, NULL))) {
            note.FileSize = stdInfo.EndOfFile.QuadPart;
            note.IsDirectory = stdInfo.Directory;
        }
        RtlCopyMemory(note.FilePath, nameInfo->Name.Buffer,
                      min(nameInfo->Name.Length, RBF_MAX_PATH * sizeof(WCHAR)));
        RtlCopyMemory(note.StorePath, storePath.Buffer,
                      min(storePath.Length, RBF_MAX_PATH * sizeof(WCHAR)));
        if (sidStr.Length < RBF_MAX_NAME * sizeof(WCHAR))
            RtlCopyMemory(note.SidString, sidStr.Buffer, sidStr.Length);
        RbfQueueNotify(&note);

        ExFreePoolWithTag(storePath.Buffer, RBF_TAG);
        if (sidStr.Buffer) ExFreePoolWithTag(sidStr.Buffer, RBF_TAG);
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_COMPLETE;
    }

    /* Failure: allow real delete (fail-open), just log */
    DbgPrint("[RBF] rename failed 0x%X, allowing real delete\n", status);
    ExFreePoolWithTag(storePath.Buffer, RBF_TAG);
    if (sidStr.Buffer) ExFreePoolWithTag(sidStr.Buffer, RBF_TAG);
    FltReleaseFileNameInformation(nameInfo);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
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
    KeInitializeEvent(&G.NotifyEvent, NotificationEvent, FALSE);
    G.QueueActive = TRUE;
    G.ClientPort = NULL;

    /* Default StoreRoot (reserved, paths derived from source volume) */
    RtlInitUnicodeString(&G.StoreRoot, L"\\??\\C:\\RBStore");

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
        RbfPortConnect, RbfPortDisconnect, NULL, 1);
    FltFreeSecurityDescriptor(sd);
    if (!NT_SUCCESS(status)) { FltUnregisterFilter(G.Filter); RbfFreeConfig(); return status; }

    /* Altitude string (some WDK versions require it in registry; INF already sets it) */
    RtlInitUnicodeString(&altitude, RBF_ALTITUDE);
    UNREFERENCED_PARAMETER(altitude);

    status = FltStartFiltering(G.Filter);
    if (!NT_SUCCESS(status)) {
        FltCloseCommunicationPort(G.ServerPort);
        FltUnregisterFilter(G.Filter);
        RbfFreeConfig();
    }
    return status;
}
