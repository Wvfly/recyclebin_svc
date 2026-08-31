/*
 * rbminiflt.h - Shared constants and driver<->user mode message structures
 *
 * The user-mode service (service_c/rbservice.exe) mirrors RBF_NOTIFICATION /
 * RBF_REPLY / RBF_STATS in service_c/rbf_protocol.h, which carries compile-time
 * static asserts on the byte layout. Change a struct here and the service fails
 * to BUILD rather than silently mis-parsing notifications at runtime.
 *
 * Note: rbf_protocol.h cannot simply #include this file because ntifs.h /
 * fltKernel.h are kernel-only. Keep the two wire structs in sync; the asserts
 * in rbf_protocol.h enforce it.
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
/* Path buffer (WCHAR count).
 *
 * Was 16383, which made one RBF_NOTIFICATION roughly 64 KB -- far beyond the
 * 24 KB kernel stack, so declaring one as a local in a pre-callback
 * overflowed the stack and bugchecked on the very first intercepted delete
 * (RB-01). Notifications are now VARIABLE LENGTH: a fixed 48-byte header plus
 * only as many path bytes as the request actually needs, allocated from paged
 * pool. 1024 WCHAR still covers every realistic share layout while keeping
 * the worst case near 5 KB. */
#define RBF_MAX_PATH        1024
#define RBF_MAX_NAME        256

/* Maximum number of protected path entries cached in driver */
#define RBF_MAX_PROTECTED   16

/* Bounded async notification queue (fire-and-forget, never blocks delete path) */
#define RBF_QUEUE_MAX       512

/* User-mode -> driver commands (RBF_REPLY.Ack) */
#define RBF_CMD_QUERY_STATS 1

/* Sanity tag written into every notification so a malformed or stale buffer
   can be rejected instead of parsed. */
#define RBF_NOTIFY_MAGIC    0x52424654UL   /* 'RBFT' */

/* Upper bound on one serialized notification (header + payload), in bytes. */
#define RBF_NOTIFY_MAX_SIZE ((ULONG)(sizeof(RBF_NOTIFICATION) + \
    ((RBF_MAX_PATH + RBF_MAX_PATH + RBF_MAX_NAME) * sizeof(WCHAR))))

/* Driver -> user-mode notification.
 *
 * VARIABLE LENGTH -- never declare one of these on the stack. Layout:
 *
 *   [ header ][ FilePath UTF16+NUL ][ StorePath UTF16+NUL ][ Sid UTF16+NUL ]
 *
 * Offsets are byte offsets from the start of the header. The *Length fields
 * count payload bytes EXCLUDING the terminating NUL, which is the same
 * convention the old fixed-layout struct used, so user-mode parsing
 * semantics are unchanged.
 *
 * Allocate with RbfAllocNotify() and fill with RbfNotifySetPath() /
 * RbfNotifySetStorePath() / RbfNotifySetSid().
 */
#pragma pack(push, 1)
typedef struct _RBF_NOTIFICATION {
    ULONG   Magic;              /* RBF_NOTIFY_MAGIC */
    ULONG   TotalSize;          /* header + all payload, in bytes */
    ULONG   PathOffset;         /* FilePath, byte offset from struct start */
    ULONG   PathLength;         /* bytes, excluding NUL */
    ULONG   StorePathOffset;    /* staging path, byte offset */
    ULONG   StorePathLength;    /* bytes, excluding NUL */
    ULONG   SidOffset;          /* requestor SID string, byte offset */
    ULONG   SidLength;          /* bytes, excluding NUL */
    ULONG64 FileSize;           /* bytes */
    ULONG   SessionId;          /* requestor session ID */
    ULONG   IsDirectory;        /* 1 = directory */
} RBF_NOTIFICATION, *PRBF_NOTIFICATION;

/* Payload accessors. The offsets come from the header, so a truncated buffer
   is always addressed inside its own TotalSize. */
#define RBF_NOTIFY_PATH(n)      ((PWCH)((PUCHAR)(n) + (n)->PathOffset))
#define RBF_NOTIFY_STOREPATH(n) ((PWCH)((PUCHAR)(n) + (n)->StorePathOffset))
#define RBF_NOTIFY_SID(n)       ((PWCH)((PUCHAR)(n) + (n)->SidOffset))

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
    ULONG64 DeleteDenied;     /* fail-closed: delete refused, data preserved */
    ULONG   QueueDepth;       /* current queue depth */
    ULONG   MaxQueueDepth;    /* high-water mark */
} RBF_STATS, *PRBF_STATS;
#pragma pack(pop)

/* Async notification queue node (fire-and-forget).
 *
 * Holds a POINTER to a pool-allocated, variable-length notification -- never
 * an inline copy -- so the node stays small regardless of path length and
 * the queue's memory footprint tracks actual usage instead of the maximum
 * possible path (RB-07). */
typedef struct _RBF_NOTIFY_NODE {
    LIST_ENTRY        Entry;
    PFLT_PORT         Port;         /* client port snapshot at enqueue time */
    ULONG             Size;         /* bytes of *Notification */
    PRBF_NOTIFICATION Notification; /* paged pool, owned by this node */
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
    ULONG           QueueDepth;    /* entries sitting in NotifyQueue */
    ULONG           MaxQueueDepth;
    /* Slots handed out to callbacks that have decided to stage a file but
       have not enqueued their notification yet (RB-08).
       Effective occupancy is QueueDepth + Reserved; a slot must be reserved
       BEFORE the rename so that a full queue can never turn a staged file
       into an orphan. */
    ULONG           Reserved;

    /* Async send thread (single consumer, serialized by design) */
    KEVENT          NotifyEvent;
    HANDLE          SendThreadHandle;

    /* Statistics (visible to user-mode via RBF_CMD_QUERY_STATS) */
    RBF_STATS       Stats;

    /* Fail-closed policy (RB-04).
     *   1 = refuse a delete we cannot stage, so data is never destroyed
     *       silently (recommended, and the default)
     *   0 = legacy fail-open: let the real delete through and lose the file
     * Registry: Parameters\FailClosed (REG_DWORD). */
    ULONG           FailClosed;

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

/* Notification allocation / fill (variable length, paged pool) */
NTSTATUS RbfAllocNotify(_In_     ULONG               PathBytes,
                        _In_     ULONG               StoreBytes,
                        _In_     ULONG               SidBytes,
                        _Outptr_ PRBF_NOTIFICATION  *OutNote);
VOID     RbfNotifySetPath(_Inout_  PRBF_NOTIFICATION Note,
                          _In_opt_ PCUNICODE_STRING  Src);
VOID     RbfNotifySetStorePath(_Inout_  PRBF_NOTIFICATION Note,
                               _In_opt_ PCUNICODE_STRING  Src);
VOID     RbfNotifySetSid(_Inout_  PRBF_NOTIFICATION Note,
                         _In_opt_ PCUNICODE_STRING  Src);
VOID     RbfNotifyFree(_In_ PRBF_NOTIFICATION Note);

/* Queue slot reservation (RB-08): reserve BEFORE staging so a full queue is
   detected while the file is still untouched. */
BOOLEAN  RbfReserveQueueSlot(VOID);
VOID     RbfReleaseQueueSlot(VOID);

/* Notification queue -- takes ownership of Note and of the reserved slot */
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
