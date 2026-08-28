/*
 * rbminiflt.h - Shared constants and driver<->user mode message structures
 *
 * The user-mode service (rb_service.py) mirrors RBF_NOTIFICATION / RBF_REPLY /
 * RBF_STATS using ctypes (field layout must match exactly).
 */
#ifndef _RBMINIFLT_H_
#define _RBMINIFLT_H_

/* ntifs.h must precede fltKernel.h; it provides the Se and Rtl
   security APIs used for SID/ACL handling. */
#include <ntifs.h>
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

/* Bounded async notification queue (fire-and-forget, never blocks delete path) */
#define RBF_QUEUE_MAX       512

/* User-mode -> driver commands (RBF_REPLY.Ack) */
#define RBF_CMD_QUERY_STATS 1

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

/* User-mode -> driver command */
typedef struct _RBF_REPLY {
    ULONG   Ack;
} RBF_REPLY, *PRBF_REPLY;

/* Driver statistics (returned for RBF_CMD_QUERY_STATS) */
typedef struct _RBF_STATS {
    ULONG64 Intercepts;       /* remote + protected hit */
    ULONG64 RenameOk;         /* staged successfully */
    ULONG64 RenameFail;       /* failed -> fail-open real delete */
    ULONG64 NotifySent;       /* delivered to user-mode */
    ULONG64 NotifyDropped;    /* dropped (no client / alloc failure / send error) */
    ULONG64 NotifyQueueFull;  /* dropped because queue was full */
    ULONG   QueueDepth;       /* current queue depth */
    ULONG   MaxQueueDepth;    /* high-water mark */
} RBF_STATS, *PRBF_STATS;
#pragma pack(pop)

/* Async notification queue node (fire-and-forget) */
typedef struct _RBF_NOTIFY_NODE {
    LIST_ENTRY      Entry;
    PFLT_PORT       Port;           /* client port snapshot at enqueue time */
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

    /* Async notification queue (bounded) */
    KSPIN_LOCK      QueueLock;
    LIST_ENTRY      NotifyQueue;
    BOOLEAN         QueueActive;
    ULONG           QueueDepth;
    ULONG           MaxQueueDepth;

    /* Async send thread (single consumer, serialized by design) */
    KEVENT          NotifyEvent;
    HANDLE          SendThreadHandle;

    /* Statistics (visible to user-mode via RBF_CMD_QUERY_STATS) */
    RBF_STATS       Stats;

    /* Monotonic sequence for flat staging names (<seq>_<basename>) */
    ULONG64         StageSeq;

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
NTSTATUS RbfPortMessage(PFLT_PORT, PVOID, PVOID, ULONG, PVOID, ULONG, PULONG);
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
NTSTATUS RbfEnsureStoreDir(_In_ PCUNICODE_STRING VolumeName,
                           _In_ PCUNICODE_STRING SidString);
NTSTATUS RbfCreateDirectory(_In_ PCUNICODE_STRING DirPath);
NTSTATUS RbfMoveToStore(_In_ PCFLT_RELATED_OBJECTS FltObjects,
                        _In_ PFILE_OBJECT FileObject,
                        _In_ PCUNICODE_STRING StorePath);

/* Notification queue */
NTSTATUS RbfQueueNotify(_In_ PRBF_NOTIFICATION Note);
VOID     RbfFlushQueue(VOID);
VOID     RbfSendThread(_In_ PVOID Context);

/* Session / SID */
NTSTATUS RbfGetRequestorSid(_In_ PFLT_CALLBACK_DATA Data,
                            _In_ ULONG SessionId,
                            _Out_ PUNICODE_STRING SidString);

/* Build staging target path (flat): <source volume>\RBStore\<Sid>\<seq>_<basename> */
NTSTATUS RbfBuildStorePath(_In_ PCUNICODE_STRING SidString,
                           _In_ PCUNICODE_STRING SrcPath,
                           _Out_ PUNICODE_STRING StorePath);

#endif /* _RBMINIFLT_H_ */
