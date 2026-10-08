/**
 * @file sys/stgunit.c
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

#include <sys/driver.h>

ERESOURCE SpdGlobalDeviceResource;
SPD_DEVICE_EXTENSION *SpdGlobalDeviceExtension;
ULONG SpdStorageUnitCapacity = SPD_IOCTL_STORAGE_UNIT_CAPACITY;

#define SPD_RING_MAX_PRODUCER_BATCH 4

static VOID SpdDeviceExtensionNotifyRoutine(HANDLE ParentId, HANDLE ProcessId0, BOOLEAN Create);
static VOID SpdStorageUnitRingProducerDpc(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2);
static BOOLEAN SpdStorageUnitRingEnter(SPD_STORAGE_UNIT *StorageUnit);
static VOID SpdStorageUnitRingLeave(SPD_STORAGE_UNIT *StorageUnit);
static VOID SpdStorageUnitRingCompleteWait(
    SPD_STORAGE_UNIT *StorageUnit,
    NTSTATUS Status,
    BOOLEAN RequireRequest);
static VOID SpdStorageUnitRingCancelWait(SPD_STORAGE_UNIT *StorageUnit);

static BOOLEAN SpdRingSizeAdd(SIZE_T *PSize, SIZE_T Add)
{
    if (*PSize > (SIZE_T)-1 - Add)
        return FALSE;
    *PSize += Add;
    return TRUE;
}

static BOOLEAN SpdRingSizeAlign(SIZE_T *PSize, SIZE_T Alignment)
{
    SIZE_T Remainder = *PSize % Alignment;
    return 0 == Remainder ||
        SpdRingSizeAdd(PSize, Alignment - Remainder);
}

static BOOLEAN SpdRingQueueDepthValid(UINT32 QueueDepth)
{
    return SPD_RING_MIN_QUEUE_DEPTH <= QueueDepth &&
        SPD_RING_MAX_QUEUE_DEPTH >= QueueDepth &&
        0 == (QueueDepth & (QueueDepth - 1));
}

static VOID SpdStorageUnitRingReset(
    SPD_STORAGE_UNIT *StorageUnit,
    BOOLEAN UserProcessExiting)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;

    if (0 != Ring->Mdl)
    {
        MmUnlockPages(Ring->Mdl);
        IoFreeMdl(Ring->Mdl);
        Ring->Mdl = 0;
    }

    /* The MDL owns the kernel mapping returned for its locked pages. */
    Ring->SystemAddress = 0;

    if (0 != Ring->UserAddress)
    {
        if (!UserProcessExiting)
            ZwUnmapViewOfSection(ZwCurrentProcess(), Ring->UserAddress);

        Ring->UserAddress = 0;
    }

    if (0 != Ring->SectionHandle)
    {
        ZwClose(Ring->SectionHandle);
        Ring->SectionHandle = 0;
    }

    if (0 != Ring->Buffers.FreeIds)
        SpdFree(Ring->Buffers.FreeIds, SpdTagStorageUnit);
    if (0 != Ring->Buffers.Meta)
        SpdFree(Ring->Buffers.Meta, SpdTagStorageUnit);
    Ring->Buffers.FreeIds = 0;
    Ring->Buffers.Meta = 0;
    Ring->Buffers.FreeCount = 0;
    Ring->SectionSize = 0;
    Ring->RequestOffset = 0;
    Ring->CompletionOffset = 0;
    Ring->BufferOffset = 0;
    Ring->QueueDepth = 0;
    Ring->BufferCount = 0;
    Ring->BufferSize = 0;
    Ring->RequestTail = 0;
    Ring->CompletionHead = 0;
    Ring->ProcessId = 0;
    Ring->UserProcessId = 0;
    InterlockedExchangePointer(&Ring->WaitIrp, 0);
    InterlockedExchange(&Ring->ProducerState, 0);
    InterlockedExchange(&Ring->KickActive, 0);
    /* Close is terminal for this storage unit. */
    Ring->Stopping = TRUE;
}

static BOOLEAN SpdRingBufferAllocLocked(
    SPD_RING_STATE *Ring,
    UINT32 *PBufferId)
{
    SPD_RING_BUFFER_POOL *Pool = &Ring->Buffers;
    UINT32 Id;

    if (0 == Pool->FreeCount)
        return FALSE;

    Id = Pool->FreeIds[--Pool->FreeCount];
    ASSERT(Id < Ring->BufferCount);
    ASSERT(!Pool->Meta[Id].Allocated);
    Pool->Meta[Id].Allocated = TRUE;
    Pool->Meta[Id].OwnerHint = 0;
    Pool->Meta[Id].Length = 0;
    Pool->Meta[Id].Kind = 0;
    *PBufferId = Id;
    return TRUE;
}

static VOID SpdRingBufferFreeLocked(
    SPD_RING_STATE *Ring,
    UINT32 BufferId)
{
    SPD_RING_BUFFER_POOL *Pool = &Ring->Buffers;

    ASSERT(BufferId < Ring->BufferCount);
    ASSERT(Pool->Meta[BufferId].Allocated);
    ASSERT(Pool->FreeCount < Ring->BufferCount);
    Pool->Meta[BufferId].Allocated = FALSE;
    Pool->Meta[BufferId].OwnerHint = 0;
    Pool->Meta[BufferId].Length = 0;
    Pool->Meta[BufferId].Kind = 0;
    Pool->FreeIds[Pool->FreeCount++] = BufferId;
}

static PVOID SpdRingBufferAddress(
    SPD_RING_STATE *Ring,
    UINT32 BufferId,
    UINT32 Offset,
    UINT32 Length)
{
    SIZE_T BufferOffset;

    if (BufferId >= Ring->BufferCount ||
        Offset > Ring->BufferSize ||
        Length > Ring->BufferSize - Offset)
        return 0;

    BufferOffset = (SIZE_T)Ring->BufferOffset +
        (SIZE_T)BufferId * Ring->BufferSize + Offset;
    if (BufferOffset > Ring->SectionSize ||
        Length > Ring->SectionSize - BufferOffset)
        return 0;

    return (PUINT8)Ring->SystemAddress + BufferOffset;
}

static VOID SpdStorageUnitRingMarkFailed(
    SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    KIRQL Irql;

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    Ring->Failed = TRUE;
    Ring->Stopping = TRUE;
    KeReleaseSpinLock(&Ring->Lock, Irql);
    SpdIoqReset(StorageUnit->Ioq, TRUE,
        SpdIoqResetReasonRingFailure);
    SpdStorageUnitRingCancelWait(StorageUnit);
}

NTSTATUS SpdStorageUnitRingOpen(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId,
    SPD_IOCTL_RING_OPEN_PARAMS *Params)
{
    NTSTATUS Result;
    HANDLE SectionHandle = 0;
    PVOID SystemAddress = 0;
    PVOID UserAddress = 0;
    PMDL Mdl = 0;
    BOOLEAN PagesLocked = FALSE;
    SIZE_T SectionSize;
    SIZE_T ViewSize;
    SIZE_T Offset;
    SIZE_T RequestBytes;
    SIZE_T CompletionBytes;
    SIZE_T BufferBytes;
    LARGE_INTEGER MaximumSize;
    OBJECT_ATTRIBUTES ObjectAttributes;
    SPD_RING_HEADER *Header;
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    UINT32 *FreeIds = 0;
    SPD_RING_BUFFER_META *Meta = 0;
    KIRQL Irql;

    if (ProcessId != StorageUnit->TransactProcessId)
        return STATUS_ACCESS_DENIED;
    if (SPD_RING_VERSION_4 != Params->Version)
        return STATUS_NOT_SUPPORTED;
    if (0 != Params->Flags || 0 == Params->BufferCount ||
        !SpdRingQueueDepthValid(Params->QueueDepth) ||
        Params->BufferSize < StorageUnit->StorageUnitParams.MaxTransferLength)
        return STATUS_INVALID_PARAMETER;

    if ((SIZE_T)-1 / Params->BufferCount < Params->BufferSize)
        return STATUS_INVALID_PARAMETER;
    if ((SIZE_T)Params->BufferCount >
            (SIZE_T)-1 / sizeof(UINT32) ||
        (SIZE_T)Params->BufferCount >
            (SIZE_T)-1 / sizeof(SPD_RING_BUFFER_META))
        return STATUS_INVALID_PARAMETER;
    BufferBytes = (SIZE_T)Params->BufferCount * Params->BufferSize;
    RequestBytes = (SIZE_T)Params->QueueDepth * sizeof(SPD_RING_REQUEST);
    CompletionBytes = (SIZE_T)Params->QueueDepth * sizeof(SPD_RING_COMPLETION);

    Offset = sizeof(SPD_RING_HEADER);
    if (!SpdRingSizeAlign(&Offset, SPD_RING_CACHE_LINE_SIZE) ||
        !SpdRingSizeAdd(&Offset, RequestBytes) ||
        !SpdRingSizeAlign(&Offset, SPD_RING_CACHE_LINE_SIZE) ||
        !SpdRingSizeAdd(&Offset, CompletionBytes) ||
        !SpdRingSizeAlign(&Offset, PAGE_SIZE))
        return STATUS_INVALID_PARAMETER;
    if ((SIZE_T)-1 > MAXULONG && Offset > MAXULONG)
        return STATUS_INVALID_PARAMETER;
    if (!SpdRingSizeAdd(&Offset, BufferBytes) ||
        !SpdRingSizeAlign(&Offset, PAGE_SIZE))
        return STATUS_INVALID_PARAMETER;
    SectionSize = Offset;
    if (SPD_RING_MAX_SECTION_BYTES < SectionSize)
        return STATUS_INVALID_PARAMETER;

    FreeIds = SpdAllocNonPaged(
        (SIZE_T)Params->BufferCount * sizeof *FreeIds,
        SpdTagStorageUnit);
    Meta = SpdAllocNonPaged(
        (SIZE_T)Params->BufferCount * sizeof *Meta,
        SpdTagStorageUnit);
    if (0 == FreeIds || 0 == Meta)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    RtlZeroMemory(Meta,
        (SIZE_T)Params->BufferCount * sizeof *Meta);
    for (UINT32 I = 0; Params->BufferCount > I; I++)
        FreeIds[I] = I;

    MaximumSize.QuadPart = (LONGLONG)SectionSize;
    InitializeObjectAttributes(&ObjectAttributes, 0,
        OBJ_KERNEL_HANDLE, 0, 0);
    Result = ZwCreateSection(&SectionHandle,
        SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
        &ObjectAttributes, &MaximumSize, PAGE_READWRITE, SEC_COMMIT, 0);
    if (!NT_SUCCESS(Result))
        goto exit;

    ViewSize = SectionSize;
    Result = ZwMapViewOfSection(SectionHandle, ZwCurrentProcess(),
        &UserAddress, 0, 0, 0, &ViewSize, ViewUnmap, 0, PAGE_READWRITE);
    if (!NT_SUCCESS(Result))
        goto exit;
    if (ViewSize < SectionSize)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    /* Pin the pages because Storport prepare/complete callbacks may copy
     * payloads at DISPATCH_LEVEL. */
    Mdl = IoAllocateMdl(UserAddress, (ULONG)SectionSize,
        FALSE, FALSE, NULL);
    if (0 == Mdl)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    __try
    {
        MmProbeAndLockPages(Mdl, UserMode, IoModifyAccess);
        PagesLocked = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Result = GetExceptionCode();
        goto exit;
    }

    SystemAddress = MmGetSystemAddressForMdlSafe(
        Mdl,
        NormalPagePriority | MdlMappingNoExecute);
    if (0 == SystemAddress)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    RtlZeroMemory(SystemAddress, SectionSize);
    Header = (SPD_RING_HEADER *)SystemAddress;
    Header->Version = SPD_RING_VERSION_4;
    Header->HeaderSize = sizeof *Header;
    Header->RequestOffset = (UINT32)SPD_IOCTL_ALIGN_UP(
        sizeof *Header, SPD_RING_CACHE_LINE_SIZE);
    Header->CompletionOffset = (UINT32)SPD_IOCTL_ALIGN_UP(
        Header->RequestOffset + RequestBytes,
        SPD_RING_CACHE_LINE_SIZE);
    Header->BufferOffset = (UINT32)SPD_IOCTL_ALIGN_UP(
        Header->CompletionOffset + CompletionBytes, PAGE_SIZE);
    Header->QueueDepth = Params->QueueDepth;
    Header->BufferCount = Params->BufferCount;
    Header->BufferSize = Params->BufferSize;

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    if (ProcessId != StorageUnit->TransactProcessId)
    {
        KeReleaseSpinLock(&Ring->Lock, Irql);
        Result = STATUS_ACCESS_DENIED;
        goto exit;
    }
    if (0 != Ring->SectionHandle || Ring->Stopping)
    {
        KeReleaseSpinLock(&Ring->Lock, Irql);
        Result = STATUS_DEVICE_BUSY;
        goto exit;
    }
    Ring->SystemAddress = SystemAddress;
    Ring->SectionSize = SectionSize;
    Ring->RequestOffset = Header->RequestOffset;
    Ring->CompletionOffset = Header->CompletionOffset;
    Ring->BufferOffset = Header->BufferOffset;
    Ring->QueueDepth = Params->QueueDepth;
    Ring->BufferCount = Params->BufferCount;
    Ring->BufferSize = Params->BufferSize;
    Ring->RequestTail = 0;
    Ring->CompletionHead = 0;
    Ring->Buffers.FreeIds = FreeIds;
    Ring->Buffers.FreeCount = Params->BufferCount;
    Ring->Buffers.Meta = Meta;
    Ring->SectionHandle = SectionHandle;
    Ring->Mdl = Mdl;
    Ring->UserAddress = UserAddress;
    Ring->ProcessId = ProcessId;
    Ring->UserProcessId = ProcessId;
    Ring->Failed = FALSE;
    InterlockedExchangePointer(&Ring->WaitIrp, 0);
    InterlockedExchange(&Ring->ProducerState, 0);
    InterlockedExchange(&Ring->KickActive, 0);
    KeSetEvent(&Ring->IdleEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&Ring->Lock, Irql);

    SpdIoqSetNonblockingConsumer(StorageUnit->Ioq);

    Params->UserAddress = (UINT64)(UINT_PTR)UserAddress;
    Params->SectionSize = SectionSize;
    Params->Features = 0;
    SectionHandle = 0;
    Mdl = 0;
    UserAddress = 0;
    FreeIds = 0;
    Meta = 0;
    SystemAddress = 0;
    PagesLocked = FALSE;
    Result = STATUS_SUCCESS;

exit:
    if (0 != Mdl)
    {
        if (PagesLocked)
            MmUnlockPages(Mdl);

        IoFreeMdl(Mdl);
        Mdl = 0;
    }
    if (0 != UserAddress)
    {
        ZwUnmapViewOfSection(ZwCurrentProcess(), UserAddress);
        UserAddress = 0;
    }
    if (0 != SectionHandle)
    {
        ZwClose(SectionHandle);
        SectionHandle = 0;
    }
    if (0 != FreeIds)
    {
        SpdFree(FreeIds, SpdTagStorageUnit);
        FreeIds = 0;
    }
    if (0 != Meta)
    {
        SpdFree(Meta, SpdTagStorageUnit);
        Meta = 0;
    }
    return Result;
}

VOID SpdStorageUnitRingClose(
    SPD_STORAGE_UNIT *StorageUnit,
    BOOLEAN UserProcessExiting)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    KIRQL Irql;

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    if (0 == Ring->SectionHandle)
    {
        Ring->Stopping = TRUE;
        KeReleaseSpinLock(&Ring->Lock, Irql);
        return;
    }
    Ring->Stopping = TRUE;
    KeReleaseSpinLock(&Ring->Lock, Irql);

    SpdIoqReset(StorageUnit->Ioq, TRUE,
        SpdIoqResetReasonRingClose);
    SpdStorageUnitRingCancelWait(StorageUnit);
    KeWaitForSingleObject(&Ring->IdleEvent,
        Executive, KernelMode, FALSE, NULL);
    DbgPrint(DRIVER_NAME ": SharedRing close depth=%lu buffers=%lu "
        "sq_full=%ld buffer_starved=%ld waits=%ld/%ld producer_runs=%ld "
        "producer_reruns=%ld produced=%ld max_batch=%ld\n",
        Ring->QueueDepth, Ring->BufferCount,
        Ring->SqFullEvents, Ring->BufferPoolExhaustions,
        Ring->WaitSubmissions, Ring->WaitCompletions,
        Ring->ProducerDpcRuns, Ring->ProducerDpcReruns,
        Ring->ProducerProduced, Ring->ProducerMaxBatch);
    SpdStorageUnitRingReset(StorageUnit, UserProcessExiting);
}

NTSTATUS SpdStorageUnitRingStop(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    KIRQL Irql;

    if (ProcessId != StorageUnit->TransactProcessId)
        return STATUS_ACCESS_DENIED;

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    if (0 == Ring->SectionHandle)
    {
        KeReleaseSpinLock(&Ring->Lock, Irql);
        return STATUS_INVALID_DEVICE_STATE;
    }
    Ring->Stopping = TRUE;
    KeReleaseSpinLock(&Ring->Lock, Irql);

    /* Keep the section mapped while userspace drains and joins its threads. */
    SpdIoqReset(StorageUnit->Ioq, TRUE,
        SpdIoqResetReasonRingStop);
    SpdStorageUnitRingCancelWait(StorageUnit);
    KeWaitForSingleObject(&Ring->IdleEvent,
        Executive, KernelMode, FALSE, NULL);
    return STATUS_SUCCESS;
}

static BOOLEAN SpdStorageUnitRingEnter(SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    KIRQL Irql;
    BOOLEAN Entered = FALSE;

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    if (0 != Ring->SectionHandle && !Ring->Stopping && !Ring->Failed)
    {
        if (0 == Ring->ActiveCalls)
            KeResetEvent(&Ring->IdleEvent);
        Ring->ActiveCalls++;
        Entered = TRUE;
    }
    KeReleaseSpinLock(&Ring->Lock, Irql);
    return Entered;
}

static VOID SpdStorageUnitRingLeave(SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    KIRQL Irql;

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    ASSERT(0 != Ring->ActiveCalls);
    if (0 != --Ring->ActiveCalls)
        KeReleaseSpinLock(&Ring->Lock, Irql);
    else
    {
        KeSetEvent(&Ring->IdleEvent, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&Ring->Lock, Irql);
    }
}

static BOOLEAN SpdRingGetRequestDataLength(
    SPD_STORAGE_UNIT *StorageUnit,
    const SPD_IOCTL_TRANSACT_REQ *Request,
    UINT32 *PLength,
    BOOLEAN *PNeedsBuffer)
{
    UINT64 Length;

    switch (Request->Kind)
    {
    case SpdIoctlTransactReadKind:
        Length = (UINT64)Request->Op.Read.BlockCount *
            StorageUnit->StorageUnitParams.BlockLength;
        *PNeedsBuffer = TRUE;
        break;
    case SpdIoctlTransactWriteKind:
        Length = (UINT64)Request->Op.Write.BlockCount *
            StorageUnit->StorageUnitParams.BlockLength;
        *PNeedsBuffer = TRUE;
        break;
    case SpdIoctlTransactUnmapKind:
        Length = (UINT64)Request->Op.Unmap.Count *
            sizeof(SPD_IOCTL_UNMAP_DESCRIPTOR);
        *PNeedsBuffer = TRUE;
        break;
    case SpdIoctlTransactFlushKind:
        Length = 0;
        *PNeedsBuffer = FALSE;
        break;
    default:
        return FALSE;
    }

    if (Length > MAXULONG ||
        Length > StorageUnit->Ring.BufferSize)
        return FALSE;
    *PLength = (UINT32)Length;
    return TRUE;
}

static VOID SpdRingFreeProducedBuffers(
    SPD_RING_STATE *Ring,
    SPD_RING_REQUEST *RequestRing,
    UINT32 InitialTail,
    UINT32 Produced)
{
    KIRQL Irql;

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    for (UINT32 I = 0; Produced > I; I++)
    {
        SPD_RING_REQUEST *Entry = &RequestRing[
            (InitialTail + I) & (Ring->QueueDepth - 1)];
        if (SPD_RING_NO_BUFFER != Entry->Data.BufferId)
            SpdRingBufferFreeLocked(Ring, Entry->Data.BufferId);
    }
    KeReleaseSpinLock(&Ring->Lock, Irql);
}

static UINT32 SpdRingProduceBatch(
    SPD_STORAGE_UNIT *StorageUnit,
    BOOLEAN *PBatchLimitReached)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_RING_HEADER *Header = (SPD_RING_HEADER *)Ring->SystemAddress;
    SPD_RING_REQUEST *RequestRing = (SPD_RING_REQUEST *)
        ((PUINT8)Ring->SystemAddress + Ring->RequestOffset);
    UINT32 Head = SpdRingLoadAcquire32(&Header->RequestHead.Value);
    UINT32 Tail = Ring->RequestTail;
    UINT32 Used = Tail - Head;
    UINT32 Free;
    UINT32 Budget;
    UINT32 Produced = 0;
    UINT32 InitialTail = Tail;
    BOOLEAN Failed = FALSE;
    BOOLEAN Cancelled = FALSE;
    KIRQL Irql;

    *PBatchLimitReached = FALSE;
    if (Used > Ring->QueueDepth)
    {
        SpdStorageUnitRingMarkFailed(StorageUnit);
        return 0;
    }
    Free = Ring->QueueDepth - Used;
    if (0 == Free)
    {
        InterlockedIncrement(&Ring->SqFullEvents);
        return 0;
    }
    Budget = min(Free, SPD_RING_MAX_PRODUCER_BATCH);

    while (Produced != Budget)
    {
        UINT32 BufferId;
        PVOID DataBuffer;
        SPD_IOCTL_TRANSACT_REQ Request;
        NTSTATUS StartResult;
        UINT32 DataLength;
        BOOLEAN NeedsBuffer;
        SPD_RING_BUFFER_REF Data;
        SPD_RING_REQUEST *Entry;

        KeAcquireSpinLock(&Ring->Lock, &Irql);
        if (Ring->Stopping || Ring->Failed)
        {
            KeReleaseSpinLock(&Ring->Lock, Irql);
            Cancelled = TRUE;
            break;
        }
        if (!SpdRingBufferAllocLocked(Ring, &BufferId))
        {
            InterlockedIncrement(&Ring->BufferPoolExhaustions);
            KeReleaseSpinLock(&Ring->Lock, Irql);
            break;
        }
        KeReleaseSpinLock(&Ring->Lock, Irql);

        DataBuffer = SpdRingBufferAddress(Ring, BufferId, 0,
            Ring->BufferSize);
        if (0 == DataBuffer)
        {
            KeAcquireSpinLock(&Ring->Lock, &Irql);
            SpdRingBufferFreeLocked(Ring, BufferId);
            KeReleaseSpinLock(&Ring->Lock, Irql);
            Failed = TRUE;
            break;
        }

        RtlZeroMemory(&Request, sizeof Request);
        StartResult = SpdIoqTryStartProcessingSrb(StorageUnit->Ioq,
            SpdSrbExecuteScsiPrepare, &Request, DataBuffer);
        if (STATUS_NOT_FOUND == StartResult)
        {
            KeAcquireSpinLock(&Ring->Lock, &Irql);
            SpdRingBufferFreeLocked(Ring, BufferId);
            KeReleaseSpinLock(&Ring->Lock, Irql);
            break;
        }
        if (!NT_SUCCESS(StartResult))
        {
            KeAcquireSpinLock(&Ring->Lock, &Irql);
            SpdRingBufferFreeLocked(Ring, BufferId);
            if (STATUS_CANCELLED == StartResult)
                Ring->Stopping = TRUE;
            KeReleaseSpinLock(&Ring->Lock, Irql);
            Cancelled = STATUS_CANCELLED == StartResult;
            Failed = !Cancelled;
            break;
        }

        if (0 == Request.Hint ||
            !SpdRingGetRequestDataLength(StorageUnit, &Request,
                &DataLength, &NeedsBuffer))
        {
            KeAcquireSpinLock(&Ring->Lock, &Irql);
            SpdRingBufferFreeLocked(Ring, BufferId);
            KeReleaseSpinLock(&Ring->Lock, Irql);
            Failed = TRUE;
            break;
        }

        if (NeedsBuffer)
        {
            Data.BufferId = BufferId;
            Data.Offset = 0;
            Data.Length = DataLength;
            Data.Flags = SPD_RING_BUFFER_FLAG_NONE;
            KeAcquireSpinLock(&Ring->Lock, &Irql);
            ASSERT(Ring->Buffers.Meta[BufferId].Allocated);
            Ring->Buffers.Meta[BufferId].OwnerHint = Request.Hint;
            Ring->Buffers.Meta[BufferId].Length = DataLength;
            Ring->Buffers.Meta[BufferId].Kind = Request.Kind;
            KeReleaseSpinLock(&Ring->Lock, Irql);
        }
        else
        {
            /* FLUSH is bufferless but still follows the normal reservation
             * path so every SRB starts from the same resource check. */
            KeAcquireSpinLock(&Ring->Lock, &Irql);
            SpdRingBufferFreeLocked(Ring, BufferId);
            KeReleaseSpinLock(&Ring->Lock, Irql);
            Data.BufferId = SPD_RING_NO_BUFFER;
            Data.Offset = 0;
            Data.Length = 0;
            Data.Flags = SPD_RING_BUFFER_FLAG_NONE;
        }

        Entry = &RequestRing[Tail & (Ring->QueueDepth - 1)];
        Entry->Request = Request;
        Entry->Data = Data;
        Tail++;
        Produced++;
    }

    if (Failed)
    {
        SpdRingFreeProducedBuffers(Ring, RequestRing,
            InitialTail, Produced);
        SpdStorageUnitRingMarkFailed(StorageUnit);
        return 0;
    }
    if (Cancelled)
    {
        SpdRingFreeProducedBuffers(Ring, RequestRing,
            InitialTail, Produced);
        SpdIoqReset(StorageUnit->Ioq, TRUE,
            SpdIoqResetReasonRingStop);
        SpdStorageUnitRingCancelWait(StorageUnit);
        return 0;
    }

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    if (Ring->Stopping || Ring->Failed)
    {
        KeReleaseSpinLock(&Ring->Lock, Irql);
        SpdRingFreeProducedBuffers(Ring, RequestRing,
            InitialTail, Produced);
        return 0;
    }
    if (0 != Produced)
    {
        Ring->RequestTail = Tail;
        SpdRingStoreRelease32(&Header->RequestTail.Value, Tail);
    }
    KeReleaseSpinLock(&Ring->Lock, Irql);

    if (0 != Produced)
    {
        InterlockedExchangeAdd(&Ring->ProducerProduced, (LONG)Produced);
        for (;;)
        {
            LONG MaxBatch = InterlockedCompareExchange(
                &Ring->ProducerMaxBatch, 0, 0);
            if ((LONG)Produced <= MaxBatch ||
                MaxBatch == InterlockedCompareExchange(
                    &Ring->ProducerMaxBatch, (LONG)Produced, MaxBatch))
                break;
        }
    }

    if (Produced == Budget && Budget == SPD_RING_MAX_PRODUCER_BATCH &&
        Free > Budget)
        *PBatchLimitReached = TRUE;

    return Produced;
}

static VOID SpdStorageUnitRingCompleteWait(
    SPD_STORAGE_UNIT *StorageUnit,
    NTSTATUS Status,
    BOOLEAN RequireRequest)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_DEVICE_EXTENSION *DeviceExtension =
        StorageUnit->Ioq->DeviceExtension;
    KIRQL Irql;
    BOOLEAN Stopping;

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    Stopping = Ring->Stopping || Ring->Failed;
    KeReleaseSpinLock(&Ring->Lock, Irql);
    if (Stopping)
        Status = STATUS_CANCELLED;
    else if (RequireRequest)
    {
        SPD_RING_HEADER *Header = (SPD_RING_HEADER *)Ring->SystemAddress;
        UINT32 Head = SpdRingLoadAcquire32(&Header->RequestHead.Value);
        UINT32 Tail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
        UINT32 Available = Tail - Head;
        if (Available > Ring->QueueDepth)
        {
            SpdStorageUnitRingMarkFailed(StorageUnit);
            Status = STATUS_INVALID_PARAMETER;
        }
        else if (0 == Available)
            return;
        else
            Status = STATUS_SUCCESS;
    }

    PIRP Irp = InterlockedExchangePointer(&Ring->WaitIrp, 0);
    if (0 == Irp)
        return;

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = NT_SUCCESS(Status) ?
        sizeof(SPD_IOCTL_RING_WAIT_PARAMS) : 0;
    InterlockedIncrement(&Ring->WaitCompletions);
    StorPortCompleteServiceIrp(DeviceExtension, Irp);
    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
}

static VOID SpdStorageUnitRingCancelWait(SPD_STORAGE_UNIT *StorageUnit)
{
    SpdStorageUnitRingCompleteWait(StorageUnit, STATUS_CANCELLED, FALSE);
}

static VOID SpdStorageUnitRingProducerDpc(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    SPD_STORAGE_UNIT *StorageUnit = DeferredContext;
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_DEVICE_EXTENSION *DeviceExtension =
        StorageUnit->Ioq->DeviceExtension;

    ASSERT(DISPATCH_LEVEL == KeGetCurrentIrql());
    InterlockedIncrement(&Ring->ProducerDpcRuns);

    BOOLEAN BatchLimitReached = FALSE;
    KIRQL Irql;
    BOOLEAN Stopping;

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    Stopping = Ring->Stopping || Ring->Failed;
    KeReleaseSpinLock(&Ring->Lock, Irql);

    if (!Stopping)
        SpdRingProduceBatch(StorageUnit, &BatchLimitReached);
    SpdStorageUnitRingCompleteWait(
        StorageUnit, STATUS_SUCCESS, TRUE);

    if (Stopping)
    {
        InterlockedExchange(&Ring->ProducerState, 0);
        goto exit;
    }

    if (BatchLimitReached &&
        1 == InterlockedCompareExchange(&Ring->ProducerState, 2, 1))
        InterlockedIncrement(&Ring->ProducerDpcReruns);

    for (;;)
    {
        LONG State = InterlockedCompareExchange(
            &Ring->ProducerState, 0, 1);
        if (1 == State)
            goto exit;
        if (2 == State)
        {
            if (2 != InterlockedCompareExchange(
                    &Ring->ProducerState, 1, 2))
                continue;

            /* Transfer this execution's lifetime reference to the queued
             * continuation. FALSE means another queued invocation already
             * owns its own reference; release this invocation's reference. */
            if (KeInsertQueueDpc(&Ring->ProducerDpc, 0, 0))
                return;
            goto exit;
        }

        /* A scheduler observed the idle transition and queued a fresh DPC. */
        goto exit;
    }

exit:
    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
}

VOID SpdStorageUnitRingScheduleProducer(SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_DEVICE_EXTENSION *DeviceExtension =
        StorageUnit->Ioq->DeviceExtension;
    SPD_STORAGE_UNIT *Reference;

    Reference = SpdStorageUnitReferenceByBtl(DeviceExtension,
        StorageUnit->Btl);
    if (Reference != StorageUnit)
    {
        if (0 != Reference)
            SpdStorageUnitDereference(DeviceExtension, Reference);
        return;
    }
    if (!SpdStorageUnitRingEnter(StorageUnit))
    {
        SpdStorageUnitDereference(DeviceExtension, StorageUnit);
        return;
    }

    for (;;)
    {
        LONG State = InterlockedCompareExchange(
            &Ring->ProducerState, 1, 0);
        if (0 == State)
        {
            if (KeInsertQueueDpc(&Ring->ProducerDpc, 0, 0))
                return;

            /* FALSE means this KDPC is already queued. That queued/running
             * instance owns its own rundown reference; request another pass
             * and release this scheduler's temporary references. */
            if (1 == InterlockedCompareExchange(
                    &Ring->ProducerState, 2, 1))
                InterlockedIncrement(&Ring->ProducerDpcReruns);
            SpdStorageUnitRingLeave(StorageUnit);
            SpdStorageUnitDereference(DeviceExtension, StorageUnit);
            return;
        }
        if (1 == State)
        {
            if (1 == InterlockedCompareExchange(
                    &Ring->ProducerState, 2, 1))
            {
                InterlockedIncrement(&Ring->ProducerDpcReruns);
                SpdStorageUnitRingLeave(StorageUnit);
                SpdStorageUnitDereference(DeviceExtension, StorageUnit);
                return;
            }
            continue;
        }

        SpdStorageUnitRingLeave(StorageUnit);
        SpdStorageUnitDereference(DeviceExtension, StorageUnit);
        return;
    }
}

SPD_SERVICE_IRP_DISPOSITION SpdStorageUnitRingWait(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId,
    SPD_IOCTL_RING_WAIT_PARAMS *Params,
    PIRP Irp)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_DEVICE_EXTENSION *DeviceExtension =
        StorageUnit->Ioq->DeviceExtension;
    KIRQL Irql;

    Irp->IoStatus.Information = 0;

    if (ProcessId != StorageUnit->TransactProcessId)
    {
        Irp->IoStatus.Status = STATUS_ACCESS_DENIED;
        return SpdServiceIrpCompleteNow;
    }
    for (UINT32 I = 0; ARRAYSIZE(Params->Reserved) > I; I++)
        if (0 != Params->Reserved[I])
        {
            Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
            return SpdServiceIrpCompleteNow;
        }

    /* Protect the handler's short registration tail separately from the
     * reference transferred to the retained WAIT IRP. */
    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    StorageUnit->RefCount++;
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);

    if (!SpdStorageUnitRingEnter(StorageUnit))
    {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;
        SpdStorageUnitDereference(DeviceExtension, StorageUnit);
        return SpdServiceIrpCompleteNow;
    }
    if (!SpdStorageUnitRingEnter(StorageUnit))
    {
        SpdStorageUnitRingLeave(StorageUnit);
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;
        SpdStorageUnitDereference(DeviceExtension, StorageUnit);
        return SpdServiceIrpCompleteNow;
    }

    if (0 != InterlockedCompareExchangePointer(
            &Ring->WaitIrp, Irp, 0))
    {
        SpdStorageUnitRingLeave(StorageUnit);
        SpdStorageUnitRingLeave(StorageUnit);
        Irp->IoStatus.Status = STATUS_DEVICE_BUSY;
        SpdStorageUnitDereference(DeviceExtension, StorageUnit);
        return SpdServiceIrpCompleteNow;
    }

    InterlockedIncrement(&Ring->WaitSubmissions);
    SpdStorageUnitRingScheduleProducer(StorageUnit);

    /* STOP may have won after RingEnter but before waiter installation. */
    KeAcquireSpinLock(&Ring->Lock, &Irql);
    BOOLEAN Stopping = Ring->Stopping || Ring->Failed;
    KeReleaseSpinLock(&Ring->Lock, Irql);
    if (Stopping)
        SpdStorageUnitRingCancelWait(StorageUnit);

    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
    return SpdServiceIrpDeferred;
}

VOID SpdStorageUnitRingStopForRemoval(SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    KIRQL Irql;

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    Ring->Stopping = TRUE;
    KeReleaseSpinLock(&Ring->Lock, Irql);

    SpdIoqReset(StorageUnit->Ioq, TRUE,
        SpdIoqResetReasonRemoval);
    SpdStorageUnitRingCancelWait(StorageUnit);
    KeWaitForSingleObject(&Ring->IdleEvent,
        Executive, KernelMode, FALSE, NULL);
}

NTSTATUS SpdStorageUnitRingKick(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId,
    SPD_IOCTL_RING_KICK_PARAMS *Params)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_RING_HEADER *Header;
    SPD_RING_COMPLETION *CompletionRing;
    UINT32 Head;
    UINT32 Tail;
    UINT32 Count;
    UINT32 Consumed = 0;
    NTSTATUS Result = STATUS_SUCCESS;
    BOOLEAN Failed = FALSE;
    KIRQL Irql;

    Params->Consumed = 0;
    if (ProcessId != StorageUnit->TransactProcessId)
        return STATUS_ACCESS_DENIED;
    if (!SpdStorageUnitRingEnter(StorageUnit))
        return STATUS_INVALID_DEVICE_STATE;

    if (0 != InterlockedCompareExchange(&Ring->KickActive, 1, 0))
    {
        SpdStorageUnitRingLeave(StorageUnit);
        return STATUS_DEVICE_BUSY;
    }

    Header = (SPD_RING_HEADER *)Ring->SystemAddress;
    CompletionRing = (SPD_RING_COMPLETION *)
        ((PUINT8)Ring->SystemAddress + Ring->CompletionOffset);
    Head = Ring->CompletionHead;
    Tail = SpdRingLoadAcquire32(&Header->CompletionTail.Value);
    Count = Tail - Head;
    if (Count > Ring->QueueDepth)
    {
        Failed = TRUE;
        Result = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    while (Head != Tail)
    {
        SPD_RING_COMPLETION Completion =
            CompletionRing[Head & (Ring->QueueDepth - 1)];
        SPD_IOCTL_TRANSACT_RSP *Response = &Completion.Response;
        SPD_RING_BUFFER_REF *Data = &Completion.Data;
        PVOID DataBuffer = 0;
        UINT32 BufferId = Data->BufferId;

        if (SPD_RING_NO_BUFFER != BufferId)
        {
            SPD_RING_BUFFER_META Meta;

            if (BufferId >= Ring->BufferCount ||
                0 != Data->Offset ||
                SPD_RING_BUFFER_FLAG_NONE != Data->Flags ||
                Data->Length > Ring->BufferSize)
            {
                Failed = TRUE;
                Result = STATUS_INVALID_PARAMETER;
                break;
            }

            KeAcquireSpinLock(&Ring->Lock, &Irql);
            Meta = Ring->Buffers.Meta[BufferId];
            KeReleaseSpinLock(&Ring->Lock, Irql);
            if (!Meta.Allocated ||
                Meta.OwnerHint != Response->Hint ||
                Meta.Kind != Response->Kind ||
                Meta.Length != Data->Length)
            {
                Failed = TRUE;
                Result = STATUS_INVALID_PARAMETER;
                break;
            }

            DataBuffer = SpdRingBufferAddress(Ring, BufferId,
                Data->Offset, Data->Length);
            if (0 == DataBuffer)
            {
                Failed = TRUE;
                Result = STATUS_INVALID_PARAMETER;
                break;
            }
        }
        else if (0 != Data->Offset || 0 != Data->Length ||
            SPD_RING_BUFFER_FLAG_NONE != Data->Flags)
        {
            Failed = TRUE;
            Result = STATUS_INVALID_PARAMETER;
            break;
        }
        else if (SpdIoctlTransactFlushKind != Response->Kind)
        {
            Failed = TRUE;
            Result = STATUS_INVALID_PARAMETER;
            break;
        }

        /* Hint remains the opaque canonical IOQ correlation value. */
        Result = SpdIoqEndProcessingSrb(StorageUnit->Ioq,
            Response->Hint, SpdSrbExecuteScsiComplete,
            Response, DataBuffer);
        if (!NT_SUCCESS(Result))
        {
            Failed = TRUE;
            break;
        }

        if (SPD_RING_NO_BUFFER != BufferId)
        {
            KeAcquireSpinLock(&Ring->Lock, &Irql);
            SpdRingBufferFreeLocked(Ring, BufferId);
            KeReleaseSpinLock(&Ring->Lock, Irql);
        }
        Head++;
        Consumed++;
    }

exit:
    if (0 != Consumed)
    {
        Ring->CompletionHead = Head;
        SpdRingStoreRelease32(&Header->CompletionHead.Value, Head);
    }
    Params->Consumed = Consumed;
    if (Failed)
    {
        KeAcquireSpinLock(&Ring->Lock, &Irql);
        Ring->Failed = TRUE;
        Ring->Stopping = TRUE;
        KeReleaseSpinLock(&Ring->Lock, Irql);
    }
    InterlockedExchange(&Ring->KickActive, 0);
    if (Failed)
    {
        SpdIoqReset(StorageUnit->Ioq, TRUE,
            SpdIoqResetReasonRingFailure);
        SpdStorageUnitRingCancelWait(StorageUnit);
    }
    else
        SpdStorageUnitRingScheduleProducer(StorageUnit);
    SpdStorageUnitRingLeave(StorageUnit);
    return Result;
}
NTSTATUS SpdDeviceExtensionInit(SPD_DEVICE_EXTENSION *DeviceExtension, PVOID BusInformation)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(0 != DeviceExtension);

    NTSTATUS Result;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&SpdGlobalDeviceResource, TRUE);

    if (0 != SpdGlobalDeviceExtension)
    {
        Result = DeviceExtension == SpdGlobalDeviceExtension ?
            STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
        goto exit;
    }

    Result = PsSetCreateProcessNotifyRoutine(SpdDeviceExtensionNotifyRoutine, FALSE);
    if (!NT_SUCCESS(Result))
        goto exit;

    KeInitializeSpinLock(&DeviceExtension->SpinLock);
    DeviceExtension->DeviceObject = BusInformation;
    DeviceExtension->StorageUnitCapacity = SpdStorageUnitCapacity;
    SpdGlobalDeviceExtension = DeviceExtension;

    Result = STATUS_SUCCESS;

exit:
    ExReleaseResourceLite(&SpdGlobalDeviceResource);
    KeLeaveCriticalRegion();

    return Result;
}

VOID SpdDeviceExtensionFini(SPD_DEVICE_EXTENSION *DeviceExtension)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(0 != DeviceExtension);

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&SpdGlobalDeviceResource, TRUE);

    if (DeviceExtension == SpdGlobalDeviceExtension)
    {
        PsSetCreateProcessNotifyRoutine(SpdDeviceExtensionNotifyRoutine, TRUE);
        SpdGlobalDeviceExtension = 0;
    }

    ExReleaseResourceLite(&SpdGlobalDeviceResource);
    KeLeaveCriticalRegion();
}

static VOID SpdDeviceExtensionNotifyRoutine(HANDLE ParentId, HANDLE ProcessId0, BOOLEAN Create)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());

    if (Create)
        return;

    ULONG ProcessId = (ULONG)(UINT_PTR)ProcessId0;
    UINT8 Bitmap[32];
    ULONG Count;

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&SpdGlobalDeviceResource, TRUE);

    ASSERT(0 != SpdGlobalDeviceExtension);

    Count = SpdStorageUnitGetUseBitmap(SpdGlobalDeviceExtension, &ProcessId, Bitmap);

    for (ULONG I = 0; 0 < Count && sizeof Bitmap * 8 > I; I++)
        if (FlagOn(Bitmap[I >> 3], 1 << (I & 7)))
        {
            SpdStorageUnitUnprovision(SpdGlobalDeviceExtension, 0, I, ProcessId);
            Count--;
        }

    ExReleaseResourceLite(&SpdGlobalDeviceResource);
    KeLeaveCriticalRegion();
}

SPD_DEVICE_EXTENSION *SpdDeviceExtensionAcquire(VOID)
{
    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&SpdGlobalDeviceResource, TRUE);
    if (0 != SpdGlobalDeviceExtension)
        return SpdGlobalDeviceExtension;

    ExReleaseResourceLite(&SpdGlobalDeviceResource);
    KeLeaveCriticalRegion();
    return 0;
}

VOID SpdDeviceExtensionRelease(SPD_DEVICE_EXTENSION *DeviceExtension)
{
    ExReleaseResourceLite(&SpdGlobalDeviceResource);
    KeLeaveCriticalRegion();
}

NTSTATUS SpdStorageUnitProvision(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    SPD_IOCTL_STORAGE_UNIT_PARAMS *StorageUnitParams,
    ULONG ProcessId,
    PUINT32 PBtl)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(0 != DeviceExtension);

    NTSTATUS Result;
    CHAR SerialNumber[RTL_FIELD_SIZE(SPD_STORAGE_UNIT, SerialNumber) + 1];
    SPD_STORAGE_UNIT *StorageUnit = 0;
    SPD_STORAGE_UNIT *DuplicateUnit;
    UINT32 Btl;
    KIRQL Irql;

    *PBtl = (UINT32)-1;

    StorageUnit = SpdAllocNonPaged(sizeof *StorageUnit, SpdTagStorageUnit);
    if (0 == StorageUnit)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    RtlZeroMemory(StorageUnit, sizeof *StorageUnit);
    KeInitializeSpinLock(&StorageUnit->Ring.Lock);
    KeInitializeDpc(&StorageUnit->Ring.ProducerDpc,
        SpdStorageUnitRingProducerDpc, StorageUnit);
    KeInitializeEvent(&StorageUnit->Ring.IdleEvent,
        NotificationEvent, TRUE);
    StorageUnit->RefCount = 1;
    StorageUnit->Btl = (UINT32)-1;
    RtlCopyMemory(&StorageUnit->StorageUnitParams, StorageUnitParams,
        sizeof *StorageUnitParams);
    /* "left align" ProductId except that we allow all-NUL for testing */
    if ('\0' != StorageUnit->StorageUnitParams.ProductId[0])
        for (UCHAR *P = StorageUnit->StorageUnitParams.ProductId,
            *EndP = P + sizeof StorageUnit->StorageUnitParams.ProductId,
            Spaces = FALSE;
            EndP > P; P++)
        {
            if (Spaces || ' ' > *P || *P >= 0x7f)
            {
                *P = ' ';
                Spaces = TRUE;
            }
        }
    /* "left align" ProductRevisionLevel except that we allow all-NUL for testing */
    if ('\0' != StorageUnit->StorageUnitParams.ProductRevisionLevel[0])
        for (UCHAR *P = StorageUnit->StorageUnitParams.ProductRevisionLevel,
            *EndP = P + sizeof StorageUnit->StorageUnitParams.ProductRevisionLevel,
            Spaces = FALSE;
            EndP > P; P++)
        {
            if (Spaces || ' ' > *P || *P >= 0x7f)
            {
                *P = ' ';
                Spaces = TRUE;
            }
        }
#define Guid                            StorageUnit->StorageUnitParams.Guid
    RtlStringCbPrintfA(SerialNumber, sizeof SerialNumber,
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        Guid.Data1, Guid.Data2, Guid.Data3,
        Guid.Data4[0], Guid.Data4[1], Guid.Data4[2], Guid.Data4[3],
        Guid.Data4[4], Guid.Data4[5], Guid.Data4[6], Guid.Data4[7]);
#undef Guid
    RtlCopyMemory(StorageUnit->SerialNumber, SerialNumber, sizeof StorageUnit->SerialNumber);
    StorageUnit->OwnerProcessId = ProcessId;
    StorageUnit->TransactProcessId = ProcessId;

    Result = SpdIoqCreate(DeviceExtension, &StorageUnit->Ioq);
    if (!NT_SUCCESS(Result))
        goto exit;

    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    DuplicateUnit = 0;
    Btl = (UINT32)-1;
    for (ULONG I = 0; DeviceExtension->StorageUnitCapacity > I; I++)
    {
        SPD_STORAGE_UNIT *Unit = DeviceExtension->StorageUnits[I];
        if (0 == Unit)
        {
            if ((UINT32)-1 == Btl)
                Btl = SPD_BTL_FROM_INDEX(I);
            continue;
        }

        if (RtlEqualMemory(&StorageUnit->StorageUnitParams.Guid, &Unit->StorageUnitParams.Guid,
            sizeof Unit->StorageUnitParams.Guid))
        {
            DuplicateUnit = Unit;
            break;
        }
    }
    if (0 == DuplicateUnit && -1 != Btl)
    {
        StorageUnit->Btl = Btl;
        DeviceExtension->StorageUnits[SPD_INDEX_FROM_BTL(Btl)] = StorageUnit;
        DeviceExtension->StorageUnitCount++;
    }
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);

    if (0 != DuplicateUnit)
    {
        Result = STATUS_OBJECT_NAME_COLLISION;
        goto exit;
    }
    if (-1 == Btl)
    {
        Result = STATUS_CANNOT_MAKE;
        goto exit;
    }

    StorPortNotification(BusChangeDetected, DeviceExtension, (UCHAR)0);

    *PBtl = Btl;
    Result = STATUS_SUCCESS;

exit:
    if (!NT_SUCCESS(Result))
    {
        if (0 != StorageUnit->Ioq)
            SpdIoqDelete(StorageUnit->Ioq);

        if (0 != StorageUnit)
            SpdFree(StorageUnit, SpdTagStorageUnit);
    }

    return Result;
}

NTSTATUS SpdStorageUnitUnprovision(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    PGUID Guid, ULONG Index,
    ULONG ProcessId)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(0 != DeviceExtension);

    NTSTATUS Result;
    SPD_STORAGE_UNIT *StorageUnit;
    KIRQL Irql;

    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    StorageUnit = 0;
    if (0 != Guid)
    {
        for (ULONG I = 0; DeviceExtension->StorageUnitCapacity > I; I++)
        {
            SPD_STORAGE_UNIT *Unit = DeviceExtension->StorageUnits[I];
            if (0 == Unit)
                continue;

            if (RtlEqualMemory(Guid, &Unit->StorageUnitParams.Guid,
                sizeof Unit->StorageUnitParams.Guid))
            {
                StorageUnit = Unit;
                Index = I;
                break;
            }
        }
    }
    else
    {
        if (DeviceExtension->StorageUnitCapacity > Index)
            StorageUnit = DeviceExtension->StorageUnits[Index];
    }
    if (0 != StorageUnit && ProcessId == StorageUnit->OwnerProcessId)
    {
        DeviceExtension->StorageUnitCount--;
        DeviceExtension->StorageUnits[Index] = 0;
    }
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);

    if (0 == StorageUnit)
    {
        Result = STATUS_OBJECT_NAME_NOT_FOUND;
        goto exit;
    }
    if (ProcessId != StorageUnit->OwnerProcessId)
    {
        Result = STATUS_ACCESS_DENIED;
        goto exit;
    }

    /* stop the ioq before reclaiming ring request state and mappings */
    SpdIoqReset(StorageUnit->Ioq, TRUE,
        SpdIoqResetReasonRemoval);
    SpdStorageUnitRingClose(StorageUnit,
        0 != StorageUnit->Ring.SectionHandle &&
        StorageUnit->Ring.UserProcessId !=
            (ULONG)(ULONG_PTR)PsGetCurrentProcessId());
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);

    StorPortNotification(BusChangeDetected, DeviceExtension, (UCHAR)0);

    Result = STATUS_SUCCESS;

exit:
    return Result;
}

SPD_STORAGE_UNIT *SpdStorageUnitReferenceByBtl(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    UINT32 Btl)
{
    SPD_STORAGE_UNIT *StorageUnit;
    UINT8 B, T, L;
    KIRQL Irql;

    B = SPD_IOCTL_BTL_B(Btl);
    T = SPD_IOCTL_BTL_T(Btl);
    L = SPD_IOCTL_BTL_L(Btl);

    if (0 != B || 0 != L)
        return 0;

    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    StorageUnit = DeviceExtension->StorageUnitCapacity > T ? DeviceExtension->StorageUnits[T] : 0;
    if (0 != StorageUnit)
        StorageUnit->RefCount++;
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);

    return StorageUnit;
}

static SPD_STORAGE_UNIT *SpdStorageUnitReferenceByDevice(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    PDEVICE_OBJECT DeviceObject)
{
    SPD_STORAGE_UNIT *StorageUnit;
    KIRQL Irql;

    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    StorageUnit = 0;
    for (ULONG I = 0; DeviceExtension->StorageUnitCapacity > I; I++)
    {
        SPD_STORAGE_UNIT *Unit = DeviceExtension->StorageUnits[I];
        if (0 == Unit)
            continue;

        if (DeviceObject == Unit->DeviceObject)
        {
            StorageUnit = Unit;
            break;
        }
    }
    if (0 != StorageUnit)
        StorageUnit->RefCount++;
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);

    return StorageUnit;
}

VOID SpdStorageUnitDereference(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    SPD_STORAGE_UNIT *StorageUnit)
{
    BOOLEAN Delete;
    KIRQL Irql;

    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    StorageUnit->RefCount--;
    Delete = 0 == StorageUnit->RefCount;
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);

    if (Delete)
    {
        SpdIoqDelete(StorageUnit->Ioq);
        SpdFree(StorageUnit, SpdTagStorageUnit);
    }
}

ULONG SpdStorageUnitGetUseBitmap(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    PULONG PProcessId,
    UINT8 Bitmap[32])
{
    ULONG Count = 0;
    KIRQL Irql;

    RtlZeroMemory(Bitmap, 32);

    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    for (ULONG I = 0;
        DeviceExtension->StorageUnitCount > Count && DeviceExtension->StorageUnitCapacity > I;
        I++)
    {
        SPD_STORAGE_UNIT *Unit = DeviceExtension->StorageUnits[I];
        if (0 != Unit && (0 == PProcessId || *PProcessId == Unit->OwnerProcessId))
        {
            SetFlag(Bitmap[I >> 3], 1 << (I & 7));
            Count++;
        }
    }
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);

    return Count;
}

NTSTATUS SpdStorageUnitGlobalSetDevice(
    PDEVICE_OBJECT DeviceObject)
{
    SPD_STORAGE_UNIT *StorageUnit;
    SCSI_ADDRESS ScsiAddress;
    NTSTATUS Result;

    Result = SpdGetScsiAddress(DeviceObject, &ScsiAddress);
    if (!NT_SUCCESS(Result))
        return Result;

    Result = STATUS_OBJECT_NAME_NOT_FOUND;
    if (0 != SpdDeviceExtensionAcquire())
    {
        StorageUnit = SpdStorageUnitReferenceByBtl(SpdGlobalDeviceExtension,
            SPD_IOCTL_BTL(ScsiAddress.PathId, ScsiAddress.TargetId, ScsiAddress.Lun));
        if (0 != StorageUnit)
        {
            Result = 0 == InterlockedCompareExchangePointer(&StorageUnit->DeviceObject, DeviceObject, 0) ?
                STATUS_SUCCESS : STATUS_OBJECT_NAME_COLLISION;
            SpdStorageUnitDereference(SpdGlobalDeviceExtension, StorageUnit);
        }
        SpdDeviceExtensionRelease(SpdGlobalDeviceExtension);
    }

    return Result;
}

SPD_STORAGE_UNIT *SpdStorageUnitGlobalReferenceByDevice(
    PDEVICE_OBJECT DeviceObject)
{
    SPD_STORAGE_UNIT *StorageUnit = 0;

    if (0 != SpdDeviceExtensionAcquire())
    {
        StorageUnit = SpdStorageUnitReferenceByDevice(SpdGlobalDeviceExtension,
            DeviceObject);
        if (0 == StorageUnit)
            SpdDeviceExtensionRelease(SpdGlobalDeviceExtension);
    }

    return StorageUnit;
}

VOID SpdStorageUnitGlobalDereference(
    SPD_STORAGE_UNIT *StorageUnit)
{
    SpdStorageUnitDereference(SpdGlobalDeviceExtension, StorageUnit);
    SpdDeviceExtensionRelease(SpdGlobalDeviceExtension);
}
