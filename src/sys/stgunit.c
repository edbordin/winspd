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

static VOID SpdRingUnmapPayload(SPD_STORAGE_UNIT *StorageUnit,
    SPD_WORK_SLOT *Slot, BOOLEAN UserProcessExiting);

ERESOURCE SpdGlobalDeviceResource;
KMUTEX SpdGlobalDeviceLifecycleMutex;
SPD_DEVICE_EXTENSION *SpdGlobalDeviceExtension;
ULONG SpdStorageUnitCapacity = SPD_IOCTL_STORAGE_UNIT_CAPACITY;

static VOID SpdDeviceExtensionNotifyRoutine(HANDLE ParentId, HANDLE ProcessId0, BOOLEAN Create);
static VOID SpdStorageUnitRingProducerWorker(
    PVOID HwDeviceExtension,
    PVOID Context,
    PVOID Worker);
static IO_WORKITEM_ROUTINE SpdStorageUnitRingProducerRetryWorker;
static ULONG SpdStorageUnitRingQueueProducerWorker(SPD_STORAGE_UNIT *StorageUnit);
static VOID SpdStorageUnitRingCompletionWorker(
    PVOID HwDeviceExtension,
    PVOID Context,
    PVOID Worker);
#if !SPD_RING_COMPLETION_THREAD
static IO_WORKITEM_ROUTINE SpdStorageUnitRingCompletionRetryWorker;
#endif
static ULONG SpdStorageUnitRingQueueCompletionWorker(SPD_STORAGE_UNIT *StorageUnit);
static VOID SpdStorageUnitRingPreparationWorker(
    PVOID HwDeviceExtension,
    PVOID Context,
    PVOID Worker);
#if !SPD_RING_PREPARATION_THREAD
static IO_WORKITEM_ROUTINE SpdStorageUnitRingPreparationRetryWorker;
#endif
static ULONG SpdStorageUnitRingQueuePreparationWorker(SPD_STORAGE_UNIT *StorageUnit);
static BOOLEAN SpdStorageUnitRingEnter(SPD_STORAGE_UNIT *StorageUnit);
static VOID SpdStorageUnitRingLeave(SPD_STORAGE_UNIT *StorageUnit);
static VOID SpdStorageUnitRingCompleteWait(
    SPD_STORAGE_UNIT *StorageUnit,
    NTSTATUS Status,
    BOOLEAN RequireRequest);
static VOID SpdStorageUnitRingCancelWait(SPD_STORAGE_UNIT *StorageUnit);
static DRIVER_CANCEL SpdStorageUnitRingCancelWaitIrp;
static IO_WORKITEM_ROUTINE SpdStorageUnitOwnerRundownWorker;
static NTSTATUS SpdStorageUnitUnprovisionInternal(
    SPD_DEVICE_EXTENSION *DeviceExtension, PGUID Guid, ULONG Index,
    ULONG ProcessId, SPD_STORAGE_UNIT *ExitingUnit);

#if defined(WINSPD_TEST_BUILD)
static VOID SpdRingTestOpenCommitBarrier(VOID);
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
        {
            KAPC_STATE ApcState;
            BOOLEAN Attached = StorageUnit->RingProcess != PsGetCurrentProcess();
            if (Attached)
                KeStackAttachProcess(StorageUnit->RingProcess, &ApcState);
            ZwUnmapViewOfSection(ZwCurrentProcess(), Ring->UserAddress);
            if (Attached)
                KeUnstackDetachProcess(&ApcState);
        }

        Ring->UserAddress = 0;
    }

    if (0 != Ring->SectionHandle)
    {
        ZwClose(Ring->SectionHandle);
        Ring->SectionHandle = 0;
    }

    SPD_WORK_SLOT *Slots = SpdIoqRingDetachSlots(StorageUnit->Ioq);
    if (0 != Slots)
        SpdFree(Slots, SpdTagStorageUnit);
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
    if (0 != Ring->TestCompletionClaimEnteredEvent)
        ObDereferenceObject(Ring->TestCompletionClaimEnteredEvent);
    if (0 != Ring->TestCompletionClaimReleaseEvent)
        ObDereferenceObject(Ring->TestCompletionClaimReleaseEvent);
    if (0 != Ring->TestPrepareCommitEnteredEvent)
        ObDereferenceObject(Ring->TestPrepareCommitEnteredEvent);
    if (0 != Ring->TestPrepareCommitReleaseEvent)
        ObDereferenceObject(Ring->TestPrepareCommitReleaseEvent);
    if (0 != Ring->TestReadyEnteredEvent)
        ObDereferenceObject(Ring->TestReadyEnteredEvent);
    if (0 != Ring->TestReadyReleaseEvent)
        ObDereferenceObject(Ring->TestReadyReleaseEvent);
    if (0 != Ring->TestPreparationClaimReleaseEvent)
        ObDereferenceObject(Ring->TestPreparationClaimReleaseEvent);
    Ring->TestIoqPostEnteredEvent = 0;
    Ring->TestIoqPostReleaseEvent = 0;
    Ring->TestIoqPostResetDoneEvent = 0;
    Ring->TestProducerPrepareEnteredEvent = 0;
    Ring->TestProducerPrepareReleaseEvent = 0;
    Ring->TestCompletionCopyEnteredEvent = 0;
    Ring->TestCompletionCopyReleaseEvent = 0;
    Ring->TestCompletionClaimEnteredEvent = 0;
    Ring->TestCompletionClaimReleaseEvent = 0;
    Ring->TestPrepareCommitEnteredEvent = 0;
    Ring->TestPrepareCommitReleaseEvent = 0;
    Ring->TestReadyEnteredEvent = 0;
    Ring->TestReadyReleaseEvent = 0;
    Ring->TestPreparationClaimReleaseEvent = 0;
    InterlockedExchange(&Ring->TestIoqPostBarrierActive, 0);
#endif
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
    InterlockedExchange(&Ring->PreparationState, SPD_RING_PRODUCER_IDLE);
    InterlockedExchange(&Ring->CompletionState, SPD_RING_PRODUCER_IDLE);
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

#if SPD_RING_PREPARATION_THREAD || SPD_RING_COMPLETION_THREAD
struct _SPD_RING_THREAD
{
    SPD_RING_THREAD *Next;
    SPD_STORAGE_UNIT *StorageUnit;
    PVOID StorportContext;
    PETHREAD Object;
    KEVENT Started;
    KEVENT Wake;
    volatile LONG Stop;
    volatile LONG State;
    UINT64 QueuedTicks;
    BOOLEAN Preparation;
};

static VOID SpdStorageUnitRingRunThread(SPD_RING_THREAD *Thread);

static VOID SpdStorageUnitRingThreadMain(PVOID Context)
{
    SPD_RING_THREAD *Thread = Context;
    SPD_STORAGE_UNIT *StorageUnit = Thread->StorageUnit;
    PVOID DeviceExtension = StorageUnit->Ioq->DeviceExtension;
    /* Retain a waitable thread object so CLOSE joins actual termination, not
     * merely a callback's final event or lifetime-reference release. */
    Thread->Object = PsGetCurrentThread();
    ObReferenceObject(Thread->Object);
    KeSetEvent(&Thread->Started, IO_NO_INCREMENT, FALSE);
    for (;;)
    {
        KeWaitForSingleObject(&Thread->Wake, Executive, KernelMode, FALSE, NULL);
        if (InterlockedCompareExchange(&Thread->Stop, 0, 0))
            break;
        SpdStorageUnitRingRunThread(Thread);
    }
    /* The creator stores StorportContext before the first wake. The joining
     * owner keeps Thread and StorageUnit alive until this thread terminates. */
    StorPortTerminateSystemThread(DeviceExtension, Thread->StorportContext);
}

static NTSTATUS SpdStorageUnitRingCreateOneThread(
    SPD_STORAGE_UNIT *StorageUnit, BOOLEAN Preparation, SPD_RING_THREAD **PThread)
{
    SPD_DEVICE_EXTENSION *DeviceExtension = StorageUnit->Ioq->DeviceExtension;
    SPD_STORAGE_UNIT *Reference = SpdStorageUnitReferenceByBtl(
        DeviceExtension, StorageUnit->Btl);
    if (Reference != StorageUnit)
    {
        if (0 != Reference)
            SpdStorageUnitDereference(DeviceExtension, Reference);
        return STATUS_DEVICE_REMOVED;
    }
    SPD_RING_THREAD *Thread = SpdAllocNonPaged(sizeof *Thread, SpdTagStorageUnit);
    if (0 == Thread)
    {
        SpdStorageUnitDereference(DeviceExtension, StorageUnit);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Thread, sizeof *Thread);
    Thread->StorageUnit = StorageUnit;
    Thread->Preparation = Preparation;
    KeInitializeEvent(&Thread->Started, NotificationEvent, FALSE);
    KeInitializeEvent(&Thread->Wake, SynchronizationEvent, FALSE);
    STOR_THREAD_PRIORITY Priority = (STOR_THREAD_PRIORITY)SPD_RING_THREAD_PRIORITY;
    ULONG StorStatus = StorPortCreateSystemThread(DeviceExtension,
        SpdStorageUnitRingThreadMain, Thread, &Priority, &Thread->StorportContext);
    if (STOR_STATUS_SUCCESS != StorStatus)
    {
        DbgPrint(DRIVER_NAME ": SharedRing thread creation failed preparation=%u status=%lu\n",
            Preparation, StorStatus);
        SpdFree(Thread, SpdTagStorageUnit);
        SpdStorageUnitDereference(DeviceExtension, StorageUnit);
        return SpdNtStatusFromStorStatus(StorStatus);
    }
    KeWaitForSingleObject(&Thread->Started, Executive, KernelMode, FALSE, NULL);
    *PThread = Thread;
    return STATUS_SUCCESS;
}

static VOID SpdStorageUnitRingFreeOneThread(SPD_RING_THREAD *Thread)
{
    if (0 == Thread)
        return;
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    InterlockedExchange(&Thread->Stop, 1);
    KeSetEvent(&Thread->Wake, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(Thread->Object, Executive, KernelMode, FALSE, NULL);
    ASSERT(SPD_RING_PRODUCER_IDLE == Thread->State);
    ObDereferenceObject(Thread->Object);
    SpdStorageUnitDereference(Thread->StorageUnit->Ioq->DeviceExtension,
        Thread->StorageUnit);
    SpdFree(Thread, SpdTagStorageUnit);
}

static VOID SpdStorageUnitRingFreeThread(SPD_RING_THREAD *Thread)
{
    while (0 != Thread)
    {
        SPD_RING_THREAD *Next = Thread->Next;
        SpdStorageUnitRingFreeOneThread(Thread);
        Thread = Next;
    }
}

static NTSTATUS SpdStorageUnitRingCreateThread(
    SPD_STORAGE_UNIT *StorageUnit, BOOLEAN Preparation, SPD_RING_THREAD **PThread)
{
    SPD_RING_THREAD *Head = 0, **Tail = &Head;
    ULONG Count = Preparation ? SPD_RING_PREPARATION_THREADS : SPD_RING_COMPLETION_THREADS;
    for (ULONG I = 0; I < Count; I++)
    {
        NTSTATUS Status = SpdStorageUnitRingCreateOneThread(StorageUnit, Preparation, Tail);
        if (!NT_SUCCESS(Status))
        {
            SpdStorageUnitRingFreeThread(Head);
            return Status;
        }
        Tail = &(*Tail)->Next;
    }
    *PThread = Head;
    return STATUS_SUCCESS;
}

/* Caller holds a ring-call and unit reference while traversing the pool.
 * Each ACTIVE/RERUN thread takes its own pair before it can be awakened. */
static VOID SpdStorageUnitRingScheduleThreads(SPD_STORAGE_UNIT *StorageUnit,
    SPD_RING_THREAD *Thread)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    for (; 0 != Thread; Thread = Thread->Next)
    {
        SPD_STORAGE_UNIT *Reference = SpdStorageUnitReferenceByBtl(
            StorageUnit->Ioq->DeviceExtension, StorageUnit->Btl);
        if (Reference != StorageUnit)
        {
            if (0 != Reference)
                SpdStorageUnitDereference(StorageUnit->Ioq->DeviceExtension, Reference);
            return;
        }
        if (!SpdStorageUnitRingEnter(StorageUnit))
        {
            SpdStorageUnitDereference(StorageUnit->Ioq->DeviceExtension, Reference);
            return;
        }
        BOOLEAN Activated = FALSE;
        for (;;)
        {
            LONG State = InterlockedCompareExchange(&Thread->State,
                SPD_RING_PRODUCER_ACTIVE, SPD_RING_PRODUCER_IDLE);
            if (SPD_RING_PRODUCER_IDLE == State)
            {
#if DBG
                InterlockedIncrement(Thread->Preparation ?
                    &Ring->PreparationLiveReferences : &Ring->CompletionLiveReferences);
#endif
                Thread->QueuedTicks = KeQueryPerformanceCounter(0).QuadPart;
                KeSetEvent(&Thread->Wake, IO_NO_INCREMENT, FALSE);
                Activated = TRUE;
                break;
            }
            if (SPD_RING_PRODUCER_RERUN == State)
                break;
            if (SPD_RING_PRODUCER_ACTIVE == InterlockedCompareExchange(&Thread->State,
                    SPD_RING_PRODUCER_RERUN, SPD_RING_PRODUCER_ACTIVE))
            {
                InterlockedIncrement64(Thread->Preparation ?
                    &Ring->PreparationWorkerReruns : &Ring->CompletionWorkerReruns);
                break;
            }
        }
        if (!Activated)
        {
            SpdStorageUnitRingLeave(StorageUnit);
            SpdStorageUnitDereference(StorageUnit->Ioq->DeviceExtension, Reference);
        }
    }
}
#endif

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

static VOID SpdStorageUnitRingReleaseCompletionReference(
    SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_DEVICE_EXTENSION *DeviceExtension =
        StorageUnit->Ioq->DeviceExtension;

#if DBG
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    LONG LiveReferences = InterlockedDecrement(
        &Ring->CompletionLiveReferences);
    ASSERT(0 <= LiveReferences);
#endif
    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
}

static VOID SpdStorageUnitRingReleasePreparationReference(
    SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_DEVICE_EXTENSION *DeviceExtension =
        StorageUnit->Ioq->DeviceExtension;

#if DBG
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    LONG LiveReferences = InterlockedDecrement(
        &Ring->PreparationLiveReferences);
    ASSERT(0 <= LiveReferences);
#endif
    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
}

NTSTATUS SpdStorageUnitRingOpen(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId,
    PFILE_OBJECT FileObject,
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
    SPD_WORK_SLOT *Slots = 0;
    BOOLEAN SlotsAttached = FALSE;
    PEPROCESS RingProcess = 0;
    PIO_WORKITEM RundownWorker = 0;
    PVOID Worker = 0;
    PIO_WORKITEM RetryWorker = 0;
    PVOID PreparationWorker = 0;
#if SPD_RING_PREPARATION_THREAD
    SPD_RING_THREAD *PreparationThread = 0;
#endif
    PIO_WORKITEM PreparationRetryWorker = 0;
    PVOID CompletionWorker = 0;
#if SPD_RING_COMPLETION_THREAD
    SPD_RING_THREAD *CompletionThread = 0;
#endif
    PIO_WORKITEM CompletionRetryWorker = 0;
    KIRQL Irql;
#if defined(WINSPD_TEST_BUILD)
    PKEVENT TestPostEnteredEvent = 0;
    PKEVENT TestPostReleaseEvent = 0;
    PKEVENT TestPostResetDoneEvent = 0;
    PKEVENT TestPrepareEnteredEvent = 0;
    PKEVENT TestPrepareReleaseEvent = 0;
    PKEVENT TestCompletionEnteredEvent = 0;
    PKEVENT TestCompletionReleaseEvent = 0;
    PKEVENT TestCompletionClaimEnteredEvent = 0;
    PKEVENT TestCompletionClaimReleaseEvent = 0;
    PKEVENT TestPrepareCommitEnteredEvent = 0;
    PKEVENT TestPrepareCommitReleaseEvent = 0;
    PKEVENT TestReadyEnteredEvent = 0;
    PKEVENT TestReadyReleaseEvent = 0;
    PKEVENT TestPreparationClaimReleaseEvent = 0;
    PKEVENT TestOwnerRundownDoneEvent = 0;
#endif

    if (ProcessId != StorageUnit->TransactProcessId)
        return STATUS_ACCESS_DENIED;
    /* This API maps into the requesting process. Retain the actual process
     * object; a PID difference is not evidence of process termination. */
    if (ProcessId != (ULONG)(ULONG_PTR)PsGetCurrentProcessId())
        return STATUS_INVALID_DEVICE_STATE;
    if (SPD_RING_VERSION != Params->Version)
        return STATUS_NOT_SUPPORTED;
    if (0 == Params->BufferCount || Params->BufferCount > 65536 ||
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
    if ((SIZE_T)Params->BufferCount > (SIZE_T)-1 / sizeof *Slots)
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

    #if defined(WINSPD_TEST_BUILD)
    TestCompletionClaimEnteredEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestCompletionClaimEntered");
    TestCompletionClaimReleaseEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestCompletionClaimRelease");
    TestPrepareCommitEnteredEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestPrepareCommitEntered");
    TestPrepareCommitReleaseEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestPrepareCommitRelease");
    TestReadyEnteredEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestReadyEntered");
    TestReadyReleaseEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestReadyRelease");
    TestPreparationClaimReleaseEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestPreparationClaimRelease");
    TestOwnerRundownDoneEvent = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestOwnerRundownDone");
    #endif
    Slots = SpdAllocNonPaged((SIZE_T)Params->BufferCount * sizeof *Slots,
        SpdTagStorageUnit);
    if (0 == Slots)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    RtlZeroMemory(Slots, (SIZE_T)Params->BufferCount * sizeof *Slots);

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
    Header->MapThresholdBytes = SPD_RING_MAP_THRESHOLD_BYTES;

    ULONG StorStatus = StorPortInitializeWorker(
        StorageUnit->Ioq->DeviceExtension, &Worker);
    if (STOR_STATUS_SUCCESS != StorStatus)
    {
        Result = SpdNtStatusFromStorStatus(StorStatus);
        goto exit;
    }

    RetryWorker = IoAllocateWorkItem(
        ((SPD_DEVICE_EXTENSION *)StorageUnit->Ioq->DeviceExtension)->DeviceObject);
    if (0 == RetryWorker)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

#if SPD_RING_PREPARATION_THREAD
    Result = SpdStorageUnitRingCreateThread(StorageUnit, TRUE, &PreparationThread);
    if (!NT_SUCCESS(Result))
        goto exit;
#else
    StorStatus = StorPortInitializeWorker(
        StorageUnit->Ioq->DeviceExtension, &PreparationWorker);
    if (STOR_STATUS_SUCCESS != StorStatus)
    {
        Result = SpdNtStatusFromStorStatus(StorStatus);
        goto exit;
    }
    PreparationRetryWorker = IoAllocateWorkItem(
        ((SPD_DEVICE_EXTENSION *)StorageUnit->Ioq->DeviceExtension)->DeviceObject);
    if (0 == PreparationRetryWorker)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
#endif

#if SPD_RING_COMPLETION_THREAD
    Result = SpdStorageUnitRingCreateThread(StorageUnit, FALSE, &CompletionThread);
    if (!NT_SUCCESS(Result))
        goto exit;
#else
    StorStatus = StorPortInitializeWorker(
        StorageUnit->Ioq->DeviceExtension, &CompletionWorker);
    if (STOR_STATUS_SUCCESS != StorStatus)
    {
        Result = SpdNtStatusFromStorStatus(StorStatus);
        goto exit;
    }
    CompletionRetryWorker = IoAllocateWorkItem(
        ((SPD_DEVICE_EXTENSION *)StorageUnit->Ioq->DeviceExtension)->DeviceObject);
    if (0 == CompletionRetryWorker)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
#endif

    RingProcess = PsGetCurrentProcess();
    ObReferenceObject(RingProcess);
    RundownWorker = IoAllocateWorkItem(
        ((SPD_DEVICE_EXTENSION *)StorageUnit->Ioq->DeviceExtension)->DeviceObject);
    if (0 == RundownWorker)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    Result = SpdIoqRingAttachSlots(StorageUnit->Ioq, Slots, Params->BufferCount);
    if (!NT_SUCCESS(Result))
        goto exit;
    SlotsAttached = TRUE;

#if defined(WINSPD_TEST_BUILD)
    SpdRingTestOpenCommitBarrier();
#endif
    KeAcquireSpinLock(&Ring->Lock, &Irql);
    /* SET_TRANSACT_PID may have won while OPEN allocated its private resources.
     * Commit mapping identity only while that authorized process is unchanged. */
    if (ProcessId != StorageUnit->TransactProcessId)
    {
        KeReleaseSpinLock(&Ring->Lock, Irql);
        Result = STATUS_ACCESS_DENIED;
        goto exit;
    }
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
    Ring->ProducerWorker = Worker;
#if SPD_RING_COMPLETION_THREAD
    Ring->CompletionThread = CompletionThread;
#endif
    Ring->CompletionWorker = CompletionWorker;
    Ring->CompletionRetryWorker = CompletionRetryWorker;
    Ring->CompletionRetryQueued = FALSE;
#if SPD_RING_PREPARATION_THREAD
    Ring->PreparationThread = PreparationThread;
#endif
    Ring->PreparationWorker = PreparationWorker;
    Ring->PreparationRetryWorker = PreparationRetryWorker;
    Ring->PreparationRetryQueued = FALSE;
    Ring->ProducerRetryWorker = RetryWorker;
    Ring->ProducerRetryQueued = FALSE;
    Ring->SectionHandle = SectionHandle;
    Ring->Mdl = Mdl;
    Ring->UserAddress = UserAddress;
    Ring->ProcessId = ProcessId;
    Ring->UserProcessId = ProcessId;
    StorageUnit->RingProcess = RingProcess;
    ObReferenceObject(FileObject);
    StorageUnit->RingFileObject = FileObject;
    StorageUnit->RundownWorker = RundownWorker;
    Ring->Failed = FALSE;
#if defined(WINSPD_TEST_BUILD)
    Ring->TestIoqPostEnteredEvent = TestPostEnteredEvent;
    Ring->TestIoqPostReleaseEvent = TestPostReleaseEvent;
    Ring->TestIoqPostResetDoneEvent = TestPostResetDoneEvent;
    Ring->TestProducerPrepareEnteredEvent = TestPrepareEnteredEvent;
    Ring->TestProducerPrepareReleaseEvent = TestPrepareReleaseEvent;
    Ring->TestCompletionCopyEnteredEvent = TestCompletionEnteredEvent;
    Ring->TestCompletionCopyReleaseEvent = TestCompletionReleaseEvent;
    Ring->TestCompletionClaimEnteredEvent = TestCompletionClaimEnteredEvent;
    Ring->TestCompletionClaimReleaseEvent = TestCompletionClaimReleaseEvent;
    Ring->TestPrepareCommitEnteredEvent = TestPrepareCommitEnteredEvent;
    Ring->TestPrepareCommitReleaseEvent = TestPrepareCommitReleaseEvent;
    Ring->TestReadyEnteredEvent = TestReadyEnteredEvent;
    Ring->TestReadyReleaseEvent = TestReadyReleaseEvent;
    Ring->TestPreparationClaimReleaseEvent = TestPreparationClaimReleaseEvent;
    StorageUnit->TestOwnerRundownDoneEvent = TestOwnerRundownDoneEvent;
    InterlockedExchange(&Ring->TestIoqPostBarrierActive, 0);
#endif
    InterlockedExchangePointer(&Ring->WaitIrp, 0);
    InterlockedExchange(&Ring->ProducerState, SPD_RING_PRODUCER_IDLE);
    InterlockedExchange(&Ring->PreparationState, SPD_RING_PRODUCER_IDLE);
    InterlockedExchange(&Ring->CompletionState, SPD_RING_PRODUCER_IDLE);
    InterlockedExchange(&Ring->KickActive, 0);
    KeSetEvent(&Ring->IdleEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&Ring->Lock, Irql);


    Params->UserAddress = (UINT64)(UINT_PTR)UserAddress;
    Params->SectionSize = SectionSize;
    SectionHandle = 0;
    Mdl = 0;
    UserAddress = 0;
    Slots = 0;
    SlotsAttached = FALSE;
    RingProcess = 0;
    RundownWorker = 0;
    Worker = 0;
    RetryWorker = 0;
#if SPD_RING_PREPARATION_THREAD
    PreparationThread = 0;
#endif
    PreparationWorker = 0;
    PreparationRetryWorker = 0;
#if SPD_RING_COMPLETION_THREAD
    CompletionThread = 0;
#endif
    CompletionWorker = 0;
    CompletionRetryWorker = 0;
#if defined(WINSPD_TEST_BUILD)
    TestPostEnteredEvent = 0;
    TestPostReleaseEvent = 0;
    TestPostResetDoneEvent = 0;
    TestPrepareEnteredEvent = 0;
    TestPrepareReleaseEvent = 0;
    TestCompletionEnteredEvent = 0;
    TestCompletionReleaseEvent = 0;
    TestCompletionClaimEnteredEvent = 0;
    TestCompletionClaimReleaseEvent = 0;
    TestPrepareCommitEnteredEvent = 0;
    TestPrepareCommitReleaseEvent = 0;
    TestReadyEnteredEvent = 0;
    TestReadyReleaseEvent = 0;
    TestPreparationClaimReleaseEvent = 0;
    TestOwnerRundownDoneEvent = 0;
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
    if (0 != TestCompletionClaimEnteredEvent)
        ObDereferenceObject(TestCompletionClaimEnteredEvent);
    if (0 != TestCompletionClaimReleaseEvent)
        ObDereferenceObject(TestCompletionClaimReleaseEvent);
    if (0 != TestPrepareCommitEnteredEvent)
        ObDereferenceObject(TestPrepareCommitEnteredEvent);
    if (0 != TestPrepareCommitReleaseEvent)
        ObDereferenceObject(TestPrepareCommitReleaseEvent);
    if (0 != TestReadyEnteredEvent)
        ObDereferenceObject(TestReadyEnteredEvent);
    if (0 != TestReadyReleaseEvent)
        ObDereferenceObject(TestReadyReleaseEvent);
    if (0 != TestPreparationClaimReleaseEvent)
        ObDereferenceObject(TestPreparationClaimReleaseEvent);
    if (0 != TestOwnerRundownDoneEvent)
        ObDereferenceObject(TestOwnerRundownDoneEvent);
#endif
    if (0 != Worker)
    {
        SpdStorageUnitRingFreeWorker(
            StorageUnit->Ioq->DeviceExtension, Worker,
            "open cleanup");
        Worker = 0;
    }
    if (0 != RetryWorker)
        IoFreeWorkItem(RetryWorker);
#if SPD_RING_COMPLETION_THREAD
    SpdStorageUnitRingFreeThread(CompletionThread);
#endif
    if (0 != CompletionWorker)
        SpdStorageUnitRingFreeWorker(StorageUnit->Ioq->DeviceExtension,
            CompletionWorker, "completion open cleanup");
    if (0 != CompletionRetryWorker)
        IoFreeWorkItem(CompletionRetryWorker);
#if SPD_RING_PREPARATION_THREAD
    SpdStorageUnitRingFreeThread(PreparationThread);
#endif
    if (0 != PreparationWorker)
        SpdStorageUnitRingFreeWorker(StorageUnit->Ioq->DeviceExtension,
            PreparationWorker, "preparation open cleanup");
    if (0 != PreparationRetryWorker)
        IoFreeWorkItem(PreparationRetryWorker);
    if (0 != RundownWorker)
        IoFreeWorkItem(RundownWorker);
    if (0 != RingProcess)
        ObDereferenceObject(RingProcess);
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
    if (SlotsAttached)
    {
        SPD_WORK_SLOT *Detached = SpdIoqRingDetachSlots(StorageUnit->Ioq);
        ASSERT(Detached == Slots);
        UNREFERENCED_PARAMETER(Detached);
    }
    if (0 != Slots)
        SpdFree(Slots, SpdTagStorageUnit);
    if (NT_SUCCESS(Result))
        SpdStorageUnitRingSchedulePreparation(StorageUnit);
    return Result;
}

static VOID SpdRingLogBatchSizes(PCSTR Name, const UINT64 *Sizes, UINT32 Limit)
{
    UINT64 Batches = 0, Requests = 0;
    for (UINT32 I = 1; I <= Limit; I++)
    {
        Batches += Sizes[I];
        Requests += Sizes[I] * I;
        if (0 != Sizes[I])
            DbgPrint(DRIVER_NAME ": batch_histogram name=%s size=%lu count=%I64u\n", Name, I, Sizes[I]);
    }
    DbgPrint(DRIVER_NAME ": batch_summary name=%s batches=%I64u requests=%I64u empty=%I64u\n",
        Name, Batches, Requests, Sizes[0]);
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

    if (0 != InterlockedCompareExchange(&Ring->CloseState, 1, 0))
    {
        KeWaitForSingleObject(&Ring->CloseDoneEvent, Executive, KernelMode, FALSE, NULL);
        return;
    }

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    if (0 == Ring->SectionHandle)
    {
        Ring->Stopping = TRUE;
        KeReleaseSpinLock(&Ring->Lock, Irql);
        InterlockedExchange(&Ring->CloseState, 2);
        KeSetEvent(&Ring->CloseDoneEvent, IO_NO_INCREMENT, FALSE);
        return;
    }
    Ring->Stopping = TRUE;
    KeReleaseSpinLock(&Ring->Lock, Irql);

    SpdIoqReset(StorageUnit->Ioq, TRUE,
        SpdIoqResetReasonRingClose);
    SpdIoqRingBeginClose(StorageUnit->Ioq);
    SpdStorageUnitRingCancelWait(StorageUnit);
    KeWaitForSingleObject(&Ring->IdleEvent,
        Executive, KernelMode, FALSE, NULL);
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
#if SPD_RING_PREPARATION_THREAD
    SpdStorageUnitRingFreeThread(Ring->PreparationThread);
    Ring->PreparationThread = 0;
#endif
#if SPD_RING_COMPLETION_THREAD
    SpdStorageUnitRingFreeThread(Ring->CompletionThread);
    Ring->CompletionThread = 0;
#endif
    /* CLOSE follows backend worker/response rundown (or owner-process exit).
     * Kernel owners have left; release SRB pages held by terminal reset. */
    for (;;)
    {
        SPD_WORK_SLOT *Slots[SPD_RING_COMPLETION_LIMIT];
        UCHAR Statuses[SPD_RING_COMPLETION_LIMIT], FinalStatuses[SPD_RING_COMPLETION_LIMIT];
        PVOID Srbs[SPD_RING_COMPLETION_LIMIT];
        SPD_RING_RETIRE_TIMING Timings[SPD_RING_COMPLETION_LIMIT];
        UINT32 Count = SpdIoqRingClaimMappedCleanup(StorageUnit->Ioq,
            Slots, SPD_RING_COMPLETION_LIMIT);
        if (0 == Count)
            break;
        for (UINT32 I = 0; I < Count; I++)
        {
            SpdRingUnmapPayload(StorageUnit, Slots[I], UserProcessExiting);
            Statuses[I] = SRB_STATUS_ABORTED;
        }
        UINT32 Retired = SpdIoqRingFinishCompletedBatch(StorageUnit->Ioq,
            Slots, Statuses, Count, Srbs, FinalStatuses, Timings);
        for (UINT32 I = 0; I < Retired; I++)
            SpdSrbComplete(StorageUnit->Ioq->DeviceExtension, Srbs[I], FinalStatuses[I]);
    }
    Delay.QuadPart = -100000; /* 10 ms */
    for (;;)
    {
        LeasedCount = SpdIoqRingLeasedCount(StorageUnit->Ioq);
        if (0 == LeasedCount)
            break;
        if (0 == (++WaitAttempts % 500))
            DbgPrint(DRIVER_NAME ": SharedRing close waiting for %lu copy/reset owners\n",
                LeasedCount);
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
#if DBG
    ASSERT(0 == Ring->ProducerExecuting);
    ASSERT(0 == Ring->CompletionExecuting);
    ASSERT(0 == Ring->CompletionLiveReferences);
    ASSERT(SPD_RING_PRODUCER_IDLE == Ring->CompletionState);
    ASSERT(0 == Ring->PreparationExecuting);
    ASSERT(0 == Ring->PreparationLiveReferences);
    ASSERT(SPD_RING_PRODUCER_IDLE == Ring->PreparationState);
    ASSERT(0 == Ring->ProducerLiveReferences);
    ASSERT(0 == Ring->ActiveCalls);
    ASSERT(SPD_RING_PRODUCER_IDLE == InterlockedCompareExchange(
        &Ring->ProducerState, SPD_RING_PRODUCER_IDLE,
        SPD_RING_PRODUCER_IDLE));
#endif
    DbgPrint(DRIVER_NAME ": SharedRing close depth=%lu buffers=%lu "
        "sq_full=%I64d buffer_starved=%I64d waits=%I64d/%I64d "
        "producer_runs=%I64d producer_reruns=%I64d busy_deferrals=%I64d "
        "produced=%I64d batches=[%I64d,%I64d,%I64d,%I64d,%I64d] max_batch=%ld\n",
        Ring->QueueDepth, Ring->BufferCount,
        Ring->SqFullEvents, Ring->BufferPoolExhaustions,
        Ring->WaitSubmissions, Ring->WaitCompletions,
        Ring->ProducerWorkerRuns, Ring->ProducerWorkerReruns,
        Ring->ProducerBusyDeferrals,
        Ring->ProducerProduced, Ring->ProducerBatchLe1,
        Ring->ProducerBatch2To4, Ring->ProducerBatch5To16,
        Ring->ProducerBatch17To64, Ring->ProducerBatchOver64,
        Ring->ProducerMaxBatch);
    DbgPrint(DRIVER_NAME ": SharedRing execution preparation_thread=%u completion_thread=%u preparation_threads=%u completion_threads=%u preparation_parallel=%ld completion_parallel=%ld completion_claim_limit=%u ioq_queued_spin_lock=%u thread_priority=%u\n",
        SPD_RING_PREPARATION_THREAD, SPD_RING_COMPLETION_THREAD,
        SPD_RING_PREPARATION_THREAD ? SPD_RING_PREPARATION_THREADS : 0,
        SPD_RING_COMPLETION_THREAD ? SPD_RING_COMPLETION_THREADS : 0,
        Ring->PreparationMaxOwnedBatches, Ring->CompletionMaxOwnedBatches,
        SPD_RING_COMPLETION_CLAIM_LIMIT, SPD_IOQ_QUEUED_SPIN_LOCK,
        SPD_RING_THREAD_PRIORITY);
    (void)KeQueryPerformanceCounter(&QpcFrequency);
    DbgPrint(DRIVER_NAME ": SharedRing copy prepare_ticks=%I64d completion_ticks=%I64d deferred_aborts=%I64d qpc_hz=%I64d\n",
        Ring->PreparationCopyTicks, Ring->CompletionCopyTicks,
        Ring->DeferredSrbAborts, QpcFrequency.QuadPart);
    DbgPrint(DRIVER_NAME ": SharedRing preparation runs=%I64d reruns=%I64d busy_deferrals=%I64d claimed=%I64d commits=%I64d max_claim=%ld callback_ticks=%I64d\n",
        Ring->PreparationWorkerRuns, Ring->PreparationWorkerReruns,
        Ring->PreparationBusyDeferrals, Ring->PreparationClaimed,
        Ring->PreparationCommits, Ring->PreparationMaxClaim,
        Ring->PreparationCallbackTicks);
    DbgPrint(DRIVER_NAME ": SharedRing completion runs=%I64d reruns=%I64d busy_deferrals=%I64d claimed=%I64d max_claim=%ld callback_ticks=%I64d\n",
        Ring->CompletionWorkerRuns, Ring->CompletionWorkerReruns,
        Ring->CompletionBusyDeferrals, Ring->CompletionClaimed,
        Ring->CompletionMaxClaim, Ring->CompletionCallbackTicks);
    DbgPrint(DRIVER_NAME ": SharedRing preparation_limits claim=%lu claim_bytes=%lu commit=%lu commit_bytes=%lu time_us=%lu copied_bytes=%I64u oversized=%I64u budget_violations=%I64u\n",
        (ULONG)SPD_RING_CLAIM_LIMIT, (ULONG)SPD_RING_PREPARE_COPY_LIMIT,
        (ULONG)SPD_RING_READY_COMMIT_LIMIT, (ULONG)SPD_RING_READY_COPY_LIMIT,
        (ULONG)SPD_RING_PREPARE_TIME_US, Ring->PreparationClaimCopyBytes,
        Ring->PreparationOversizedClaims, Ring->PreparationCopyBudgetViolations);
    SpdHistogramLog(&Ring->PendingToPreparing, "pending_to_preparing");
    SpdHistogramLog(&Ring->PreparingToReady, "preparing_to_ready");
    SpdHistogramLog(&Ring->ReadyToSq, "ready_to_sq");
    SpdHistogramLog(&Ring->CqToRetire, "cq_to_retire");
    SpdHistogramLog(&Ring->CqToComplete, "cq_to_complete");
    SpdHistogramLog(&Ring->FlushRoundTrip, "flush_round_trip");
    SpdHistogramLog(&Ring->PreparationDuration, "preparation_callback");
    SpdHistogramLog(&Ring->CompletionDuration, "completion_callback");
    SpdRingLogBatchSizes("preparation_claim", Ring->PreparationClaimSizes, SPD_RING_CLAIM_LIMIT);
    SpdRingLogBatchSizes("ready_commit", Ring->PreparationCommitSizes, SPD_RING_CLAIM_LIMIT);
    SpdRingLogBatchSizes("completion_claim", Ring->CompletionClaimSizes, SPD_RING_COMPLETION_CLAIM_LIMIT);
    DbgPrint(DRIVER_NAME ": SharedRing noncdb_flush=%I64d shutdown=%I64d\n",
        Ring->NonCdbFlushSubmissions, Ring->ShutdownSubmissions);
    DbgPrint(DRIVER_NAME ": SharedRing continuations producer=%I64d preparation=%I64d completion=%I64d\n",
        Ring->ProducerContinuations, Ring->PreparationContinuations, Ring->CompletionContinuations);
    SPD_DEVICE_EXTENSION *PerfDevice = StorageUnit->Ioq->DeviceExtension;
    DbgPrint(DRIVER_NAME ": SharedRing completion_perf mode=%lu version=%lu query_status=%lu apply_status=%lu supported=%lu requested=%lu applied=%lu\n",
        PerfDevice->CompletionPerfMode, PerfDevice->CompletionPerfVersion,
        PerfDevice->CompletionPerfQueryStatus, PerfDevice->CompletionPerfApplyStatus,
        PerfDevice->CompletionPerfSupported, PerfDevice->CompletionPerfRequested,
        PerfDevice->CompletionPerfApplied);
#if SPD_RING_LATENCY_DIAGNOSTICS
    static const PCSTR LatencyNames[SpdLatencyCount] = {
        "lat_pending", "lat_prepare", "lat_ready", "lat_flight",
        "lat_cq_queue", "lat_completion_batch_wait", "lat_completion_service",
        "lat_completion_retire_wait", "lat_notify_wait", "lat_notify_service",
        "lat_kernel_total", "lat_cq_notify", "lat_completion_enqueue_lock",
        "lat_completion_claim_lock", "lat_completion_retire_lock",
        "lat_preparation_dispatch", "lat_producer_dispatch",
        "lat_completion_dispatch", "lat_kick", "lat_completion_backlog", "lat_cq_depth"
    };
    DbgPrint(DRIVER_NAME ": SharedRing latency_diagnostics=1 kick_calls=%I64d kick_busy=%I64d queued_completions=%u\n",
        Ring->KickCalls, Ring->KickBusy, StorageUnit->Ioq->CompletionQueuedCount);
    for (UINT32 I = 0; I < SpdLatencyCount; I++)
        SpdLatencyLog(&Ring->Latency[I], LatencyNames[I], I >= SpdLatencyCompletionBacklog);
#endif
    DbgPrint(DRIVER_NAME ": SharedRing mapping threshold=%u attempts=%I64d mapped=%I64d fallback=%I64d unmapped=%I64d bytes=%I64d map_ticks=%I64d unmap_ticks=%I64d mdl_flags=%lx\n",
        SPD_RING_MAP_THRESHOLD_BYTES, Ring->MapAttempts, Ring->MapSuccesses,
        Ring->MapFallbacks, Ring->MapUnmaps, Ring->MapBytes, Ring->MapTicks, Ring->UnmapTicks,
        Ring->MapSourceFlagsOr);
    ASSERT(Ring->MapSuccesses == Ring->MapUnmaps);
    DbgPrint(DRIVER_NAME ": SharedRing mapping-origin samples=%I64d user=%I64d kernel=%I64d unknown=%I64d self=%I64d\n",
        Ring->MapOriginSamples, Ring->MapOriginUser, Ring->MapOriginKernel,
        Ring->MapOriginUnknown, Ring->MapOriginSelf);
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
    /* Retry callbacks are included in ActiveCalls/IdleEvent rundown. */
    ASSERT(!Ring->ProducerRetryQueued);
    IoFreeWorkItem(Ring->ProducerRetryWorker);
    Ring->ProducerRetryWorker = 0;
    ASSERT(!Ring->CompletionRetryQueued);
    SpdStorageUnitRingFreeWorker(StorageUnit->Ioq->DeviceExtension,
        Ring->CompletionWorker, "completion close");
    Ring->CompletionWorker = 0;
    if (0 != Ring->CompletionRetryWorker)
        IoFreeWorkItem(Ring->CompletionRetryWorker);
    Ring->CompletionRetryWorker = 0;
    ASSERT(!Ring->PreparationRetryQueued);
    SpdStorageUnitRingFreeWorker(StorageUnit->Ioq->DeviceExtension,
        Ring->PreparationWorker, "preparation close");
    Ring->PreparationWorker = 0;
    if (0 != Ring->PreparationRetryWorker)
        IoFreeWorkItem(Ring->PreparationRetryWorker);
    Ring->PreparationRetryWorker = 0;
    SpdStorageUnitRingReset(StorageUnit, UserProcessExiting);
    InterlockedExchange(&Ring->CloseState, 2);
    KeSetEvent(&Ring->CloseDoneEvent, IO_NO_INCREMENT, FALSE);
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

#if defined(WINSPD_TEST_BUILD)
static VOID SpdRingTestOpenCommitBarrier(VOID)
{
    PKEVENT Entered = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestOpenCommitEntered");
    PKEVENT Release = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestOpenCommitRelease");
    SpdRingTestBarrier(Entered, Release);
    if (0 != Entered)
        ObDereferenceObject(Entered);
    if (0 != Release)
        ObDereferenceObject(Release);
}
#endif

VOID SpdStorageUnitRingTestResetRetireBarrier(SPD_STORAGE_UNIT *StorageUnit)
{
    UNREFERENCED_PARAMETER(StorageUnit);
#if defined(WINSPD_TEST_BUILD)
    if (PASSIVE_LEVEL != KeGetCurrentIrql())
        return;
    /* Only the reset/CLOSE race test creates these locally referenced events. */
    PKEVENT Entered = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestResetRetireEntered");
    PKEVENT Release = SpdRingTestOpenEvent(
        L"\\BaseNamedObjects\\WinSpdSharedRingTestResetRetireRelease");
    if (0 != Entered && 0 != Release && 0 == KeReadStateEvent(Entered))
        SpdRingTestBarrier(Entered, Release);
    if (0 != Entered)
        ObDereferenceObject(Entered);
    if (0 != Release)
        ObDereferenceObject(Release);
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

static VOID SpdRingEnterOwnedBatch(volatile LONG *Active, volatile LONG *Peak)
{
    LONG Count = InterlockedIncrement(Active);
    LONG Previous = InterlockedCompareExchange(Peak, 0, 0);
    while (Count > Previous)
    {
        LONG Observed = InterlockedCompareExchange(Peak, Count, Previous);
        if (Previous == Observed)
            break;
        Previous = Observed;
    }
}

#if defined(WINSPD_TEST_BUILD)
static VOID SpdRingTestParallelBatch(SPD_RING_STATE *Ring, BOOLEAN Preparation)
{
    if (InterlockedCompareExchange(Preparation ? &Ring->PreparationOwnedBatches :
            &Ring->CompletionOwnedBatches, 0, 0) < 2)
        return;
    /* Only parallel-worker tests create this event. It proves a second batch
     * owns slots while the first worker is held at an existing copy barrier. */
    PKEVENT Event = SpdRingTestOpenEvent(Preparation ?
        L"\\BaseNamedObjects\\WinSpdSharedRingTestParallelPrepare" :
        L"\\BaseNamedObjects\\WinSpdSharedRingTestParallelComplete");
    if (0 != Event)
    {
        KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
        ObDereferenceObject(Event);
    }
}
#endif

/* These helpers run only while a worker owns the slot, or after CLOSE rundown.
 * The source MDL/page locks are borrowed and live until final SRB completion. */
static VOID SpdRingUnmapPayload(SPD_STORAGE_UNIT *StorageUnit,
    SPD_WORK_SLOT *Slot, BOOLEAN UserProcessExiting)
{
    if (0 != Slot->MappedUserAddress)
    {
        LARGE_INTEGER Start = KeQueryPerformanceCounter(0);
        if (!UserProcessExiting)
        {
            KAPC_STATE ApcState;
            BOOLEAN Attached = StorageUnit->RingProcess != PsGetCurrentProcess();
            if (Attached)
                KeStackAttachProcess(StorageUnit->RingProcess, &ApcState);
            MmUnmapLockedPages(Slot->MappedUserAddress, Slot->MappingMdl);
            if (Attached)
                KeUnstackDetachProcess(&ApcState);
        }
        Slot->MappedUserAddress = 0;
        InterlockedIncrement64(&StorageUnit->Ring.MapUnmaps);
        InterlockedExchangeAdd64(&StorageUnit->Ring.UnmapTicks,
            KeQueryPerformanceCounter(0).QuadPart - Start.QuadPart);
    }
    if (0 != Slot->MappingMdl)
    {
        IoFreeMdl(Slot->MappingMdl);
        Slot->MappingMdl = 0;
    }
}

static BOOLEAN SpdRingTryMapPayload(SPD_STORAGE_UNIT *StorageUnit, SPD_WORK_SLOT *Slot)
{
    SPD_SRB_EXTENSION *Extension = Slot->Srb;
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    PMDL Source = Extension->OriginalMdl;
    ULONG Length = min(Extension->SystemDataLength - Extension->ChunkOffset,
        StorageUnit->StorageUnitParams.MaxTransferLength);
#if defined(WINSPD_TEST_BUILD)
    if (0 != StorageUnit->TestChunkLimit)
        Length = min(Length, StorageUnit->TestChunkLimit);
#endif
    if (!Slot->MappingLease)
        return FALSE;
    InterlockedIncrement64(&Ring->MapAttempts);
    LARGE_INTEGER Start = KeQueryPerformanceCounter(0);
    BOOLEAN Mapped = FALSE;
    if (0 != Source)
        InterlockedOr(&Ring->MapSourceFlagsOr, Source->MdlFlags);
    /* No chained MDLs or partial pages in this first experiment. */
    /* A partial MDL borrows its parent's locks; the outstanding SRB retains
     * those pages. Do not require the partial descriptor to own page locks. */
    if (0 != Source && 0 == Source->Next &&
        (Source->MdlFlags & (MDL_PAGES_LOCKED | MDL_PARTIAL)) &&
        0 != Length && 0 == (Length & (PAGE_SIZE - 1)))
    {
        PVOID Base = MmGetSystemAddressForMdlSafe(Source, NormalPagePriority | MdlMappingNoExecute);
        SIZE_T Offset = (UINT_PTR)Extension->SystemDataBuffer - (UINT_PTR)Base;
        SIZE_T Bytes = MmGetMdlByteCount(Source);
        if (0 != Base && Offset <= Bytes && Extension->SystemDataLength <= Bytes - Offset &&
            Extension->ChunkOffset <= Bytes - Offset &&
            Length <= Bytes - Offset - Extension->ChunkOffset)
        {
            Offset += Extension->ChunkOffset;
            PVOID Address = (PUINT8)MmGetMdlVirtualAddress(Source) + Offset;
            if (0 == ((UINT_PTR)Address & (PAGE_SIZE - 1)))
            {
                Slot->MappingMdl = IoAllocateMdl(Address, Length, FALSE, FALSE, NULL);
                if (0 != Slot->MappingMdl)
                {
                    IoBuildPartialMdl(Source, Slot->MappingMdl, Address, Length);
                    KAPC_STATE ApcState;
                    BOOLEAN Attached = StorageUnit->RingProcess != PsGetCurrentProcess();
                    if (Attached)
                        KeStackAttachProcess(StorageUnit->RingProcess, &ApcState);
                    __try
                    {
                        PCDB Cdb = SrbGetCdb(Extension->Srb);
                        UCHAR Op = Cdb->AsByte[0];
                        BOOLEAN Write = SCSIOP_WRITE6 == Op || SCSIOP_WRITE == Op ||
                            SCSIOP_WRITE12 == Op || SCSIOP_WRITE16 == Op;
                        Slot->MappedUserAddress = MmMapLockedPagesSpecifyCache(
                            Slot->MappingMdl, UserMode, MmCached, NULL, FALSE,
                            NormalPagePriority | (Write ? MdlMappingNoWrite : 0));
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        Slot->MappedUserAddress = 0;
                    }
                    if (Attached)
                        KeUnstackDetachProcess(&ApcState);
                    Mapped = 0 != Slot->MappedUserAddress;
                }
            }
        }
    }
    InterlockedExchangeAdd64(&Ring->MapTicks, KeQueryPerformanceCounter(0).QuadPart - Start.QuadPart);
    if (Mapped)
    {
        InterlockedIncrement64(&Ring->MapSuccesses);
        InterlockedExchangeAdd64(&Ring->MapBytes, Length);
    }
    else
    {
        SpdRingUnmapPayload(StorageUnit, Slot, FALSE);
        InterlockedIncrement64(&Ring->MapFallbacks);
    }
    return Mapped;
}

static BOOLEAN SpdRingPrepareBatch(SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_IOQ *Ioq = StorageUnit->Ioq;
    SPD_WORK_SLOT *Slots[SPD_RING_CLAIM_LIMIT];
    SPD_RING_REQUEST Requests[SPD_RING_CLAIM_LIMIT];
    PVOID AbortedSrbs[SPD_RING_CLAIM_LIMIT];
    UINT32 Count = 0, First = 0, GroupBytes = 0;
    BOOLEAN Failed = FALSE;
    NTSTATUS Status = SpdIoqRingClaimBatch(Ioq, Slots, SPD_RING_CLAIM_LIMIT,
        SPD_RING_PREPARE_COPY_LIMIT, &Count);
    if (!NT_SUCCESS(Status) && STATUS_CANCELLED != Status)
        Failed = TRUE;
    InterlockedExchangeAdd64(&Ring->PreparationClaimed, Count);
    InterlockedIncrement64((volatile LONG64 *)&Ring->PreparationClaimSizes[Count]);
    if (0 != Count)
    {
        SpdRingEnterOwnedBatch(&Ring->PreparationOwnedBatches, &Ring->PreparationMaxOwnedBatches);
#if defined(WINSPD_TEST_BUILD)
        SpdRingTestParallelBatch(Ring, TRUE);
#endif
    }
    for (;;)
    {
        LONG Maximum = InterlockedCompareExchange(&Ring->PreparationMaxClaim, 0, 0);
        if ((LONG)Count <= Maximum || Maximum == InterlockedCompareExchange(
                &Ring->PreparationMaxClaim, (LONG)Count, Maximum))
            break;
    }
    RtlZeroMemory(Requests, sizeof Requests);
    for (UINT32 I = 0; I < Count; I++)
    {
        if (!Failed)
        {
            UINT32 Index = (UINT32)(Slots[I] - Ioq->Slots);
            PVOID DataBuffer = SpdRingBufferAddress(Ring, Index, Ring->BufferSize);
            UINT32 Length;
            BOOLEAN NeedsBuffer;
            if (0 == DataBuffer)
                Failed = TRUE;
            else
            {
                if (SpdRingTryMapPayload(StorageUnit, Slots[I]))
                    DataBuffer = (PUINT8)Slots[I]->Srb->SystemDataBuffer + Slots[I]->Srb->ChunkOffset;
                GroupBytes += SpdSrbPreparationCopyBytes(Slots[I]->Srb);
                SpdStorageUnitRingTestPrepareCopyBarrier(StorageUnit);
                LARGE_INTEGER Start = KeQueryPerformanceCounter(0);
                SpdSrbExecuteScsiPrepare(Slots[I]->Srb, &Requests[I].Request, DataBuffer);
                InterlockedExchangeAdd64(&Ring->PreparationCopyTicks,
                    KeQueryPerformanceCounter(0).QuadPart - Start.QuadPart);
                if (0 == Requests[I].Request.Hint || !SpdRingGetRequestDataLength(
                        StorageUnit, &Requests[I].Request, &Length, &NeedsBuffer))
                    Failed = TRUE;
                else
                {
                    Requests[I].Data.BufferId = NeedsBuffer ? Index : SPD_RING_NO_BUFFER;
                    Requests[I].Data.Length = Length;
                }
            }
        }
        if (Failed || I + 1 == Count || I + 1 - First >= SPD_RING_READY_COMMIT_LIMIT ||
            GroupBytes >= SPD_RING_READY_COPY_LIMIT ||
            SpdSrbPreparationCopyBytes(Slots[I + 1]->Srb) >
                SPD_RING_READY_COPY_LIMIT - GroupBytes)
        {
            if (Failed)
            {
                SpdStorageUnitRingMarkFailed(StorageUnit);
                /* Reset flags the remaining preparation owners; retire all of
                 * this claimed batch, including any unprepared descriptors. */
                I = Count - 1;
            }
            UINT32 GroupCount = I + 1 - First;
            BOOLEAN QueueCleanup;
            UINT32 Aborted = SpdIoqRingCommitBatch(Ioq, Slots + First,
                Requests + First, GroupCount, Failed, AbortedSrbs, &QueueCleanup);
            if (QueueCleanup)
                SpdStorageUnitRingScheduleCompletion(StorageUnit);
            InterlockedIncrement64(&Ring->PreparationCommits);
            InterlockedIncrement64((volatile LONG64 *)&Ring->PreparationCommitSizes[GroupCount - Aborted]);
            for (UINT32 J = 0; J < Aborted; J++)
                SpdSrbComplete(Ioq->DeviceExtension, AbortedSrbs[J], SRB_STATUS_ABORTED);
            /* Every committed lease has transferred ownership. Do not touch
             * those slots: publication or cancellation may already reuse them. */
            if (GroupCount > Aborted)
                SpdStorageUnitRingScheduleProducer(StorageUnit);
#if defined(WINSPD_TEST_BUILD)
            if (I + 1 < Count && !Failed)
                SpdRingTestBarrier(Ring->TestPrepareCommitEnteredEvent,
                    Ring->TestPrepareCommitReleaseEvent);
#endif
            First = I + 1;
            GroupBytes = 0;
        }
    }
    /* Generation exhaustion can occur before the first lease. */
    if (Failed && 0 == Count)
        SpdStorageUnitRingMarkFailed(StorageUnit);
    if (0 != Count)
        InterlockedDecrement(&Ring->PreparationOwnedBatches);
    return 0 != Count && !Failed;
}

static BOOLEAN SpdRingCompleteBatch(SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_IOQ *Ioq = StorageUnit->Ioq;
    SPD_WORK_SLOT *Slots[SPD_RING_COMPLETION_CLAIM_LIMIT];
    UCHAR Statuses[SPD_RING_COMPLETION_CLAIM_LIMIT], FinalStatuses[SPD_RING_COMPLETION_CLAIM_LIMIT];
    PVOID Srbs[SPD_RING_COMPLETION_CLAIM_LIMIT];
    SPD_RING_RETIRE_TIMING Timings[SPD_RING_COMPLETION_CLAIM_LIMIT];
#if defined(WINSPD_TEST_BUILD)
    if (SpdIoqRingHasCompleted(Ioq))
        SpdRingTestBarrier(Ring->TestCompletionClaimEnteredEvent,
            Ring->TestCompletionClaimReleaseEvent);
#endif
    UINT32 Count = SpdIoqRingClaimCompletedBatch(Ioq, Slots, SPD_RING_COMPLETION_CLAIM_LIMIT);
    InterlockedExchangeAdd64(&Ring->CompletionClaimed, Count);
    InterlockedIncrement64((volatile LONG64 *)&Ring->CompletionClaimSizes[Count]);
    if (0 != Count)
    {
        SpdRingEnterOwnedBatch(&Ring->CompletionOwnedBatches, &Ring->CompletionMaxOwnedBatches);
#if defined(WINSPD_TEST_BUILD)
        SpdRingTestParallelBatch(Ring, FALSE);
#endif
    }
    for (;;)
    {
        LONG Maximum = InterlockedCompareExchange(&Ring->CompletionMaxClaim, 0, 0);
        if ((LONG)Count <= Maximum || Maximum == InterlockedCompareExchange(
                &Ring->CompletionMaxClaim, (LONG)Count, Maximum))
            break;
    }
    for (UINT32 I = 0; I < Count; I++)
    {
        SPD_WORK_SLOT *Slot = Slots[I];
        PVOID DataBuffer = 0 != Slot->MappedUserAddress ?
            (PUINT8)Slot->Srb->SystemDataBuffer + Slot->Srb->ChunkOffset :
            SpdIoctlTransactFlushKind == Slot->Request.Kind ? 0 :
            SpdRingBufferAddress(Ring, (UINT32)(Slot - Ioq->Slots), Slot->DataLength);
        LARGE_INTEGER CopyStart = KeQueryPerformanceCounter(0);
#if SPD_RING_LATENCY_DIAGNOSTICS
        SpdLatencyRecord(&Ring->Latency[SpdLatencyCompletionBatchWait], CopyStart.QuadPart - Slot->CompletionClaimTicks);
#endif
        Statuses[I] = Slot->CancelRequested ? SRB_STATUS_ABORTED :
            SpdSrbExecuteScsiComplete(Slot->Srb, &Slot->Response, DataBuffer);
        SpdRingUnmapPayload(StorageUnit, Slot, FALSE);
        UINT64 CopyEnd = KeQueryPerformanceCounter(0).QuadPart;
        InterlockedExchangeAdd64(&Ring->CompletionCopyTicks, CopyEnd - CopyStart.QuadPart);
#if SPD_RING_LATENCY_DIAGNOSTICS
        Slot->CompletionServiceEnd = CopyEnd;
        SpdLatencyRecord(&Ring->Latency[SpdLatencyCompletionService], CopyEnd - CopyStart.QuadPart);
#endif
    }
    UINT32 Retired = SpdIoqRingFinishCompletedBatch(Ioq, Slots, Statuses,
        Count, Srbs, FinalStatuses, Timings);
    /* No slot may be touched after retirement. Copies have finished and every
     * released slot is available to preparation, including next-chunk leases. */
    for (UINT32 I = 0; I < Retired; I++)
    {
        SpdHistogramRecord(&Ring->CqToComplete,
            KeQueryPerformanceCounter(0).QuadPart - Timings[I].CqTicks);
#if SPD_RING_LATENCY_DIAGNOSTICS
        UINT64 NotifyStart = KeQueryPerformanceCounter(0).QuadPart;
        SpdLatencyRecord(&Ring->Latency[SpdLatencyNotifyWait], NotifyStart - Timings[I].RetiredTicks);
        SpdLatencyRecord(&Ring->Latency[SpdLatencyKernelTotal], NotifyStart - Timings[I].AcceptedTicks);
        SpdLatencyRecord(&Ring->Latency[SpdLatencyCqNotify], NotifyStart - Timings[I].CqTicks);
#endif
        SpdSrbComplete(Ioq->DeviceExtension, Srbs[I], FinalStatuses[I]);
#if SPD_RING_LATENCY_DIAGNOSTICS
        SpdLatencyRecord(&Ring->Latency[SpdLatencyNotifyService], KeQueryPerformanceCounter(0).QuadPart - NotifyStart);
#endif
    }
    if (0 != Count && SpdIoqRingHasPending(Ioq))
        SpdStorageUnitRingSchedulePreparation(StorageUnit);
    /* Acknowledge earlier wakeups before the authoritative queue check.
     * A later enqueue either appears in that check or raises RERUN again. */
    if (0 != Count)
        InterlockedDecrement(&Ring->CompletionOwnedBatches);
#if !SPD_RING_COMPLETION_THREAD
    InterlockedCompareExchange(&Ring->CompletionState,
        SPD_RING_PRODUCER_ACTIVE, SPD_RING_PRODUCER_RERUN);
#endif
    return SpdIoqRingHasCompleted(Ioq);
}

#if SPD_RING_PREPARATION_THREAD || SPD_RING_COMPLETION_THREAD
static VOID SpdStorageUnitRingRunThread(SPD_RING_THREAD *Thread)
{
    SPD_STORAGE_UNIT *StorageUnit = Thread->StorageUnit;
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    BOOLEAN Preparation = Thread->Preparation;
    for (;;)
    {
        KIRQL Irql;
        KeAcquireSpinLock(&Ring->Lock, &Irql);
        BOOLEAN Stopping = Ring->Stopping || Ring->Failed;
        KeReleaseSpinLock(&Ring->Lock, Irql);
        Stopping = Stopping || SpdIoqStopped(StorageUnit->Ioq);
        /* Acknowledge earlier wakeups before checking/claiming the queue.
         * Enqueues after this exchange either get claimed or set RERUN. */
        InterlockedExchange(&Thread->State, SPD_RING_PRODUCER_ACTIVE);
        InterlockedIncrement64(Preparation ?
            &Ring->PreparationWorkerRuns : &Ring->CompletionWorkerRuns);
#if SPD_RING_LATENCY_DIAGNOSTICS
        SpdLatencyRecord(&Ring->Latency[Preparation ?
                SpdLatencyPreparationDispatch : SpdLatencyCompletionDispatch],
            KeQueryPerformanceCounter(0).QuadPart - Thread->QueuedTicks);
#endif
        BOOLEAN More = FALSE;
        LARGE_INTEGER Frequency;
        LARGE_INTEGER Start = KeQueryPerformanceCounter(&Frequency);
        if (!Stopping)
        {
#if DBG
            LONG Executing = InterlockedIncrement(Preparation ?
                &Ring->PreparationExecuting : &Ring->CompletionExecuting);
            ASSERT(Executing <= (Preparation ? SPD_RING_PREPARATION_THREADS : SPD_RING_COMPLETION_THREADS));
#endif
            do
            {
                More = Preparation ? SpdRingPrepareBatch(StorageUnit) : SpdRingCompleteBatch(StorageUnit);
            } while (Preparation && More &&
                KeQueryPerformanceCounter(0).QuadPart - Start.QuadPart <
                    Frequency.QuadPart * SPD_RING_PREPARE_TIME_US / 1000000);
#if DBG
            InterlockedDecrement(Preparation ? &Ring->PreparationExecuting : &Ring->CompletionExecuting);
#endif
        }
        UINT64 Duration = KeQueryPerformanceCounter(0).QuadPart - Start.QuadPart;
        InterlockedExchangeAdd64(Preparation ?
            &Ring->PreparationCallbackTicks : &Ring->CompletionCallbackTicks, Duration);
        SpdHistogramRecord(Preparation ? &Ring->PreparationDuration : &Ring->CompletionDuration, Duration);
        if (Stopping)
        {
            InterlockedExchange(&Thread->State, SPD_RING_PRODUCER_IDLE);
            break;
        }
        if (More)
            InterlockedCompareExchange(&Thread->State,
                SPD_RING_PRODUCER_RERUN, SPD_RING_PRODUCER_ACTIVE);
        if (SPD_RING_PRODUCER_ACTIVE == InterlockedCompareExchange(&Thread->State,
                SPD_RING_PRODUCER_IDLE, SPD_RING_PRODUCER_ACTIVE))
            break;
        Thread->QueuedTicks = KeQueryPerformanceCounter(0).QuadPart;
        InterlockedIncrement64(Preparation ?
            &Ring->PreparationContinuations : &Ring->CompletionContinuations);
    }
    if (Preparation)
        SpdStorageUnitRingReleasePreparationReference(StorageUnit);
    else
        SpdStorageUnitRingReleaseCompletionReference(StorageUnit);
}
#endif

static UINT32 SpdRingProduceBatch(SPD_STORAGE_UNIT *StorageUnit,
    BOOLEAN *PBatchLimitReached)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_IOQ *Ioq = StorageUnit->Ioq;
    SPD_RING_HEADER *Header = Ring->SystemAddress;
    UINT32 Published, Used;
    BOOLEAN InvalidHead;
    *PBatchLimitReached = FALSE;
#if defined(WINSPD_TEST_BUILD)
    if (SpdIoqRingHasReady(Ioq))
        SpdRingTestBarrier(Ring->TestReadyEnteredEvent, Ring->TestReadyReleaseEvent);
#endif
    Published = SpdIoqRingPublishBatch(Ioq, SPD_RING_PUBLISH_LIMIT, &InvalidHead);
    if (InvalidHead)
        SpdStorageUnitRingMarkFailed(StorageUnit);
    if (0 != Published)
    {
        InterlockedExchangeAdd64(&Ring->ProducerProduced, (LONG64)Published);
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
            LONG Maximum = InterlockedCompareExchange(&Ring->ProducerMaxBatch, 0, 0);
            if ((LONG)Published <= Maximum || Maximum == InterlockedCompareExchange(
                    &Ring->ProducerMaxBatch, (LONG)Published, Maximum))
                break;
        }
    }
    InterlockedCompareExchange(&Ring->ProducerState,
        SPD_RING_PRODUCER_ACTIVE, SPD_RING_PRODUCER_RERUN);
    if (SpdIoqRingHasReady(Ioq))
    {
        Used = Ring->RequestTail - SpdRingLoadAcquire32(&Header->RequestHead.Value);
        if (Used < Ring->QueueDepth)
            *PBatchLimitReached = TRUE;
        else if (Used == Ring->QueueDepth)
            InterlockedIncrement64(&Ring->SqFullEvents);
        else if (Used > Ring->QueueDepth)
            SpdStorageUnitRingMarkFailed(StorageUnit);
    }
    return Published;
}

static VOID SpdStorageUnitRingCancelWaitIrp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    SPD_STORAGE_UNIT *StorageUnit = Irp->Tail.Overlay.DriverContext[0];
    SPD_DEVICE_EXTENSION *DeviceExtension = StorageUnit->Ioq->DeviceExtension;
    UNREFERENCED_PARAMETER(DeviceObject);

    /* Registration and normal dequeue use the I/O cancel lock. The retained
     * WAIT reference belongs to exactly one completion path. */
    ASSERT(StorageUnit->Ring.WaitIrp == Irp);
    StorageUnit->Ring.WaitIrp = 0;
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    InterlockedIncrement64(&StorageUnit->Ring.WaitCompletions);
    StorPortCompleteServiceIrp(DeviceExtension, Irp);
    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
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

    IoAcquireCancelSpinLock(&Irql);
    PIRP Irp = Ring->WaitIrp;
    if (0 != Irp)
    {
        Ring->WaitIrp = 0;
        PDRIVER_CANCEL CancelRoutine = IoSetCancelRoutine(Irp, 0);
        ASSERT(0 != CancelRoutine);
        UNREFERENCED_PARAMETER(CancelRoutine);
        if (Irp->Cancel)
            Status = STATUS_CANCELLED;
    }
    IoReleaseCancelSpinLock(Irql);
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

/* ProducerState owns the producer reference. A retry has an additional
 * callback reference, so a successfully queued producer cannot release the
 * last reference while this scheduling callback is still accessing Ring. */
static ULONG SpdStorageUnitRingQueueProducerWorker(SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_DEVICE_EXTENSION *DeviceExtension = StorageUnit->Ioq->DeviceExtension;
#if SPD_RING_LATENCY_DIAGNOSTICS
    InterlockedExchange64(&Ring->ProducerQueuedTicks, KeQueryPerformanceCounter(0).QuadPart);
#endif
    ULONG Status = StorPortQueueWorkItem(DeviceExtension,
        SpdStorageUnitRingProducerWorker, Ring->ProducerWorker, StorageUnit);
    KIRQL Irql;

    if (STOR_STATUS_BUSY != Status)
        return Status;

    SPD_STORAGE_UNIT *Reference = SpdStorageUnitReferenceByBtl(
        DeviceExtension, StorageUnit->Btl);
    if (Reference != StorageUnit)
    {
        if (0 != Reference)
            SpdStorageUnitDereference(DeviceExtension, Reference);
        return STOR_STATUS_INVALID_DEVICE_STATE;
    }
    if (!SpdStorageUnitRingEnter(StorageUnit))
    {
        SpdStorageUnitDereference(DeviceExtension, Reference);
        return STOR_STATUS_INVALID_DEVICE_STATE;
    }

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    ASSERT(!Ring->ProducerRetryQueued);
    Ring->ProducerRetryQueued = TRUE;
    InterlockedIncrement64(&Ring->ProducerBusyDeferrals);
    KeReleaseSpinLock(&Ring->Lock, Irql);

    /* Unlike the Storport worker, an IO work item is dequeued before its
     * callback starts and may be queued again from a running callback. */
    IoQueueWorkItem(Ring->ProducerRetryWorker,
        SpdStorageUnitRingProducerRetryWorker, DelayedWorkQueue, StorageUnit);
    return STOR_STATUS_SUCCESS;
}

static VOID SpdStorageUnitRingProducerRetryWorker(
    PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    SPD_STORAGE_UNIT *StorageUnit = Context;
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_DEVICE_EXTENSION *DeviceExtension = StorageUnit->Ioq->DeviceExtension;
    KIRQL Irql;
    BOOLEAN Stopping;
    ULONG Status;

    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(DeviceObject == DeviceExtension->DeviceObject);
    KeAcquireSpinLock(&Ring->Lock, &Irql);
    ASSERT(Ring->ProducerRetryQueued);
    Ring->ProducerRetryQueued = FALSE;
    Stopping = Ring->Stopping || Ring->Failed;
    KeReleaseSpinLock(&Ring->Lock, Irql);
    Stopping = Stopping || SpdIoqStopped(StorageUnit->Ioq);

    if (Stopping)
        goto cancel;

    Status = StorPortQueueWorkItem(DeviceExtension,
        SpdStorageUnitRingProducerWorker, Ring->ProducerWorker, StorageUnit);
    if (STOR_STATUS_SUCCESS == Status)
        goto done;
    if (STOR_STATUS_BUSY == Status)
    {
        /* One attempt per callback, with no busy-wait. The old Storport
         * callback can finish independently; retain both references across
         * this queued retry. This path does not retry SQ/slot exhaustion. */
        KeAcquireSpinLock(&Ring->Lock, &Irql);
        Stopping = Ring->Stopping || Ring->Failed;
        if (!Stopping)
        {
            Ring->ProducerRetryQueued = TRUE;
            InterlockedIncrement64(&Ring->ProducerBusyDeferrals);
        }
        KeReleaseSpinLock(&Ring->Lock, Irql);
        if (Stopping)
            goto cancel;
        IoQueueWorkItem(Ring->ProducerRetryWorker,
            SpdStorageUnitRingProducerRetryWorker, DelayedWorkQueue, StorageUnit);
        return;
    }

    DbgPrint(DRIVER_NAME ": SharedRing producer retry queue failed status=%lu\n", Status);
    SpdStorageUnitRingMarkFailed(StorageUnit);
cancel:
    InterlockedExchange(&Ring->ProducerState, SPD_RING_PRODUCER_IDLE);
    SpdStorageUnitRingReleaseProducerReference(StorageUnit);
done:
    /* Release the retry callback's own reference after the producer handoff. */
    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
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
#if SPD_RING_LATENCY_DIAGNOSTICS
    SpdLatencyRecord(&Ring->Latency[SpdLatencyProducerDispatch],
        KeQueryPerformanceCounter(0).QuadPart - InterlockedCompareExchange64(&Ring->ProducerQueuedTicks, 0, 0));
#endif

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
            InterlockedIncrement64(&Ring->ProducerContinuations);
            ULONG StorStatus = SpdStorageUnitRingQueueProducerWorker(StorageUnit);
            if (STOR_STATUS_SUCCESS == StorStatus)
                return; /* The continuation owns the reference; touch no ring state. */
            InterlockedDecrement64(&Ring->ProducerContinuations);
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
            ULONG StorStatus = SpdStorageUnitRingQueueProducerWorker(StorageUnit);
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

/* CompletionState owns the completion reference. A retry has an additional
 * callback reference, so a successfully queued completion cannot release the
 * last reference while this scheduling callback is still accessing Ring. */
static ULONG SpdStorageUnitRingQueueCompletionWorker(SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
#if SPD_RING_LATENCY_DIAGNOSTICS
    InterlockedExchange64(&Ring->CompletionQueuedTicks, KeQueryPerformanceCounter(0).QuadPart);
#endif
#if SPD_RING_COMPLETION_THREAD
    KeSetEvent(&Ring->CompletionThread->Wake, IO_NO_INCREMENT, FALSE);
    return STOR_STATUS_SUCCESS;
#else
    SPD_DEVICE_EXTENSION *DeviceExtension = StorageUnit->Ioq->DeviceExtension;
    ULONG Status = StorPortQueueWorkItem(DeviceExtension,
        SpdStorageUnitRingCompletionWorker, Ring->CompletionWorker, StorageUnit);
    KIRQL Irql;

    if (STOR_STATUS_BUSY != Status)
        return Status;

    SPD_STORAGE_UNIT *Reference = SpdStorageUnitReferenceByBtl(
        DeviceExtension, StorageUnit->Btl);
    if (Reference != StorageUnit)
    {
        if (0 != Reference)
            SpdStorageUnitDereference(DeviceExtension, Reference);
        return STOR_STATUS_INVALID_DEVICE_STATE;
    }
    if (!SpdStorageUnitRingEnter(StorageUnit))
    {
        SpdStorageUnitDereference(DeviceExtension, Reference);
        return STOR_STATUS_INVALID_DEVICE_STATE;
    }

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    ASSERT(!Ring->CompletionRetryQueued);
    Ring->CompletionRetryQueued = TRUE;
    InterlockedIncrement64(&Ring->CompletionBusyDeferrals);
    KeReleaseSpinLock(&Ring->Lock, Irql);

    /* Unlike the Storport worker, an IO work item is dequeued before its
     * callback starts and may be queued again from a running callback. */
    IoQueueWorkItem(Ring->CompletionRetryWorker,
        SpdStorageUnitRingCompletionRetryWorker, DelayedWorkQueue, StorageUnit);
    return STOR_STATUS_SUCCESS;
#endif
}

#if !SPD_RING_COMPLETION_THREAD
static VOID SpdStorageUnitRingCompletionRetryWorker(
    PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    SPD_STORAGE_UNIT *StorageUnit = Context;
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_DEVICE_EXTENSION *DeviceExtension = StorageUnit->Ioq->DeviceExtension;
    KIRQL Irql;
    BOOLEAN Stopping;
    ULONG Status;

    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(DeviceObject == DeviceExtension->DeviceObject);
    KeAcquireSpinLock(&Ring->Lock, &Irql);
    ASSERT(Ring->CompletionRetryQueued);
    Ring->CompletionRetryQueued = FALSE;
    Stopping = Ring->Stopping || Ring->Failed;
    KeReleaseSpinLock(&Ring->Lock, Irql);
    Stopping = Stopping || SpdIoqStopped(StorageUnit->Ioq);

    if (Stopping)
        goto cancel;

    Status = StorPortQueueWorkItem(DeviceExtension,
        SpdStorageUnitRingCompletionWorker, Ring->CompletionWorker, StorageUnit);
    if (STOR_STATUS_SUCCESS == Status)
        goto done;
    if (STOR_STATUS_BUSY == Status)
    {
        /* One attempt per callback, with no busy-wait. The old Storport
         * callback can finish independently; retain both references across
         * this queued retry. This path does not retry SQ/slot exhaustion. */
        KeAcquireSpinLock(&Ring->Lock, &Irql);
        Stopping = Ring->Stopping || Ring->Failed;
        if (!Stopping)
        {
            Ring->CompletionRetryQueued = TRUE;
            InterlockedIncrement64(&Ring->CompletionBusyDeferrals);
        }
        KeReleaseSpinLock(&Ring->Lock, Irql);
        if (Stopping)
            goto cancel;
        IoQueueWorkItem(Ring->CompletionRetryWorker,
            SpdStorageUnitRingCompletionRetryWorker, DelayedWorkQueue, StorageUnit);
        return;
    }

    DbgPrint(DRIVER_NAME ": SharedRing completion retry queue failed status=%lu\n", Status);
    SpdStorageUnitRingMarkFailed(StorageUnit);
cancel:
    InterlockedExchange(&Ring->CompletionState, SPD_RING_PRODUCER_IDLE);
    SpdStorageUnitRingReleaseCompletionReference(StorageUnit);
done:
    /* Release the retry callback's own reference after the completion handoff. */
    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
}

#endif

static VOID SpdStorageUnitRingCompletionWorker(
    PVOID HwDeviceExtension,
    PVOID Context,
    PVOID Worker)
{
    SPD_STORAGE_UNIT *StorageUnit = Context;
    SPD_RING_STATE *Ring = &StorageUnit->Ring;

    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(HwDeviceExtension == StorageUnit->Ioq->DeviceExtension);
    ASSERT(Worker == Ring->CompletionWorker);
    InterlockedIncrement64(&Ring->CompletionWorkerRuns);
#if SPD_RING_LATENCY_DIAGNOSTICS
    SpdLatencyRecord(&Ring->Latency[SpdLatencyCompletionDispatch],
        KeQueryPerformanceCounter(0).QuadPart - InterlockedCompareExchange64(&Ring->CompletionQueuedTicks, 0, 0));
#endif

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
        &Ring->CompletionExecuting, 1, 0));
#endif
    LARGE_INTEGER Start = KeQueryPerformanceCounter(0);
    if (!Stopping)
        BatchLimitReached = SpdRingCompleteBatch(StorageUnit);
    UINT64 Duration = KeQueryPerformanceCounter(0).QuadPart - Start.QuadPart;
    InterlockedExchangeAdd64(&Ring->CompletionCallbackTicks, Duration);
    SpdHistogramRecord(&Ring->CompletionDuration, Duration);
#if DBG
    ASSERT(1 == InterlockedExchange(
        &Ring->CompletionExecuting, 0));
#endif

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    Stopping = Ring->Stopping || Ring->Failed;
    KeReleaseSpinLock(&Ring->Lock, Irql);
    Stopping = Stopping || SpdIoqStopped(StorageUnit->Ioq);

    if (Stopping)
    {
        InterlockedExchange(&Ring->CompletionState,
            SPD_RING_PRODUCER_IDLE);
        goto exit;
    }

    if (BatchLimitReached &&
        SPD_RING_PRODUCER_ACTIVE == InterlockedCompareExchange(
            &Ring->CompletionState, SPD_RING_PRODUCER_RERUN,
            SPD_RING_PRODUCER_ACTIVE))
        InterlockedIncrement64(&Ring->CompletionWorkerReruns);

    for (;;)
    {
        LONG State = InterlockedCompareExchange(
            &Ring->CompletionState, SPD_RING_PRODUCER_IDLE,
            SPD_RING_PRODUCER_ACTIVE);
        if (SPD_RING_PRODUCER_ACTIVE == State)
            goto exit;
        if (SPD_RING_PRODUCER_RERUN == State)
        {
            if (SPD_RING_PRODUCER_RERUN != InterlockedCompareExchange(
                    &Ring->CompletionState, SPD_RING_PRODUCER_ACTIVE,
                    SPD_RING_PRODUCER_RERUN))
                continue;

            /* Keep one lifetime reference across the queued continuation. */
            InterlockedIncrement64(&Ring->CompletionContinuations);
            ULONG StorStatus = SpdStorageUnitRingQueueCompletionWorker(StorageUnit);
            if (STOR_STATUS_SUCCESS == StorStatus)
                return; /* The continuation owns the reference; touch no ring state. */
            InterlockedDecrement64(&Ring->CompletionContinuations);
            SpdStorageUnitRingMarkFailed(StorageUnit);
            InterlockedExchange(&Ring->CompletionState,
                SPD_RING_PRODUCER_IDLE);
            DbgPrint(DRIVER_NAME ": SharedRing completion continuation queue failed status=%lu\n",
                StorStatus);
            SchedulingFailed = TRUE;
            goto exit;
        }

        /* A scheduler observed the idle transition and queued fresh work. */
        goto exit;
    }

exit:
    SpdStorageUnitRingReleaseCompletionReference(StorageUnit);
    if (SchedulingFailed)
        ASSERT(FALSE);
}

VOID SpdStorageUnitRingScheduleCompletion(SPD_STORAGE_UNIT *StorageUnit)
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

#if SPD_RING_COMPLETION_THREAD
    SpdStorageUnitRingScheduleThreads(StorageUnit, Ring->CompletionThread);
    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
    return;
#else
    for (;;)
    {
        LONG State = InterlockedCompareExchange(
            &Ring->CompletionState, SPD_RING_PRODUCER_ACTIVE,
            SPD_RING_PRODUCER_IDLE);
        if (SPD_RING_PRODUCER_IDLE == State)
        {
#if DBG
            /* Count before queueing so a fast worker cannot release first. */
            InterlockedIncrement(&Ring->CompletionLiveReferences);
#endif
            ULONG StorStatus = SpdStorageUnitRingQueueCompletionWorker(StorageUnit);
            if (STOR_STATUS_SUCCESS == StorStatus)
                return;

            InterlockedExchange(&Ring->CompletionState,
                SPD_RING_PRODUCER_IDLE);
            SpdStorageUnitRingMarkFailed(StorageUnit);
            DbgPrint(DRIVER_NAME ": SharedRing completion queue failed status=%lu\n",
                StorStatus);
            SpdStorageUnitRingReleaseCompletionReference(StorageUnit);
            ASSERT(FALSE);
            return;
        }
        if (SPD_RING_PRODUCER_ACTIVE == State)
        {
            if (SPD_RING_PRODUCER_ACTIVE == InterlockedCompareExchange(
                    &Ring->CompletionState, SPD_RING_PRODUCER_RERUN,
                    SPD_RING_PRODUCER_ACTIVE))
            {
                InterlockedIncrement64(&Ring->CompletionWorkerReruns);
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
#endif
}

/* PreparationState owns the preparation reference. A retry has an additional
 * callback reference, so a successfully queued preparation cannot release the
 * last reference while this scheduling callback is still accessing Ring. */
static ULONG SpdStorageUnitRingQueuePreparationWorker(SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
#if SPD_RING_LATENCY_DIAGNOSTICS
    InterlockedExchange64(&Ring->PreparationQueuedTicks, KeQueryPerformanceCounter(0).QuadPart);
#endif
#if SPD_RING_PREPARATION_THREAD
    KeSetEvent(&Ring->PreparationThread->Wake, IO_NO_INCREMENT, FALSE);
    return STOR_STATUS_SUCCESS;
#else
    SPD_DEVICE_EXTENSION *DeviceExtension = StorageUnit->Ioq->DeviceExtension;
    ULONG Status = StorPortQueueWorkItem(DeviceExtension,
        SpdStorageUnitRingPreparationWorker, Ring->PreparationWorker, StorageUnit);
    KIRQL Irql;

    if (STOR_STATUS_BUSY != Status)
        return Status;

    SPD_STORAGE_UNIT *Reference = SpdStorageUnitReferenceByBtl(
        DeviceExtension, StorageUnit->Btl);
    if (Reference != StorageUnit)
    {
        if (0 != Reference)
            SpdStorageUnitDereference(DeviceExtension, Reference);
        return STOR_STATUS_INVALID_DEVICE_STATE;
    }
    if (!SpdStorageUnitRingEnter(StorageUnit))
    {
        SpdStorageUnitDereference(DeviceExtension, Reference);
        return STOR_STATUS_INVALID_DEVICE_STATE;
    }

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    ASSERT(!Ring->PreparationRetryQueued);
    Ring->PreparationRetryQueued = TRUE;
    InterlockedIncrement64(&Ring->PreparationBusyDeferrals);
    KeReleaseSpinLock(&Ring->Lock, Irql);

    /* Unlike the Storport worker, an IO work item is dequeued before its
     * callback starts and may be queued again from a running callback. */
    IoQueueWorkItem(Ring->PreparationRetryWorker,
        SpdStorageUnitRingPreparationRetryWorker, DelayedWorkQueue, StorageUnit);
    return STOR_STATUS_SUCCESS;
#endif
}

#if !SPD_RING_PREPARATION_THREAD
static VOID SpdStorageUnitRingPreparationRetryWorker(
    PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    SPD_STORAGE_UNIT *StorageUnit = Context;
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    SPD_DEVICE_EXTENSION *DeviceExtension = StorageUnit->Ioq->DeviceExtension;
    KIRQL Irql;
    BOOLEAN Stopping;
    ULONG Status;

    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(DeviceObject == DeviceExtension->DeviceObject);
    KeAcquireSpinLock(&Ring->Lock, &Irql);
    ASSERT(Ring->PreparationRetryQueued);
    Ring->PreparationRetryQueued = FALSE;
    Stopping = Ring->Stopping || Ring->Failed;
    KeReleaseSpinLock(&Ring->Lock, Irql);
    Stopping = Stopping || SpdIoqStopped(StorageUnit->Ioq);

    if (Stopping)
        goto cancel;

    Status = StorPortQueueWorkItem(DeviceExtension,
        SpdStorageUnitRingPreparationWorker, Ring->PreparationWorker, StorageUnit);
    if (STOR_STATUS_SUCCESS == Status)
        goto done;
    if (STOR_STATUS_BUSY == Status)
    {
        /* One attempt per callback, with no busy-wait. The old Storport
         * callback can finish independently; retain both references across
         * this queued retry. This path does not retry SQ/slot exhaustion. */
        KeAcquireSpinLock(&Ring->Lock, &Irql);
        Stopping = Ring->Stopping || Ring->Failed;
        if (!Stopping)
        {
            Ring->PreparationRetryQueued = TRUE;
            InterlockedIncrement64(&Ring->PreparationBusyDeferrals);
        }
        KeReleaseSpinLock(&Ring->Lock, Irql);
        if (Stopping)
            goto cancel;
        IoQueueWorkItem(Ring->PreparationRetryWorker,
            SpdStorageUnitRingPreparationRetryWorker, DelayedWorkQueue, StorageUnit);
        return;
    }

    DbgPrint(DRIVER_NAME ": SharedRing preparation retry queue failed status=%lu\n", Status);
    SpdStorageUnitRingMarkFailed(StorageUnit);
cancel:
    InterlockedExchange(&Ring->PreparationState, SPD_RING_PRODUCER_IDLE);
    SpdStorageUnitRingReleasePreparationReference(StorageUnit);
done:
    /* Release the retry callback's own reference after the preparation handoff. */
    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
}

#endif

static VOID SpdStorageUnitRingPreparationWorker(
    PVOID HwDeviceExtension,
    PVOID Context,
    PVOID Worker)
{
    SPD_STORAGE_UNIT *StorageUnit = Context;
    SPD_RING_STATE *Ring = &StorageUnit->Ring;

    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(HwDeviceExtension == StorageUnit->Ioq->DeviceExtension);
    ASSERT(Worker == Ring->PreparationWorker);
    InterlockedIncrement64(&Ring->PreparationWorkerRuns);
#if SPD_RING_LATENCY_DIAGNOSTICS
    SpdLatencyRecord(&Ring->Latency[SpdLatencyPreparationDispatch],
        KeQueryPerformanceCounter(0).QuadPart - InterlockedCompareExchange64(&Ring->PreparationQueuedTicks, 0, 0));
#endif

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
        &Ring->PreparationExecuting, 1, 0));
#endif
    LARGE_INTEGER Frequency;
    LARGE_INTEGER Start = KeQueryPerformanceCounter(&Frequency);
    if (!Stopping)
    {
        do
        {
            InterlockedCompareExchange(&Ring->PreparationState,
                SPD_RING_PRODUCER_ACTIVE, SPD_RING_PRODUCER_RERUN);
            if (!SpdRingPrepareBatch(StorageUnit))
                break;
            InterlockedCompareExchange(&Ring->PreparationState,
                SPD_RING_PRODUCER_ACTIVE, SPD_RING_PRODUCER_RERUN);
            BatchLimitReached = SpdIoqRingHasPending(StorageUnit->Ioq);
        } while (BatchLimitReached &&
            KeQueryPerformanceCounter(0).QuadPart - Start.QuadPart <
                Frequency.QuadPart * SPD_RING_PREPARE_TIME_US / 1000000);
    }
    UINT64 Duration = KeQueryPerformanceCounter(0).QuadPart - Start.QuadPart;
    InterlockedExchangeAdd64(&Ring->PreparationCallbackTicks, Duration);
    SpdHistogramRecord(&Ring->PreparationDuration, Duration);
#if DBG
    ASSERT(1 == InterlockedExchange(
        &Ring->PreparationExecuting, 0));
#endif

    KeAcquireSpinLock(&Ring->Lock, &Irql);
    Stopping = Ring->Stopping || Ring->Failed;
    KeReleaseSpinLock(&Ring->Lock, Irql);
    Stopping = Stopping || SpdIoqStopped(StorageUnit->Ioq);

    if (Stopping)
    {
        InterlockedExchange(&Ring->PreparationState,
            SPD_RING_PRODUCER_IDLE);
        goto exit;
    }

    if (BatchLimitReached &&
        SPD_RING_PRODUCER_ACTIVE == InterlockedCompareExchange(
            &Ring->PreparationState, SPD_RING_PRODUCER_RERUN,
            SPD_RING_PRODUCER_ACTIVE))
        InterlockedIncrement64(&Ring->PreparationWorkerReruns);

    for (;;)
    {
        LONG State = InterlockedCompareExchange(
            &Ring->PreparationState, SPD_RING_PRODUCER_IDLE,
            SPD_RING_PRODUCER_ACTIVE);
        if (SPD_RING_PRODUCER_ACTIVE == State)
            goto exit;
        if (SPD_RING_PRODUCER_RERUN == State)
        {
            if (SPD_RING_PRODUCER_RERUN != InterlockedCompareExchange(
                    &Ring->PreparationState, SPD_RING_PRODUCER_ACTIVE,
                    SPD_RING_PRODUCER_RERUN))
                continue;

            /* Keep one lifetime reference across the queued continuation. */
            InterlockedIncrement64(&Ring->PreparationContinuations);
            ULONG StorStatus = SpdStorageUnitRingQueuePreparationWorker(StorageUnit);
            if (STOR_STATUS_SUCCESS == StorStatus)
                return; /* The continuation owns the reference; touch no ring state. */
            InterlockedDecrement64(&Ring->PreparationContinuations);
            SpdStorageUnitRingMarkFailed(StorageUnit);
            InterlockedExchange(&Ring->PreparationState,
                SPD_RING_PRODUCER_IDLE);
            DbgPrint(DRIVER_NAME ": SharedRing preparation continuation queue failed status=%lu\n",
                StorStatus);
            SchedulingFailed = TRUE;
            goto exit;
        }

        /* A scheduler observed the idle transition and queued fresh work. */
        goto exit;
    }

exit:
    SpdStorageUnitRingReleasePreparationReference(StorageUnit);
    if (SchedulingFailed)
        ASSERT(FALSE);
}

VOID SpdStorageUnitRingSchedulePreparation(SPD_STORAGE_UNIT *StorageUnit)
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

#if SPD_RING_PREPARATION_THREAD
    SpdStorageUnitRingScheduleThreads(StorageUnit, Ring->PreparationThread);
    SpdStorageUnitRingLeave(StorageUnit);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
    return;
#else
    for (;;)
    {
        LONG State = InterlockedCompareExchange(
            &Ring->PreparationState, SPD_RING_PRODUCER_ACTIVE,
            SPD_RING_PRODUCER_IDLE);
        if (SPD_RING_PRODUCER_IDLE == State)
        {
#if DBG
            /* Count before queueing so a fast worker cannot release first. */
            InterlockedIncrement(&Ring->PreparationLiveReferences);
#endif
            ULONG StorStatus = SpdStorageUnitRingQueuePreparationWorker(StorageUnit);
            if (STOR_STATUS_SUCCESS == StorStatus)
                return;

            InterlockedExchange(&Ring->PreparationState,
                SPD_RING_PRODUCER_IDLE);
            SpdStorageUnitRingMarkFailed(StorageUnit);
            DbgPrint(DRIVER_NAME ": SharedRing preparation queue failed status=%lu\n",
                StorStatus);
            SpdStorageUnitRingReleasePreparationReference(StorageUnit);
            ASSERT(FALSE);
            return;
        }
        if (SPD_RING_PRODUCER_ACTIVE == State)
        {
            if (SPD_RING_PRODUCER_ACTIVE == InterlockedCompareExchange(
                    &Ring->PreparationState, SPD_RING_PRODUCER_RERUN,
                    SPD_RING_PRODUCER_ACTIVE))
            {
                InterlockedIncrement64(&Ring->PreparationWorkerReruns);
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
#endif
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

    IoAcquireCancelSpinLock(&Irql);
    if (0 != Ring->WaitIrp || Irp->Cancel)
    {
        NTSTATUS Status = Irp->Cancel ? STATUS_CANCELLED : STATUS_DEVICE_BUSY;
        IoReleaseCancelSpinLock(Irql);
        SpdStorageUnitRingLeave(StorageUnit);
        SpdStorageUnitRingLeave(StorageUnit);
        Irp->IoStatus.Status = Status;
        SpdStorageUnitDereference(DeviceExtension, StorageUnit);
        return SpdServiceIrpCompleteNow;
    }
    Irp->Tail.Overlay.DriverContext[0] = StorageUnit;
    Ring->WaitIrp = Irp;
    PDRIVER_CANCEL CancelRoutine = IoSetCancelRoutine(
        Irp, SpdStorageUnitRingCancelWaitIrp);
    ASSERT(0 == CancelRoutine);
    UNREFERENCED_PARAMETER(CancelRoutine);
    IoReleaseCancelSpinLock(Irql);

    InterlockedIncrement64(&Ring->WaitSubmissions);
    /* Commit always wakes publication. A new WAIT only needs another producer
     * when its SQ-head progress can unblock ready descriptors. Already-published
     * requests can notify this waiter directly without another work item. */
    if (SpdIoqRingHasReady(StorageUnit->Ioq))
        SpdStorageUnitRingScheduleProducer(StorageUnit);
    SpdStorageUnitRingCompleteWait(StorageUnit, STATUS_SUCCESS, TRUE);

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

VOID SpdStorageUnitRingTerminalReset(SPD_STORAGE_UNIT *StorageUnit)
{
    SPD_RING_STATE *Ring = &StorageUnit->Ring;
    KIRQL Irql;
    /* IOQ has already stopped claims and publication under its metadata lock.
     * Wake transport waiters separately, without nesting the two locks or
     * waiting for a worker that may currently own a payload copy. */
    KeAcquireSpinLock(&Ring->Lock, &Irql);
    Ring->Stopping = TRUE;
    KeReleaseSpinLock(&Ring->Lock, Irql);
    SpdStorageUnitRingCancelWait(StorageUnit);
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

#if SPD_RING_LATENCY_DIAGNOSTICS
    UINT64 KickStart = 0;
    InterlockedIncrement64(&Ring->KickCalls);
#endif
    Params->Consumed = 0;
    if (ProcessId != StorageUnit->TransactProcessId)
        return STATUS_ACCESS_DENIED;
    if (!SpdStorageUnitRingEnter(StorageUnit))
        return STATUS_INVALID_DEVICE_STATE;

    if (0 != InterlockedCompareExchange(&Ring->KickActive, 1, 0))
    {
#if SPD_RING_LATENCY_DIAGNOSTICS
        InterlockedIncrement64(&Ring->KickBusy);
#endif
        SpdStorageUnitRingLeave(StorageUnit);
        return STATUS_DEVICE_BUSY;
    }

#if SPD_RING_LATENCY_DIAGNOSTICS
    KickStart = KeQueryPerformanceCounter(0).QuadPart;
#endif
    Header = (SPD_RING_HEADER *)Ring->SystemAddress;
    CompletionRing = (SPD_RING_COMPLETION *)
        ((PUINT8)Ring->SystemAddress + Ring->CompletionOffset);
    Head = Ring->CompletionHead;
    Tail = SpdRingLoadAcquire32(&Header->CompletionTail.Value);
    Count = Tail - Head;
#if SPD_RING_LATENCY_DIAGNOSTICS
    SpdLatencyRecord(&Ring->Latency[SpdLatencyCqDepth], Count);
#endif
    if (Count > Ring->QueueDepth)
    {
        Failed = TRUE;
        Result = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    while (Head != Tail)
    {
        SPD_RING_COMPLETION Completions[SPD_RING_COMPLETION_LIMIT];
        UINT32 BatchCount = min(Tail - Head, SPD_RING_COMPLETION_LIMIT);
        UINT32 Accepted;
        /* Snapshot descriptors before taking the metadata lock. No untrusted
         * shared descriptor is consulted after validation changes ownership. */
        for (UINT32 I = 0; I < BatchCount; I++)
            Completions[I] = CompletionRing[(Head + I) & (Ring->QueueDepth - 1)];
        Result = SpdIoqRingEnqueueCompletions(StorageUnit->Ioq,
            Completions, BatchCount, &Accepted);
        Head += Accepted;
        Consumed += Accepted;
        if (!NT_SUCCESS(Result))
        {
            Failed = STATUS_CANCELLED != Result;
            break;
        }
    }

exit:
    if (0 != Consumed)
    {
        Ring->CompletionHead = Head;
        SpdRingStoreRelease32(&Header->CompletionHead.Value, Head);
    }
    Params->Consumed = Consumed;
#if defined(WINSPD_TEST_BUILD)
    /* Park at the CQ ownership boundary before broader failure rundown. The
     * completion handoff gate is otherwise unused when this CQ prefix is bad. */
    if (Failed)
        SpdRingTestBarrier(Ring->TestCompletionClaimEnteredEvent,
            Ring->TestCompletionClaimReleaseEvent);
#endif
    if (Failed)
    {
        KeAcquireSpinLock(&Ring->Lock, &Irql);
        Ring->Failed = TRUE;
        Ring->Stopping = TRUE;
        KeReleaseSpinLock(&Ring->Lock, Irql);
    }
#if SPD_RING_LATENCY_DIAGNOSTICS
    SpdLatencyRecord(&Ring->Latency[SpdLatencyKick], KeQueryPerformanceCounter(0).QuadPart - KickStart);
#endif
    InterlockedExchange(&Ring->KickActive, 0);
    if (Failed)
    {
        SpdIoqReset(StorageUnit->Ioq, TRUE,
            SpdIoqResetReasonRingFailure);
        SpdStorageUnitRingCancelWait(StorageUnit);
    }
    else
    {
        if (0 != Consumed)
            SpdStorageUnitRingScheduleCompletion(StorageUnit);
        /* Head progress only needs publication work when descriptors await it.
         * A subsequent ready commit supplies its own wakeup. */
        if (SpdIoqRingHasReady(StorageUnit->Ioq))
            SpdStorageUnitRingScheduleProducer(StorageUnit);
    }
    SpdStorageUnitRingLeave(StorageUnit);
    return Result;
}
NTSTATUS SpdDeviceExtensionInit(SPD_DEVICE_EXTENSION *DeviceExtension, PVOID BusInformation)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(0 != DeviceExtension);

    NTSTATUS Result;

    KeWaitForSingleObject(&SpdGlobalDeviceLifecycleMutex, Executive, KernelMode, FALSE, NULL);
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
    DeviceExtension->RundownWorkers = 0;
    KeInitializeEvent(&DeviceExtension->RundownIdleEvent, NotificationEvent, TRUE);
    DeviceExtension->DeviceObject = BusInformation;
    DeviceExtension->StorageUnitCapacity = SpdStorageUnitCapacity;
    SpdGlobalDeviceExtension = DeviceExtension;

    Result = STATUS_SUCCESS;

exit:
    ExReleaseResourceLite(&SpdGlobalDeviceResource);
    KeLeaveCriticalRegion();
    KeReleaseMutex(&SpdGlobalDeviceLifecycleMutex, FALSE);

    return Result;
}

VOID SpdDeviceExtensionFini(SPD_DEVICE_EXTENSION *DeviceExtension)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(0 != DeviceExtension);

    BOOLEAN RemoveNotify;
    KeWaitForSingleObject(&SpdGlobalDeviceLifecycleMutex, Executive, KernelMode, FALSE, NULL);
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&SpdGlobalDeviceResource, TRUE);

    RemoveNotify = DeviceExtension == SpdGlobalDeviceExtension;
    if (RemoveNotify)
    {
        SpdGlobalDeviceExtension = 0;
    }

    ExReleaseResourceLite(&SpdGlobalDeviceResource);
    KeLeaveCriticalRegion();
    /* Unregister can wait for a callback that is acquiring the resource.
     * Do not hold that resource here. The lifecycle mutex serializes restart. */
    if (RemoveNotify)
    {
        PsSetCreateProcessNotifyRoutine(SpdDeviceExtensionNotifyRoutine, TRUE);
        KeWaitForSingleObject(&DeviceExtension->RundownIdleEvent,
            Executive, KernelMode, FALSE, NULL);
        /* The final worker signals while holding this lock. Synchronize its
         * final release before Storport may free the adapter extension. */
        KIRQL Irql;
        KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
        ASSERT(0 == DeviceExtension->RundownWorkers);
        KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);
    }
    KeReleaseMutex(&SpdGlobalDeviceLifecycleMutex, FALSE);
}

static VOID SpdDeviceExtensionNotifyRoutine(HANDLE ParentId, HANDLE ProcessId0, BOOLEAN Create)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());

    if (Create)
        return;

    PEPROCESS Process = PsGetCurrentProcess();
    UNREFERENCED_PARAMETER(ParentId);
    UNREFERENCED_PARAMETER(ProcessId0);

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&SpdGlobalDeviceResource, TRUE);

    if (0 != SpdGlobalDeviceExtension)
    {
        SPD_STORAGE_UNIT *Units[SPD_IOCTL_STORAGE_UNIT_MAX_CAPACITY];
        ULONG Count = 0;
        KIRQL Irql;
        KeAcquireSpinLock(&SpdGlobalDeviceExtension->SpinLock, &Irql);
        ASSERT(SpdGlobalDeviceExtension->StorageUnitCapacity <= RTL_NUMBER_OF(Units));
        for (ULONG I = 0; I < SpdGlobalDeviceExtension->StorageUnitCapacity; I++)
        {
            SPD_STORAGE_UNIT *Unit = SpdGlobalDeviceExtension->StorageUnits[I];
            if (0 == Unit)
                continue;
            Unit->RefCount++;
            Units[Count++] = Unit;
        }
        KeReleaseSpinLock(&SpdGlobalDeviceExtension->SpinLock, Irql);
        for (ULONG I = 0; I < Count; I++)
        {
            SPD_STORAGE_UNIT *Unit = Units[I];
            KeAcquireSpinLock(&Unit->Ring.Lock, &Irql);
            BOOLEAN OwnsMapping = Unit->RingProcess == Process;
            KeReleaseSpinLock(&Unit->Ring.Lock, Irql);
            if (Unit->OwnerProcess == Process || OwnsMapping)
                SpdStorageUnitUnprovisionInternal(SpdGlobalDeviceExtension,
                    0, SPD_IOCTL_BTL_T(Unit->Btl), Unit->OwnerProcessId, Unit);
            SpdStorageUnitDereference(SpdGlobalDeviceExtension, Unit);
        }
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
    KeInitializeEvent(&StorageUnit->Ring.CloseDoneEvent, NotificationEvent, FALSE);
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
    Result = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)ProcessId, &StorageUnit->OwnerProcess);
    if (!NT_SUCCESS(Result))
        goto exit;

    Result = SpdIoqCreate(DeviceExtension, &StorageUnit->Ioq);
    if (!NT_SUCCESS(Result))
        goto exit;
    StorageUnit->Ioq->StorageUnit = StorageUnit;

    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    DuplicateUnit = 0;
    Btl = (UINT32)-1;
    for (ULONG I = 0; DeviceExtension->StorageUnitCapacity > I; I++)
    {
        SPD_STORAGE_UNIT *Unit = DeviceExtension->StorageUnits[I];
        if (0 == Unit)
            Unit = DeviceExtension->RetiringUnits[I];
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
    if (!NT_SUCCESS(Result) && 0 != StorageUnit)
    {
        if (0 != StorageUnit->Ioq)
            SpdIoqDelete(StorageUnit->Ioq);
        if (0 != StorageUnit->OwnerProcess)
            ObDereferenceObject(StorageUnit->OwnerProcess);
        SpdFree(StorageUnit, SpdTagStorageUnit);
    }

    return Result;
}

NTSTATUS SpdStorageUnitUnprovision(
    SPD_DEVICE_EXTENSION *DeviceExtension, PGUID Guid, ULONG Index, ULONG ProcessId)
{
    return SpdStorageUnitUnprovisionInternal(DeviceExtension, Guid, Index, ProcessId, 0);
}

static VOID SpdStorageUnitOwnerRundownWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    SPD_STORAGE_UNIT *StorageUnit = Context;
    SPD_DEVICE_EXTENSION *DeviceExtension = StorageUnit->Ioq->DeviceExtension;
    PVOID Objects[2] = { StorageUnit->RingProcess, &StorageUnit->Ring.CloseDoneEvent };
    KWAIT_BLOCK WaitBlocks[2];
    KIRQL Irql;
    UNREFERENCED_PARAMETER(DeviceObject);
    /* The exit notification is on the last exiting thread. Waiting there
     * would prevent the process from becoming signaled. A cooperative CLOSE
     * may alternatively have established quiescence while removal was racing. */
    NTSTATUS Status = KeWaitForMultipleObjects(2, Objects, WaitAny, Executive,
        KernelMode, FALSE, NULL, WaitBlocks);
    ASSERT(STATUS_WAIT_0 == Status || STATUS_WAIT_1 == Status);
    UNREFERENCED_PARAMETER(Status);
    SpdStorageUnitRingClose(StorageUnit, TRUE);
#if defined(WINSPD_TEST_BUILD)
    if (0 != StorageUnit->TestOwnerRundownDoneEvent)
        KeSetEvent(StorageUnit->TestOwnerRundownDoneEvent, IO_NO_INCREMENT, FALSE);
#endif
    /* Unprovision transferred its original unit reference to this callback.
     * Adapter rundown keeps DeviceExtension alive through the final release. */
    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    ASSERT(StorageUnit == DeviceExtension->RetiringUnits[SPD_INDEX_FROM_BTL(StorageUnit->Btl)]);
    DeviceExtension->RetiringUnits[SPD_INDEX_FROM_BTL(StorageUnit->Btl)] = 0;
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);
    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    ASSERT(0 != DeviceExtension->RundownWorkers);
    if (0 == --DeviceExtension->RundownWorkers)
        KeSetEvent(&DeviceExtension->RundownIdleEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);
}

static NTSTATUS SpdStorageUnitUnprovisionInternal(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    PGUID Guid, ULONG Index,
    ULONG ProcessId, SPD_STORAGE_UNIT *ExitingUnit)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(0 != DeviceExtension);

    NTSTATUS Result;
    SPD_STORAGE_UNIT *StorageUnit;
    KIRQL Irql;
    BOOLEAN ProcessExiting = 0 != ExitingUnit;

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
    if (0 != StorageUnit && ProcessId == StorageUnit->OwnerProcessId &&
        (!ProcessExiting || StorageUnit == ExitingUnit))
    {
        DeviceExtension->StorageUnitCount--;
        DeviceExtension->StorageUnits[Index] = 0;
        DeviceExtension->RetiringUnits[Index] = StorageUnit;
    }
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);

    if (0 == StorageUnit)
    {
        Result = STATUS_OBJECT_NAME_NOT_FOUND;
        goto exit;
    }
    if (ProcessId != StorageUnit->OwnerProcessId ||
        (ProcessExiting && StorageUnit != ExitingUnit))
    {
        Result = STATUS_ACCESS_DENIED;
        goto exit;
    }

    /* stop the ioq before reclaiming ring request state and mappings */
    SpdIoqReset(StorageUnit->Ioq, TRUE,
        SpdIoqResetReasonRemoval);
    /* Serialize with the final OPEN commit before inspecting mapping identity.
     * An exit notification holds ExitingUnit referenced, preventing BTL reuse
     * from causing an unrelated replacement unit to be removed. */
    SpdStorageUnitRingTerminalReset(StorageUnit);
#if SPD_RING_MAP_THRESHOLD_BYTES
    /* Do not finish process exit while a kernel owner can still map/unmap in
     * this address space. Never wait here for userspace or process exit. */
    if (ProcessExiting)
        KeWaitForSingleObject(&StorageUnit->Ring.IdleEvent,
            Executive, KernelMode, FALSE, NULL);
#endif
    KeAcquireSpinLock(&StorageUnit->Ring.Lock, &Irql);
    PEPROCESS MappingProcess = StorageUnit->RingProcess;
    KeReleaseSpinLock(&StorageUnit->Ring.Lock, Irql);
    /* Being in the mapping's process does not prove its callbacks have left.
     * Only cooperative CLOSE or process termination permits physical reclaim.
     * A completed CLOSE needs no deferred wait and permits immediate reuse. */
    if (0 != MappingProcess && !KeReadStateEvent(&StorageUnit->Ring.CloseDoneEvent))
    {
        KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
        if (0 == DeviceExtension->RundownWorkers++)
            KeClearEvent(&DeviceExtension->RundownIdleEvent);
        KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);
        IoQueueWorkItem(StorageUnit->RundownWorker,
            SpdStorageUnitOwnerRundownWorker, DelayedWorkQueue, StorageUnit);
    }
    else
    {
        SpdStorageUnitRingClose(StorageUnit, FALSE);
        KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
        DeviceExtension->RetiringUnits[Index] = 0;
        KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);
        SpdStorageUnitDereference(DeviceExtension, StorageUnit);
    }

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

SPD_STORAGE_UNIT *SpdStorageUnitReferenceForClose(
    SPD_DEVICE_EXTENSION *DeviceExtension, UINT32 Btl)
{
    SPD_STORAGE_UNIT *StorageUnit;
    KIRQL Irql;
    if (0 != SPD_IOCTL_BTL_B(Btl) || 0 != SPD_IOCTL_BTL_L(Btl) ||
        DeviceExtension->StorageUnitCapacity <= SPD_IOCTL_BTL_T(Btl))
        return 0;
    ULONG Index = SPD_INDEX_FROM_BTL(Btl);
    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    StorageUnit = DeviceExtension->StorageUnits[Index];
    if (0 == StorageUnit)
        StorageUnit = DeviceExtension->RetiringUnits[Index];
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
        if (0 != StorageUnit->RundownWorker)
            IoFreeWorkItem(StorageUnit->RundownWorker);
        if (0 != StorageUnit->RingProcess)
            ObDereferenceObject(StorageUnit->RingProcess);
        if (0 != StorageUnit->RingFileObject)
            ObDereferenceObject(StorageUnit->RingFileObject);
        if (0 != StorageUnit->OwnerProcess)
            ObDereferenceObject(StorageUnit->OwnerProcess);
#if defined(WINSPD_TEST_BUILD)
        if (0 != StorageUnit->TestOwnerRundownDoneEvent)
            ObDereferenceObject(StorageUnit->TestOwnerRundownDoneEvent);
#endif
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
