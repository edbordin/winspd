/**
 * @file winspd/ioctl.h
 *
 * @copyright 2018-2020 Bill Zissimopoulos
 */
/*
 * This file is part of WinSpd.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 3 as published by the Free Software
 * Foundation.
 *
 * Licensees holding a valid commercial license may use this software
 * in accordance with the commercial license agreement provided in
 * conjunction with the software.  The terms and conditions of any such
 * commercial license agreement shall govern, supersede, and render
 * ineffective any application of the GPLv3 license to this software,
 * notwithstanding of any reference thereto in the software or
 * associated repository.
 */

#ifndef WINSPD_IOCTL_H_INCLUDED
#define WINSPD_IOCTL_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#define SPD_IOCTL_DRIVER_NAME           "WinSpd"
#define SPD_IOCTL_VENDOR_ID             "WinSpd  "
#define SPD_IOCTL_HARDWARE_ID           "root\\winspd"

#define SPD_IOCTL_BTL(B,T,L)            (((B) << 16) | ((T) << 8) | (L))
#define SPD_IOCTL_BTL_B(Btl)            (((Btl) >> 16) & 0xff)
#define SPD_IOCTL_BTL_T(Btl)            (((Btl) >> 8) & 0xff)
#define SPD_IOCTL_BTL_L(Btl)            ((Btl) & 0xff)
#define SPD_IOCTL_STORAGE_UNIT_CAPACITY 16
#define SPD_IOCTL_STORAGE_UNIT_MAX_CAPACITY 64

/* alignment macros */
#define SPD_IOCTL_ALIGN_UP(x, s)        (((x) + ((s) - 1L)) & ~((s) - 1L))
#define SPD_IOCTL_DEFAULT_ALIGNMENT     8
#define SPD_IOCTL_DEFAULT_ALIGN_UP(x)   SPD_IOCTL_ALIGN_UP(x, SPD_IOCTL_DEFAULT_ALIGNMENT)
#define SPD_IOCTL_DECLSPEC_ALIGN        __declspec(align(SPD_IOCTL_DEFAULT_ALIGNMENT))

/* IOCTL_MINIPORT_PROCESS_SERVICE_IRP codes */
#define SPD_IOCTL_PROVISION             ('p')
#define SPD_IOCTL_UNPROVISION           ('u')
#define SPD_IOCTL_LIST                  ('l')
#define SPD_IOCTL_TRANSACT              ('t')
#define SPD_IOCTL_SET_TRANSACT_PID      ('i')
#define SPD_IOCTL_RING_OPEN             ('r')
#define SPD_IOCTL_RING_CLOSE            ('c')
#define SPD_IOCTL_RING_STOP             ('s')
#define SPD_IOCTL_RING_WAIT             ('w')
#define SPD_IOCTL_RING_KICK             ('k')

/* IOCTL_MINIPORT_PROCESS_SERVICE_IRP marshalling */
#pragma warning(push)
#pragma warning(disable:4200)           /* zero-sized array in struct/union */
enum
{
    SpdIoctlTransactReservedKind = 0,
    SpdIoctlTransactReadKind,
    SpdIoctlTransactWriteKind,
    SpdIoctlTransactFlushKind,
    SpdIoctlTransactUnmapKind,
    SpdIoctlTransactKindCount,
};
typedef struct
{
    GUID Guid;                          /* identity */
    UINT64 BlockCount;                  /* geometry */
    UINT32 BlockLength;                 /* geometry */
    UCHAR ProductId[16];
    UCHAR ProductRevisionLevel[4];
    UINT8 DeviceType;                   /* must be 0: direct access block device */
    UINT32 WriteProtected:1;
    UINT32 CacheSupported:1;
    UINT32 UnmapSupported:1;
    UINT32 EjectDisabled:1;             /* disables UI eject */
    UINT32 FuaSupported:1;              /* supports force-unit-access writes */
    UINT32 MaxTransferLength;
    UINT32 PhysicalBlockLength;          /* geometry; 0 means BlockLength */
    UINT32 PhysicalBlockOffset;          /* byte offset of virtual LBA 0 */
    UINT64 Reserved[7];
} SPD_IOCTL_STORAGE_UNIT_PARAMS;
#if defined(WINSPD_SYS_INTERNAL)
static_assert(128 == sizeof(SPD_IOCTL_STORAGE_UNIT_PARAMS),
    "128 == sizeof(SPD_IOCTL_STORAGE_UNIT_PARAMS)");
#endif
typedef struct
{
    UINT8 ScsiStatus;
    UINT8 SenseKey;
    UINT8 ASC;
    UINT8 ASCQ;
    UINT64 Information;
    UINT64 ReservedCSI;
    UINT32 ReservedSKS;
    UINT32 ReservedFRU:8;
    UINT32 InformationValid:1;
} SPD_IOCTL_STORAGE_UNIT_STATUS;
typedef struct
{
    UINT64 BlockAddress;
    UINT32 BlockCount;
    UINT32 Reserved;
} SPD_IOCTL_UNMAP_DESCRIPTOR;
#if defined(WINSPD_SYS_INTERNAL)
static_assert(16 == sizeof(SPD_IOCTL_UNMAP_DESCRIPTOR),
    "16 == sizeof(SPD_IOCTL_UNMAP_DESCRIPTOR)");
#endif
typedef struct
{
    UINT64 Hint;
    UINT8 Kind;
    union
    {
        struct
        {
            UINT64 BlockAddress;
            UINT32 BlockCount;
            UINT32 ForceUnitAccess:1;
            UINT32 Reserved:31;
        } Read;
        struct
        {
            UINT64 BlockAddress;
            UINT32 BlockCount;
            UINT32 ForceUnitAccess:1;
            UINT32 Reserved:31;
        } Write;
        struct
        {
            UINT64 BlockAddress;
            UINT32 BlockCount;
        } Flush;
        struct
        {
            UINT32 Count;
        } Unmap;
    } Op;
} SPD_IOCTL_TRANSACT_REQ;
typedef struct
{
    UINT64 Hint;
    UINT8 Kind;
    SPD_IOCTL_STORAGE_UNIT_STATUS Status;
} SPD_IOCTL_TRANSACT_RSP;
typedef struct
{
    SPD_IOCTL_DECLSPEC_ALIGN UINT16 Size;
    UINT16 Code;
} SPD_IOCTL_BASE_PARAMS;
typedef struct
{
    SPD_IOCTL_BASE_PARAMS Base;
    union
    {
        struct
        {
            SPD_IOCTL_STORAGE_UNIT_PARAMS StorageUnitParams;
        } Par;
        struct
        {
            UINT32 Btl;
        } Ret;
    } Dir;
} SPD_IOCTL_PROVISION_PARAMS;
typedef struct
{
    SPD_IOCTL_BASE_PARAMS Base;
    union
    {
        struct
        {
            GUID Guid;                  /* identity */
        } Par;
    } Dir;
} SPD_IOCTL_UNPROVISION_PARAMS;
typedef struct
{
    SPD_IOCTL_BASE_PARAMS Base;
} SPD_IOCTL_LIST_PARAMS;
typedef struct
{
    SPD_IOCTL_BASE_PARAMS Base;
    UINT32 Btl;
    UINT32 ReqValid:1;
    UINT32 RspValid:1;
    UINT64 DataBuffer;
    union
    {
        SPD_IOCTL_TRANSACT_REQ Req;
        SPD_IOCTL_TRANSACT_RSP Rsp;
    } Dir;
} SPD_IOCTL_TRANSACT_PARAMS;
typedef struct
{
    SPD_IOCTL_BASE_PARAMS Base;
    UINT32 Btl;
    UINT32 ProcessId;
} SPD_IOCTL_SET_TRANSACT_PID_PARAMS;

/* SharedRingV1 transport ABI. The operation request/response structures
 * above remain the canonical WinSpd transaction ABI. */
#define SPD_RING_VERSION_1              1
#define SPD_RING_NO_BUFFER              ((UINT32)-1)
#define SPD_RING_MAX_SECTION_BYTES      (256ULL * 1024ULL * 1024ULL)

typedef struct
{
    UINT32 Slot;
    UINT32 Offset;
    UINT32 Length;
    UINT32 Flags;
} SPD_RING_BUFFER_REF;
#if defined(WINSPD_SYS_INTERNAL)
static_assert(16 == sizeof(SPD_RING_BUFFER_REF),
    "16 == sizeof(SPD_RING_BUFFER_REF)");
#endif

typedef struct
{
    SPD_IOCTL_TRANSACT_REQ Request;
    SPD_RING_BUFFER_REF Data;
} SPD_RING_REQUEST;

typedef struct
{
    SPD_IOCTL_TRANSACT_RSP Response;
} SPD_RING_COMPLETION;

#if defined(WINSPD_SYS_INTERNAL)
static_assert(sizeof(SPD_RING_REQUEST) ==
    sizeof(SPD_IOCTL_TRANSACT_REQ) + sizeof(SPD_RING_BUFFER_REF),
    "ring request must remain an envelope around the transaction request");
static_assert(sizeof(SPD_RING_COMPLETION) == sizeof(SPD_IOCTL_TRANSACT_RSP),
    "ring completion must preserve the transaction response ABI");
#endif

typedef struct
{
    UINT32 Version;
    UINT32 HeaderSize;
    UINT32 SubmissionOffset;
    UINT32 CompletionOffset;
    UINT32 BufferOffset;
    UINT32 QueueDepth;
    UINT32 BufferSize;
    UINT32 Flags;
    UINT64 SubmissionProducer;
    UINT64 SubmissionConsumer;
    UINT64 CompletionProducer;
    UINT64 CompletionConsumer;
    UINT64 KernelHeartbeat;
    UINT64 UserHeartbeat;
} SPD_RING_HEADER;

typedef struct
{
    SPD_IOCTL_BASE_PARAMS Base;
    UINT32 Btl;
    UINT16 Version;
    UINT16 Flags;
    UINT32 QueueDepth;
    UINT32 BufferSize;
    UINT64 UserAddress;
    UINT64 SectionSize;
    UINT32 Features;
    UINT32 Reserved;
} SPD_IOCTL_RING_OPEN_PARAMS;

typedef struct
{
    SPD_IOCTL_BASE_PARAMS Base;
    UINT32 Btl;
    UINT32 Reserved;
} SPD_IOCTL_RING_CLOSE_PARAMS;

typedef struct
{
    SPD_IOCTL_BASE_PARAMS Base;
    UINT32 Btl;
    UINT32 Produced;
    UINT32 Reserved[2];
} SPD_IOCTL_RING_WAIT_PARAMS;

typedef struct
{
    SPD_IOCTL_BASE_PARAMS Base;
    UINT32 Btl;
    UINT32 Consumed;
    UINT32 Reserved[2];
} SPD_IOCTL_RING_KICK_PARAMS;
#pragma warning(pop)

/* These helpers deliberately use interlocked operations rather than volatile
 * as the synchronization mechanism for counters shared by kernel and user. */
static inline UINT64 SpdRingLoadCounter(UINT64 *Counter)
{
    return (UINT64)InterlockedCompareExchange64(
        (volatile LONG64 *)Counter, 0, 0);
}

static inline VOID SpdRingStoreCounter(UINT64 *Counter, UINT64 Value)
{
    InterlockedExchange64((volatile LONG64 *)Counter, (LONG64)Value);
}

#if !defined(WINSPD_SYS_INTERNAL)
DWORD SpdIoctlGetDevicePath(GUID *ClassGuid, PWSTR DeviceName,
    PWCHAR PathBuf, UINT32 PathBufSize);
DWORD SpdIoctlOpenDevice(PWSTR DeviceName, PHANDLE PDeviceHandle);
DWORD SpdIoctlScsiExecute(HANDLE DeviceHandle,
    UINT32 Btl, PCDB Cdb, INT DataDirection, PVOID DataBuffer, PUINT32 PDataLength,
    PUCHAR PScsiStatus, UCHAR SenseInfoBuffer[32]);
DWORD SpdIoctlScsiInquiry(HANDLE DeviceHandle,
    UINT32 Btl, PINQUIRYDATA InquiryData, ULONG Timeout);
DWORD SpdIoctlProvision(HANDLE DeviceHandle,
    const SPD_IOCTL_STORAGE_UNIT_PARAMS *Params, PUINT32 PBtl);
DWORD SpdIoctlUnprovision(HANDLE DeviceHandle,
    const GUID *Guid);
DWORD SpdIoctlGetList(HANDLE DeviceHandle,
    PUINT32 ListBuf, PUINT32 PListSize);
DWORD SpdIoctlTransact(HANDLE DeviceHandle,
    UINT32 Btl,
    SPD_IOCTL_TRANSACT_RSP *Rsp,
    SPD_IOCTL_TRANSACT_REQ *Req,
    PVOID DataBuffer);
DWORD SpdIoctlSetTransactProcessId(HANDLE DeviceHandle,
    UINT32 Btl,
    ULONG ProcessId);
DWORD SpdIoctlRingOpen(HANDLE DeviceHandle,
    UINT32 Btl, SPD_IOCTL_RING_OPEN_PARAMS *Params);
DWORD SpdIoctlRingClose(HANDLE DeviceHandle, UINT32 Btl);
DWORD SpdIoctlRingStop(HANDLE DeviceHandle, UINT32 Btl);
DWORD SpdIoctlRingWait(HANDLE DeviceHandle,
    UINT32 Btl, SPD_IOCTL_RING_WAIT_PARAMS *Params);
DWORD SpdIoctlRingKick(HANDLE DeviceHandle,
    UINT32 Btl, SPD_IOCTL_RING_KICK_PARAMS *Params);
#endif

#ifdef __cplusplus
}
#endif

#endif
