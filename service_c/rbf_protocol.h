/*
 * rbf_protocol.h - Wire structures shared between rbminiflt.sys and user mode
 *
 * *** THIS FILE IS GENERATED FROM driver\rbminiflt.h -- KEEP THEM IN SYNC ***
 *
 * Why a copy instead of #include "..\driver\rbminiflt.h"?
 *   The driver header pulls in <ntifs.h> / <fltKernel.h>, which are kernel-only
 *   and cannot be included from a user-mode TU. So we mirror the *wire* structs
 *   here (the only ones that cross the boundary) and add compile-time asserts
 *   that verify byte layout against the driver's own constants.
 *
 * If someone changes rbminiflt.h and forgets this file, rbsvc.c fails to build
 * (static asserts) rather than silently mis-parsing every notification.
 *
 * Regenerate guidance: the field order/width below must match
 * RBF_NOTIFICATION / RBF_REPLY / RBF_STATS in driver\rbminiflt.h exactly.
 */

#ifndef _RBF_PROTOCOL_H_
#define _RBF_PROTOCOL_H_

#include <windows.h>

/* Mirrored from driver\rbminiflt.h -- basic constants */
#define RBF_PORT_NAME_USHORT  L"\\RecycleBinPort"
#define RBF_ALTITUDE          L"370030"

/* Path buffer (WCHAR count). Kept <= 16383 so MaximumLength fits USHORT. */
#define RBF_MAX_PATH          16383
#define RBF_MAX_NAME          256

/* User-mode -> driver commands (RBF_REPLY.Ack) */
#define RBF_CMD_QUERY_STATS   1

#pragma pack(push, 1)

/* Driver -> user-mode notification. Mirrors driver RBF_NOTIFICATION. */
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

/* User-mode -> driver command. Mirrors driver RBF_REPLY. */
typedef struct _RBF_REPLY {
    ULONG   Ack;
} RBF_REPLY, *PRBF_REPLY;

/* Driver statistics. Mirrors driver RBF_STATS. */
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

/* ------------------------------------------------------------------ */
/* Compile-time layout verification                                    */
/* ------------------------------------------------------------------ */
/* These mirror the arithmetic the driver's own structures produce.
   If a field is added/reordered/resized in rbminiflt.h without updating
   this file, the build breaks HERE rather than corrupting data at runtime. */

/* RBF_NOTIFICATION: two path buffers + 2 ULONG + ULONG64 + 2 ULONG + name buf */
typedef char rbf_assert_notification_size[
    (sizeof(RBF_NOTIFICATION) ==
     (2 * RBF_MAX_PATH * sizeof(WCHAR)) +
     (2 * sizeof(ULONG)) +
     sizeof(ULONG64) +
     (2 * sizeof(ULONG)) +
     (RBF_MAX_NAME * sizeof(WCHAR))) ? 1 : -1];

/* RBF_STATS: 6 * ULONG64 + 2 * ULONG, packed => exactly 56 bytes */
typedef char rbf_assert_stats_size[
    (sizeof(RBF_STATS) == 56) ? 1 : -1];

/* RBF_REPLY is a single ULONG */
typedef char rbf_assert_reply_size[
    (sizeof(RBF_REPLY) == sizeof(ULONG)) ? 1 : -1];

/* Sanity: packed structs must have no trailing padding beyond the asserts above */
typedef char rbf_assert_stats_intercepts_offset[
    (offsetof(RBF_STATS, Intercepts) == 0) ? 1 : -1];
typedef char rbf_assert_stats_queuedepth_offset[
    (offsetof(RBF_STATS, QueueDepth) == 48) ? 1 : -1];

#endif /* _RBF_PROTOCOL_H_ */
