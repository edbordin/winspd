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

static VOID SpdDeviceExtensionNotifyRoutine(HANDLE ParentId, HANDLE ProcessId0, BOOLEAN Create);
static VOID SpdStorageUnitRingProducerWorker(
    PVOID HwDeviceExtension,
    PVOID Context,
    PVOID Worker);
static BOOLEAN SpdStorageUnitRingEnter(SPD_STORAGE_UNIT *StorageUnit);
static VOID SpdStorageUnitRingLeave(SPD_STORAGE_UNIT *StorageUnit);
static VOID SpdStorageUnitRingCompleteWait(
    SPD_STORAGE_UNIT *StorageUnit,
    NTSTATUS Status,
    BOOLEAN RequireRequest);
static VOID SpdStorageUnitRingCancelWait(SPD_STORAGE_UNIT *StorageUnit);

#if defined(WINSPD_TEST_BUILD)
static PKEVENT SpdRingTestOpenEvent(PCWSTR Name)
{
    UNICODE_STRING NameString;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle = 0;
    PKEVENT Event = 0;
    NTSTATUS Status;

    RtlInitUnicodeString(&NameString, Name);
    InitializeObjectAttributes(&ObjectAttributes, &NameString,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, 0, 0);
    Status = ZwOpenEvent(&Handle, EVENT_MODIFY_STATE, &ObjectAttributes);
    if (NT_SUCCESS(Status))
    {
        Status = ObReferenceObjectByHandle(Handle, EVENT_MODIFY_STATE,
            *ExEventObjectType, KernelMode, (PVOID *)&Event, 0);
        ZwClose(Handle);
        if (!NT_SUCCESS(Status))
            Event = 0;
    }
    return Event;
}

VOID SpdStorageUnitRingTestIoqPostBarrier(
    SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    PKEVENT EnteredEvent;
    PKEVENT ReleaseEvent;

    /* Ring close releases these event references after active-call rundown. */
    if (!SpdStorageUnitRingEnter(StorageUnit))
        return;

    EnteredEvent = Ring->TestIoqPostEnteredEvent;
    ReleaseEvent = Ring->TestIoqPostReleaseEvent;
    if (0 == EnteredEvent || 0 == ReleaseEvent)
    {
        SpdStorageUnitRingLeave(StorageUnit);
        return;
    }

    InterlockedExchange(&Ring->TestIoqPostBarrierActive, 1);
    KeSetEvent(EnteredEvent, IO_NO_INCREMENT, FALSE);
    while (0 == KeReadStateEvent(ReleaseEvent))
        KeStallExecutionProcessor(50);
    InterlockedExchange(&Ring->TestIoqPostBarrierActive, 0);

    SpdStorageUnitRingLeave(StorageUnit);
}

VOID SpdStorageUnitRingTestIoqResetComplete(
    SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;

    if (0 != InterlockedCompareExchange(
            &Ring->TestIoqPostBarrierActive, 0, 0) &&
        0 != Ring->TestIoqPostResetDoneEvent)
        KeSetEvent(Ring->TestIoqPostResetDoneEvent,
            IO_NO_INCREMENT, FALSE);
}
#endif

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
    if (0 != Ring->ProducerClaims)
        SpdFree(Ring->ProducerClaims, SpdTagStorageUnit);
    if (0 != Ring->ProducerAbortedSrbs)
        SpdFree(Ring->ProducerAbortedSrbs, SpdTagStorageUnit);
#if defined(WINSPD_TEST_BUILD)
    if (0 != Ring->TestIoqPostEnteredEvent)
        ObDereferenceObject(Ring->TestIoqPostEnteredEvent);
    if (0 != Ring->TestIoqPostReleaseEvent)
        ObDereferenceObject(Ring->TestIoqPostReleaseEvent);
    if (0 != Ring->TestIoqPostResetDoneEvent)
        ObDereferenceObject(Ring->TestIoqPostResetDoneEvent);
    if (0 != Ring->TestProducerPrepareEnteredEvent)
        ObDereferenceObject(Ring->TestProducerPrepareEnteredEvent);
    if (0 != Ring->TestProducerPrepareReleaseEvent)
        ObDereferenceObject(Ring->TestProducerPrepareReleaseEvent);
    if (0 != Ring->TestCompletionCopyEnteredEvent)
        ObDereferenceObject(Ring->TestCompletionCopyEnteredEvent);
    if (0 != Ring->TestCompletionCopyReleaseEvent)
        ObDereferenceObject(Ring->TestCompletionCopyReleaseEvent);
    Ring->TestIoqPostEnteredEvent = 0;
    Ring->TestIoqPostReleaseEvent = 0;
    Ring->TestIoqPostResetDoneEvent = 0;
    Ring->TestProducerPrepareEnteredEvent = 0;
    Ring->TestProducerPrepareReleaseEvent = 0;
    Ring->TestCompletionCopyEnteredEvent = 0;
    Ring->TestCompletionCopyReleaseEvent = 0;
    InterlockedExchange(&Ring->TestIoqPostBarrierActive, 0);
#endif
    Ring->Buffers.FreeIds = 0;
    Ring->Buffers.Meta = 0;
    Ring->ProducerClaims = 0;
    Ring->ProducerAbortedSrbs = 0;
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
    InterlockedExchange(&Ring->ProducerState, SPD_RING_PRODUCER_IDLE);
    InterlockedExchange(&Ring->KickActive, 0);
    /* Close is terminal for this storage unit. */
    Ring->Stopping = TRUE;
}

static VOID SpdStorageUnitRingFreeWorker(
    PVOID DeviceExtension, PVOID Worker, const char *Operation)
{
    LARGE_INTEGER Delay;
    ULONG StorStatus;
    ULONG Attempts = 0;

    if (0 == Worker)
        return;

    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    Delay.QuadPart = -100000; /* 10 ms */
    for (;;)
    {
        StorStatus = StorPortFreeWorker(DeviceExtension, Worker);
        if (STOR_STATUS_SUCCESS == StorStatus)
            return;
        if (0 == (++Attempts % 500))
            DbgPrint(DRIVER_NAME ": %s waiting to free Storport worker %p status=%lu attempts=%lu\n",
                Operation, Worker, StorStatus, Attempts);
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
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
    UINT32 Length)
{
    SIZE_T BufferOffset;

    if (BufferId >= Ring->BufferCount ||
        Length > Ring->BufferSize)
        return 0;

    BufferOffset = (SIZE_T)Ring->BufferOffset +
        (SIZE_T)BufferId * Ring->BufferSize;
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

static VOID SpdStorageUnitRingReleaseProducerReference(
    SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_DEVICE_EXTENSION *DeviceExtension =
        StorageUnit->Ioq->DeviceExtension;

#if DBG
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    LONG LiveReferences = InterlockedDecrement(
        &Ring->ProducerLiveReferences);
    ASSERT(0 <= LiveReferences);
#endif
    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
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
    SPD_SRB_EXTENSION **ProducerClaims = 0;
    PVOID *ProducerAbortedSrbs = 0;
    PVOID Worker = 0;
    KIRQL Irql;
#if defined(WINSPD_TEST_BUILD)
    PKEVENT TestPostEnteredEvent = 0;
    PKEVENT TestPostReleaseEvent = 0;
    PKEVENT TestPostResetDoneEvent = 0;
    PKEVENT TestPrepareEnteredEvent = 0;
    PKEVENT TestPrepareReleaseEvent = 0;
    PKEVENT TestCompletionEnteredEvent = 0;
    PKEVENT TestCompletionReleaseEvent = 0;
#endif

    if (ProcessId != StorageUnit->TransactProcessId)
        return STATUS_ACCESS_DENIED;
    if (SPD_RING_VERSION != Params->Version)
        return STATUS_NOT_SUPPORTED;
    if (0 == Params->BufferCount ||
        !SpdRingQueueDepthValid(Params->QueueDepth) ||
        Params->BufferSize < StorageUnit->StorageUnitParams.MaxTransferLength)
        return STATUS_INVALID_PARAMETER;

#if defined(WINSPD_TEST_BUILD)
    TestPostEnteredEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestPostEntered");
    TestPostReleaseEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestPostRelease");
    TestPostResetDoneEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestPostResetDone");
    if (0 == TestPostEnteredEvent || 0 == TestPostReleaseEvent ||
        0 == TestPostResetDoneEvent)
    {
        if (0 != TestPostEnteredEvent)
        {
            ObDereferenceObject(TestPostEnteredEvent);
            TestPostEnteredEvent = 0;
        }
        if (0 != TestPostReleaseEvent)
        {
            ObDereferenceObject(TestPostReleaseEvent);
            TestPostReleaseEvent = 0;
        }
        if (0 != TestPostResetDoneEvent)
        {
            ObDereferenceObject(TestPostResetDoneEvent);
            TestPostResetDoneEvent = 0;
        }
    }
    TestPrepareEnteredEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestPrepareEntered");
    TestPrepareReleaseEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestPrepareRelease");
    if (0 == TestPrepareEnteredEvent || 0 == TestPrepareReleaseEvent)
    {
        if (0 != TestPrepareEnteredEvent)
        {
            ObDereferenceObject(TestPrepareEnteredEvent);
            TestPrepareEnteredEvent = 0;
        }
        if (0 != TestPrepareReleaseEvent)
        {
            ObDereferenceObject(TestPrepareReleaseEvent);
            TestPrepareReleaseEvent = 0;
        }
    }
    TestCompletionEnteredEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestCompletionEntered");
    TestCompletionReleaseEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestCompletionRelease");
    if (0 == TestCompletionEnteredEvent ||
        0 == TestCompletionReleaseEvent)
    {
        if (0 != TestCompletionEnteredEvent)
        {
            ObDereferenceObject(TestCompletionEnteredEvent);
            TestCompletionEnteredEvent = 0;
        }
        if (0 != TestCompletionReleaseEvent)
        {
            ObDereferenceObject(TestCompletionReleaseEvent);
            TestCompletionReleaseEvent = 0;
        }
    }
#endif

    if ((SIZE_T)-1 / Params->BufferCount < Params->BufferSize)
    {
        Result = STATUS_INVALID_PARAMETER;
        goto exit;
    }
    if ((SIZE_T)Params->BufferCount >
            (SIZE_T)-1 / sizeof(UINT32) ||
        (SIZE_T)Params->BufferCount >
            (SIZE_T)-1 / sizeof(SPD_RING_BUFFER_META))
    {
        Result = STATUS_INVALID_PARAMETER;
        goto exit;
    }
    if ((SIZE_T)Params->QueueDepth >
            (SIZE_T)-1 / sizeof *ProducerClaims ||
        (SIZE_T)Params->QueueDepth >
            (SIZE_T)-1 / sizeof *ProducerAbortedSrbs)
    {
        Result = STATUS_INVALID_PARAMETER;
        goto exit;
    }
    BufferBytes = (SIZE_T)Params->BufferCount * Params->BufferSize;
    RequestBytes = (SIZE_T)Params->QueueDepth * sizeof(SPD_RING_REQUEST);
    CompletionBytes = (SIZE_T)Params->QueueDepth * sizeof(SPD_RING_COMPLETION);

    Offset = sizeof(SPD_RING_HEADER);
    if (!SpdRingSizeAlign(&Offset, SPD_RING_CACHE_LINE_SIZE) ||
        !SpdRingSizeAdd(&Offset, RequestBytes) ||
        !SpdRingSizeAlign(&Offset, SPD_RING_CACHE_LINE_SIZE) ||
        !SpdRingSizeAdd(&Offset, CompletionBytes) ||
        !SpdRingSizeAlign(&Offset, PAGE_SIZE))
    {
        Result = STATUS_INVALID_PARAMETER;
        goto exit;
    }
    if ((SIZE_T)-1 > MAXULONG && Offset > MAXULONG)
    {
        Result = STATUS_INVALID_PARAMETER;
        goto exit;
    }
    if (!SpdRingSizeAdd(&Offset, BufferBytes) ||
        !SpdRingSizeAlign(&Offset, PAGE_SIZE))
    {
        Result = STATUS_INVALID_PARAMETER;
        goto exit;
    }
    SectionSize = Offset;
    if (SPD_RING_MAX_SECTION_BYTES < SectionSize)
    {
        Result = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    FreeIds = SpdAllocNonPaged(
        (SIZE_T)Params->BufferCount * sizeof *FreeIds,
        SpdTagStorageUnit);
    Meta = SpdAllocNonPaged(
        (SIZE_T)Params->BufferCount * sizeof *Meta,
        SpdTagStorageUnit);
    ProducerClaims = SpdAllocNonPaged(
        (SIZE_T)Params->QueueDepth * sizeof *ProducerClaims,
        SpdTagStorageUnit);
    ProducerAbortedSrbs = SpdAllocNonPaged(
        (SIZE_T)Params->QueueDepth * sizeof *ProducerAbortedSrbs,
        SpdTagStorageUnit);
    if (0 == FreeIds || 0 == Meta ||
        0 == ProducerClaims || 0 == ProducerAbortedSrbs)
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
    Header->Version = SPD_RING_VERSION;
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

    ULONG StorStatus = StorPortInitializeWorker(
        StorageUnit->Ioq->DeviceExtension, &Worker);
    if (STOR_STATUS_SUCCESS != StorStatus)
    {
        Result = SpdNtStatusFromStorStatus(StorStatus);
        goto exit;
    }

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
    Ring->ProducerWorker = Worker;
    Ring->ProducerClaims = ProducerClaims;
    Ring->ProducerAbortedSrbs = ProducerAbortedSrbs;
    Ring->SectionHandle = SectionHandle;
    Ring->Mdl = Mdl;
    Ring->UserAddress = UserAddress;
    Ring->ProcessId = ProcessId;
    Ring->UserProcessId = ProcessId;
    Ring->Failed = FALSE;
#if defined(WINSPD_TEST_BUILD)
    Ring->TestIoqPostEnteredEvent = TestPostEnteredEvent;
    Ring->TestIoqPostReleaseEvent = TestPostReleaseEvent;
    Ring->TestIoqPostResetDoneEvent = TestPostResetDoneEvent;
    Ring->TestProducerPrepareEnteredEvent = TestPrepareEnteredEvent;
    Ring->TestProducerPrepareReleaseEvent = TestPrepareReleaseEvent;
    Ring->TestCompletionCopyEnteredEvent = TestCompletionEnteredEvent;
    Ring->TestCompletionCopyReleaseEvent = TestCompletionReleaseEvent;
    InterlockedExchange(&Ring->TestIoqPostBarrierActive, 0);
#endif
    InterlockedExchangePointer(&Ring->WaitIrp, 0);
    InterlockedExchange(&Ring->ProducerState, SPD_RING_PRODUCER_IDLE);
    InterlockedExchange(&Ring->KickActive, 0);
    KeSetEvent(&Ring->IdleEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&Ring->Lock, Irql);

    SpdIoqSetNonblockingConsumer(StorageUnit->Ioq);

    Params->UserAddress = (UINT64)(UINT_PTR)UserAddress;
    Params->SectionSize = SectionSize;
    SectionHandle = 0;
    Mdl = 0;
    UserAddress = 0;
    FreeIds = 0;
    Meta = 0;
    ProducerClaims = 0;
    ProducerAbortedSrbs = 0;
    Worker = 0;
#if defined(WINSPD_TEST_BUILD)
    TestPostEnteredEvent = 0;
    TestPostReleaseEvent = 0;
    TestPostResetDoneEvent = 0;
    TestPrepareEnteredEvent = 0;
    TestPrepareReleaseEvent = 0;
    TestCompletionEnteredEvent = 0;
    TestCompletionReleaseEvent = 0;
#endif
    SystemAddress = 0;
    PagesLocked = FALSE;
    Result = STATUS_SUCCESS;

exit:
#if defined(WINSPD_TEST_BUILD)
    if (0 != TestPostEnteredEvent)
        ObDereferenceObject(TestPostEnteredEvent);
    if (0 != TestPostReleaseEvent)
        ObDereferenceObject(TestPostReleaseEvent);
    if (0 != TestPostResetDoneEvent)
        ObDereferenceObject(TestPostResetDoneEvent);
    if (0 != TestPrepareEnteredEvent)
        ObDereferenceObject(TestPrepareEnteredEvent);
    if (0 != TestPrepareReleaseEvent)
        ObDereferenceObject(TestPrepareReleaseEvent);
    if (0 != TestCompletionEnteredEvent)
        ObDereferenceObject(TestCompletionEnteredEvent);
    if (0 != TestCompletionReleaseEvent)
        ObDereferenceObject(TestCompletionReleaseEvent);
#endif
    if (0 != Worker)
    {
        SpdStorageUnitRingFreeWorker(
            StorageUnit->Ioq->DeviceExtension, Worker,
            "open cleanup");
        Worker = 0;
    }
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
    if (0 != ProducerClaims)
    {
        SpdFree(ProducerClaims, SpdTagStorageUnit);
        ProducerClaims = 0;
    }
    if (0 != ProducerAbortedSrbs)
    {
        SpdFree(ProducerAbortedSrbs, SpdTagStorageUnit);
        ProducerAbortedSrbs = 0;
    }
    return Result;
}

VOID SpdStorageUnitRingClose(
    SPD_STORAGE_UNIT *StorageUnit,
    BOOLEAN UserProcessExiting)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    KIRQL Irql;
    LARGE_INTEGER Delay;
    LARGE_INTEGER QpcFrequency;
    ULONG LeasedCount;
    ULONG StorStatus;
    ULONG WaitAttempts = 0;

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
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    Delay.QuadPart = -100000; /* 10 ms */
    for (;;)
    {
        LeasedCount = SpdIoqRingLeasedCount(StorageUnit->Ioq);
        if (0 == LeasedCount)
            break;
        if (0 == (++WaitAttempts % 500))
            DbgPrint(DRIVER_NAME ": SharedRing close waiting for %lu leased SRBs\n",
                LeasedCount);
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
#if DBG
    ASSERT(0 == Ring->ProducerExecuting);
    ASSERT(0 == Ring->ProducerLiveReferences);
    ASSERT(0 == Ring->ActiveCalls);
    ASSERT(SPD_RING_PRODUCER_IDLE == InterlockedCompareExchange(
        &Ring->ProducerState, SPD_RING_PRODUCER_IDLE,
        SPD_RING_PRODUCER_IDLE));
#endif
    DbgPrint(DRIVER_NAME ": SharedRing close depth=%lu buffers=%lu "
        "sq_full=%I64d buffer_starved=%I64d waits=%I64d/%I64d "
        "producer_runs=%I64d producer_reruns=%I64d "
        "produced=%I64d batches=[%I64d,%I64d,%I64d,%I64d,%I64d] max_batch=%ld\n",
        Ring->QueueDepth, Ring->BufferCount,
        Ring->SqFullEvents, Ring->BufferPoolExhaustions,
        Ring->WaitSubmissions, Ring->WaitCompletions,
        Ring->ProducerWorkerRuns, Ring->ProducerWorkerReruns,
        Ring->ProducerProduced, Ring->ProducerBatchLe1,
        Ring->ProducerBatch2To4, Ring->ProducerBatch5To16,
        Ring->ProducerBatch17To64, Ring->ProducerBatchOver64,
        Ring->ProducerMaxBatch);
    (void)KeQueryPerformanceCounter(&QpcFrequency);
    DbgPrint(DRIVER_NAME ": SharedRing copy prepare_ticks=%I64d completion_ticks=%I64d deferred_aborts=%I64d qpc_hz=%I64d\n",
        Ring->ProducerPrepareTicks, Ring->CompletionCopyTicks,
        Ring->DeferredSrbAborts, QpcFrequency.QuadPart);
    SpdIoqLogDiagnostics(StorageUnit->Ioq);

    StorStatus = StorPortFreeWorker(
        StorageUnit->Ioq->DeviceExtension, Ring->ProducerWorker);
    while (STOR_STATUS_SUCCESS != StorStatus)
    {
        if (0 == (++WaitAttempts % 500))
            DbgPrint(DRIVER_NAME ": SharedRing close waiting to free worker %p status=%lu attempts=%lu\n",
                Ring->ProducerWorker, StorStatus, WaitAttempts);
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        StorStatus = StorPortFreeWorker(
            StorageUnit->Ioq->DeviceExtension, Ring->ProducerWorker);
    }
    KeAcquireSpinLock(&Ring->Lock, &Irql);
    Ring->ProducerWorker = 0;
    KeReleaseSpinLock(&Ring->Lock, Irql);
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

static VOID SpdRingTestBarrier(PKEVENT EnteredEvent, PKEVENT ReleaseEvent)
{
#if defined(WINSPD_TEST_BUILD)
    if (0 != EnteredEvent && 0 != ReleaseEvent &&
        0 == KeReadStateEvent(ReleaseEvent))
    {
        KeSetEvent(EnteredEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(ReleaseEvent,
            Executive, KernelMode, FALSE, NULL);
    }
#else
    UNREFERENCED_PARAMETER(EnteredEvent);
    UNREFERENCED_PARAMETER(ReleaseEvent);
#endif
}

VOID SpdStorageUnitRingTestPrepareCopyBarrier(
    SPD_STORAGE_UNIT *StorageUnit)
{
#if defined(WINSPD_TEST_BUILD)
    if (PASSIVE_LEVEL == KeGetCurrentIrql() &&
        StorageUnit->Ioq->NonblockingConsumer)
        SpdRingTestBarrier(
            StorageUnit->Ring.TestProducerPrepareEnteredEvent,
            StorageUnit->Ring.TestProducerPrepareReleaseEvent);
#else
    UNREFERENCED_PARAMETER(StorageUnit);
#endif
}

VOID SpdStorageUnitRingTestCompletionCopyBarrier(
    SPD_STORAGE_UNIT *StorageUnit)
{
#if defined(WINSPD_TEST_BUILD)
    if (PASSIVE_LEVEL == KeGetCurrentIrql() &&
        StorageUnit->Ioq->NonblockingConsumer)
        SpdRingTestBarrier(
            StorageUnit->Ring.TestCompletionCopyEnteredEvent,
            StorageUnit->Ring.TestCompletionCopyReleaseEvent);
#else
    UNREFERENCED_PARAMETER(StorageUnit);
#endif
}

static UINT32 SpdRingCommitPreparedBatch(
    SPD_STORAGE_UNIT *StorageUnit,
    UINT32 InitialTail,
    UINT32 StagedCount,
    BOOLEAN AbortAll)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_IOQ *Ioq = StorageUnit->Ioq;
    SPD_RING_HEADER *Header = (SPD_RING_HEADER *)Ring->SystemAddress;
    SPD_RING_REQUEST *RequestRing = (SPD_RING_REQUEST *)
        ((PUINT8)Ring->SystemAddress + Ring->RequestOffset);
    KIRQL RingIrql;
    KIRQL IoqIrql;
    LARGE_INTEGER IoqLockStart;
    UINT32 Published = 0;
    UINT32 Aborted = 0;
    BOOLEAN InvalidRequestHead = FALSE;

    KeAcquireSpinLock(&Ring->Lock, &RingIrql);
    if (Ring->Stopping || Ring->Failed)
        AbortAll = TRUE;
    UINT32 CurrentHead =
        SpdRingLoadAcquire32(&Header->RequestHead.Value);
    UINT32 CurrentUsed = Ring->RequestTail - CurrentHead;
    if (CurrentUsed > Ring->QueueDepth ||
        StagedCount > Ring->QueueDepth - min(CurrentUsed, Ring->QueueDepth))
    {
        Ring->Failed = TRUE;
        Ring->Stopping = TRUE;
        AbortAll = TRUE;
        InvalidRequestHead = TRUE;
    }

    /* The only nested order is Ring->Lock followed by Ioq->SpinLock. */
    SpdIoqRingAcquireCommitLock(Ioq, &IoqIrql, &IoqLockStart);
    if (Ioq->Stopped)
    {
        AbortAll = TRUE;
        Ring->Stopping = TRUE;
    }

    for (UINT32 I = 0; I < StagedCount; I++)
    {
        UINT32 SourceIndex = (InitialTail + I) &
            (Ring->QueueDepth - 1);
        SPD_RING_REQUEST *Source = &RequestRing[SourceIndex];
        SPD_SRB_EXTENSION *SrbExtension = Ring->ProducerClaims[I];
        PVOID Srb;
        BOOLEAN AbortRequested;
        BOOLEAN Publish = SpdIoqRingCommitPreparedSrbNoLock(
            Ioq, SrbExtension, AbortAll, &Srb, &AbortRequested);

        Ring->ProducerClaims[I] = 0;
        if (!Publish)
        {
            if (AbortRequested)
                InterlockedIncrement64(&Ring->DeferredSrbAborts);
            if (SPD_RING_NO_BUFFER != Source->Data.BufferId)
                SpdRingBufferFreeLocked(Ring, Source->Data.BufferId);
            Ring->ProducerAbortedSrbs[Aborted++] = Srb;
            continue;
        }

        if (SPD_RING_NO_BUFFER != Source->Data.BufferId)
        {
            SPD_RING_BUFFER_META *Meta =
                &Ring->Buffers.Meta[Source->Data.BufferId];
            ASSERT(Meta->Allocated);
            Meta->OwnerHint = Source->Request.Hint;
            Meta->Length = Source->Data.Length;
            Meta->Kind = Source->Request.Kind;
        }
        if (Published != I)
            RequestRing[(InitialTail + Published) &
                (Ring->QueueDepth - 1)] = *Source;
        Published++;
    }

    if (!AbortAll && 0 != Published)
    {
        Ring->RequestTail = InitialTail + Published;
        SpdRingStoreRelease32(&Header->RequestTail.Value,
            Ring->RequestTail);
    }
    SpdIoqRingReleaseCommitLock(Ioq, IoqIrql, IoqLockStart);
    KeReleaseSpinLock(&Ring->Lock, RingIrql);

    for (UINT32 I = 0; I < Aborted; I++)
    {
        PVOID Srb = Ring->ProducerAbortedSrbs[I];
        Ring->ProducerAbortedSrbs[I] = 0;
        SpdSrbComplete(Ioq->DeviceExtension, Srb, SRB_STATUS_ABORTED);
    }

    if (InvalidRequestHead)
        SpdStorageUnitRingMarkFailed(StorageUnit);

    if (AbortAll)
        return 0;
    return Published;
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
    UINT32 Tail;
    UINT32 Used;
    UINT32 Free;
    UINT32 Budget;
    UINT32 StagedCount = 0;
    UINT32 Published;
    UINT32 InitialTail;
    BOOLEAN Failed = FALSE;
    BOOLEAN Stopping = FALSE;
    KIRQL Irql;

    *PBatchLimitReached = FALSE;
    KeAcquireSpinLock(&Ring->Lock, &Irql);
    Tail = Ring->RequestTail;
    Stopping = Ring->Stopping || Ring->Failed;
    KeReleaseSpinLock(&Ring->Lock, Irql);
    if (Stopping)
        return 0;

    Used = Tail - Head;
    if (Used > Ring->QueueDepth)
    {
        SpdStorageUnitRingMarkFailed(StorageUnit);
        return 0;
    }
    Free = Ring->QueueDepth - Used;
    if (0 == Free)
    {
        InterlockedIncrement64(&Ring->SqFullEvents);
        return 0;
    }
    Budget = min(Free, Ring->BufferCount);
    InitialTail = Tail;

    while (StagedCount < Budget)
    {
        UINT32 BufferId;
        PVOID DataBuffer;
        SPD_SRB_EXTENSION *SrbExtension = 0;
        SPD_IOCTL_TRANSACT_REQ Request;
        SPD_RING_REQUEST *Entry;
        NTSTATUS Status;
        UINT32 DataLength;
        BOOLEAN NeedsBuffer;

        KeAcquireSpinLock(&Ring->Lock, &Irql);
        if (Ring->Stopping || Ring->Failed)
        {
            KeReleaseSpinLock(&Ring->Lock, Irql);
            Stopping = TRUE;
            break;
        }
        if (!SpdRingBufferAllocLocked(Ring, &BufferId))
        {
            InterlockedIncrement64(&Ring->BufferPoolExhaustions);
            KeReleaseSpinLock(&Ring->Lock, Irql);
            break;
        }
        KeReleaseSpinLock(&Ring->Lock, Irql);

        DataBuffer = SpdRingBufferAddress(Ring, BufferId,
            Ring->BufferSize);
        if (0 == DataBuffer)
        {
            KeAcquireSpinLock(&Ring->Lock, &Irql);
            SpdRingBufferFreeLocked(Ring, BufferId);
            KeReleaseSpinLock(&Ring->Lock, Irql);
            Failed = TRUE;
            break;
        }

        Status = SpdIoqRingClaimSrb(StorageUnit->Ioq, &SrbExtension);
        if (!NT_SUCCESS(Status))
        {
            KeAcquireSpinLock(&Ring->Lock, &Irql);
            SpdRingBufferFreeLocked(Ring, BufferId);
            if (STATUS_CANCELLED == Status)
                Ring->Stopping = TRUE;
            KeReleaseSpinLock(&Ring->Lock, Irql);
            if (STATUS_CANCELLED == Status)
                Stopping = TRUE;
            else if (STATUS_NOT_FOUND != Status)
                Failed = TRUE;
            break;
        }

        Ring->ProducerClaims[StagedCount] = SrbExtension;
        Entry = &RequestRing[(InitialTail + StagedCount) &
            (Ring->QueueDepth - 1)];
        RtlZeroMemory(&Entry->Request, sizeof Entry->Request);
        Entry->Data.BufferId = BufferId;
        Entry->Data.Length = 0;
        StagedCount++;

        RtlZeroMemory(&Request, sizeof Request);
        LARGE_INTEGER PrepareStart = KeQueryPerformanceCounter(0);
        SpdSrbExecuteScsiPrepare(
            SrbExtension, &Request, DataBuffer);
        LARGE_INTEGER PrepareEnd = KeQueryPerformanceCounter(0);
        InterlockedExchangeAdd64(&Ring->ProducerPrepareTicks,
            PrepareEnd.QuadPart - PrepareStart.QuadPart);

        if (0 == Request.Hint ||
            !SpdRingGetRequestDataLength(StorageUnit, &Request,
                &DataLength, &NeedsBuffer))
        {
            Failed = TRUE;
            break;
        }

        Entry->Request = Request;
        if (NeedsBuffer)
        {
            Entry->Data.BufferId = BufferId;
            Entry->Data.Length = DataLength;
        }
        else
        {
            /* FLUSH reserves through the usual pool, then returns the buffer. */
            KeAcquireSpinLock(&Ring->Lock, &Irql);
            SpdRingBufferFreeLocked(Ring, BufferId);
            KeReleaseSpinLock(&Ring->Lock, Irql);
            Entry->Data.BufferId = SPD_RING_NO_BUFFER;
            Entry->Data.Length = 0;
        }
    }

    Published = SpdRingCommitPreparedBatch(
        StorageUnit, InitialTail, StagedCount, Failed);
    if (Failed)
    {
        SpdStorageUnitRingMarkFailed(StorageUnit);
        return 0;
    }
    if (Stopping)
    {
        SpdIoqReset(StorageUnit->Ioq, TRUE,
            SpdIoqResetReasonRingStop);
        SpdStorageUnitRingCancelWait(StorageUnit);
        return 0;
    }

    if (0 != Published)
    {
        InterlockedExchangeAdd64(&Ring->ProducerProduced,
            (LONG64)Published);
        if (1 >= Published)
            InterlockedIncrement64(&Ring->ProducerBatchLe1);
        else if (4 >= Published)
            InterlockedIncrement64(&Ring->ProducerBatch2To4);
        else if (16 >= Published)
            InterlockedIncrement64(&Ring->ProducerBatch5To16);
        else if (64 >= Published)
            InterlockedIncrement64(&Ring->ProducerBatch17To64);
        else
            InterlockedIncrement64(&Ring->ProducerBatchOver64);
        for (;;)
        {
            LONG MaxBatch = InterlockedCompareExchange(
                &Ring->ProducerMaxBatch, 0, 0);
            if ((LONG)Published <= MaxBatch ||
                MaxBatch == InterlockedCompareExchange(
                    &Ring->ProducerMaxBatch, (LONG)Published, MaxBatch))
                break;
        }
    }

    if (StagedCount == Budget && SpdIoqRingHasPending(StorageUnit->Ioq))
    {
        UINT32 RequestHead =
            SpdRingLoadAcquire32(&Header->RequestHead.Value);
        KeAcquireSpinLock(&Ring->Lock, &Irql);
        UINT32 RequestUsed = Ring->RequestTail - RequestHead;
        UINT32 RequestFree = RequestUsed <= Ring->QueueDepth ?
            Ring->QueueDepth - RequestUsed : 0;
        BOOLEAN BuffersAvailable = 0 != Ring->Buffers.FreeCount;
        BOOLEAN RingRunning = !Ring->Stopping && !Ring->Failed;
        KeReleaseSpinLock(&Ring->Lock, Irql);
        if (RingRunning && 0 != RequestFree && BuffersAvailable)
            *PBatchLimitReached = TRUE;
    }

    return Published;
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
        InterlockedIncrement64(&Ring->WaitCompletions);
    StorPortCompleteServiceIrp(DeviceExtension, Irp);
    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
}

static VOID SpdStorageUnitRingCancelWait(SPD_STORAGE_UNIT *StorageUnit)
{
    SpdStorageUnitRingCompleteWait(StorageUnit, STATUS_CANCELLED, FALSE);
}

static VOID SpdStorageUnitRingProducerWorker(
    PVOID HwDeviceExtension,
    PVOID Context,
    PVOID Worker)
{
    SPD_STORAGE_UNIT *StorageUnit = Context;
    SPD_RING_STATE *Ring = &StorageUnit->Ring;

    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(HwDeviceExtension == StorageUnit->Ioq->DeviceExtension);
    ASSERT(Worker == Ring->ProducerWorker);
    InterlockedIncrement64(&Ring->ProducerWorkerRuns);

    BOOLEAN BatchLimitReached = FALSE;
    BOOLEAN SchedulingFailed = FALSE;
    KIRQL Irql;
    BOOLEAN Stopping;

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    Stopping = Ring->Stopping || Ring->Failed;
    KeReleaseSpinLock(&Ring->Lock, Irql);
    Stopping = Stopping || SpdIoqStopped(StorageUnit->Ioq);

#if DBG
    ASSERT(0 == InterlockedCompareExchange(
        &Ring->ProducerExecuting, 1, 0));
#endif
    if (!Stopping)
        (void)SpdRingProduceBatch(StorageUnit, &BatchLimitReached);
    SpdStorageUnitRingCompleteWait(
        StorageUnit, STATUS_SUCCESS, TRUE);
#if DBG
    ASSERT(1 == InterlockedExchange(
        &Ring->ProducerExecuting, 0));
#endif

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    Stopping = Ring->Stopping || Ring->Failed;
    KeReleaseSpinLock(&Ring->Lock, Irql);
    Stopping = Stopping || SpdIoqStopped(StorageUnit->Ioq);

    if (Stopping)
    {
        InterlockedExchange(&Ring->ProducerState,
            SPD_RING_PRODUCER_IDLE);
        goto exit;
    }

    if (BatchLimitReached &&
        SPD_RING_PRODUCER_ACTIVE == InterlockedCompareExchange(
            &Ring->ProducerState, SPD_RING_PRODUCER_RERUN,
            SPD_RING_PRODUCER_ACTIVE))
        InterlockedIncrement64(&Ring->ProducerWorkerReruns);

    for (;;)
    {
        LONG State = InterlockedCompareExchange(
            &Ring->ProducerState, SPD_RING_PRODUCER_IDLE,
            SPD_RING_PRODUCER_ACTIVE);
        if (SPD_RING_PRODUCER_ACTIVE == State)
            goto exit;
        if (SPD_RING_PRODUCER_RERUN == State)
        {
            if (SPD_RING_PRODUCER_RERUN != InterlockedCompareExchange(
                    &Ring->ProducerState, SPD_RING_PRODUCER_ACTIVE,
                    SPD_RING_PRODUCER_RERUN))
                continue;

            /* Keep one lifetime reference across the queued continuation. */
            ULONG StorStatus = StorPortQueueWorkItem(
                HwDeviceExtension,
                SpdStorageUnitRingProducerWorker,
                Ring->ProducerWorker,
                StorageUnit);
            if (STOR_STATUS_SUCCESS == StorStatus)
                return;
            SpdStorageUnitRingMarkFailed(StorageUnit);
            InterlockedExchange(&Ring->ProducerState,
                SPD_RING_PRODUCER_IDLE);
            DbgPrint(DRIVER_NAME ": SharedRing producer continuation queue failed status=%lu\n",
                StorStatus);
            SchedulingFailed = TRUE;
            goto exit;
        }

        /* A scheduler observed the idle transition and queued fresh work. */
        goto exit;
    }

exit:
    SpdStorageUnitRingReleaseProducerReference(StorageUnit);
    if (SchedulingFailed)
        ASSERT(FALSE);
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
    if (SpdIoqStopped(StorageUnit->Ioq))
    {
        KIRQL Irql;
        KeAcquireSpinLock(&Ring->Lock, &Irql);
        Ring->Stopping = TRUE;
        KeReleaseSpinLock(&Ring->Lock, Irql);
        SpdStorageUnitRingLeave(StorageUnit);
        SpdStorageUnitDereference(DeviceExtension, StorageUnit);
        return;
    }

    for (;;)
    {
        LONG State = InterlockedCompareExchange(
            &Ring->ProducerState, SPD_RING_PRODUCER_ACTIVE,
            SPD_RING_PRODUCER_IDLE);
        if (SPD_RING_PRODUCER_IDLE == State)
        {
#if DBG
            /* Count before queueing so a fast worker cannot release first. */
            InterlockedIncrement(&Ring->ProducerLiveReferences);
#endif
            ULONG StorStatus = StorPortQueueWorkItem(
                DeviceExtension,
                SpdStorageUnitRingProducerWorker,
                Ring->ProducerWorker,
                StorageUnit);
            if (STOR_STATUS_SUCCESS == StorStatus)
                return;

            InterlockedExchange(&Ring->ProducerState,
                SPD_RING_PRODUCER_IDLE);
            SpdStorageUnitRingMarkFailed(StorageUnit);
            DbgPrint(DRIVER_NAME ": SharedRing producer queue failed status=%lu\n",
                StorStatus);
            SpdStorageUnitRingReleaseProducerReference(StorageUnit);
            ASSERT(FALSE);
            return;
        }
        if (SPD_RING_PRODUCER_ACTIVE == State)
        {
            if (SPD_RING_PRODUCER_ACTIVE == InterlockedCompareExchange(
                    &Ring->ProducerState, SPD_RING_PRODUCER_RERUN,
                    SPD_RING_PRODUCER_ACTIVE))
            {
                InterlockedIncrement64(&Ring->ProducerWorkerReruns);
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

    InterlockedIncrement64(&Ring->WaitSubmissions);
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
                Data->Length);
            if (0 == DataBuffer)
            {
                Failed = TRUE;
                Result = STATUS_INVALID_PARAMETER;
                break;
            }
        }
        else if (0 != Data->Length)
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

        /* Match the opaque hint against a live process-bucket entry. */
        SPD_SRB_EXTENSION *SrbExtension = 0;
        Result = SpdIoqRingClaimCompletion(
            StorageUnit->Ioq, Response->Hint, &SrbExtension);
        if (!NT_SUCCESS(Result))
        {
            if (SPD_RING_NO_BUFFER != BufferId)
            {
                KeAcquireSpinLock(&Ring->Lock, &Irql);
                if (BufferId < Ring->BufferCount &&
                    Ring->Buffers.Meta[BufferId].Allocated &&
                    Ring->Buffers.Meta[BufferId].OwnerHint == Response->Hint)
                    SpdRingBufferFreeLocked(Ring, BufferId);
                KeReleaseSpinLock(&Ring->Lock, Irql);
            }
            Failed = TRUE;
            break;
        }

        LARGE_INTEGER CopyStart = KeQueryPerformanceCounter(0);
        UCHAR SrbStatus = SpdSrbExecuteScsiComplete(
            SrbExtension, Response, DataBuffer);
        LARGE_INTEGER CopyEnd = KeQueryPerformanceCounter(0);
        InterlockedExchangeAdd64(&Ring->CompletionCopyTicks,
            CopyEnd.QuadPart - CopyStart.QuadPart);
        Result = SpdIoqRingFinishCompletion(
            StorageUnit->Ioq, SrbExtension, SrbStatus);
        if (!NT_SUCCESS(Result))
        {
            if (SPD_RING_NO_BUFFER != BufferId)
            {
                KeAcquireSpinLock(&Ring->Lock, &Irql);
                if (BufferId < Ring->BufferCount &&
                    Ring->Buffers.Meta[BufferId].Allocated &&
                    Ring->Buffers.Meta[BufferId].OwnerHint == Response->Hint)
                    SpdRingBufferFreeLocked(Ring, BufferId);
                KeReleaseSpinLock(&Ring->Lock, Irql);
            }
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
