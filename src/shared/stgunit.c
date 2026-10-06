/**
 * @file dll/stgunit.c
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

#include <assert.h>
#include <shared/shared.h>
#pragma warning(push)
#pragma warning(disable:4091)
#include <ntddscsi.h>
#pragma warning(pop)

DWORD SpdStorageUnitHandleOpen(PWSTR Name,
    const SPD_IOCTL_STORAGE_UNIT_PARAMS *StorageUnitParams,
    PHANDLE PHandle, PUINT32 PBtl);
DWORD SpdStorageUnitHandleTransact(HANDLE Handle,
    UINT32 Btl,
    SPD_IOCTL_TRANSACT_RSP *Rsp,
    SPD_IOCTL_TRANSACT_REQ *Req,
    PVOID DataBuffer);
DWORD SpdStorageUnitHandleShutdown(HANDLE Handle,
    const GUID *Guid);
DWORD SpdStorageUnitHandleClose(HANDLE Handle);
DWORD SpdStorageUnitHandleRingOpen(HANDLE Handle,
    UINT32 Btl, SPD_IOCTL_RING_OPEN_PARAMS *Params);
DWORD SpdStorageUnitHandleRingClose(HANDLE Handle, UINT32 Btl);
DWORD SpdStorageUnitHandleRingStop(HANDLE Handle, UINT32 Btl);
DWORD SpdStorageUnitHandleRingWait(HANDLE Handle,
    UINT32 Btl, SPD_IOCTL_RING_WAIT_PARAMS *Params);
DWORD SpdStorageUnitHandleRingKick(HANDLE Handle,
    UINT32 Btl, SPD_IOCTL_RING_KICK_PARAMS *Params);

static SPD_STORAGE_UNIT_INTERFACE SpdStorageUnitNullInterface;

static DWORD SpdStorageUnitTlsCount = 0;
static SRWLOCK SpdStorageUnitTlsLock = SRWLOCK_INIT;
static DWORD SpdStorageUnitTlsKey = TLS_OUT_OF_INDEXES;

typedef struct _SPD_RING_RUNTIME SPD_RING_RUNTIME;
typedef struct _SPD_RING_USER_ITEM SPD_RING_USER_ITEM;
typedef struct
{
    LIST_ENTRY *Buckets;
    UINT32 BucketCount;
} SPD_RING_ACTIVE_TABLE;
typedef enum
{
    SpdRingUserItemFree,
    SpdRingUserItemWorkQueued,
    SpdRingUserItemProcessing,
    SpdRingUserItemDeferred,
    SpdRingUserItemDoneQueued
} SPD_RING_USER_ITEM_STATE;
struct _SPD_RING_USER_ITEM
{
    LIST_ENTRY QueueLink;
    LIST_ENTRY ActiveLink;
    SPD_RING_BUFFER_REF Data;
    PVOID DataBuffer;
    SPD_RING_USER_ITEM_STATE State;
    union
    {
        SPD_IOCTL_TRANSACT_REQ Request;
        SPD_IOCTL_TRANSACT_RSP Response;
    } Message;
    SPD_IOCTL_TRANSACT_RSP EarlyResponse;
    BOOLEAN EarlyResponseValid;
    BOOLEAN SendResponseActive;
    BOOLEAN CallbackFinished;
    BOOLEAN CallbackComplete;
};
struct _SPD_RING_RUNTIME
{
    SPD_STORAGE_UNIT *StorageUnit;
    PVOID RingBase;
    SPD_RING_HEADER *Header;
    UINT32 QueueDepth;
    UINT32 BufferSize;
    UINT32 RequestOffset;
    UINT32 CompletionOffset;
    UINT32 BufferOffset;
    UINT32 RequestHead;
    UINT32 CompletionTail;
    SPD_RING_USER_ITEM *Items;
    LIST_ENTRY FreeItems;
    UINT32 FreeItemCount;
    SRWLOCK WorkLock;
    CONDITION_VARIABLE WorkAvailable;
    LIST_ENTRY WorkQueue;
    SRWLOCK CompletionLock;
    LIST_ENTRY DoneQueue;
    SPD_RING_ACTIVE_TABLE Active;
    BOOLEAN DoneNotificationPending;
    volatile LONG ResponseCopies;
    HANDLE Iocp;
    OVERLAPPED WaitOverlapped;
    OVERLAPPED KickOverlapped;
    OVERLAPPED StopOverlapped;
    SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
    SPD_IOCTL_RING_KICK_PARAMS KickParams;
    BOOLEAN WaitOutstanding;
    BOOLEAN KickOutstanding;
    HANDLE *WorkerThreads;
    ULONG WorkerCount;
    volatile LONG References;
    volatile LONG Stopping;
    DWORD Error;
    UINT64 SubmissionBatches;
    UINT64 SubmittedRequests;
    UINT32 MaxSubmissionBatch;
    UINT64 CompletionBatches;
    UINT64 QueuedResponses;
    UINT64 CompletedRequests;
    UINT32 MaxCompletionBatch;
};

static SPD_RING_RUNTIME *SpdStorageUnitRingRuntimeAcquire(
    SPD_STORAGE_UNIT *StorageUnit);
static VOID SpdStorageUnitRingRuntimeRelease(SPD_RING_RUNTIME *Runtime);
static VOID SpdRingRuntimeStop(SPD_RING_RUNTIME *Runtime);
static VOID SpdRingRuntimeRequestShutdown(SPD_RING_RUNTIME *Runtime);
static BOOLEAN SpdRingQueueDepthValid(UINT32 QueueDepth);

#define SPD_RING_IOCP_DONE ((ULONG_PTR)1)
#define SPD_RING_IOCP_STOP ((ULONG_PTR)2)

static VOID SpdRingListInitialize(PLIST_ENTRY Head)
{
    Head->Flink = Head;
    Head->Blink = Head;
}

static BOOLEAN SpdRingListEmpty(const LIST_ENTRY *Head)
{
    return Head->Flink == Head;
}

static VOID SpdRingListInsertTail(PLIST_ENTRY Head, PLIST_ENTRY Entry)
{
    Entry->Blink = Head->Blink;
    Entry->Flink = Head;
    Head->Blink->Flink = Entry;
    Head->Blink = Entry;
}

static VOID SpdRingListRemove(PLIST_ENTRY Entry)
{
    Entry->Blink->Flink = Entry->Flink;
    Entry->Flink->Blink = Entry->Blink;
}

static PLIST_ENTRY SpdRingListRemoveHead(PLIST_ENTRY Head)
{
    PLIST_ENTRY Entry = Head->Flink;
    SpdRingListRemove(Entry);
    return Entry;
}

static BOOLEAN SpdRingQueueDepthValid(UINT32 QueueDepth)
{
    return SPD_RING_MIN_QUEUE_DEPTH <= QueueDepth &&
        SPD_RING_MAX_QUEUE_DEPTH >= QueueDepth &&
        0 == (QueueDepth & (QueueDepth - 1));
}

static VOID WINAPI SpdStorageUnitTlsInit(VOID)
{
    AcquireSRWLockExclusive(&SpdStorageUnitTlsLock);
    if (1 == ++SpdStorageUnitTlsCount)
        SpdStorageUnitTlsKey = TlsAlloc();
    ReleaseSRWLockExclusive(&SpdStorageUnitTlsLock);
}
static VOID SpdStorageUnitTlsFini(VOID)
{
    AcquireSRWLockExclusive(&SpdStorageUnitTlsLock);
    if (0 == --SpdStorageUnitTlsCount &&
        TLS_OUT_OF_INDEXES != SpdStorageUnitTlsKey)
    {
        TlsFree(SpdStorageUnitTlsKey);
        SpdStorageUnitTlsKey = TLS_OUT_OF_INDEXES;
    }
    ReleaseSRWLockExclusive(&SpdStorageUnitTlsLock);
}

DWORD SpdStorageUnitCreate(
    PWSTR DeviceName,
    const SPD_STORAGE_UNIT_PARAMS *StorageUnitParams,
    const SPD_STORAGE_UNIT_INTERFACE *Interface,
    SPD_STORAGE_UNIT **PStorageUnit)
{
    DWORD Error;
    SPD_STORAGE_UNIT *StorageUnit = 0;
    HANDLE Handle;
    UINT32 Btl;

    *PStorageUnit = 0;

    if (0 == Interface)
        Interface = &SpdStorageUnitNullInterface;

    SpdStorageUnitTlsInit();
    if (TLS_OUT_OF_INDEXES == SpdStorageUnitTlsKey)
    {
        Error = ERROR_NO_SYSTEM_RESOURCES;
        goto exit;
    }

    StorageUnit = MemAlloc(sizeof *StorageUnit);
    if (0 == StorageUnit)
    {
        Error = ERROR_NO_SYSTEM_RESOURCES;
        goto exit;
    }
    memset(StorageUnit, 0, sizeof *StorageUnit);

    if (0 == DeviceName)
        DeviceName = L"" SPD_IOCTL_HARDWARE_ID;

    Error = SpdStorageUnitHandleOpen(DeviceName, StorageUnitParams, &Handle, &Btl);
    if (ERROR_SUCCESS != Error)
        goto exit;

    memcpy(&StorageUnit->StorageUnitParams, StorageUnitParams, sizeof *StorageUnitParams);
    StorageUnit->Interface = Interface;
    StorageUnit->Handle = Handle;
    StorageUnit->Btl = Btl;
    InitializeSRWLock(&StorageUnit->SharedRingRuntimeLock);
    SpdStorageUnitSetBufferAllocator(StorageUnit, MemAlloc, MemFree);

    *PStorageUnit = StorageUnit;

    Error = ERROR_SUCCESS;

exit:
    if (ERROR_SUCCESS != Error)
    {
        MemFree(StorageUnit);
        SpdStorageUnitTlsFini();
    }

    return Error;
}

DWORD SpdStorageUnitOpenSharedRing(SPD_STORAGE_UNIT *StorageUnit,
    SPD_IOCTL_RING_OPEN_PARAMS *Params)
{
    DWORD Error;

    if (0 != StorageUnit->SharedRingAddress)
        return ERROR_ALREADY_EXISTS;
    if (0 == Params)
        return ERROR_INVALID_PARAMETER;

    Error = SpdStorageUnitHandleRingOpen(StorageUnit->Handle,
        StorageUnit->Btl, Params);
    if (ERROR_SUCCESS != Error)
        return Error;

    SPD_RING_HEADER *Header;
    SIZE_T SectionSize;
    SIZE_T RequestOffset;
    SIZE_T CompletionOffset;
    SIZE_T BufferOffset;
    SIZE_T ExpectedSize;
    SIZE_T BufferBytes;

    if (0 == Params->UserAddress ||
        Params->UserAddress > (UINT64)(UINT_PTR)-1 ||
        Params->SectionSize > (UINT64)(SIZE_T)-1)
    {
        SpdStorageUnitHandleRingClose(StorageUnit->Handle,
            StorageUnit->Btl);
        return ERROR_INVALID_DATA;
    }
    Header = (SPD_RING_HEADER *)(UINT_PTR)Params->UserAddress;
    SectionSize = (SIZE_T)Params->SectionSize;

    if (0 == Header || 0 == SectionSize ||
        SPD_RING_MAX_SECTION_BYTES < SectionSize ||
        SPD_RING_VERSION_3 != Header->Version ||
        sizeof *Header != Header->HeaderSize ||
        !SpdRingQueueDepthValid(Header->QueueDepth) ||
        Header->QueueDepth != Params->QueueDepth ||
        Header->BufferCount != Header->QueueDepth ||
        0 == Header->BufferSize ||
        Header->BufferSize <
            StorageUnit->StorageUnitParams.MaxTransferLength ||
        Header->BufferSize != Params->BufferSize ||
        (SIZE_T)Header->QueueDepth >
            (SIZE_T)-1 / Header->BufferSize ||
        0 != Header->Flags)
    {
        SpdStorageUnitHandleRingClose(StorageUnit->Handle,
            StorageUnit->Btl);
        return ERROR_INVALID_DATA;
    }

    BufferBytes = (SIZE_T)Header->QueueDepth * Header->BufferSize;
    if (BufferBytes > SPD_RING_MAX_SECTION_BYTES)
    {
        SpdStorageUnitHandleRingClose(StorageUnit->Handle,
            StorageUnit->Btl);
        return ERROR_INVALID_DATA;
    }
    RequestOffset = SPD_IOCTL_ALIGN_UP(sizeof *Header,
        SPD_RING_CACHE_LINE_SIZE);
    CompletionOffset = SPD_IOCTL_ALIGN_UP(RequestOffset +
        (SIZE_T)Header->QueueDepth * sizeof(SPD_RING_REQUEST),
        SPD_RING_CACHE_LINE_SIZE);
    BufferOffset = SPD_IOCTL_ALIGN_UP(CompletionOffset +
        (SIZE_T)Header->QueueDepth * sizeof(SPD_RING_COMPLETION), 4096);
    ExpectedSize = SPD_IOCTL_ALIGN_UP(BufferOffset + BufferBytes, 4096);
    if (Header->RequestOffset != RequestOffset ||
        Header->CompletionOffset != CompletionOffset ||
        Header->BufferOffset != BufferOffset ||
        ExpectedSize != SectionSize)
    {
        SpdStorageUnitHandleRingClose(StorageUnit->Handle,
            StorageUnit->Btl);
        return ERROR_INVALID_DATA;
    }
    for (UINT32 I = 0; ARRAYSIZE(Header->Reserved) > I; I++)
        if (0 != Header->Reserved[I])
        {
            SpdStorageUnitHandleRingClose(StorageUnit->Handle,
                StorageUnit->Btl);
            return ERROR_INVALID_DATA;
        }
    if (0 != SpdRingLoadAcquire32(&Header->RequestHead.Value) ||
        0 != SpdRingLoadAcquire32(&Header->RequestTail.Value) ||
        0 != SpdRingLoadAcquire32(&Header->CompletionHead.Value) ||
        0 != SpdRingLoadAcquire32(&Header->CompletionTail.Value))
    {
        SpdStorageUnitHandleRingClose(StorageUnit->Handle,
            StorageUnit->Btl);
        return ERROR_INVALID_DATA;
    }
    for (UINT32 I = 0; sizeof Header->RequestHead.Reserved > I; I++)
        if (0 != Header->RequestHead.Reserved[I] ||
            0 != Header->RequestTail.Reserved[I] ||
            0 != Header->CompletionHead.Reserved[I] ||
            0 != Header->CompletionTail.Reserved[I])
        {
            SpdStorageUnitHandleRingClose(StorageUnit->Handle,
                StorageUnit->Btl);
            return ERROR_INVALID_DATA;
        }

    StorageUnit->SharedRingAddress = Header;
    StorageUnit->SharedRingSize = SectionSize;
    StorageUnit->SharedRingHeader =
        Header;
    StorageUnit->SharedRingQueueDepth = Header->QueueDepth;
    StorageUnit->SharedRingBufferSize = Header->BufferSize;
    StorageUnit->SharedRingRequestOffset = Header->RequestOffset;
    StorageUnit->SharedRingCompletionOffset = Header->CompletionOffset;
    StorageUnit->SharedRingBufferOffset = Header->BufferOffset;
    return ERROR_SUCCESS;
}

VOID SpdStorageUnitCloseSharedRing(SPD_STORAGE_UNIT *StorageUnit)
{
    if (0 == StorageUnit->SharedRingAddress)
        return;

    if (0 != StorageUnit->DispatcherThread &&
        GetCurrentThreadId() != StorageUnit->DispatcherThreadId)
    {
        SPD_RING_RUNTIME *Runtime =
            SpdStorageUnitRingRuntimeAcquire(StorageUnit);
        if (0 != Runtime)
        {
            SpdRingRuntimeRequestShutdown(Runtime);
            SpdStorageUnitRingRuntimeRelease(Runtime);
        }
        SpdStorageUnitWaitDispatcher(StorageUnit);
    }

    SpdStorageUnitHandleRingClose(StorageUnit->Handle, StorageUnit->Btl);
    StorageUnit->SharedRingAddress = 0;
    StorageUnit->SharedRingSize = 0;
    StorageUnit->SharedRingHeader = 0;
    StorageUnit->SharedRingQueueDepth = 0;
    StorageUnit->SharedRingBufferSize = 0;
    StorageUnit->SharedRingRequestOffset = 0;
    StorageUnit->SharedRingCompletionOffset = 0;
    StorageUnit->SharedRingBufferOffset = 0;
}

VOID SpdStorageUnitDelete(SPD_STORAGE_UNIT *StorageUnit)
{
    if (0 != StorageUnit->DispatcherThread)
    {
        SpdStorageUnitShutdown(StorageUnit);
        SpdStorageUnitWaitDispatcher(StorageUnit);
    }
    SpdStorageUnitCloseSharedRing(StorageUnit);
    SpdStorageUnitHandleShutdown(StorageUnit->Handle, &StorageUnit->StorageUnitParams.Guid);
    SpdStorageUnitHandleClose(StorageUnit->Handle);
    MemFree(StorageUnit);
    SpdStorageUnitTlsFini();
}

VOID SpdStorageUnitShutdown(SPD_STORAGE_UNIT *StorageUnit)
{
    /* SharedRing shutdown must stop the userspace dispatcher before the
     * driver is allowed to unmap the section. The legacy transport can use
     * the original unprovision call directly because it has no user mapping
     * that the dispatcher dereferences after a cancelled transaction. */
    if (0 != StorageUnit->SharedRingAddress)
    {
        SPD_RING_RUNTIME *Runtime =
            SpdStorageUnitRingRuntimeAcquire(StorageUnit);
        if (0 != Runtime)
        {
            SpdRingRuntimeRequestShutdown(Runtime);
            SpdStorageUnitRingRuntimeRelease(Runtime);
            return;
        }
    }
    SpdStorageUnitHandleShutdown(StorageUnit->Handle, &StorageUnit->StorageUnitParams.Guid);
}

BOOLEAN SpdStorageUnitProcessRequest(SPD_STORAGE_UNIT *StorageUnit,
    SPD_IOCTL_TRANSACT_REQ *Request, PVOID DataBuffer,
    SPD_IOCTL_TRANSACT_RSP *Response)
{
    BOOLEAN Complete;

    memset(Response, 0, sizeof *Response);
    Response->Hint = Request->Hint;
    Response->Kind = Request->Kind;

    switch (Request->Kind)
    {
    case SpdIoctlTransactReadKind:
        if (0 == StorageUnit->Interface->Read)
            goto invalid;
        Complete = StorageUnit->Interface->Read(
            StorageUnit,
            DataBuffer,
            Request->Op.Read.BlockAddress,
            Request->Op.Read.BlockCount,
            Request->Op.Read.ForceUnitAccess,
            &Response->Status);
        break;
    case SpdIoctlTransactWriteKind:
        if (0 == StorageUnit->Interface->Write)
            goto invalid;
        Complete = StorageUnit->Interface->Write(
            StorageUnit,
            DataBuffer,
            Request->Op.Write.BlockAddress,
            Request->Op.Write.BlockCount,
            Request->Op.Write.ForceUnitAccess,
            &Response->Status);
        break;
    case SpdIoctlTransactFlushKind:
        if (0 == StorageUnit->Interface->Flush)
            goto invalid;
        Complete = StorageUnit->Interface->Flush(
            StorageUnit,
            Request->Op.Flush.BlockAddress,
            Request->Op.Flush.BlockCount,
            &Response->Status);
        break;
    case SpdIoctlTransactUnmapKind:
        if (0 == StorageUnit->Interface->Unmap)
            goto invalid;
        Complete = StorageUnit->Interface->Unmap(
            StorageUnit,
            DataBuffer,
            Request->Op.Unmap.Count,
            &Response->Status);
        break;
    default:
    invalid:
        SpdStorageUnitStatusSetSense(&Response->Status,
            SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_ILLEGAL_COMMAND, 0);
        Complete = TRUE;
        break;
    }

    if (Complete && StorageUnit->DebugLog)
    {
        if (SpdIoctlTransactKindCount <= Response->Kind ||
            (StorageUnit->DebugLog & (1 << Response->Kind)))
            SpdDebugLogResponse(Response);
    }

    return Complete;
}

static DWORD WINAPI SpdStorageUnitDispatcherThread(PVOID StorageUnit0)
{
    SPD_STORAGE_UNIT *StorageUnit = StorageUnit0;
    SPD_IOCTL_TRANSACT_REQ RequestBuf, *Request = &RequestBuf;
    SPD_IOCTL_TRANSACT_RSP ResponseBuf, *Response;
    SPD_STORAGE_UNIT_OPERATION_CONTEXT OperationContext;
    PVOID DataBuffer = 0;
    HANDLE DispatcherThread = 0;
    BOOLEAN Complete;
    DWORD Error;

    DataBuffer = StorageUnit->BufferAlloc(StorageUnit->StorageUnitParams.MaxTransferLength);
    if (0 == DataBuffer)
    {
        Error = ERROR_NO_SYSTEM_RESOURCES;
        goto exit;
    }

    OperationContext.Request = &RequestBuf;
    OperationContext.Response = &ResponseBuf;
    OperationContext.DataBuffer = DataBuffer;
    TlsSetValue(SpdStorageUnitTlsKey, &OperationContext);

    if (1 < StorageUnit->DispatcherThreadCount)
    {
        StorageUnit->DispatcherThreadCount--;
        DispatcherThread = CreateThread(0, 0, SpdStorageUnitDispatcherThread, StorageUnit, 0, 0);
        if (0 == DispatcherThread)
        {
            Error = GetLastError();
            goto exit;
        }
    }

    Response = 0;
    for (;;)
    {
        memset(Request, 0, sizeof *Request);
        Error = SpdStorageUnitHandleTransact(StorageUnit->Handle,
            StorageUnit->Btl, Response, Request, DataBuffer);
        if (ERROR_SUCCESS != Error)
            goto exit;

        if (0 == Request->Hint)
        {
            Response = 0;
            continue;
        }

        if (StorageUnit->DebugLog)
        {
            if (SpdIoctlTransactKindCount <= Request->Kind ||
                (StorageUnit->DebugLog & (1 << Request->Kind)))
                SpdDebugLogRequest(Request);
        }

        Response = &ResponseBuf;
        Complete = SpdStorageUnitProcessRequest(StorageUnit,
            Request, DataBuffer, Response);

        if (!Complete)
            Response = 0;
    }

exit:
    SpdStorageUnitSetDispatcherError(StorageUnit, Error);

    SpdStorageUnitHandleShutdown(StorageUnit->Handle, &StorageUnit->StorageUnitParams.Guid);

    if (0 != DispatcherThread)
    {
        WaitForSingleObject(DispatcherThread, INFINITE);
        CloseHandle(DispatcherThread);
    }

    if (GetCurrentThreadId() == StorageUnit->DispatcherThreadId)
    {
        if (StorageUnit->StorageUnitParams.CacheSupported && 0 != StorageUnit->Interface->Flush)
        {
            Response = &ResponseBuf;
            memset(Request, 0, sizeof *Request);
            memset(Response, 0, sizeof *Response);
            StorageUnit->Interface->Flush(
                StorageUnit,
                0,
                0,
                &Response->Status);
        }
    }

    TlsSetValue(SpdStorageUnitTlsKey, 0);

    StorageUnit->BufferFree(DataBuffer);

    return Error;
}

static UINT32 SpdRingHashHint(UINT64 Value)
{
    Value ^= Value >> 33;
    Value *= 0xff51afd7ed558ccdULL;
    Value ^= Value >> 33;
    Value *= 0xc4ceb9fe1a85ec53ULL;
    Value ^= Value >> 33;
    return (UINT32)Value;
}

/* CompletionLock is held by all active-table helpers. */
static SPD_RING_USER_ITEM *SpdRingActiveLookupLocked(
    SPD_RING_RUNTIME *Runtime,
    UINT64 Hint)
{
    UINT32 Index = SpdRingHashHint(Hint) &
        (Runtime->Active.BucketCount - 1);
    LIST_ENTRY *Head = &Runtime->Active.Buckets[Index];

    for (LIST_ENTRY *Entry = Head->Flink;
        Entry != Head; Entry = Entry->Flink)
    {
        SPD_RING_USER_ITEM *Item = CONTAINING_RECORD(
            Entry, SPD_RING_USER_ITEM, ActiveLink);
        if (Item->Message.Request.Hint == Hint)
            return Item;
    }

    return 0;
}

static BOOLEAN SpdRingActiveInsertLocked(
    SPD_RING_RUNTIME *Runtime,
    SPD_RING_USER_ITEM *Item)
{
    UINT32 Index = SpdRingHashHint(
        Item->Message.Request.Hint) &
        (Runtime->Active.BucketCount - 1);
    LIST_ENTRY *Head = &Runtime->Active.Buckets[Index];

    if (0 != SpdRingActiveLookupLocked(
            Runtime, Item->Message.Request.Hint))
        return FALSE;
    SpdRingListInsertTail(Head, &Item->ActiveLink);
    return TRUE;
}

static VOID SpdRingActiveRemoveLocked(
    SPD_RING_USER_ITEM *Item)
{
    SpdRingListRemove(&Item->ActiveLink);
    SpdRingListInitialize(&Item->ActiveLink);
}

static PVOID SpdRingBufferAddress(
    SPD_RING_RUNTIME *Runtime,
    UINT32 BufferId,
    UINT32 Offset,
    UINT32 Length)
{
    SIZE_T BufferOffset;

    if (BufferId >= Runtime->QueueDepth ||
        Offset > Runtime->BufferSize ||
        Length > Runtime->BufferSize - Offset)
        return 0;

    BufferOffset = (SIZE_T)Runtime->BufferOffset +
        (SIZE_T)BufferId * Runtime->BufferSize + Offset;
    if (BufferOffset > Runtime->StorageUnit->SharedRingSize ||
        Length > Runtime->StorageUnit->SharedRingSize - BufferOffset)
        return 0;
    return (PUINT8)Runtime->RingBase + BufferOffset;
}

static BOOLEAN SpdRingValidateRequestData(
    SPD_RING_RUNTIME *Runtime,
    const SPD_IOCTL_TRANSACT_REQ *Request,
    const SPD_RING_BUFFER_REF *Data,
    PVOID *PDataBuffer)
{
    UINT64 Length = 0;
    BOOLEAN NeedsBuffer;

    switch (Request->Kind)
    {
    case SpdIoctlTransactReadKind:
        Length = (UINT64)Request->Op.Read.BlockCount *
            Runtime->StorageUnit->StorageUnitParams.BlockLength;
        NeedsBuffer = TRUE;
        break;
    case SpdIoctlTransactWriteKind:
        Length = (UINT64)Request->Op.Write.BlockCount *
            Runtime->StorageUnit->StorageUnitParams.BlockLength;
        NeedsBuffer = TRUE;
        break;
    case SpdIoctlTransactUnmapKind:
        Length = (UINT64)Request->Op.Unmap.Count *
            sizeof(SPD_IOCTL_UNMAP_DESCRIPTOR);
        NeedsBuffer = TRUE;
        break;
    case SpdIoctlTransactFlushKind:
        NeedsBuffer = FALSE;
        break;
    default:
        return FALSE;
    }

    if (Length > 0xffffffffULL ||
        Length > Runtime->BufferSize ||
        Data->Offset != 0 ||
        Data->Flags != SPD_RING_BUFFER_FLAG_NONE ||
        Data->Length != (UINT32)Length)
        return FALSE;

    if (!NeedsBuffer)
    {
        *PDataBuffer = 0;
        return SPD_RING_NO_BUFFER == Data->BufferId &&
            0 == Data->Offset && 0 == Data->Length &&
            SPD_RING_BUFFER_FLAG_NONE == Data->Flags;
    }

    if (SPD_RING_NO_BUFFER == Data->BufferId)
        return FALSE;
    *PDataBuffer = SpdRingBufferAddress(Runtime, Data->BufferId,
        Data->Offset, Data->Length);
    return 0 != *PDataBuffer;
}

static VOID SpdRingWakeWorkers(SPD_RING_RUNTIME *Runtime)
{
    AcquireSRWLockExclusive(&Runtime->WorkLock);
    WakeAllConditionVariable(&Runtime->WorkAvailable);
    ReleaseSRWLockExclusive(&Runtime->WorkLock);
}

static VOID SpdRingRuntimeRequestShutdown(SPD_RING_RUNTIME *Runtime)
{
    if (0 == InterlockedExchange(&Runtime->Stopping, TRUE))
    {
        SpdRingWakeWorkers(Runtime);
        if (0 != Runtime->Iocp)
            PostQueuedCompletionStatus(Runtime->Iocp, 0,
                SPD_RING_IOCP_STOP, &Runtime->StopOverlapped);
    }
}

static VOID SpdRingRuntimeSetError(
    SPD_RING_RUNTIME *Runtime,
    DWORD Error)
{
    if (ERROR_SUCCESS != Error)
        InterlockedCompareExchange((volatile LONG *)&Runtime->Error,
            (LONG)Error, ERROR_SUCCESS);
    SpdRingRuntimeRequestShutdown(Runtime);
}

static VOID SpdRingRuntimeStop(SPD_RING_RUNTIME *Runtime)
{
    SpdRingRuntimeRequestShutdown(Runtime);
}

static BOOLEAN SpdRingQueueDoneLocked(
    SPD_RING_RUNTIME *Runtime,
    SPD_RING_USER_ITEM *Item)
{
    SpdRingListInsertTail(&Runtime->DoneQueue, &Item->QueueLink);
    Runtime->QueuedResponses++;
    if (!Runtime->DoneNotificationPending)
    {
        Runtime->DoneNotificationPending = TRUE;
        if (!PostQueuedCompletionStatus(Runtime->Iocp, 0,
                SPD_RING_IOCP_DONE, 0))
        {
            Runtime->DoneNotificationPending = FALSE;
            return FALSE;
        }
    }
    return TRUE;
}

static VOID SpdRingQueueWork(
    SPD_RING_RUNTIME *Runtime,
    SPD_RING_USER_ITEM *Item)
{
    AcquireSRWLockExclusive(&Runtime->WorkLock);
    SpdRingListInsertTail(&Runtime->WorkQueue, &Item->QueueLink);
    WakeConditionVariable(&Runtime->WorkAvailable);
    ReleaseSRWLockExclusive(&Runtime->WorkLock);
}

static DWORD WINAPI SpdStorageUnitRingWorkerThread(PVOID Runtime0)
{
    SPD_RING_RUNTIME *Runtime = Runtime0;
    SPD_STORAGE_UNIT *StorageUnit = Runtime->StorageUnit;

    for (;;)
    {
        SPD_RING_USER_ITEM *Item;
        SPD_IOCTL_TRANSACT_RSP Response;
        SPD_STORAGE_UNIT_OPERATION_CONTEXT OperationContext;
        UINT64 ResponseHint;
        UINT64 QueuedResponses = 0;
        BOOLEAN Complete;
        BOOLEAN ResponseQueued = FALSE;
        BOOLEAN ProtocolError = FALSE;
        BOOLEAN NotifyError = FALSE;

        AcquireSRWLockExclusive(&Runtime->WorkLock);
        while (SpdRingListEmpty(&Runtime->WorkQueue) &&
            !InterlockedCompareExchange(&Runtime->Stopping, 0, 0))
            SleepConditionVariableSRW(&Runtime->WorkAvailable,
                &Runtime->WorkLock, INFINITE, 0);
        if (InterlockedCompareExchange(&Runtime->Stopping, 0, 0))
        {
            ReleaseSRWLockExclusive(&Runtime->WorkLock);
            break;
        }
        Item = CONTAINING_RECORD(
            SpdRingListRemoveHead(&Runtime->WorkQueue),
            SPD_RING_USER_ITEM, QueueLink);
        ReleaseSRWLockExclusive(&Runtime->WorkLock);

        AcquireSRWLockExclusive(&Runtime->CompletionLock);
        if (SpdRingUserItemWorkQueued != Item->State)
        {
            ReleaseSRWLockExclusive(&Runtime->CompletionLock);
            SpdRingRuntimeSetError(Runtime, ERROR_INVALID_DATA);
            break;
        }
        Item->State = SpdRingUserItemProcessing;
        Item->EarlyResponseValid = FALSE;
        Item->SendResponseActive = FALSE;
        Item->CallbackFinished = FALSE;
        Item->CallbackComplete = FALSE;
        ResponseHint = Item->Message.Request.Hint;
        ReleaseSRWLockExclusive(&Runtime->CompletionLock);

        if (StorageUnit->DebugLog &&
            (SpdIoctlTransactKindCount <= Item->Message.Request.Kind ||
             (StorageUnit->DebugLog &
                (1 << Item->Message.Request.Kind))))
            SpdDebugLogRequest(&Item->Message.Request);

        OperationContext.Request = &Item->Message.Request;
        OperationContext.Response = &Response;
        OperationContext.DataBuffer = Item->DataBuffer;
        TlsSetValue(SpdStorageUnitTlsKey, &OperationContext);
        Complete = SpdStorageUnitProcessRequest(StorageUnit,
            &Item->Message.Request, Item->DataBuffer, &Response);
        TlsSetValue(SpdStorageUnitTlsKey, 0);

        AcquireSRWLockExclusive(&Runtime->CompletionLock);
        if (Item->SendResponseActive)
        {
            Item->CallbackFinished = TRUE;
            Item->CallbackComplete = Complete;
        }
        else if (Complete)
        {
            if (Item->EarlyResponseValid)
                ProtocolError = TRUE;
            else
            {
                Item->Message.Response = Response;
                SpdRingActiveRemoveLocked(Item);
                Item->State = SpdRingUserItemDoneQueued;
                NotifyError = !SpdRingQueueDoneLocked(Runtime, Item);
                ResponseQueued = TRUE;
                QueuedResponses = Runtime->QueuedResponses;
            }
        }
        else if (Item->EarlyResponseValid)
        {
            Item->Message.Response = Item->EarlyResponse;
            Item->EarlyResponseValid = FALSE;
            SpdRingActiveRemoveLocked(Item);
            Item->State = SpdRingUserItemDoneQueued;
            NotifyError = !SpdRingQueueDoneLocked(Runtime, Item);
            ResponseQueued = TRUE;
            QueuedResponses = Runtime->QueuedResponses;
        }
        else
            Item->State = SpdRingUserItemDeferred;
        ReleaseSRWLockExclusive(&Runtime->CompletionLock);

        if (ResponseQueued)
            SpdDebugLog("SharedRing response queued tick=%I64u hint=%I64u "
                "count=%I64u\n", GetTickCount64(), ResponseHint,
                QueuedResponses);

        if (ProtocolError || NotifyError)
        {
            SpdRingRuntimeSetError(Runtime,
                ProtocolError ? ERROR_INVALID_DATA : ERROR_GEN_FAILURE);
            break;
        }
    }

    return ERROR_SUCCESS;
}

static DWORD SpdRingSubmitOverlapped(
    SPD_RING_RUNTIME *Runtime,
    UINT32 Code,
    PVOID Params,
    DWORD ParamsSize,
    OVERLAPPED *Overlapped)
{
    DWORD BytesTransferred = 0;

    ((SPD_IOCTL_BASE_PARAMS *)Params)->Size = (UINT16)ParamsSize;
    ((SPD_IOCTL_BASE_PARAMS *)Params)->Code = Code;

    if (!DeviceIoControl(Runtime->StorageUnit->Handle,
            IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
            Params, ParamsSize, Params, ParamsSize,
            &BytesTransferred, Overlapped))
    {
        DWORD Error = GetLastError();
        if (ERROR_IO_PENDING != Error)
            return Error;
    }

    return ERROR_SUCCESS;
}

static VOID SpdRingEnsureKickOutstanding(
    SPD_RING_RUNTIME *Runtime)
{
    SPD_RING_HEADER *Header = Runtime->Header;
    UINT32 Head;

    if (InterlockedCompareExchange(&Runtime->Stopping, 0, 0) ||
        Runtime->KickOutstanding)
        return;

    Head = SpdRingLoadAcquire32(&Header->CompletionHead.Value);
    if (Head == Runtime->CompletionTail)
        return;

    memset(&Runtime->KickParams, 0, sizeof Runtime->KickParams);
    Runtime->KickParams.Btl = Runtime->StorageUnit->Btl;
    memset(&Runtime->KickOverlapped, 0,
        sizeof Runtime->KickOverlapped);
    Runtime->KickOutstanding = TRUE;
    DWORD Error = SpdRingSubmitOverlapped(Runtime,
        SPD_IOCTL_RING_KICK, &Runtime->KickParams,
        sizeof Runtime->KickParams, &Runtime->KickOverlapped);
    if (ERROR_SUCCESS != Error)
    {
        Runtime->KickOutstanding = FALSE;
        SpdRingRuntimeSetError(Runtime, Error);
    }
}

static VOID SpdRingMaybeIssueWait(
    SPD_RING_RUNTIME *Runtime)
{
    if (InterlockedCompareExchange(&Runtime->Stopping, 0, 0) ||
        Runtime->WaitOutstanding ||
        Runtime->KickOutstanding ||
        0 == Runtime->FreeItemCount)
        return;

    memset(&Runtime->WaitParams, 0, sizeof Runtime->WaitParams);
    Runtime->WaitParams.Btl = Runtime->StorageUnit->Btl;
    Runtime->WaitParams.MaxRequests =
        min(Runtime->FreeItemCount, Runtime->QueueDepth);
    if (0 == Runtime->WaitParams.MaxRequests)
        return;
    memset(&Runtime->WaitOverlapped, 0,
        sizeof Runtime->WaitOverlapped);
    Runtime->WaitOutstanding = TRUE;
    DWORD Error = SpdRingSubmitOverlapped(Runtime,
        SPD_IOCTL_RING_WAIT, &Runtime->WaitParams,
        sizeof Runtime->WaitParams, &Runtime->WaitOverlapped);
    if (ERROR_SUCCESS != Error)
    {
        Runtime->WaitOutstanding = FALSE;
        SpdRingRuntimeSetError(Runtime, Error);
    }
}

static VOID SpdRingConsumeRequests(
    SPD_RING_RUNTIME *Runtime)
{
    SPD_RING_HEADER *Header = Runtime->Header;
    SPD_RING_REQUEST *RequestRing = (SPD_RING_REQUEST *)
        ((PUINT8)Runtime->RingBase + Runtime->RequestOffset);
    UINT32 Head = Runtime->RequestHead;
    UINT32 Tail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    UINT32 Count = Tail - Head;

    if (Count > Runtime->QueueDepth ||
        Count != Runtime->WaitParams.Produced ||
        Count > Runtime->WaitParams.MaxRequests)
    {
        SpdRingRuntimeSetError(Runtime, ERROR_INVALID_DATA);
        return;
    }

    while (Head != Tail)
    {
        SPD_RING_REQUEST Entry =
            RequestRing[Head & (Runtime->QueueDepth - 1)];
        SPD_RING_USER_ITEM *Item;
        PVOID DataBuffer = 0;

        if (Runtime->FreeItemCount == 0 ||
            !SpdRingValidateRequestData(Runtime, &Entry.Request,
                &Entry.Data, &DataBuffer))
        {
            SpdRingRuntimeSetError(Runtime, ERROR_INVALID_DATA);
            return;
        }

        Item = CONTAINING_RECORD(
            SpdRingListRemoveHead(&Runtime->FreeItems),
            SPD_RING_USER_ITEM, QueueLink);
        Runtime->FreeItemCount--;
        Item->Message.Request = Entry.Request;
        Item->Data = Entry.Data;
        Item->DataBuffer = DataBuffer;
        Item->EarlyResponseValid = FALSE;
        Item->SendResponseActive = FALSE;
        Item->CallbackFinished = FALSE;
        Item->CallbackComplete = FALSE;

        AcquireSRWLockExclusive(&Runtime->CompletionLock);
        Item->State = SpdRingUserItemWorkQueued;
        BOOLEAN Inserted = SpdRingActiveInsertLocked(Runtime, Item);
        ReleaseSRWLockExclusive(&Runtime->CompletionLock);
        if (!Inserted)
        {
            SpdRingRuntimeSetError(Runtime, ERROR_INVALID_DATA);
            return;
        }

        SpdRingQueueWork(Runtime, Item);
        Head++;
    }

    Runtime->RequestHead = Head;
    SpdRingStoreRelease32(&Header->RequestHead.Value, Head);
    Runtime->SubmissionBatches++;
    Runtime->SubmittedRequests += Count;
    if (Count > Runtime->MaxSubmissionBatch)
        Runtime->MaxSubmissionBatch = Count;
}

static VOID SpdRingPublishCompletions(
    SPD_RING_RUNTIME *Runtime)
{
    SPD_RING_HEADER *Header = Runtime->Header;
    SPD_RING_COMPLETION *CompletionRing = (SPD_RING_COMPLETION *)
        ((PUINT8)Runtime->RingBase + Runtime->CompletionOffset);
    LIST_ENTRY LocalDone;
    LIST_ENTRY LocalRecycle;
    UINT32 Head;
    UINT32 Used;
    UINT32 Available;
    UINT32 Published = 0;
    UINT32 Tail = Runtime->CompletionTail;

    if (InterlockedCompareExchange(&Runtime->Stopping, 0, 0))
        return;

    Head = SpdRingLoadAcquire32(&Header->CompletionHead.Value);
    Used = Tail - Head;
    if (Used > Runtime->QueueDepth)
    {
        SpdRingRuntimeSetError(Runtime, ERROR_INVALID_DATA);
        return;
    }
    Available = Runtime->QueueDepth - Used;
    if (0 == Available)
    {
        SpdRingEnsureKickOutstanding(Runtime);
        return;
    }

    SpdRingListInitialize(&LocalDone);
    SpdRingListInitialize(&LocalRecycle);
    AcquireSRWLockExclusive(&Runtime->CompletionLock);
    while (Available != 0 &&
        !SpdRingListEmpty(&Runtime->DoneQueue))
    {
        SpdRingListInsertTail(&LocalDone,
            SpdRingListRemoveHead(&Runtime->DoneQueue));
        Available--;
    }
    if (SpdRingListEmpty(&Runtime->DoneQueue))
        Runtime->DoneNotificationPending = FALSE;
    ReleaseSRWLockExclusive(&Runtime->CompletionLock);

    while (!SpdRingListEmpty(&LocalDone))
    {
        SPD_RING_USER_ITEM *Item = CONTAINING_RECORD(
            SpdRingListRemoveHead(&LocalDone),
            SPD_RING_USER_ITEM, QueueLink);
        SPD_RING_COMPLETION *Entry =
            &CompletionRing[Tail & (Runtime->QueueDepth - 1)];

        Entry->Response = Item->Message.Response;
        Entry->Data = Item->Data;
        Item->DataBuffer = 0;
        SpdRingListInsertTail(&LocalRecycle, &Item->QueueLink);
        Tail++;
        Published++;
    }

    if (0 == Published)
        return;

    Runtime->CompletionTail = Tail;
    SpdRingStoreRelease32(&Header->CompletionTail.Value, Tail);
    SpdDebugLog("SharedRing completions published tick=%I64u count=%lu "
        "tail=%lu\n", GetTickCount64(), (unsigned long)Published,
        (unsigned long)Tail);
    Runtime->CompletionBatches++;
    Runtime->CompletedRequests += Published;
    if (Published > Runtime->MaxCompletionBatch)
        Runtime->MaxCompletionBatch = Published;

    while (!SpdRingListEmpty(&LocalRecycle))
    {
        SPD_RING_USER_ITEM *Item = CONTAINING_RECORD(
            SpdRingListRemoveHead(&LocalRecycle),
            SPD_RING_USER_ITEM, QueueLink);
        AcquireSRWLockExclusive(&Runtime->CompletionLock);
        Item->State = SpdRingUserItemFree;
        Item->EarlyResponseValid = FALSE;
        Item->SendResponseActive = FALSE;
        Item->CallbackFinished = FALSE;
        Item->CallbackComplete = FALSE;
        ReleaseSRWLockExclusive(&Runtime->CompletionLock);
        SpdRingListInsertTail(&Runtime->FreeItems, &Item->QueueLink);
        Runtime->FreeItemCount++;
    }

    SpdRingEnsureKickOutstanding(Runtime);
}

static VOID SpdRingHandleWaitCompletion(
    SPD_RING_RUNTIME *Runtime,
    DWORD Error)
{
    Runtime->WaitOutstanding = FALSE;
    if (InterlockedCompareExchange(&Runtime->Stopping, 0, 0) &&
        (ERROR_OPERATION_ABORTED == Error ||
         ERROR_CANCELLED == Error || ERROR_SUCCESS == Error))
        return;
    if (ERROR_SUCCESS != Error)
    {
        SpdRingRuntimeSetError(Runtime, Error);
        return;
    }

    if (Runtime->WaitParams.Flags &
        ~SPD_RING_WAIT_FLAG_BUFFER_STARVED)
    {
        SpdRingRuntimeSetError(Runtime, ERROR_INVALID_DATA);
        return;
    }

    SpdRingConsumeRequests(Runtime);
    if (!InterlockedCompareExchange(&Runtime->Stopping, 0, 0) &&
        0 == (Runtime->WaitParams.Flags &
            SPD_RING_WAIT_FLAG_BUFFER_STARVED))
        SpdRingMaybeIssueWait(Runtime);
}

static VOID SpdRingHandleKickCompletion(
    SPD_RING_RUNTIME *Runtime,
    DWORD Error)
{
    Runtime->KickOutstanding = FALSE;
    SpdDebugLog("SharedRing KICK completed tick=%I64u error=%lu "
        "consumed=%lu head=%lu tail=%lu\n", GetTickCount64(),
        (unsigned long)Error,
        (unsigned long)Runtime->KickParams.Consumed,
        (unsigned long)SpdRingLoadAcquire32(
            &Runtime->Header->CompletionHead.Value),
        (unsigned long)Runtime->CompletionTail);
    if (InterlockedCompareExchange(&Runtime->Stopping, 0, 0) &&
        ERROR_OPERATION_ABORTED == Error)
        return;
    if (ERROR_SUCCESS != Error)
    {
        SpdRingRuntimeSetError(Runtime, Error);
        return;
    }

    SpdRingPublishCompletions(Runtime);

    /*
     * A completion may have been published while the completed KICK was
     * still outstanding. PublishCompletions can have no DoneQueue entries
     * in that case and return without scheduling another KICK. Drain any
     * such already-published CQ entries before admitting more requests.
     */
    SpdRingEnsureKickOutstanding(Runtime);
    SpdRingMaybeIssueWait(Runtime);
}

static VOID SpdRingBeginPumpShutdown(
    SPD_RING_RUNTIME *Runtime,
    BOOLEAN *PStopIssued)
{
    SpdRingRuntimeRequestShutdown(Runtime);
    if (*PStopIssued)
        return;
    *PStopIssued = TRUE;

    DWORD Error = SpdStorageUnitHandleRingStop(
        Runtime->StorageUnit->Handle, Runtime->StorageUnit->Btl);
    if (ERROR_SUCCESS != Error)
    {
        InterlockedCompareExchange((volatile LONG *)&Runtime->Error,
            (LONG)Error, ERROR_SUCCESS);
        SpdDebugLog("SharedRing stop failed error=%lu\n",
            (unsigned long)Error);
    }
}

static VOID SpdRingWaitForResponseCopies(
    SPD_RING_RUNTIME *Runtime)
{
    for (;;)
    {
        LONG Observed;
        AcquireSRWLockShared(&Runtime->CompletionLock);
        Observed = Runtime->ResponseCopies;
        ReleaseSRWLockShared(&Runtime->CompletionLock);
        if (0 == Observed)
            return;
        WaitOnAddress(&Runtime->ResponseCopies, &Observed,
            sizeof Observed, INFINITE);
    }
}

static DWORD WINAPI SpdStorageUnitRingPumpThread(PVOID Runtime0)
{
    SPD_RING_RUNTIME *Runtime = Runtime0;
    SPD_STORAGE_UNIT *StorageUnit = Runtime->StorageUnit;
    BOOLEAN StopIssued = FALSE;
    DWORD Error = ERROR_SUCCESS;
    DWORD BytesTransferred;
    ULONG_PTR CompletionKey;
    OVERLAPPED *Overlapped;

    if (0 == CreateIoCompletionPort(StorageUnit->Handle,
            Runtime->Iocp, 0, 1))
    {
        Error = GetLastError();
        SpdRingRuntimeSetError(Runtime, Error);
    }
    else
        SpdRingMaybeIssueWait(Runtime);

    while (Runtime->WaitOutstanding || Runtime->KickOutstanding ||
        !InterlockedCompareExchange(&Runtime->Stopping, 0, 0))
    {
        BOOL Success = GetQueuedCompletionStatus(Runtime->Iocp,
            &BytesTransferred, &CompletionKey, &Overlapped, INFINITE);
        DWORD CompletionError = Success ? ERROR_SUCCESS : GetLastError();

        if (SPD_RING_IOCP_DONE == CompletionKey)
        {
            if (!InterlockedCompareExchange(&Runtime->Stopping, 0, 0))
                SpdRingPublishCompletions(Runtime);
            continue;
        }

        if (SPD_RING_IOCP_STOP == CompletionKey ||
            &Runtime->StopOverlapped == Overlapped)
        {
            SpdRingBeginPumpShutdown(Runtime, &StopIssued);
            continue;
        }

        if (&Runtime->WaitOverlapped == Overlapped)
        {
            SpdRingHandleWaitCompletion(Runtime, CompletionError);
            continue;
        }

        if (&Runtime->KickOverlapped == Overlapped)
        {
            SpdRingHandleKickCompletion(Runtime, CompletionError);
            continue;
        }

        if (ERROR_SUCCESS == CompletionError)
            CompletionError = ERROR_INVALID_DATA;
        SpdRingRuntimeSetError(Runtime, CompletionError);
        SpdRingBeginPumpShutdown(Runtime, &StopIssued);
    }

    SpdRingBeginPumpShutdown(Runtime, &StopIssued);
    SpdRingWakeWorkers(Runtime);
    for (ULONG I = 0; Runtime->WorkerCount > I; I++)
        WaitForSingleObject(Runtime->WorkerThreads[I], INFINITE);

    SpdRingWaitForResponseCopies(Runtime);
    Runtime->Error = InterlockedCompareExchange(
        (volatile LONG *)&Runtime->Error, 0, 0);
    SpdStorageUnitSetDispatcherError(StorageUnit, Runtime->Error);
    SpdDebugLog("SharedRing batches submissions=%I64u requests=%I64u "
        "max=%lu completions=%I64u responses_queued=%I64u "
        "responses_published=%I64u max=%lu "
        "workers=%lu depth=%lu buffer_size=%lu error=%lu\n",
        Runtime->SubmissionBatches, Runtime->SubmittedRequests,
        (unsigned long)Runtime->MaxSubmissionBatch,
        Runtime->CompletionBatches, Runtime->QueuedResponses,
        Runtime->CompletedRequests,
        (unsigned long)Runtime->MaxCompletionBatch,
        (unsigned long)Runtime->WorkerCount,
        (unsigned long)Runtime->QueueDepth,
        (unsigned long)Runtime->BufferSize,
        (unsigned long)Runtime->Error);

    /* Worker and response paths have stopped touching shared buffers. */
    SpdStorageUnitHandleShutdown(StorageUnit->Handle,
        &StorageUnit->StorageUnitParams.Guid);
    return Runtime->Error;
}

static DWORD SpdStorageUnitRingRuntimeCreate(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG WorkerCount)
{
    SPD_RING_RUNTIME *Runtime;
    SPD_RING_HEADER *Header = StorageUnit->SharedRingHeader;
    UINT32 BucketCount;
    UINT32 QueueDepth = StorageUnit->SharedRingQueueDepth;
    UINT32 BufferSize = StorageUnit->SharedRingBufferSize;
    UINT32 RequestOffsetExpected = StorageUnit->SharedRingRequestOffset;
    UINT32 CompletionOffsetExpected =
        StorageUnit->SharedRingCompletionOffset;
    UINT32 BufferOffsetExpected = StorageUnit->SharedRingBufferOffset;
    ULONG WorkerCapacity;
    SIZE_T RequestOffset;
    SIZE_T CompletionOffset;
    SIZE_T BufferOffset;
    SIZE_T BufferBytes;
    SIZE_T ExpectedSize;
    DWORD Error = ERROR_SUCCESS;

    if (0 != StorageUnit->SharedRingRuntime ||
        0 == Header || SPD_RING_VERSION_3 != Header->Version ||
        Header->HeaderSize != sizeof *Header ||
        QueueDepth < SPD_RING_MIN_QUEUE_DEPTH ||
        QueueDepth > SPD_RING_MAX_QUEUE_DEPTH ||
        0 != (QueueDepth & (QueueDepth - 1)) ||
        Header->QueueDepth != QueueDepth ||
        Header->BufferCount != QueueDepth ||
        Header->BufferSize != BufferSize ||
        BufferSize < StorageUnit->StorageUnitParams.MaxTransferLength ||
        0 == BufferSize ||
        QueueDepth > (SIZE_T)-1 / BufferSize ||
        0 != Header->Flags)
        return ERROR_INVALID_PARAMETER;

    BufferBytes = (SIZE_T)QueueDepth * BufferSize;
    RequestOffset = RequestOffsetExpected;
    CompletionOffset = SPD_IOCTL_ALIGN_UP(RequestOffset +
        (SIZE_T)QueueDepth * sizeof(SPD_RING_REQUEST),
        SPD_RING_CACHE_LINE_SIZE);
    BufferOffset = SPD_IOCTL_ALIGN_UP(CompletionOffset +
        (SIZE_T)QueueDepth * sizeof(SPD_RING_COMPLETION), 4096);
    ExpectedSize = SPD_IOCTL_ALIGN_UP(BufferOffset + BufferBytes, 4096);
    if (SPD_RING_MAX_SECTION_BYTES < ExpectedSize ||
        ExpectedSize != StorageUnit->SharedRingSize ||
        RequestOffset != Header->RequestOffset ||
        RequestOffset != RequestOffsetExpected ||
        CompletionOffset != Header->CompletionOffset ||
        CompletionOffset != CompletionOffsetExpected ||
        BufferOffset != Header->BufferOffset ||
        BufferOffset != BufferOffsetExpected)
        return ERROR_INVALID_PARAMETER;
    for (UINT32 I = 0; ARRAYSIZE(Header->Reserved) > I; I++)
        if (0 != Header->Reserved[I])
            return ERROR_INVALID_PARAMETER;
    for (UINT32 I = 0; sizeof Header->RequestHead.Reserved > I; I++)
        if (0 != Header->RequestHead.Reserved[I] ||
            0 != Header->RequestTail.Reserved[I] ||
            0 != Header->CompletionHead.Reserved[I] ||
            0 != Header->CompletionTail.Reserved[I])
            return ERROR_INVALID_PARAMETER;
    if (0 != SpdRingLoadAcquire32(&Header->RequestHead.Value) ||
        0 != SpdRingLoadAcquire32(&Header->RequestTail.Value) ||
        0 != SpdRingLoadAcquire32(&Header->CompletionHead.Value) ||
        0 != SpdRingLoadAcquire32(&Header->CompletionTail.Value))
        return ERROR_INVALID_PARAMETER;

    if (0 == WorkerCount)
        WorkerCount = 1;
    if (WorkerCount > QueueDepth)
        WorkerCount = QueueDepth;
    WorkerCapacity = WorkerCount;

    for (BucketCount = 1; BucketCount <
        QueueDepth * 2; BucketCount <<= 1)
        ;
    Runtime = MemAlloc(sizeof *Runtime);
    if (0 == Runtime)
        return ERROR_NOT_ENOUGH_MEMORY;
    memset(Runtime, 0, sizeof *Runtime);
    Runtime->StorageUnit = StorageUnit;
    Runtime->RingBase = StorageUnit->SharedRingAddress;
    Runtime->Header = Header;
    Runtime->QueueDepth = QueueDepth;
    Runtime->BufferSize = BufferSize;
    Runtime->RequestOffset = RequestOffsetExpected;
    Runtime->CompletionOffset = CompletionOffsetExpected;
    Runtime->BufferOffset = BufferOffsetExpected;
    Runtime->References = 1;
    InitializeSRWLock(&Runtime->WorkLock);
    InitializeConditionVariable(&Runtime->WorkAvailable);
    InitializeSRWLock(&Runtime->CompletionLock);
    SpdRingListInitialize(&Runtime->FreeItems);
    SpdRingListInitialize(&Runtime->WorkQueue);
    SpdRingListInitialize(&Runtime->DoneQueue);
    Runtime->Active.BucketCount = BucketCount;

    Runtime->Items = MemAlloc(
        sizeof *Runtime->Items * Runtime->QueueDepth);
    Runtime->WorkerThreads = MemAlloc(
        sizeof *Runtime->WorkerThreads * WorkerCapacity);
    Runtime->Active.Buckets = MemAlloc(
        sizeof *Runtime->Active.Buckets * BucketCount);
    Runtime->Iocp = CreateIoCompletionPort(
        INVALID_HANDLE_VALUE, 0, 0, WorkerCapacity + 1);
    if (0 == Runtime->Items || 0 == Runtime->WorkerThreads ||
        0 == Runtime->Active.Buckets || 0 == Runtime->Iocp)
    {
        Error = 0 == Runtime->Iocp ? GetLastError() :
            ERROR_NOT_ENOUGH_MEMORY;
        goto exit;
    }

    memset(Runtime->Items, 0,
        sizeof *Runtime->Items * Runtime->QueueDepth);
    memset(Runtime->WorkerThreads, 0,
        sizeof *Runtime->WorkerThreads * WorkerCapacity);
    for (UINT32 I = 0; BucketCount > I; I++)
        SpdRingListInitialize(&Runtime->Active.Buckets[I]);
    for (UINT32 I = 0; Runtime->QueueDepth > I; I++)
    {
        SpdRingListInitialize(&Runtime->Items[I].QueueLink);
        SpdRingListInitialize(&Runtime->Items[I].ActiveLink);
        Runtime->Items[I].State = SpdRingUserItemFree;
        SpdRingListInsertTail(&Runtime->FreeItems,
            &Runtime->Items[I].QueueLink);
    }
    Runtime->FreeItemCount = Runtime->QueueDepth;

    for (ULONG I = 0; WorkerCapacity > I; I++)
    {
        Runtime->WorkerThreads[I] = CreateThread(0, 0,
            SpdStorageUnitRingWorkerThread, Runtime, 0, 0);
        if (0 == Runtime->WorkerThreads[I])
        {
            Error = GetLastError();
            goto exit;
        }
        Runtime->WorkerCount++;
    }

    AcquireSRWLockExclusive(&StorageUnit->SharedRingRuntimeLock);
    StorageUnit->SharedRingRuntime = Runtime;
    ReleaseSRWLockExclusive(&StorageUnit->SharedRingRuntimeLock);
    return ERROR_SUCCESS;

exit:
    SpdRingRuntimeStop(Runtime);
    for (ULONG I = 0; Runtime->WorkerCount > I; I++)
        WaitForSingleObject(Runtime->WorkerThreads[I], INFINITE);
    for (ULONG I = 0; Runtime->WorkerCount > I; I++)
        CloseHandle(Runtime->WorkerThreads[I]);
    if (0 != Runtime->Iocp)
        CloseHandle(Runtime->Iocp);
    MemFree(Runtime->Active.Buckets);
    MemFree(Runtime->WorkerThreads);
    MemFree(Runtime->Items);
    MemFree(Runtime);
    return Error;
}

static SPD_RING_RUNTIME *SpdStorageUnitRingRuntimeAcquire(
    SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_RUNTIME *Runtime;

    AcquireSRWLockShared(&StorageUnit->SharedRingRuntimeLock);
    Runtime = StorageUnit->SharedRingRuntime;
    if (0 != Runtime)
        InterlockedIncrement(&Runtime->References);
    ReleaseSRWLockShared(&StorageUnit->SharedRingRuntimeLock);
    return Runtime;
}

static VOID SpdStorageUnitRingRuntimeRelease(
    SPD_RING_RUNTIME *Runtime)
{
    if (0 == InterlockedDecrement(&Runtime->References))
        WakeByAddressAll((PVOID)&Runtime->References);
}

static DWORD SpdStorageUnitRingRuntimeDestroy(
    SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_RUNTIME *Runtime;

    AcquireSRWLockExclusive(&StorageUnit->SharedRingRuntimeLock);
    Runtime = StorageUnit->SharedRingRuntime;
    StorageUnit->SharedRingRuntime = 0;
    ReleaseSRWLockExclusive(&StorageUnit->SharedRingRuntimeLock);
    if (0 == Runtime)
        return ERROR_SUCCESS;

    SpdRingRuntimeStop(Runtime);
    SpdRingWakeWorkers(Runtime);
    for (ULONG I = 0; Runtime->WorkerCount > I; I++)
        WaitForSingleObject(Runtime->WorkerThreads[I], INFINITE);
    for (;;)
    {
        LONG Observed = InterlockedCompareExchange(&Runtime->References,
            0, 0);
        if (1 == Observed)
            break;
        WaitOnAddress(&Runtime->References, &Observed,
            sizeof Observed, INFINITE);
    }
    if (0 != Runtime->Iocp)
        CloseHandle(Runtime->Iocp);
    for (ULONG I = 0; Runtime->WorkerCount > I; I++)
        CloseHandle(Runtime->WorkerThreads[I]);
    DWORD Error = Runtime->Error;
    MemFree(Runtime->Active.Buckets);
    MemFree(Runtime->WorkerThreads);
    MemFree(Runtime->Items);
    MemFree(Runtime);
    return Error;
}

static DWORD WINAPI SpdStorageUnitRingDispatcherThread(
    PVOID Runtime0)
{
    SPD_RING_RUNTIME *Runtime = Runtime0;
    SPD_STORAGE_UNIT *StorageUnit = Runtime->StorageUnit;
    DWORD Error = (DWORD)(ULONG_PTR)
        SpdStorageUnitRingPumpThread(Runtime);

    if (ERROR_SUCCESS != Runtime->Error)
        Error = Runtime->Error;
    SpdStorageUnitSetDispatcherError(StorageUnit, Error);
    return Error;
}
DWORD SpdStorageUnitStartDispatcher(SPD_STORAGE_UNIT *StorageUnit, ULONG ThreadCount)
{
    BOOLEAN RingMode = 0 != StorageUnit->SharedRingAddress;
    SPD_RING_RUNTIME *Runtime = 0;

    if (0 != StorageUnit->DispatcherThread)
        return ERROR_INVALID_PARAMETER;

    if (0 == ThreadCount)
    {
        DWORD_PTR ProcessMask, SystemMask;

        if (!GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask))
            return GetLastError();

        for (ThreadCount = 0; 0 != ProcessMask; ProcessMask >>= 1)
            ThreadCount += ProcessMask & 1;
    }

    if (RingMode)
    {
        DWORD Error = SpdStorageUnitRingRuntimeCreate(StorageUnit,
            ThreadCount);
        if (ERROR_SUCCESS != Error)
            return Error;
        Runtime = SpdStorageUnitRingRuntimeAcquire(StorageUnit);
        if (0 == Runtime)
        {
            SpdStorageUnitRingRuntimeDestroy(StorageUnit);
            return ERROR_INVALID_STATE;
        }
    }

    StorageUnit->DispatcherThreadCount = ThreadCount;
    StorageUnit->DispatcherThread = CreateThread(0, 0,
        RingMode ?
            SpdStorageUnitRingDispatcherThread :
            SpdStorageUnitDispatcherThread,
        RingMode ? (PVOID)Runtime : (PVOID)StorageUnit,
        CREATE_SUSPENDED,
        &StorageUnit->DispatcherThreadId);
    if (0 == StorageUnit->DispatcherThread)
    {
        DWORD Error = GetLastError();
        if (0 != Runtime)
            SpdStorageUnitRingRuntimeRelease(Runtime);
        if (RingMode)
            SpdStorageUnitRingRuntimeDestroy(StorageUnit);
        return Error;
    }
    if (0xffffffff == ResumeThread(StorageUnit->DispatcherThread))
    {
        DWORD Error = GetLastError();
        if (0 != Runtime)
            SpdRingRuntimeStop(Runtime);
        TerminateThread(StorageUnit->DispatcherThread,
            ERROR_OPERATION_ABORTED);
        WaitForSingleObject(StorageUnit->DispatcherThread, INFINITE);
        CloseHandle(StorageUnit->DispatcherThread);
        StorageUnit->DispatcherThread = 0;
        if (0 != Runtime)
            SpdStorageUnitRingRuntimeRelease(Runtime);
        if (RingMode)
            SpdStorageUnitRingRuntimeDestroy(StorageUnit);
        return Error;
    }
    if (0 != Runtime)
        SpdStorageUnitRingRuntimeRelease(Runtime);

    return ERROR_SUCCESS;
}

VOID SpdStorageUnitWaitDispatcher(SPD_STORAGE_UNIT *StorageUnit)
{
    if (0 == StorageUnit->DispatcherThread)
        return;

    WaitForSingleObject(StorageUnit->DispatcherThread, INFINITE);
    CloseHandle(StorageUnit->DispatcherThread);
    StorageUnit->DispatcherThread = 0;
    SpdStorageUnitRingRuntimeDestroy(StorageUnit);
}

VOID SpdStorageUnitSendResponse(SPD_STORAGE_UNIT *StorageUnit,
    SPD_IOCTL_TRANSACT_RSP *Response, PVOID DataBuffer)
{
    DWORD Error;

    if (StorageUnit->DebugLog)
    {
        if (SpdIoctlTransactKindCount <= Response->Kind ||
            (StorageUnit->DebugLog & (1 << Response->Kind)))
            SpdDebugLogResponse(Response);
    }

    if (0 != StorageUnit->SharedRingAddress)
    {
        SPD_RING_RUNTIME *Runtime =
            SpdStorageUnitRingRuntimeAcquire(StorageUnit);
        SPD_RING_USER_ITEM *Item = 0;
        SPD_IOCTL_TRANSACT_RSP ResponseCopy = *Response;
        PVOID Destination = 0;
        UINT32 DataLength = 0;
        BOOLEAN ProtocolError = FALSE;
        BOOLEAN NotificationError = FALSE;
        LONG Copies;

        /* The dispatcher may already be tearing down the ring. */
        if (0 == Runtime)
            return;

        AcquireSRWLockExclusive(&Runtime->CompletionLock);
        if (InterlockedCompareExchange(&Runtime->Stopping, 0, 0))
        {
            ReleaseSRWLockExclusive(&Runtime->CompletionLock);
            SpdStorageUnitRingRuntimeRelease(Runtime);
            return;
        }

        Item = SpdRingActiveLookupLocked(Runtime, Response->Hint);
        if (0 == Item ||
            Item->Message.Request.Kind != Response->Kind ||
            (SpdRingUserItemProcessing != Item->State &&
             SpdRingUserItemDeferred != Item->State) ||
            Item->SendResponseActive)
        {
            ProtocolError = TRUE;
        }
        else
        {
            Item->SendResponseActive = TRUE;
            InterlockedIncrement(&Runtime->ResponseCopies);
            Destination = Item->DataBuffer;
            DataLength = Item->Data.Length;
        }
        ReleaseSRWLockExclusive(&Runtime->CompletionLock);

        if (ProtocolError)
        {
            SpdDebugLog("SharedRing response correlation invalid hint=%I64u\n",
                Response->Hint);
            SpdRingRuntimeSetError(Runtime, ERROR_INVALID_DATA);
            SpdStorageUnitRingRuntimeRelease(Runtime);
            return;
        }

        /* Do not hold a transport lock during a payload copy. */
        if (0 != DataBuffer && 0 != Destination &&
            DataBuffer != Destination && 0 != DataLength)
            memcpy(Destination, DataBuffer, DataLength);

        AcquireSRWLockExclusive(&Runtime->CompletionLock);
        Item->SendResponseActive = FALSE;
        if (Item->CallbackFinished)
        {
            if (Item->CallbackComplete)
                ProtocolError = TRUE;
            else
            {
                Item->Message.Response = ResponseCopy;
                SpdRingActiveRemoveLocked(Item);
                Item->State = SpdRingUserItemDoneQueued;
                NotificationError =
                    !SpdRingQueueDoneLocked(Runtime, Item);
            }
        }
        else if (SpdRingUserItemProcessing == Item->State)
        {
            Item->EarlyResponse = ResponseCopy;
            Item->EarlyResponseValid = TRUE;
        }
        else if (SpdRingUserItemDeferred == Item->State)
        {
            Item->Message.Response = ResponseCopy;
            SpdRingActiveRemoveLocked(Item);
            Item->State = SpdRingUserItemDoneQueued;
            NotificationError =
                !SpdRingQueueDoneLocked(Runtime, Item);
        }
        else
            ProtocolError = TRUE;
        Copies = InterlockedDecrement(&Runtime->ResponseCopies);
        ReleaseSRWLockExclusive(&Runtime->CompletionLock);
        if (0 == Copies)
            WakeByAddressAll((PVOID)&Runtime->ResponseCopies);
        if (ProtocolError || NotificationError)
            SpdRingRuntimeSetError(Runtime,
                ProtocolError ? ERROR_INVALID_DATA : ERROR_GEN_FAILURE);
        SpdStorageUnitRingRuntimeRelease(Runtime);
        return;
    }

    Error = SpdStorageUnitHandleTransact(StorageUnit->Handle,
        StorageUnit->Btl, Response, 0, DataBuffer);
    if (ERROR_SUCCESS != Error)
    {
        SpdStorageUnitSetDispatcherError(StorageUnit, Error);

        SpdStorageUnitHandleShutdown(StorageUnit->Handle, &StorageUnit->StorageUnitParams.Guid);
    }
}

SPD_STORAGE_UNIT_OPERATION_CONTEXT *SpdStorageUnitGetOperationContext(VOID)
{
    return (SPD_STORAGE_UNIT_OPERATION_CONTEXT *)TlsGetValue(SpdStorageUnitTlsKey);
}

VOID SpdStorageUnitSetBufferAllocatorF(SPD_STORAGE_UNIT *StorageUnit,
    PVOID(*BufferAlloc)(size_t),
    VOID(*BufferFree)(PVOID))
{
    SpdStorageUnitSetBufferAllocator(StorageUnit, BufferAlloc, BufferFree);
}

VOID SpdStorageUnitGetDispatcherErrorF(SPD_STORAGE_UNIT *StorageUnit,
    DWORD *PDispatcherError)
{
    SpdStorageUnitGetDispatcherError(StorageUnit, PDispatcherError);
}

VOID SpdStorageUnitSetDispatcherErrorF(SPD_STORAGE_UNIT *StorageUnit,
    DWORD DispatcherError)
{
    SpdStorageUnitSetDispatcherError(StorageUnit, DispatcherError);
}

VOID SpdStorageUnitSetDebugLogF(SPD_STORAGE_UNIT *StorageUnit,
    UINT32 DebugLog)
{
    SpdStorageUnitSetDebugLog(StorageUnit, DebugLog);
}
