/*
 * rbminiflt.h - Shared constants and driver<->user mode message structures
 *
 * The user-mode service (rb_service.py) mirrors INTERCEPT_NOTIFICATION / INTERCEPT_REPLY
 * using ctypes.
 */
#ifndef _RBMINIFLT_H_
#define _RBMINIFLT_H_

#include <fltKernel.h>

/* Basic constants */
#define RBF_TAG             'rbfR'      /* Pool tag */
#define RBF_PORT_NAME       L"\\RecycleBinPort"
#define RBF_ALTITUDE        L"370030"   /* Activity Monitor range */
/* Path buffer (WCHAR count). Kept <= 16383 so that
   MaximumLength (= count*sizeof(WCHAR)) fits in a USHORT. */
#define RBF_MAX_PATH        16383
#define RBF_MAX_NAME        256

/* Maximum number of protected path entries cached in driver */
#define RBF_MAX_PROTECTED   16

/* Driver -> user-mode notification */
#pragma pack(push, 1)
typedef struct _RBF_NOTIFICATION {
    WCHAR   FilePath[RBF_MAX_PATH];   /* Original full path (NT form) */
    ULONG   PathLength;               /* Bytes (excluding NULL) */
    WCHAR   StorePath[RBF_MAX_PATH];  /* Staging target path (NT form) */
    ULONG   StorePathLength;
    ULONG64 FileSize;                 /* Bytes */
    ULONG   SessionId;                /* Requestor session ID */
    ULONG   IsDirectory;              /* 1 = directory */
    WCHAR   SidString[RBF_MAX_NAME];  /* Requestor SID string */
} RBF_NOTIFICATION, *PRBF_NOTIFICATION;

/* User-mode -> driver: currently unused (reserved) */
typedef struct _RBF_REPLY {
    ULONG   Ack;
} RBF_REPLY, *PRBF_REPLY;
#pragma pack(pop)

/* Async notification queue node (fire-and-forget) */
typedef struct _RBF_NOTIFY_NODE {
    LIST_ENTRY      Entry;
    RBF_NOTIFICATION Notification;
} RBF_NOTIFY_NODE, *PRBF_NOTIFY_NODE;

/* Protected path cache entry */
typedef struct _RBF_PROTECTED {
    UNICODE_STRING  Prefix;         /* NT-style prefix (uppercase) */
    WCHAR           Buffer[RBF_MAX_PATH];
} RBF_PROTECTED, *PRBF_PROTECTED;

/* Driver global state */
typedef struct _RBF_GLOBAL {
    PFLT_FILTER     Filter;
    PFLT_PORT       ServerPort;
    PFLT_PORT       ClientPort;     /* Set when user-mode service connects */

    /* Async notification queue */
    KSPIN_LOCK      QueueLock;
    LIST_ENTRY      NotifyQueue;
    KEVENT          NotifyEvent;    /* Wake user-mode reader */
    BOOLEAN         QueueActive;

    /* Protected path cache (uppercase) */
    RBF_PROTECTED   Protected[RBF_MAX_PROTECTED];
    ULONG           ProtectedCount;

    /* Staging root (NT form, e.g. \Device\HarddiskVolume1\RBStore) */
    UNICODE_STRING  StoreRoot;
} RBF_GLOBAL, *PRBF_GLOBAL;

extern RBF_GLOBAL G;

/* Forward declarations */
FLT_PREOP_CALLBACK_STATUS RbfPreSetInfo(
    _Inout_ PFLT_CALLBACK_DATA,
    _In_    PCFLT_RELATED_OBJECTS,
    _Outptr_result_maybenull_ PVOID*);

NTSTATUS RbfPortConnect(PFLT_PORT, PVOID, PVOID, ULONG, PVOID*);
VOID     RbfPortDisconnect(PVOID);
NTSTATUS RbfUnload(FLT_FILTER_UNLOAD_FLAGS);
NTSTATUS RbfInstanceSetup(PCFLT_RELATED_OBJECTS, FLT_INSTANCE_SETUP_FLAGS,
                          DEVICE_TYPE, FLT_FILESYSTEM_TYPE);
NTSTATUS RbfInstanceQueryTeardown(PCFLT_RELATED_OBJECTS,
                                  FLT_INSTANCE_QUERY_TEARDOWN_FLAGS);

/* Config / policy */
NTSTATUS RbfLoadConfig(VOID);
VOID     RbfFreeConfig(VOID);
BOOLEAN  RbfIsProtected(_In_ PCUNICODE_STRING Path);

/* Staging / move */
NTSTATUS RbfEnsureStoreDir(_In_ PCFLT_RELATED_OBJECTS FltObjects,
                           _In_ PCUNICODE_STRING VolumeName);
NTSTATUS RbfMoveToStore(_In_ PCFLT_RELATED_OBJECTS FltObjects,
                        _In_ PFILE_OBJECT FileObject,
                        _In_ PCUNICODE_STRING StorePath);

/* Notification queue */
NTSTATUS RbfQueueNotify(_In_ PRBF_NOTIFICATION Note);
VOID     RbfFlushQueue(VOID);

/* Session / SID */
NTSTATUS RbfGetRequestorSid(_In_ PFLT_CALLBACK_DATA Data,
                            _In_ ULONG SessionId,
                            _Out_ PUNICODE_STRING SidString);

/* Build staging target path: <source volume>\RBStore\<Sid>\<relative path> */
NTSTATUS RbfBuildStorePath(_In_ PCUNICODE_STRING SidString,
                           _In_ PCUNICODE_STRING SrcPath,
                           _Out_ PUNICODE_STRING StorePath);

#endif /* _RBMINIFLT_H_ */
