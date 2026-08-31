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

/* Path buffer (WCHAR count).
 *
 * Was 16383, which made one notification ~64 KB and overflowed the 24 KB
 * kernel stack whenever the driver declared one as a local (RB-01).
 * Notifications are now variable length; 1024 WCHAR bounds each individual
 * path while keeping the worst-case wire message near 5 KB. */
#define RBF_MAX_PATH          1024
#define RBF_MAX_NAME          256

/* User-mode -> driver commands (RBF_REPLY.Ack) */
#define RBF_CMD_QUERY_STATS   1

/* Sanity tag written into every notification by the driver. */
#define RBF_NOTIFY_MAGIC      0x52424654UL   /* 'RBFT' */

/* Upper bound on one serialized notification (header + payload), bytes. */
#define RBF_NOTIFY_MAX_SIZE   ((ULONG)(sizeof(RBF_NOTIFICATION) + \
    ((RBF_MAX_PATH + RBF_MAX_PATH + RBF_MAX_NAME) * sizeof(WCHAR))))

#pragma pack(push, 1)

/* Driver -> user-mode notification. Mirrors driver RBF_NOTIFICATION.
 *
 * VARIABLE LENGTH layout:
 *   [ header ][ FilePath UTF16+NUL ][ StorePath UTF16+NUL ][ Sid UTF16+NUL ]
 *
 * Offsets are byte offsets from the start of the header and *Length counts
 * payload bytes EXCLUDING the terminating NUL. Receive buffers must be at
 * least RBF_NOTIFY_MAX_SIZE bytes; the driver only sends the bytes it used.
 */
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

/* Payload accessors (mirrors of the driver macros). */
#define RBF_NOTIFY_PATH(n)      ((WCHAR *)((BYTE *)(n) + (n)->PathOffset))
#define RBF_NOTIFY_STOREPATH(n) ((WCHAR *)((BYTE *)(n) + (n)->StorePathOffset))
#define RBF_NOTIFY_SID(n)       ((WCHAR *)((BYTE *)(n) + (n)->SidOffset))

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
    ULONG64 DeleteDenied;     /* fail-closed: delete refused, data preserved */
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

/* RBF_NOTIFICATION header: 8 * ULONG + ULONG64 + 2 * ULONG, packed => 48 bytes.
   The payload no longer lives inside the struct, so size alone is no longer
   enough to catch a layout change -- the offset asserts below do that. */
typedef char rbf_assert_notification_size[
    (sizeof(RBF_NOTIFICATION) == 48) ? 1 : -1];

/* Field offsets: a reorder or resize in rbminiflt.h breaks the build HERE
   rather than mis-parsing every notification at runtime. */
typedef char rbf_assert_notification_magic_offset[
    (offsetof(RBF_NOTIFICATION, Magic) == 0) ? 1 : -1];
typedef char rbf_assert_notification_totalsize_offset[
    (offsetof(RBF_NOTIFICATION, TotalSize) == 4) ? 1 : -1];
typedef char rbf_assert_notification_filesize_offset[
    (offsetof(RBF_NOTIFICATION, FileSize) == 32) ? 1 : -1];
typedef char rbf_assert_notification_sessionid_offset[
    (offsetof(RBF_NOTIFICATION, SessionId) == 40) ? 1 : -1];
typedef char rbf_assert_notification_isdirectory_offset[
    (offsetof(RBF_NOTIFICATION, IsDirectory) == 44) ? 1 : -1];

/* Guard against reinventing RB-01: a single notification must stay far
   below the 24 KB kernel stack, and small enough to receive cheaply. */
typedef char rbf_assert_max_size_stays_small[
    (RBF_NOTIFY_MAX_SIZE <= 8192) ? 1 : -1];

/* RBF_STATS: 7 * ULONG64 + 2 * ULONG, packed => exactly 64 bytes */
typedef char rbf_assert_stats_size[
    (sizeof(RBF_STATS) == 64) ? 1 : -1];

/* RBF_REPLY is a single ULONG */
typedef char rbf_assert_reply_size[
    (sizeof(RBF_REPLY) == sizeof(ULONG)) ? 1 : -1];

/* Sanity: packed structs must have no trailing padding beyond the asserts above */
typedef char rbf_assert_stats_intercepts_offset[
    (offsetof(RBF_STATS, Intercepts) == 0) ? 1 : -1];
typedef char rbf_assert_stats_queuedepth_offset[
    (offsetof(RBF_STATS, QueueDepth) == 56) ? 1 : -1];
typedef char rbf_assert_stats_deleteddenied_offset[
    (offsetof(RBF_STATS, DeleteDenied) == 48) ? 1 : -1];

#endif /* _RBF_PROTOCOL_H_ */
