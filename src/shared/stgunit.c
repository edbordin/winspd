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

#include <shared/shared.h>

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
typedef struct _SPD_RING_WORK_ITEM
{
    LIST_ENTRY Link;
    SPD_IOCTL_TRANSACT_REQ Request;
    SPD_IOCTL_TRANSACT_RSP Response;
    SPD_IOCTL_TRANSACT_RSP EarlyResponse;
    PVOID DataBuffer;
    UINT32 Slot;
    UINT32 DataLength;
    BOOLEAN InUse;
    BOOLEAN Processing;
    BOOLEAN Deferred;
    BOOLEAN EarlyResponseReady;
} SPD_RING_WORK_ITEM;
struct _SPD_RING_RUNTIME
{
    SPD_STORAGE_UNIT *StorageUnit;
    SRWLOCK Lock;
    CONDITION_VARIABLE WorkAvailable;
    CONDITION_VARIABLE SpaceAvailable;
    CONDITION_VARIABLE DoneAvailable;
    LIST_ENTRY Free;
    LIST_ENTRY Work;
    LIST_ENTRY Done;
    SPD_RING_WORK_ITEM *Items;
    UINT32 ItemCount;
    UINT32 RequestsInFlight;
    HANDLE *WorkerThreads;
    ULONG WorkerCount;
    UINT32 CompletionBatchSize;
    UINT32 CompletionWaitMicroseconds;
    HANDLE CompletionThread;
    HANDLE DoneEvent;
    HANDLE BatchTimer;
    volatile LONG References;
    BOOLEAN Stop;
    BOOLEAN ShutdownRequested;
    DWORD Error;
    UINT64 SubmissionBatches;
    UINT64 SubmittedRequests;
    UINT32 MaxSubmissionBatch;
    UINT64 CompletionBatches;
    UINT64 CompletedRequests;
    UINT32 MaxCompletionBatch;
};

static SPD_RING_RUNTIME *SpdStorageUnitRingRuntimeAcquire(
    SPD_STORAGE_UNIT *StorageUnit);
static VOID SpdStorageUnitRingRuntimeRelease(SPD_RING_RUNTIME *Runtime);
static VOID SpdRingRuntimeStop(SPD_RING_RUNTIME *Runtime);
static VOID SpdRingRuntimeRequestShutdown(SPD_RING_RUNTIME *Runtime);

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

    StorageUnit->SharedRingAddress =
        (PVOID)(UINT_PTR)Params->UserAddress;
    StorageUnit->SharedRingSize = (SIZE_T)Params->SectionSize;
    StorageUnit->SharedRingHeader =
        (SPD_RING_HEADER *)StorageUnit->SharedRingAddress;
    return ERROR_SUCCESS;
}

DWORD SpdStorageUnitSetSharedRingCompletionBatch(
    SPD_STORAGE_UNIT *StorageUnit, UINT32 MaxBatchSize,
    UINT32 MaxWaitMicroseconds)
{
    if (0 == StorageUnit || 0 == StorageUnit->SharedRingHeader)
        return ERROR_INVALID_PARAMETER;
    if (0 != StorageUnit->SharedRingRuntime)
        return ERROR_INVALID_STATE;
    if (MaxBatchSize > StorageUnit->SharedRingHeader->CompletionCount ||
        1000000 < MaxWaitMicroseconds)
        return ERROR_INVALID_PARAMETER;
    StorageUnit->SharedRingCompletionBatchSize = MaxBatchSize;
    StorageUnit->SharedRingCompletionWaitMicroseconds = MaxWaitMicroseconds;
    return ERROR_SUCCESS;
}

VOID SpdStorageUnitCloseSharedRing(SPD_STORAGE_UNIT *StorageUnit)
{
    if (0 == StorageUnit->SharedRingAddress)
        return;

    SpdStorageUnitHandleRingClose(StorageUnit->Handle, StorageUnit->Btl);
    StorageUnit->SharedRingAddress = 0;
    StorageUnit->SharedRingSize = 0;
    StorageUnit->SharedRingHeader = 0;
}

VOID SpdStorageUnitDelete(SPD_STORAGE_UNIT *StorageUnit)
{
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

static VOID SpdRingRuntimeSetError(SPD_RING_RUNTIME *Runtime, DWORD Error)
{
    AcquireSRWLockExclusive(&Runtime->Lock);
    if (ERROR_SUCCESS == Runtime->Error)
        Runtime->Error = Error;
    Runtime->Stop = TRUE;
    WakeAllConditionVariable(&Runtime->WorkAvailable);
    WakeAllConditionVariable(&Runtime->SpaceAvailable);
    WakeAllConditionVariable(&Runtime->DoneAvailable);
    ReleaseSRWLockExclusive(&Runtime->Lock);
    if (0 != Runtime->DoneEvent)
        SetEvent(Runtime->DoneEvent);

    /* Interrupt a reader blocked in the batch WAIT doorbell. */
    CancelIoEx(Runtime->StorageUnit->Handle, 0);
}

static VOID SpdRingRuntimeStop(SPD_RING_RUNTIME *Runtime)
{
    AcquireSRWLockExclusive(&Runtime->Lock);
    Runtime->Stop = TRUE;
    WakeAllConditionVariable(&Runtime->WorkAvailable);
    WakeAllConditionVariable(&Runtime->SpaceAvailable);
    WakeAllConditionVariable(&Runtime->DoneAvailable);
    ReleaseSRWLockExclusive(&Runtime->Lock);
    if (0 != Runtime->DoneEvent)
        SetEvent(Runtime->DoneEvent);

    /* Interrupt outstanding asynchronous calls during error teardown. */
    CancelIoEx(Runtime->StorageUnit->Handle, 0);
}

static VOID SpdRingRuntimeRequestShutdown(SPD_RING_RUNTIME *Runtime)
{
    AcquireSRWLockExclusive(&Runtime->Lock);
    Runtime->ShutdownRequested = TRUE;
    Runtime->Stop = TRUE;
    WakeAllConditionVariable(&Runtime->WorkAvailable);
    WakeAllConditionVariable(&Runtime->SpaceAvailable);
    WakeAllConditionVariable(&Runtime->DoneAvailable);
    ReleaseSRWLockExclusive(&Runtime->Lock);
    if (0 != Runtime->DoneEvent)
        SetEvent(Runtime->DoneEvent);

    /* Reset outstanding SRBs to release the synchronous kernel WAIT, while
     * leaving the shared mapping alive until dispatcher threads have joined. */
    DWORD Error = SpdStorageUnitHandleRingStop(
        Runtime->StorageUnit->Handle, Runtime->StorageUnit->Btl);
    if (ERROR_SUCCESS != Error)
        SpdDebugLog("SharedRing stop failed error=%lu\n",
            (unsigned long)Error);
    else
        SpdDebugLog("SharedRing stop issued\n");
}

static DWORD WINAPI SpdStorageUnitRingWorkerThread(PVOID Runtime0)
{
    SPD_RING_RUNTIME *Runtime = Runtime0;
    SPD_STORAGE_UNIT *StorageUnit = Runtime->StorageUnit;

    for (;;)
    {
        SPD_RING_WORK_ITEM *Item;
        SPD_STORAGE_UNIT_OPERATION_CONTEXT OperationContext;
        BOOLEAN Complete;

        AcquireSRWLockExclusive(&Runtime->Lock);
        while (SpdRingListEmpty(&Runtime->Work) && !Runtime->Stop)
            SleepConditionVariableSRW(&Runtime->WorkAvailable,
                &Runtime->Lock, INFINITE, 0);
        if (Runtime->Stop)
        {
            ReleaseSRWLockExclusive(&Runtime->Lock);
            break;
        }
        Item = CONTAINING_RECORD(Runtime->Work.Flink,
            SPD_RING_WORK_ITEM, Link);
        SpdRingListRemove(&Item->Link);
        Item->Processing = TRUE;
        Item->Deferred = FALSE;
        Item->EarlyResponseReady = FALSE;
        ReleaseSRWLockExclusive(&Runtime->Lock);

        if (StorageUnit->DebugLog)
        {
            if (SpdIoctlTransactKindCount <= Item->Request.Kind ||
                (StorageUnit->DebugLog & (1 << Item->Request.Kind)))
                SpdDebugLogRequest(&Item->Request);
        }

        OperationContext.Request = &Item->Request;
        OperationContext.Response = &Item->Response;
        OperationContext.DataBuffer = Item->DataBuffer;
        TlsSetValue(SpdStorageUnitTlsKey, &OperationContext);
        Complete = SpdStorageUnitProcessRequest(StorageUnit,
            &Item->Request, Item->DataBuffer, &Item->Response);
        TlsSetValue(SpdStorageUnitTlsKey, 0);

        AcquireSRWLockExclusive(&Runtime->Lock);
        Item->Processing = FALSE;
        if (Complete)
        {
            Runtime->RequestsInFlight--;
            SpdRingListInsertTail(&Runtime->Done, &Item->Link);
            WakeConditionVariable(&Runtime->DoneAvailable);
            if (0 != Runtime->DoneEvent)
                SetEvent(Runtime->DoneEvent);
        }
        else if (Item->EarlyResponseReady)
        {
            Runtime->RequestsInFlight--;
            memcpy(&Item->Response, &Item->EarlyResponse,
                sizeof Item->Response);
            Item->EarlyResponseReady = FALSE;
            SpdRingListInsertTail(&Runtime->Done, &Item->Link);
            WakeConditionVariable(&Runtime->DoneAvailable);
            if (0 != Runtime->DoneEvent)
                SetEvent(Runtime->DoneEvent);
        }
        else
            Item->Deferred = TRUE;
        ReleaseSRWLockExclusive(&Runtime->Lock);
    }

    return ERROR_SUCCESS;
}

static DWORD WINAPI SpdStorageUnitRingCompletionThread(PVOID Runtime0)
{
    SPD_RING_RUNTIME *Runtime = Runtime0;
    SPD_STORAGE_UNIT *StorageUnit = Runtime->StorageUnit;
    SPD_RING_HEADER *Header = StorageUnit->SharedRingHeader;

    for (;;)
    {
        LIST_ENTRY LocalDone;
        UINT32 BatchCount = 0;
        UINT32 PendingCount;
        UINT64 Producer;
        UINT64 Consumer;
        DWORD Error = ERROR_SUCCESS;

        SpdRingListInitialize(&LocalDone);
        AcquireSRWLockExclusive(&Runtime->Lock);
        while (SpdRingListEmpty(&Runtime->Done) && !Runtime->Stop)
            SleepConditionVariableSRW(&Runtime->DoneAvailable,
                &Runtime->Lock, INFINITE, 0);
        if (SpdRingListEmpty(&Runtime->Done) && Runtime->Stop)
        {
            ReleaseSRWLockExclusive(&Runtime->Lock);
            break;
        }
        if (0 != Runtime->CompletionWaitMicroseconds &&
            1 < Runtime->CompletionBatchSize)
        {
            PendingCount = 0;
            for (PLIST_ENTRY Entry = Runtime->Done.Flink;
                Entry != &Runtime->Done; Entry = Entry->Flink)
                PendingCount++;
            if (PendingCount < Runtime->CompletionBatchSize &&
                0 != Runtime->RequestsInFlight &&
                !Runtime->Stop)
            {
                LARGE_INTEGER DueTime;
                HANDLE WaitObjects[2];
                DWORD WaitResult;

                /* Reset while holding Lock, so producers cannot enqueue and
                 * signal between the empty-count check and the reset. */
                ResetEvent(Runtime->DoneEvent);
                DueTime.QuadPart = -((LONGLONG)
                    Runtime->CompletionWaitMicroseconds * 10);
                if (!SetWaitableTimer(Runtime->BatchTimer, &DueTime,
                    0, 0, 0, FALSE))
                    Error = GetLastError();
                else
                {
                    WaitObjects[0] = Runtime->DoneEvent;
                    WaitObjects[1] = Runtime->BatchTimer;
                    ReleaseSRWLockExclusive(&Runtime->Lock);
                    WaitResult = WaitForMultipleObjects(2, WaitObjects,
                        FALSE, INFINITE);
                    CancelWaitableTimer(Runtime->BatchTimer);
                    AcquireSRWLockExclusive(&Runtime->Lock);
                    if (WAIT_OBJECT_0 != WaitResult &&
                        WAIT_OBJECT_0 + 1 != WaitResult)
                        Error = GetLastError();
                }
            }
        }
        if (ERROR_SUCCESS != Error)
        {
            ReleaseSRWLockExclusive(&Runtime->Lock);
            SpdRingRuntimeSetError(Runtime, Error);
            break;
        }
        if (SpdRingListEmpty(&Runtime->Done) && Runtime->Stop)
        {
            ReleaseSRWLockExclusive(&Runtime->Lock);
            break;
        }
        while (!SpdRingListEmpty(&Runtime->Done) &&
            BatchCount < Runtime->CompletionBatchSize)
        {
            PLIST_ENTRY Entry = SpdRingListRemoveHead(&Runtime->Done);
            SpdRingListInsertTail(&LocalDone, Entry);
            BatchCount++;
        }
        ReleaseSRWLockExclusive(&Runtime->Lock);

        Producer = SpdRingLoadCounter(&Header->CompletionProducer);
        Consumer = SpdRingLoadCounter(&Header->CompletionConsumer);
        if (Consumer > Producer ||
            Producer - Consumer > Header->CompletionCount)
        {
            SpdDebugLog("SharedRing completion counters invalid producer=%I64u consumer=%I64u count=%lu\n",
                Producer, Consumer, (unsigned long)Header->CompletionCount);
            Error = ERROR_INVALID_DATA;
        }

        while (ERROR_SUCCESS == Error && !SpdRingListEmpty(&LocalDone))
        {
            SPD_RING_WORK_ITEM *Item = CONTAINING_RECORD(
                LocalDone.Flink, SPD_RING_WORK_ITEM, Link);
            SPD_RING_COMPLETION *Completion;

            if (Producer - Consumer >= Header->CompletionCount)
            {
                SpdDebugLog("SharedRing completion ring full producer=%I64u consumer=%I64u count=%lu\n",
                    Producer, Consumer, (unsigned long)Header->CompletionCount);
                Error = ERROR_NOT_ENOUGH_MEMORY;
                break;
            }

            SpdRingListRemove(&Item->Link);
            Completion = (SPD_RING_COMPLETION *)
                ((PUINT8)StorageUnit->SharedRingAddress +
                Header->CompletionOffset +
                (Producer % Header->CompletionCount) * sizeof *Completion);
            memcpy(&Completion->Response, &Item->Response,
                sizeof Completion->Response);
            MemoryBarrier();
            SpdRingStoreCounter(&Header->CompletionProducer, ++Producer);

            AcquireSRWLockExclusive(&Runtime->Lock);
            Item->InUse = FALSE;
            SpdRingListInsertTail(&Runtime->Free, &Item->Link);
            Item->Deferred = FALSE;
            ReleaseSRWLockExclusive(&Runtime->Lock);
        }

        if (ERROR_SUCCESS == Error)
        {
            SPD_IOCTL_RING_KICK_PARAMS KickParams;
            memset(&KickParams, 0, sizeof KickParams);
            Error = SpdStorageUnitHandleRingKick(StorageUnit->Handle,
                StorageUnit->Btl, &KickParams);
            Runtime->CompletionBatches++;
            Runtime->CompletedRequests += KickParams.Consumed;
            if (BatchCount > Runtime->MaxCompletionBatch)
                Runtime->MaxCompletionBatch = BatchCount;
            if (ERROR_SUCCESS != Error)
                SpdDebugLog("SharedRing completion kick failed error=%lu consumed=%lu\n",
                    (unsigned long)Error, (unsigned long)KickParams.Consumed);
        }

        if (ERROR_SUCCESS != Error)
        {
            while (!SpdRingListEmpty(&LocalDone))
            {
                SPD_RING_WORK_ITEM *Item = CONTAINING_RECORD(
                    LocalDone.Flink, SPD_RING_WORK_ITEM, Link);
                SpdRingListRemove(&Item->Link);
                AcquireSRWLockExclusive(&Runtime->Lock);
                Item->InUse = FALSE;
                SpdRingListInsertTail(&Runtime->Free, &Item->Link);
                ReleaseSRWLockExclusive(&Runtime->Lock);
            }
            SpdRingRuntimeSetError(Runtime, Error);
            break;
        }

        AcquireSRWLockExclusive(&Runtime->Lock);
        WakeAllConditionVariable(&Runtime->SpaceAvailable);
        ReleaseSRWLockExclusive(&Runtime->Lock);
    }

    return ERROR_SUCCESS;
}

static DWORD SpdStorageUnitRingRuntimeCreate(
    SPD_STORAGE_UNIT *StorageUnit, ULONG WorkerCount)
{
    SPD_RING_RUNTIME *Runtime;
    SPD_RING_HEADER *Header = StorageUnit->SharedRingHeader;
    DWORD Error = ERROR_SUCCESS;

    if (0 != StorageUnit->SharedRingRuntime ||
        Header->BufferCount > Header->CompletionCount)
        return ERROR_INVALID_PARAMETER;
    if (0 == WorkerCount)
        WorkerCount = 1;
    if (1024 < WorkerCount)
        WorkerCount = 1024;

    Runtime = MemAlloc(sizeof *Runtime);
    if (0 == Runtime)
        return ERROR_NOT_ENOUGH_MEMORY;
    memset(Runtime, 0, sizeof *Runtime);
    Runtime->StorageUnit = StorageUnit;
    Runtime->WorkerCount = WorkerCount;
    Runtime->ItemCount = Header->BufferCount;
    Runtime->CompletionBatchSize =
        0 != StorageUnit->SharedRingCompletionBatchSize ?
        StorageUnit->SharedRingCompletionBatchSize : Header->CompletionCount;
    Runtime->CompletionWaitMicroseconds =
        StorageUnit->SharedRingCompletionWaitMicroseconds;
    Runtime->References = 1; /* owner reference held by StorageUnit */
    InitializeSRWLock(&Runtime->Lock);
    InitializeConditionVariable(&Runtime->WorkAvailable);
    InitializeConditionVariable(&Runtime->SpaceAvailable);
    InitializeConditionVariable(&Runtime->DoneAvailable);
    SpdRingListInitialize(&Runtime->Free);
    SpdRingListInitialize(&Runtime->Work);
    SpdRingListInitialize(&Runtime->Done);

    if (0 != Runtime->CompletionWaitMicroseconds)
    {
        Runtime->DoneEvent = CreateEventW(0, TRUE, FALSE, 0);
        if (0 == Runtime->DoneEvent)
        {
            Error = GetLastError();
            goto exit;
        }
        Runtime->BatchTimer = CreateWaitableTimerExW(0, 0,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (0 == Runtime->BatchTimer)
        {
            Error = GetLastError();
            goto exit;
        }
    }

    Runtime->Items = MemAlloc(sizeof *Runtime->Items * Runtime->ItemCount);
    Runtime->WorkerThreads = MemAlloc(
        sizeof *Runtime->WorkerThreads * Runtime->WorkerCount);
    if (0 == Runtime->Items || 0 == Runtime->WorkerThreads)
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
        goto exit;
    }
    memset(Runtime->Items, 0,
        sizeof *Runtime->Items * Runtime->ItemCount);
    memset(Runtime->WorkerThreads, 0,
        sizeof *Runtime->WorkerThreads * Runtime->WorkerCount);
    for (UINT32 I = 0; Runtime->ItemCount > I; I++)
        SpdRingListInsertTail(&Runtime->Free, &Runtime->Items[I].Link);

    AcquireSRWLockExclusive(&StorageUnit->SharedRingRuntimeLock);
    StorageUnit->SharedRingRuntime = Runtime;
    ReleaseSRWLockExclusive(&StorageUnit->SharedRingRuntimeLock);
    Runtime->CompletionThread = CreateThread(0, 0,
        SpdStorageUnitRingCompletionThread, Runtime, 0, 0);
    if (0 == Runtime->CompletionThread)
    {
        Error = GetLastError();
        goto exit;
    }
    for (ULONG I = 0; Runtime->WorkerCount > I; I++)
    {
        Runtime->WorkerThreads[I] = CreateThread(0, 0,
            SpdStorageUnitRingWorkerThread, Runtime, 0, 0);
        if (0 == Runtime->WorkerThreads[I])
        {
            Error = GetLastError();
            goto exit;
        }
    }
    return ERROR_SUCCESS;

exit:
    SpdRingRuntimeStop(Runtime);
    if (0 != Runtime->CompletionThread)
        WaitForSingleObject(Runtime->CompletionThread, INFINITE);
    if (0 != Runtime->WorkerThreads)
        for (ULONG I = 0; Runtime->WorkerCount > I; I++)
            if (0 != Runtime->WorkerThreads[I])
                WaitForSingleObject(Runtime->WorkerThreads[I], INFINITE);
    if (0 != Runtime->CompletionThread)
        CloseHandle(Runtime->CompletionThread);
    if (0 != Runtime->DoneEvent)
        CloseHandle(Runtime->DoneEvent);
    if (0 != Runtime->BatchTimer)
        CloseHandle(Runtime->BatchTimer);
    if (0 != Runtime->WorkerThreads)
        for (ULONG I = 0; Runtime->WorkerCount > I; I++)
            if (0 != Runtime->WorkerThreads[I])
                CloseHandle(Runtime->WorkerThreads[I]);
    StorageUnit->SharedRingRuntime = 0;
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

static VOID SpdStorageUnitRingRuntimeRelease(SPD_RING_RUNTIME *Runtime)
{
    if (0 == InterlockedDecrement(&Runtime->References))
        WakeByAddressAll((PVOID)&Runtime->References);
}

static DWORD SpdStorageUnitRingRuntimeDestroy(SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_RUNTIME *Runtime;

    AcquireSRWLockExclusive(&StorageUnit->SharedRingRuntimeLock);
    Runtime = StorageUnit->SharedRingRuntime;
    StorageUnit->SharedRingRuntime = 0;
    ReleaseSRWLockExclusive(&StorageUnit->SharedRingRuntimeLock);
    if (0 == Runtime)
        return ERROR_SUCCESS;

    SpdRingRuntimeStop(Runtime);
    for (ULONG I = 0; Runtime->WorkerCount > I; I++)
        WaitForSingleObject(Runtime->WorkerThreads[I], INFINITE);
    WaitForSingleObject(Runtime->CompletionThread, INFINITE);
    SpdDebugLog("SharedRing batches submissions=%I64u requests=%I64u "
        "max=%lu completions=%I64u responses=%I64u max=%lu workers=%lu "
        "slots=%lu sq=%lu cq=%lu cbatch=%lu cwait_us=%lu\n",
        Runtime->SubmissionBatches, Runtime->SubmittedRequests,
        (unsigned long)Runtime->MaxSubmissionBatch,
        Runtime->CompletionBatches, Runtime->CompletedRequests,
        (unsigned long)Runtime->MaxCompletionBatch,
        (unsigned long)Runtime->WorkerCount,
        (unsigned long)Runtime->ItemCount,
        (unsigned long)StorageUnit->SharedRingHeader->SubmissionCount,
        (unsigned long)StorageUnit->SharedRingHeader->CompletionCount,
        (unsigned long)Runtime->CompletionBatchSize,
        (unsigned long)Runtime->CompletionWaitMicroseconds);
    for (ULONG I = 0; Runtime->WorkerCount > I; I++)
        CloseHandle(Runtime->WorkerThreads[I]);
    CloseHandle(Runtime->CompletionThread);
    if (0 != Runtime->DoneEvent)
        CloseHandle(Runtime->DoneEvent);
    if (0 != Runtime->BatchTimer)
        CloseHandle(Runtime->BatchTimer);
    DWORD Error = Runtime->Error;
    SpdStorageUnitRingRuntimeRelease(Runtime);
    for (;;)
    {
        LONG Observed = InterlockedCompareExchange(&Runtime->References,
            0, 0);
        if (0 == Observed)
            break;
        WaitOnAddress(&Runtime->References, &Observed,
            sizeof Observed, INFINITE);
    }
    MemFree(Runtime->WorkerThreads);
    MemFree(Runtime->Items);
    MemFree(Runtime);
    return Error;
}

static DWORD WINAPI SpdStorageUnitRingDispatcherThread(PVOID StorageUnit0)
{
    SPD_STORAGE_UNIT *StorageUnit = StorageUnit0;
    SPD_RING_RUNTIME *Runtime = StorageUnit->SharedRingRuntime;
    SPD_RING_HEADER *Header = StorageUnit->SharedRingHeader;
    DWORD Error = ERROR_SUCCESS;

    for (;;)
    {
        SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
        UINT64 Consumer;
        UINT64 Producer;
        ULONG FreeCount = 0;

        AcquireSRWLockExclusive(&Runtime->Lock);
        while (SpdRingListEmpty(&Runtime->Free) && !Runtime->Stop)
            SleepConditionVariableSRW(&Runtime->SpaceAvailable,
                &Runtime->Lock, INFINITE, 0);
        if (Runtime->Stop)
        {
            ReleaseSRWLockExclusive(&Runtime->Lock);
            break;
        }
        for (PLIST_ENTRY Entry = Runtime->Free.Flink;
            Entry != &Runtime->Free; Entry = Entry->Flink)
            FreeCount++;
        ReleaseSRWLockExclusive(&Runtime->Lock);

        memset(&WaitParams, 0, sizeof WaitParams);
        WaitParams.MaxRequests = FreeCount < Header->SubmissionCount ?
            FreeCount : Header->SubmissionCount;
        Error = SpdStorageUnitHandleRingWait(StorageUnit->Handle,
            StorageUnit->Btl, &WaitParams);
        if (ERROR_SUCCESS != Error)
        {
            SpdDebugLog("SharedRing dispatcher wait failed error=%lu max=%lu\n",
                (unsigned long)Error, (unsigned long)WaitParams.MaxRequests);
            break;
        }
        Runtime->SubmissionBatches++;
        Runtime->SubmittedRequests += WaitParams.Produced;
        if (WaitParams.Produced > Runtime->MaxSubmissionBatch)
            Runtime->MaxSubmissionBatch = WaitParams.Produced;

        Consumer = SpdRingLoadCounter(&Header->SubmissionConsumer);
        Producer = SpdRingLoadCounter(&Header->SubmissionProducer);
        if (Consumer > Producer ||
            Producer - Consumer > Header->SubmissionCount)
        {
            SpdDebugLog("SharedRing submission counters invalid producer=%I64u consumer=%I64u count=%lu\n",
                Producer, Consumer, (unsigned long)Header->SubmissionCount);
            Error = ERROR_INVALID_DATA;
            break;
        }

        while (Consumer < Producer)
        {
            SPD_RING_REQUEST *RingRequest = (SPD_RING_REQUEST *)
                ((PUINT8)StorageUnit->SharedRingAddress +
                Header->SubmissionOffset +
                (Consumer % Header->SubmissionCount) * sizeof *RingRequest);
            SPD_RING_BUFFER_REF Data = RingRequest->Data;
            SPD_RING_WORK_ITEM *Item;
            UINT32 ItemIndex = (UINT32)RingRequest->Request.Hint;
            PVOID DataBuffer = 0;
            UINT64 ExpectedLength = 0;

            switch (RingRequest->Request.Kind)
            {
            case SpdIoctlTransactReadKind:
                ExpectedLength = (UINT64)RingRequest->Request.Op.Read.BlockCount *
                    StorageUnit->StorageUnitParams.BlockLength;
                break;
            case SpdIoctlTransactWriteKind:
                ExpectedLength = (UINT64)RingRequest->Request.Op.Write.BlockCount *
                    StorageUnit->StorageUnitParams.BlockLength;
                break;
            case SpdIoctlTransactUnmapKind:
                ExpectedLength = (UINT64)RingRequest->Request.Op.Unmap.Count *
                    sizeof(SPD_IOCTL_UNMAP_DESCRIPTOR);
                break;
            case SpdIoctlTransactFlushKind:
                break;
            default:
                SpdDebugLog("SharedRing request kind invalid kind=%u\n",
                    (unsigned)RingRequest->Request.Kind);
                Error = ERROR_INVALID_DATA;
                break;
            }
            if (ERROR_SUCCESS == Error &&
                (ExpectedLength > 0xffffffffULL ||
                Data.Length != (UINT32)ExpectedLength ||
                0 != Data.Flags ||
                (SPD_RING_NO_BUFFER == Data.Slot && 0 != Data.Offset) ||
                (0 == ExpectedLength) != (SPD_RING_NO_BUFFER == Data.Slot)))
            {
                SpdDebugLog("SharedRing request invalid kind=%u slot=%lu offset=%lu length=%lu expected=%I64u flags=%lu\n",
                    (unsigned)RingRequest->Request.Kind,
                    (unsigned long)Data.Slot, (unsigned long)Data.Offset,
                    (unsigned long)Data.Length, ExpectedLength,
                    (unsigned long)Data.Flags);
                Error = ERROR_INVALID_DATA;
            }
            if (ERROR_SUCCESS == Error &&
                ((UINT32)RingRequest->Request.Hint >= Runtime->ItemCount ||
                (SPD_RING_NO_BUFFER != Data.Slot &&
                Data.Slot != (UINT32)RingRequest->Request.Hint)))
            {
                SpdDebugLog("SharedRing request token/buffer slot mismatch hint=%I64u data_slot=%lu item_count=%lu\n",
                    RingRequest->Request.Hint, (unsigned long)Data.Slot,
                    (unsigned long)Runtime->ItemCount);
                Error = ERROR_INVALID_DATA;
            }

            if (ERROR_SUCCESS == Error && Data.Slot != SPD_RING_NO_BUFFER)
            {
                if (Data.Slot >= Header->BufferCount ||
                    Data.Offset > Header->BufferSize ||
                    Data.Length > Header->BufferSize - Data.Offset)
                {
                    SpdDebugLog("SharedRing request buffer invalid slot=%lu offset=%lu length=%lu buffer_count=%lu buffer_size=%lu\n",
                        (unsigned long)Data.Slot, (unsigned long)Data.Offset,
                        (unsigned long)Data.Length,
                        (unsigned long)Header->BufferCount,
                        (unsigned long)Header->BufferSize);
                    Error = ERROR_INVALID_DATA;
                    break;
                }
                DataBuffer = (PUINT8)StorageUnit->SharedRingAddress +
                    Header->BufferOffset +
                    (SIZE_T)Data.Slot * Header->BufferSize + Data.Offset;
            }

            if (ItemIndex >= Runtime->ItemCount)
            {
                SpdDebugLog("SharedRing request token slot invalid hint=%I64u item_count=%lu\n",
                    RingRequest->Request.Hint,
                    (unsigned long)Runtime->ItemCount);
                Error = ERROR_INVALID_DATA;
                break;
            }
            AcquireSRWLockExclusive(&Runtime->Lock);
            Item = &Runtime->Items[ItemIndex];
            if (Item->InUse)
            {
                SpdDebugLog("SharedRing request token slot already in use hint=%I64u slot=%lu\n",
                    RingRequest->Request.Hint, (unsigned long)ItemIndex);
                ReleaseSRWLockExclusive(&Runtime->Lock);
                Error = ERROR_INVALID_DATA;
                break;
            }
            SpdRingListRemove(&Item->Link);
            Item->InUse = TRUE;
            ReleaseSRWLockExclusive(&Runtime->Lock);

            memcpy(&Item->Request, &RingRequest->Request,
                sizeof Item->Request);
            Item->DataBuffer = DataBuffer;
            Item->DataLength = Data.Length;
            Item->Slot = Data.Slot;
            Item->Deferred = FALSE;
            Consumer++;
            SpdRingStoreCounter(&Header->SubmissionConsumer, Consumer);

            AcquireSRWLockExclusive(&Runtime->Lock);
            Runtime->RequestsInFlight++;
            SpdRingListInsertTail(&Runtime->Work, &Item->Link);
            WakeConditionVariable(&Runtime->WorkAvailable);
            ReleaseSRWLockExclusive(&Runtime->Lock);
        }

        if (ERROR_SUCCESS != Error)
            break;
    }

    if (ERROR_SUCCESS != Error)
    {
        BOOLEAN ShutdownRequested;
        DWORD RuntimeError;

        AcquireSRWLockShared(&Runtime->Lock);
        ShutdownRequested = Runtime->ShutdownRequested;
        RuntimeError = Runtime->Error;
        ReleaseSRWLockShared(&Runtime->Lock);

        /* Cancelling the blocked WAIT is the normal shutdown doorbell. */
        if (ShutdownRequested && ERROR_OPERATION_ABORTED == Error &&
            ERROR_SUCCESS == RuntimeError)
            Error = ERROR_SUCCESS;
        else
            SpdRingRuntimeSetError(Runtime, Error);
    }
    if (ERROR_SUCCESS == Error)
        SpdRingRuntimeStop(Runtime);
    {
        DWORD RuntimeError = SpdStorageUnitRingRuntimeDestroy(StorageUnit);
        if (ERROR_SUCCESS == Error && ERROR_SUCCESS != RuntimeError)
            Error = RuntimeError;
    }
    SpdStorageUnitSetDispatcherError(StorageUnit, Error);
    SpdDebugLog("SharedRing dispatcher stopped error=%lu\n",
        (unsigned long)Error);
    /* Do not dereference Header here. A shutdown request may have caused the
     * driver to unmap the user view while RingWait was being cancelled. */
    SpdStorageUnitHandleShutdown(StorageUnit->Handle,
        &StorageUnit->StorageUnitParams.Guid);
    return Error;
}

DWORD SpdStorageUnitStartDispatcher(SPD_STORAGE_UNIT *StorageUnit, ULONG ThreadCount)
{
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

    if (0 != StorageUnit->SharedRingAddress)
    {
        DWORD Error = SpdStorageUnitRingRuntimeCreate(StorageUnit,
            ThreadCount);
        if (ERROR_SUCCESS != Error)
            return Error;
    }

    StorageUnit->DispatcherThreadCount = ThreadCount;
    StorageUnit->DispatcherThread = CreateThread(0, 0,
        0 != StorageUnit->SharedRingAddress ?
            SpdStorageUnitRingDispatcherThread :
            SpdStorageUnitDispatcherThread,
        StorageUnit, CREATE_SUSPENDED,
        &StorageUnit->DispatcherThreadId);
    if (0 == StorageUnit->DispatcherThread)
    {
        if (0 != StorageUnit->SharedRingRuntime)
            SpdStorageUnitRingRuntimeDestroy(StorageUnit);
        return GetLastError();
    }
    if (!ResumeThread(StorageUnit->DispatcherThread))
    {
        CloseHandle(StorageUnit->DispatcherThread);
        if (0 != StorageUnit->SharedRingRuntime)
            SpdStorageUnitRingRuntimeDestroy(StorageUnit);
        StorageUnit->DispatcherThread = 0;
        return GetLastError();
    }

    return ERROR_SUCCESS;
}

VOID SpdStorageUnitWaitDispatcher(SPD_STORAGE_UNIT *StorageUnit)
{
    if (0 == StorageUnit->DispatcherThread)
        return;

    WaitForSingleObject(StorageUnit->DispatcherThread, INFINITE);
    CloseHandle(StorageUnit->DispatcherThread);
    StorageUnit->DispatcherThread = 0;
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
        SPD_RING_WORK_ITEM *Found = 0;

        /* The dispatcher may already be tearing down the ring. */
        if (0 == Runtime)
            return;

        AcquireSRWLockExclusive(&Runtime->Lock);
        UINT32 ItemIndex = (UINT32)Response->Hint;
        if (ItemIndex < Runtime->ItemCount)
        {
            SPD_RING_WORK_ITEM *Item = &Runtime->Items[ItemIndex];
            if (Item->InUse && (Item->Processing || Item->Deferred) &&
                Item->Request.Hint == Response->Hint &&
                Item->Request.Kind == Response->Kind)
                Found = Item;
        }
        if (0 != Found)
        {
            if (0 != DataBuffer && 0 != Found->DataBuffer &&
                DataBuffer != Found->DataBuffer && 0 != Found->DataLength)
                memcpy(Found->DataBuffer, DataBuffer, Found->DataLength);
            if (Found->Processing)
            {
                memcpy(&Found->EarlyResponse, Response,
                    sizeof Found->EarlyResponse);
                Found->EarlyResponseReady = TRUE;
            }
            else
            {
                memcpy(&Found->Response, Response,
                    sizeof Found->Response);
                Found->Deferred = FALSE;
                Runtime->RequestsInFlight--;
                SpdRingListInsertTail(&Runtime->Done, &Found->Link);
                WakeConditionVariable(&Runtime->DoneAvailable);
                if (0 != Runtime->DoneEvent)
                    SetEvent(Runtime->DoneEvent);
            }
        }
        ReleaseSRWLockExclusive(&Runtime->Lock);

        if (0 == Found)
        {
            SpdDebugLog("SharedRing response correlation missing hint=%I64u\n",
                Response->Hint);
            SpdRingRuntimeSetError(Runtime, ERROR_NOT_FOUND);
            SpdStorageUnitRingRuntimeRelease(Runtime);
            return;
        }
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
