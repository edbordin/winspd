/**
 * @file sys/ioq.c
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

static VOID SpdIoqRingFreeSlotNoLock(SPD_IOQ *Ioq, SPD_WORK_SLOT *Slot);

/* Logical requests remain active across chunk leases. Fence state never
 * retains an SRB pointer; this helper is called exactly once at final retirement. */
static VOID SpdIoqRingRetireLogicalNoLock(SPD_IOQ *Ioq, SPD_SRB_EXTENSION *Extension)
{
    if (Ioq->NonblockingConsumer)
        Ioq->RetiredSrbs++;
    if (!Extension->ExecutionStarted)
        return;
    ASSERT(Ioq->NonblockingConsumer);
    if (Extension->RingIsFlush)
    {
        ASSERT(Ioq->FlushInProgress);
        Ioq->FlushInProgress = FALSE;
    }
    else
    {
        ASSERT(0 != Ioq->ActiveSrbs);
        Ioq->ActiveSrbs--;
    }
    Extension->ExecutionStarted = FALSE;
}

static BOOLEAN SpdIoqRingCanClaimNoLock(SPD_IOQ *Ioq)
{
    if (Ioq->Stopped || Ioq->FlushInProgress || IsListEmpty(&Ioq->PendingList) ||
        IsListEmpty(&Ioq->FreeSlots))
        return FALSE;
    SPD_SRB_EXTENSION *Head = CONTAINING_RECORD(
        Ioq->PendingList.Flink, SPD_SRB_EXTENSION, ListEntry);
    return !Head->RingIsFlush || 0 == Ioq->ActiveSrbs;
}

/* Each acquisition owns a caller-stack handle until its matching release.
 * Never mix ordinary and queued operations on the same IOQ lock. */
#if SPD_IOQ_QUEUED_SPIN_LOCK
typedef KLOCK_QUEUE_HANDLE SPD_IOQ_LOCK_HANDLE;
#else
typedef KIRQL SPD_IOQ_LOCK_HANDLE;
#endif

static VOID SpdIoqAcquireSpinLock(
    SPD_IOQ *Ioq, SPD_IOQ_LOCK_HANDLE *Handle, LARGE_INTEGER *PStart)
{
    LARGE_INTEGER Before = KeQueryPerformanceCounter(0);
    LARGE_INTEGER After;

#if SPD_IOQ_QUEUED_SPIN_LOCK
    KeAcquireInStackQueuedSpinLock(&Ioq->SpinLock, Handle);
#else
    KeAcquireSpinLock(&Ioq->SpinLock, Handle);
#endif
    After = KeQueryPerformanceCounter(0);
    InterlockedExchangeAdd64(&Ioq->SpinLockWaitTicks,
        After.QuadPart - Before.QuadPart);
    InterlockedIncrement64(&Ioq->SpinLockAcquisitions);
    SpdHistogramRecord(&Ioq->SpinLockWaitHistogram, After.QuadPart - Before.QuadPart);
    *PStart = After;
}

static VOID SpdIoqReleaseSpinLock(
    SPD_IOQ *Ioq, SPD_IOQ_LOCK_HANDLE *Handle, LARGE_INTEGER Start)
{
    LARGE_INTEGER End = KeQueryPerformanceCounter(0);
    InterlockedExchangeAdd64(&Ioq->SpinLockHoldTicks,
        End.QuadPart - Start.QuadPart);
    SpdHistogramRecord(&Ioq->SpinLockHoldHistogram, End.QuadPart - Start.QuadPart);
#if SPD_IOQ_QUEUED_SPIN_LOCK
    KeReleaseInStackQueuedSpinLock(Handle);
#else
    KeReleaseSpinLock(&Ioq->SpinLock, *Handle);
#endif
}

static VOID SpdIoqRemoveProcessHashNoLock(
    SPD_IOQ *Ioq, SPD_SRB_EXTENSION *SrbExtension)
{
    ULONG Index = SpdHashMixPointer(SrbExtension) %
        Ioq->ProcessBucketCount;
    for (PVOID *P = &Ioq->ProcessBuckets[Index]; *P;
        P = &((SPD_SRB_EXTENSION *)(*P))->HashNext)
        if (*P == SrbExtension)
        {
            *P = SrbExtension->HashNext;
            SrbExtension->HashNext = 0;
            return;
        }
    ASSERT(FALSE);
}

NTSTATUS SpdIoqCreate(PVOID DeviceExtension, SPD_IOQ **PIoq)
{
    SPD_IOQ *Ioq;
    ULONG BucketCount = (PAGE_SIZE - sizeof *Ioq) / sizeof Ioq->ProcessBuckets[0];

    *PIoq = 0;

    Ioq = SpdAllocNonPaged(PAGE_SIZE, SpdTagIoq);
    if (0 == Ioq)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Ioq, PAGE_SIZE);

    Ioq->DeviceExtension = DeviceExtension;
    KeInitializeSpinLock(&Ioq->SpinLock);
    SpdQeventInitialize(&Ioq->PendingEvent, 0);
    InitializeListHead(&Ioq->PendingList);
    InitializeListHead(&Ioq->ProcessList);
    InitializeListHead(&Ioq->FreeSlots);
    InitializeListHead(&Ioq->ReadySlots);
    InitializeListHead(&Ioq->CompletedSlots);
    Ioq->ProcessBucketCount = BucketCount;

    *PIoq = Ioq;

    return STATUS_SUCCESS;
}

VOID SpdIoqDelete(SPD_IOQ *Ioq)
{
    ASSERT(0 == Ioq->Slots);
    SpdIoqReset(Ioq, FALSE, SpdIoqResetReasonDelete);
    SpdQeventFinalize(&Ioq->PendingEvent);
    SpdFree(Ioq, SpdTagIoq);
}

NTSTATUS SpdIoqRingAttachSlots(SPD_IOQ *Ioq, SPD_WORK_SLOT *Slots, UINT32 Count)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER LockStart;

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);
    if (Ioq->Stopped || 0 != Ioq->Slots || !IsListEmpty(&Ioq->ProcessList))
    {
        SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);
        return STATUS_DEVICE_BUSY;
    }
    Ioq->Slots = Slots;
    Ioq->SlotCount = Count;
    for (UINT32 I = 0; I < Count; I++)
    {
        Slots[I].State = SpdSlotFree;
        InsertTailList(&Ioq->FreeSlots, &Slots[I].ListEntry);
    }
    Ioq->NonblockingConsumer = TRUE;
    Ioq->AcceptedSrbs = Ioq->RetiredSrbs = 0;
    /* Release a legacy waiter so it can observe the transport switch. */
    SpdQeventSetNoLock(&Ioq->PendingEvent);
    for (PLIST_ENTRY Entry = Ioq->PendingList.Flink;
        Entry != &Ioq->PendingList; Entry = Entry->Flink)
    {
        SPD_SRB_EXTENSION *SrbExtension = CONTAINING_RECORD(
            Entry, SPD_SRB_EXTENSION, ListEntry);
        SrbExtension->RingState = SpdRingSrbPending;
        SrbExtension->PendingTicks = KeQueryPerformanceCounter(0).QuadPart;
#if SPD_RING_LATENCY_DIAGNOSTICS
        SrbExtension->AcceptedTicks = SrbExtension->PendingTicks;
#endif
        Ioq->AcceptedSrbs++;
        SrbExtension->RingAbortPending = FALSE;
    }

    SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);
    return STATUS_SUCCESS;
}

SPD_WORK_SLOT *SpdIoqRingDetachSlots(SPD_IOQ *Ioq)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    SPD_WORK_SLOT *Slots;
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    /* OPEN rollback can race a reset before the ring is published. */
    for (;;)
    {
        SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
        if (0 == Ioq->ResetOwners)
            break;
        SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
        LARGE_INTEGER Delay;
        Delay.QuadPart = -100000;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    Slots = Ioq->Slots;
    for (UINT32 I = 0; I < Ioq->SlotCount; I++)
        ASSERT(0 == Slots[I].Srb &&
            (SpdSlotFree == Slots[I].State || SpdSlotInFlight == Slots[I].State));
    ASSERT(0 == Ioq->ActiveSrbs);
    Ioq->FlushInProgress = FALSE; /* Physical rundown ends a quarantined fence. */
    Ioq->Slots = 0;
    Ioq->SlotCount = 0;
    InitializeListHead(&Ioq->FreeSlots);
    InitializeListHead(&Ioq->ReadySlots);
    InitializeListHead(&Ioq->CompletedSlots);
    if (!Ioq->Stopped)
    {
        Ioq->NonblockingConsumer = FALSE; /* OPEN rollback, before any lease. */
        for (PLIST_ENTRY Entry = Ioq->PendingList.Flink;
            Entry != &Ioq->PendingList; Entry = Entry->Flink)
            CONTAINING_RECORD(Entry, SPD_SRB_EXTENSION, ListEntry)->RingState = SpdRingSrbNone;
        if (!IsListEmpty(&Ioq->PendingList))
            SpdQeventSetNoLock(&Ioq->PendingEvent);
    }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Slots;
}

VOID SpdIoqRingBeginClose(SPD_IOQ *Ioq)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    ASSERT(Ioq->Stopped);
    /* CLOSE already drained public queues and cancelled copy owners. Later
     * resets have no new work; existing detached batches must finish. */
    Ioq->RingClosing = TRUE;
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
}

VOID SpdIoqLogDiagnostics(SPD_IOQ *Ioq)
{
    LARGE_INTEGER Frequency;
    (void)KeQueryPerformanceCounter(&Frequency);
    DbgPrint(DRIVER_NAME ": IOQ spinlock acquisitions=%I64d wait_ticks=%I64d hold_ticks=%I64d qpc_hz=%I64d\n",
        InterlockedCompareExchange64(&Ioq->SpinLockAcquisitions, 0, 0),
        InterlockedCompareExchange64(&Ioq->SpinLockWaitTicks, 0, 0),
        InterlockedCompareExchange64(&Ioq->SpinLockHoldTicks, 0, 0),
        Frequency.QuadPart);
    DbgPrint(DRIVER_NAME ": IOQ logical accepted=%I64u retired=%I64u active=%lu fence=%u lock_acquisitions=%I64d\n",
        Ioq->AcceptedSrbs, Ioq->RetiredSrbs, Ioq->ActiveSrbs,
        (unsigned)Ioq->FlushInProgress, Ioq->SpinLockAcquisitions);
    SpdHistogramLog(&Ioq->SpinLockWaitHistogram, "lock_wait");
    SpdHistogramLog(&Ioq->SpinLockHoldHistogram, "lock_hold");
}

static const char *SpdIoqResetReasonString(
    SPD_IOQ_RESET_REASON Reason)
{
    switch (Reason)
    {
    case SpdIoqResetReasonDelete: return "Delete";
    case SpdIoqResetReasonBusReset: return "BusReset";
    case SpdIoqResetReasonDeviceReset: return "DeviceReset";
    case SpdIoqResetReasonLuReset: return "LuReset";
    case SpdIoqResetReasonRingFailure: return "RingFailure";
    case SpdIoqResetReasonRingStop: return "RingStop";
    case SpdIoqResetReasonRingClose: return "RingClose";
    case SpdIoqResetReasonRemoval: return "Removal";
    default: return "Unknown";
    }
}

VOID SpdIoqReset(SPD_IOQ *Ioq, BOOLEAN Stop,
    SPD_IOQ_RESET_REASON Reason)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER LockStart;
    LIST_ENTRY RetiredList;
    BOOLEAN TerminalReset;
    BOOLEAN ResetOwner;
#if defined(WINSPD_TEST_BUILD)
    SPD_STORAGE_UNIT *TestStorageUnit = 0;
#endif

    InitializeListHead(&RetiredList);
    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);
    if (Ioq->RingClosing)
    {
        SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);
        return;
    }
    ResetOwner = Ioq->NonblockingConsumer;
    if (ResetOwner)
        Ioq->ResetOwners++;
#if defined(WINSPD_TEST_BUILD)
    /* Observe the post/reset boundary independently of queue membership: an
     * earlier producer may have claimed the SRB while StartIo is paused. */
    if (Ioq->NonblockingConsumer && 0 != Ioq->StorageUnit &&
        0 != InterlockedCompareExchange(
            &Ioq->StorageUnit->Ring.TestIoqPostBarrierActive, 0, 0))
        TestStorageUnit = Ioq->StorageUnit;
#endif

    TerminalReset = Ioq->NonblockingConsumer &&
        (SpdIoqResetReasonBusReset == Reason ||
         SpdIoqResetReasonDeviceReset == Reason ||
         SpdIoqResetReasonLuReset == Reason);
    if (TerminalReset)
        Stop = TRUE;

    if (Stop)
    {
        Ioq->Stopped = TRUE;
        /* STOP rejects new leases immediately and wakes legacy consumers. */
        SpdQeventSetNoLock(&Ioq->PendingEvent);
    }

    if (!Ioq->Stopped || Stop)
    {
        ULONG PendingCount = 0;
        ULONG ProcessCount = 0;

        for (PLIST_ENTRY Entry = Ioq->PendingList.Flink;
            Entry != &Ioq->PendingList; Entry = Entry->Flink)
            PendingCount++;
        for (PLIST_ENTRY Entry = Ioq->ProcessList.Flink;
            Entry != &Ioq->ProcessList; Entry = Entry->Flink)
            ProcessCount++;
        DbgPrint(DRIVER_NAME ": IOQ RESET reason=%s stop=%u pending=%lu process-count=%lu\n",
            SpdIoqResetReasonString(Reason), (unsigned)Stop,
            PendingCount, ProcessCount);

        while (!IsListEmpty(&Ioq->PendingList))
        {
            PLIST_ENTRY PendingEntry = RemoveHeadList(&Ioq->PendingList);
            SPD_SRB_EXTENSION *SrbExtension = CONTAINING_RECORD(
                PendingEntry, SPD_SRB_EXTENSION, ListEntry);
            PVOID Srb = SrbExtension->Srb;
            SrbExtension->RingState = SpdRingSrbRetiring;
            SrbExtension->RingAbortPending = FALSE;
            InsertTailList(&RetiredList, &SrbExtension->ListEntry);
#if defined(WINSPD_TEST_BUILD)
            if (0 == TestStorageUnit &&
                0 != SrbExtension->StorageUnit &&
                0 != InterlockedCompareExchange(
                    &SrbExtension->StorageUnit->Ring.TestIoqPostBarrierActive,
                    0, 0))
                TestStorageUnit = SrbExtension->StorageUnit;
#endif
            DbgPrint(DRIVER_NAME ": IOQ RESET reason=%s pending ext=%p srb=%p aborted\n",
                SpdIoqResetReasonString(Reason), SrbExtension,
                Srb);
        }

        PLIST_ENTRY ProcessEntry = Ioq->ProcessList.Flink;
        while (ProcessEntry != &Ioq->ProcessList)
        {
            SPD_SRB_EXTENSION *SrbExtension = CONTAINING_RECORD(
                ProcessEntry, SPD_SRB_EXTENSION, ListEntry);
            PLIST_ENTRY NextEntry = ProcessEntry->Flink;

            if (SpdRingSrbPreparing == SrbExtension->RingState ||
                SpdRingSrbCompleting == SrbExtension->RingState)
            {
                SrbExtension->RingAbortPending = TRUE;
                ProcessEntry = NextEntry;
                continue;
            }

            SpdIoqRemoveProcessHashNoLock(Ioq, SrbExtension);
            RemoveEntryList(&SrbExtension->ListEntry);
            SrbExtension->RingState = SpdRingSrbRetiring;
            SrbExtension->RingAbortPending = FALSE;
            PVOID Srb = SrbExtension->Srb;
            InsertTailList(&RetiredList, &SrbExtension->ListEntry);
#if defined(WINSPD_TEST_BUILD)
            if (0 == TestStorageUnit &&
                0 != SrbExtension->StorageUnit &&
                0 != InterlockedCompareExchange(
                    &SrbExtension->StorageUnit->Ring.TestIoqPostBarrierActive,
                    0, 0))
                TestStorageUnit = SrbExtension->StorageUnit;
#endif
            DbgPrint(DRIVER_NAME ": IOQ RESET reason=%s process ext=%p srb=%p aborted\n",
                SpdIoqResetReasonString(Reason), SrbExtension,
                Srb);
            ProcessEntry = NextEntry;
        }
        for (UINT32 I = 0; I < Ioq->SlotCount; I++)
        {
            SPD_WORK_SLOT *Slot = &Ioq->Slots[I];
            SPD_SRB_EXTENSION *Extension = Slot->Srb;
            if (0 == Extension)
                continue;
            Slot->CancelRequested = TRUE;
            Extension->RingAbortPending = TRUE;
            if (SpdSlotPreparing == Slot->State || SpdSlotCompleting == Slot->State)
                continue; /* Copy owner resolves this lease. */
            if (Slot->MappingLease)
                continue; /* Retain SRB pages until terminal userspace rundown. */
            if (SpdSlotInFlight == Slot->State && Extension->RingIsFlush)
            {
                /* The SRB may retire now, but a published fence remains set
                 * until physical session rundown proves payload quiescence. */
                Extension->ExecutionStarted = FALSE;
            }
            Extension->Slot = 0;
            Extension->RingState = SpdRingSrbRetiring;
            Extension->RingAbortPending = FALSE;
            InsertTailList(&RetiredList, &Extension->ListEntry);
            Slot->Srb = 0;
            if (SpdSlotReady == Slot->State || SpdSlotCompletionQueued == Slot->State)
            {
                RemoveEntryList(&Slot->ListEntry);
                SpdIoqRingFreeSlotNoLock(Ioq, Slot);
            }
            else
                ASSERT(SpdSlotInFlight == Slot->State); /* Quarantined. */
        }
    }

    SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);
    if (TerminalReset)
        SpdStorageUnitRingTerminalReset(Ioq->StorageUnit);
    /* The detached list owns these SRBs. Finish metadata in bounded groups,
     * then notify Storport with no IOQ lock held. New arrivals and another
     * reset cannot attach to, retire, or reinitialize this private list. */
    while (!IsListEmpty(&RetiredList))
    {
        PVOID Srbs[32];
        ULONG Count = 0;
        SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);
        while (Count < RTL_NUMBER_OF(Srbs) && !IsListEmpty(&RetiredList))
        {
            SPD_SRB_EXTENSION *Extension = CONTAINING_RECORD(
                RemoveHeadList(&RetiredList), SPD_SRB_EXTENSION, ListEntry);
            ASSERT(SpdRingSrbRetiring == Extension->RingState);
            Srbs[Count++] = Extension->Srb;
            SpdIoqRingRetireLogicalNoLock(Ioq, Extension);
            Extension->Srb = 0;
            Extension->ListEntry.Flink = Extension->ListEntry.Blink = 0;
            Extension->RingState = SpdRingSrbNone;
        }
        SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);
        for (ULONG I = 0; I < Count; I++)
            SpdSrbComplete(Ioq->DeviceExtension, Srbs[I], SRB_STATUS_ABORTED);
    }
#if defined(WINSPD_TEST_BUILD)
    if (0 != TestStorageUnit)
        SpdStorageUnitRingTestIoqResetComplete(TestStorageUnit);
    if (ResetOwner)
        SpdStorageUnitRingTestResetRetireBarrier(Ioq->StorageUnit);
#endif
    if (ResetOwner)
    {
        SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);
        ASSERT(0 != Ioq->ResetOwners);
        Ioq->ResetOwners--;
        SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);
    }
}

BOOLEAN SpdIoqStopped(SPD_IOQ *Ioq)
{
    BOOLEAN Result;
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER LockStart;

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);

    Result = Ioq->Stopped;

    SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);

    return Result;
}

/* The caller owns the metadata lock and completes any detached SRB after
 * unlocking. Worker-owned requests only receive a cancellation flag. */
static NTSTATUS SpdIoqCancelSrbNoLock(SPD_IOQ *Ioq, PVOID Srb,
    PVOID *PCompleteSrb)
{
    NTSTATUS Result = STATUS_NOT_FOUND;
    *PCompleteSrb = 0;

    if (!Ioq->Stopped)
    {
        SPD_SRB_EXTENSION *SrbExtension = SpdSrbExtension(Srb);
        BOOLEAN Pending = FALSE;
        BOOLEAN Processing = FALSE;
        ULONG Index = 0;

        ASSERT(Srb == SrbExtension->Srb);

        if (Ioq->NonblockingConsumer)
        {
            if (Srb != SrbExtension->Srb)
                goto exit;
            SPD_WORK_SLOT *Slot = SrbExtension->Slot;
            switch (SrbExtension->RingState)
            {
            case SpdRingSrbPending:
                RemoveEntryList(&SrbExtension->ListEntry);
                break;
            case SpdRingSrbPreparing:
            case SpdRingSrbCompleting:
                ASSERT(0 != Slot && Slot->Srb == SrbExtension);
                Slot->CancelRequested = TRUE;
                SrbExtension->RingAbortPending = TRUE;
                Result = STATUS_SUCCESS;
                goto exit;
            case SpdRingSrbReady:
            case SpdRingSrbCompletionQueued:
                if (Slot->MappingLease)
                {
                    Slot->CancelRequested = TRUE;
                    SrbExtension->RingAbortPending = TRUE;
                    if (SpdSlotReady == Slot->State)
                    {
                        RemoveEntryList(&Slot->ListEntry);
                        Slot->State = SpdSlotCompletionQueued;
                        SrbExtension->RingState = SpdRingSrbCompletionQueued;
                        Slot->CqTicks = KeQueryPerformanceCounter(0).QuadPart;
#if SPD_RING_LATENCY_DIAGNOSTICS
                        Ioq->CompletionQueuedCount++;
#endif
                        InsertTailList(&Ioq->CompletedSlots, &Slot->ListEntry);
                    }
                    Result = STATUS_SUCCESS;
                    goto exit;
                }
                RemoveEntryList(&Slot->ListEntry);
                SpdIoqRingFreeSlotNoLock(Ioq, Slot);
                break;
            case SpdRingSrbInFlight:
                Result = STATUS_NOT_SUPPORTED;
                goto exit;
            default:
                goto exit;
            }
            SrbExtension->ListEntry.Flink = SrbExtension->ListEntry.Blink = 0;
            SpdIoqRingRetireLogicalNoLock(Ioq, SrbExtension);
            SrbExtension->Srb = 0;
            SrbExtension->Slot = 0;
            SrbExtension->RingState = SpdRingSrbNone;
            SrbExtension->RingAbortPending = FALSE;
            *PCompleteSrb = Srb;
            Result = STATUS_SUCCESS;
            goto exit;
        }

        if (Srb == SrbExtension->Srb &&
            0 != SrbExtension->ListEntry.Flink &&
            0 != SrbExtension->ListEntry.Blink)
        {
            if (SpdRingSrbInFlight == SrbExtension->RingState)
            {
                /* Publication cannot be undone by an ordinary abort: the
                 * backend may still be executing against the shared buffer. */
                Result = STATUS_NOT_SUPPORTED;
                goto exit;
            }
            if (SpdRingSrbPreparing == SrbExtension->RingState ||
                SpdRingSrbCompleting == SrbExtension->RingState)
            {
                SrbExtension->RingAbortPending = TRUE;
                Result = STATUS_SUCCESS;
                goto exit;
            }

            for (PLIST_ENTRY Entry = Ioq->PendingList.Flink;
                Entry != &Ioq->PendingList; Entry = Entry->Flink)
                if (Entry == &SrbExtension->ListEntry)
                {
                    Pending = TRUE;
                    break;
                }

            Index = SpdHashMixPointer(SrbExtension) %
                Ioq->ProcessBucketCount;
            if (!Pending)
            {
                for (PVOID *P = &Ioq->ProcessBuckets[Index]; *P;
                    P = &((SPD_SRB_EXTENSION *)(*P))->HashNext)
                    if (*P == SrbExtension)
                    {
                        *P = SrbExtension->HashNext;
                        SrbExtension->HashNext = 0;
                        Processing = TRUE;
                        break;
                    }
            }

            if (Pending || Processing)
            {
                DbgPrint(DRIVER_NAME ": IOQ CANCEL target-ext=%p target-srb=%p state=%s bucket=%lu\n",
                    SrbExtension, Srb,
                    Pending ? "pending" : "process", Index);
                RemoveEntryList(&SrbExtension->ListEntry);
                SrbExtension->ListEntry.Flink =
                    SrbExtension->ListEntry.Blink = 0;
                SrbExtension->RingState = SpdRingSrbNone;
                SrbExtension->RingAbortPending = FALSE;
                SrbExtension->Srb = 0;
                *PCompleteSrb = Srb;
                Result = STATUS_SUCCESS;
            }
            else
                DbgPrint(DRIVER_NAME ": IOQ CANCEL target-ext=%p target-srb=%p NOT_FOUND bucket=%lu\n",
                    SrbExtension, Srb, Index);
        }
        else
            DbgPrint(DRIVER_NAME ": IOQ CANCEL target-ext=%p target-srb=%p NOT_FOUND\n",
                SrbExtension, Srb);
    }
    else
        DbgPrint(DRIVER_NAME ": IOQ CANCEL target-srb=%p CANCELLED stopped=1\n",
            Srb);

exit:
    return Result;
}

/* Both Storport aborts and deterministic test cancellation must run the same
 * post-lock wakeups. A cancelled mapped Ready slot needs a completion worker
 * to unmap and retire it even when no CQ traffic is arriving. */
static VOID SpdIoqFinishCancel(SPD_IOQ *Ioq, NTSTATUS Result, PVOID CompleteSrb)
{
    if (NT_SUCCESS(Result) && Ioq->NonblockingConsumer)
        SpdStorageUnitRingScheduleCompletion(Ioq->StorageUnit);
    if (0 != CompleteSrb)
    {
        SpdSrbComplete(Ioq->DeviceExtension, CompleteSrb, SRB_STATUS_ABORTED);
        if (Ioq->NonblockingConsumer)
            SpdStorageUnitRingSchedulePreparation(Ioq->StorageUnit);
    }
}

NTSTATUS SpdIoqCancelSrb(SPD_IOQ *Ioq, PVOID Srb)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER LockStart;
    PVOID CompleteSrb;
    NTSTATUS Result;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);
    Result = SpdIoqCancelSrbNoLock(Ioq, Srb, &CompleteSrb);
    SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);
    SpdIoqFinishCancel(Ioq, Result, CompleteSrb);
    return Result;
}

#if defined(WINSPD_TEST_BUILD)
NTSTATUS SpdIoqTestExhaustGeneration(SPD_IOQ *Ioq)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    NTSTATUS Result = STATUS_DEVICE_BUSY;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    if (!Ioq->Stopped && Ioq->NonblockingConsumer && !IsListEmpty(&Ioq->FreeSlots))
    {
        SPD_WORK_SLOT *Slot = CONTAINING_RECORD(
            Ioq->FreeSlots.Flink, SPD_WORK_SLOT, ListEntry);
        Slot->RequestId = (SPD_SLOT_GENERATION_MAX << 16) | (UINT32)(Slot - Ioq->Slots);
        Result = STATUS_SUCCESS;
    }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Result;
}

/* Convert only a still-pending, never-started tagged READ. Keeping the real
 * Storport SRB/IRP and production queues lets tests reach the miniport fence
 * without the port's own untagged pass-through ordering hiding that state. */
NTSTATUS SpdIoqTestPromoteFlush(SPD_IOQ *Ioq, UINT64 BlockAddress)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    NTSTATUS Result = STATUS_NOT_FOUND;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    if (!Ioq->Stopped && Ioq->NonblockingConsumer && MAXULONG >= BlockAddress)
        for (PLIST_ENTRY Entry = Ioq->PendingList.Flink;
            Entry != &Ioq->PendingList; Entry = Entry->Flink)
        {
            SPD_SRB_EXTENSION *Extension = CONTAINING_RECORD(Entry, SPD_SRB_EXTENSION, ListEntry);
            if (!Extension->ExecutionStarted && SpdSrbTestMatchesRange(
                    Extension->Srb, SpdIoctlTransactReadKind, BlockAddress) &&
                10 <= SrbGetCdbLength(Extension->Srb))
            {
                PCDB Cdb = SrbGetCdb(Extension->Srb);
                RtlZeroMemory(Cdb, 10);
                Cdb->AsByte[0] = SCSIOP_SYNCHRONIZE_CACHE;
                for (UINT32 I = 0; I < 4; I++)
                    Cdb->AsByte[2 + I] = (UINT8)(BlockAddress >> (8 * (3 - I)));
                Extension->RingIsFlush = SpdSrbIsFlush(Extension->Srb);
                Extension->SystemDataLength = 0;
                Result = STATUS_SUCCESS;
                break;
            }
        }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Result;
}

NTSTATUS SpdIoqTestCopyBudget(SPD_IOQ *Ioq)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    NTSTATUS Result = 0 != Ioq->StorageUnit->Ring.PreparationOversizedClaims &&
        0 == Ioq->StorageUnit->Ring.PreparationCopyBudgetViolations ? STATUS_SUCCESS : STATUS_DATA_ERROR;
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Result;
}

NTSTATUS SpdIoqTestSetChunkLimit(SPD_IOQ *Ioq, UINT32 Limit)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    NTSTATUS Result = STATUS_DEVICE_BUSY;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    if (!Ioq->Stopped && (Ioq->NonblockingConsumer || IsListEmpty(&Ioq->ProcessList)) && 0 == Ioq->ActiveSrbs &&
        !Ioq->FlushInProgress && IsListEmpty(&Ioq->PendingList) &&
        IsListEmpty(&Ioq->ReadySlots) && IsListEmpty(&Ioq->CompletedSlots) &&
        0 != Limit && Limit <= Ioq->StorageUnit->StorageUnitParams.MaxTransferLength &&
        0 == Limit % Ioq->StorageUnit->StorageUnitParams.BlockLength)
    {
        Ioq->StorageUnit->TestChunkLimit = Limit;
        Result = STATUS_SUCCESS;
    }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Result;
}

NTSTATUS SpdIoqTestExpectChunked(SPD_IOQ *Ioq, UINT64 BlockAddress)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    NTSTATUS Result = STATUS_NOT_FOUND;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    for (UINT32 I = 0; I < Ioq->SlotCount; I++)
    {
        SPD_SRB_EXTENSION *Extension = Ioq->Slots[I].Srb;
        if (0 != Extension && Extension->ExecutionStarted && !Extension->RingIsFlush &&
            1 == Ioq->ActiveSrbs && SpdSrbTestMatchesRange(
                Extension->Srb, SpdIoctlTransactWriteKind, BlockAddress) &&
            0 != Extension->StorageUnit->TestChunkLimit &&
            Extension->SystemDataLength > Extension->StorageUnit->TestChunkLimit)
        {
            Result = STATUS_SUCCESS;
            break;
        }
    }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Result;
}

NTSTATUS SpdIoqTestExpectPending(SPD_IOQ *Ioq, UINT32 Kind, UINT64 BlockAddress)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    NTSTATUS Result = STATUS_NOT_FOUND;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    for (PLIST_ENTRY Entry = Ioq->PendingList.Flink;
        Entry != &Ioq->PendingList; Entry = Entry->Flink)
    {
        SPD_SRB_EXTENSION *Extension = CONTAINING_RECORD(Entry, SPD_SRB_EXTENSION, ListEntry);
        if (SpdSrbTestMatchesRange(Extension->Srb, Kind, BlockAddress))
        {
            Result = STATUS_SUCCESS;
            break;
        }
    }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Result;
}

NTSTATUS SpdIoqTestExpectFence(SPD_IOQ *Ioq)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    NTSTATUS Result = Ioq->FlushInProgress && 0 == Ioq->ActiveSrbs ?
        STATUS_SUCCESS : STATUS_NOT_FOUND;
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Result;
}

NTSTATUS SpdIoqTestCancel(SPD_IOQ *Ioq, UINT32 Kind,
    UINT64 BlockAddress, UINT32 ExpectedState)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER LockStart;
    PVOID CompleteSrb = 0;
    NTSTATUS Result = STATUS_NOT_FOUND;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);
    if (!Ioq->Stopped && Ioq->NonblockingConsumer && SPD_TEST_CANCEL_PENDING == ExpectedState)
        for (PLIST_ENTRY Entry = Ioq->PendingList.Flink;
            Entry != &Ioq->PendingList; Entry = Entry->Flink)
        {
            SPD_SRB_EXTENSION *Extension = CONTAINING_RECORD(Entry, SPD_SRB_EXTENSION, ListEntry);
            if (SpdSrbTestMatchesRange(Extension->Srb, Kind, BlockAddress))
            {
                Result = SpdIoqCancelSrbNoLock(Ioq, Extension->Srb, &CompleteSrb);
                break;
            }
        }
    if (!Ioq->Stopped && Ioq->NonblockingConsumer && SPD_TEST_CANCEL_PENDING != ExpectedState)
        for (UINT32 I = 0; I < Ioq->SlotCount; I++)
        {
            SPD_SRB_EXTENSION *Extension = Ioq->Slots[I].Srb;
            if (0 == Extension)
                continue;
            BOOLEAN Expected =
                (SPD_TEST_CANCEL_PREPARING == ExpectedState &&
                    SpdRingSrbPreparing == Extension->RingState) ||
                (SPD_TEST_CANCEL_COMPLETING == ExpectedState &&
                    SpdRingSrbCompleting == Extension->RingState) ||
                (SPD_TEST_CANCEL_INFLIGHT == ExpectedState &&
                    SpdRingSrbInFlight == Extension->RingState) ||
                (SPD_TEST_CANCEL_READY == ExpectedState &&
                    SpdRingSrbReady == Extension->RingState) ||
                (SPD_TEST_CANCEL_COMPLETION_QUEUED == ExpectedState &&
                    SpdRingSrbCompletionQueued == Extension->RingState);
            if (Expected && 0 != Extension->Srb &&
                SpdSrbTestMatchesRange(Extension->Srb, Kind, BlockAddress))
            {
                Result = SpdIoqCancelSrbNoLock(Ioq, Extension->Srb,
                    &CompleteSrb);
                break;
            }
        }
    SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);
    SpdIoqFinishCancel(Ioq, Result, CompleteSrb);
    return Result;
}
#endif

NTSTATUS SpdIoqPostSrb(SPD_STORAGE_UNIT *StorageUnit, PVOID Srb)
{
    SPD_IOQ *Ioq = StorageUnit->Ioq;
    NTSTATUS Result = STATUS_CANCELLED;
    BOOLEAN ScheduleRingPreparation = FALSE;
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER LockStart;

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);

    if (!Ioq->Stopped)
    {
        SPD_SRB_EXTENSION *SrbExtension = SpdSrbExtension(Srb);

        ASSERT(0 == SrbExtension->Srb);
        SrbExtension->Srb = Srb;
        SrbExtension->RingIsFlush = SpdSrbIsFlush(Srb);
        if (Ioq->NonblockingConsumer)
        {
            SrbExtension->PendingTicks = KeQueryPerformanceCounter(0).QuadPart;
#if SPD_RING_LATENCY_DIAGNOSTICS
        SrbExtension->AcceptedTicks = SrbExtension->PendingTicks;
#endif
            Ioq->AcceptedSrbs++;
        }
        if (Ioq->NonblockingConsumer && SRB_FUNCTION_FLUSH == SrbGetSrbFunction(Srb))
            InterlockedIncrement64(&StorageUnit->Ring.NonCdbFlushSubmissions);
        if (Ioq->NonblockingConsumer && SRB_FUNCTION_SHUTDOWN == SrbGetSrbFunction(Srb))
            InterlockedIncrement64(&StorageUnit->Ring.ShutdownSubmissions);
        ASSERT(!SrbExtension->ExecutionStarted);
        SrbExtension->RingState = Ioq->NonblockingConsumer ?
            SpdRingSrbPending : SpdRingSrbNone;
        SrbExtension->RingAbortPending = FALSE;
        SrbExtension->Slot = 0;

        ASSERT(0 == SrbExtension->ListEntry.Flink && 0 == SrbExtension->ListEntry.Blink);
        InsertTailList(&Ioq->PendingList, &SrbExtension->ListEntry);

        if (!Ioq->NonblockingConsumer)
            SpdQeventSetNoLock(&Ioq->PendingEvent);
        else
            ScheduleRingPreparation = TRUE;

        Result = STATUS_SUCCESS;
    }
    else if (Ioq->NonblockingConsumer)
        /* A stopped ring is terminal. Do not invite disk.sys to retry it. */
        Result = STATUS_DEVICE_NOT_CONNECTED;

    SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);

#if defined(WINSPD_TEST_BUILD)
    if (NT_SUCCESS(Result) && ScheduleRingPreparation)
        SpdStorageUnitRingTestIoqPostBarrier(StorageUnit);
#endif
    if (NT_SUCCESS(Result) && ScheduleRingPreparation)
        SpdStorageUnitRingSchedulePreparation(StorageUnit);

    return Result;
}

/* All slot/list transitions below use the IOQ metadata lock. Payload copying
 * belongs to the caller holding a Preparing/Completing lease, never this lock. */
static VOID SpdIoqRingFreeSlotNoLock(SPD_IOQ *Ioq, SPD_WORK_SLOT *Slot)
{
    ASSERT(0 == Slot->MappedUserAddress && 0 == Slot->MappingMdl);
    Slot->MappingLease = FALSE;
#if SPD_RING_LATENCY_DIAGNOSTICS
    if (SpdSlotCompletionQueued == Slot->State)
    {
        ASSERT(0 != Ioq->CompletionQueuedCount);
        Ioq->CompletionQueuedCount--;
    }
#endif
    Slot->Srb = 0;
    Slot->CancelRequested = FALSE;
    Slot->State = SpdSlotFree;
    /* Free-list order is not scheduling order. Retain immediate buffer reuse. */
    InsertHeadList(&Ioq->FreeSlots, &Slot->ListEntry);
}

NTSTATUS SpdIoqRingClaimBatch(SPD_IOQ *Ioq,
    SPD_WORK_SLOT **Slots, UINT32 Limit, UINT32 CopyByteLimit, UINT32 *PCount)
{
    UINT32 CopyBytes = 0, CopyRequests = 0;
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    NTSTATUS Result = STATUS_SUCCESS;
    *PCount = 0;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
#if defined(WINSPD_TEST_BUILD)
    /* Fence tests construct tagged pending commands under a claim gate.
     * Check at the ownership transition, so another preparation thread cannot
     * pass a callback barrier and claim the probe before it becomes FLUSH. */
    PKEVENT ClaimRelease = Ioq->StorageUnit->Ring.TestPreparationClaimReleaseEvent;
    if (0 != ClaimRelease && 0 == KeReadStateEvent(ClaimRelease))
    {
        SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
        return STATUS_SUCCESS;
    }
#endif
    if (Ioq->Stopped)
        Result = STATUS_CANCELLED;
    if (!Ioq->Stopped && !IsListEmpty(&Ioq->PendingList) && IsListEmpty(&Ioq->FreeSlots))
        InterlockedIncrement64(&Ioq->StorageUnit->Ring.BufferPoolExhaustions);
    UINT64 Now = KeQueryPerformanceCounter(0).QuadPart;
    if (NT_SUCCESS(Result)) while (*PCount < Limit && SpdIoqRingCanClaimNoLock(Ioq))
    {
        SPD_WORK_SLOT *Slot = CONTAINING_RECORD(
            Ioq->FreeSlots.Flink, SPD_WORK_SLOT, ListEntry);
        UINT64 Generation = Slot->RequestId >> 16;
        if (SPD_SLOT_GENERATION_MAX == Generation)
        {
            Ioq->Stopped = TRUE;
            Result = STATUS_INTEGER_OVERFLOW;
            break; /* Never wrap a lease token. Caller fails the session. */
        }
        SPD_SRB_EXTENSION *Extension = CONTAINING_RECORD(
            Ioq->PendingList.Flink, SPD_SRB_EXTENSION, ListEntry);
        UINT32 Bytes = SpdSrbPreparationCopyBytes(Extension);
        /* Always permit a single request larger than the nominal budget. */
        if (0 != *PCount && Bytes > CopyByteLimit - min(CopyBytes, CopyByteLimit))
            break;
        if (!Extension->ExecutionStarted)
        {
            if (Extension->RingIsFlush)
            {
                ASSERT(0 == Ioq->ActiveSrbs && !Ioq->FlushInProgress);
                Ioq->FlushInProgress = TRUE;
            }
            else
            {
                if (MAXULONG == Ioq->ActiveSrbs)
                {
                    Result = STATUS_INTEGER_OVERFLOW;
                    break;
                }
                Ioq->ActiveSrbs++;
            }
            Extension->ExecutionStarted = TRUE;
        }
        CopyBytes += Bytes;
        if (0 != Bytes)
            CopyRequests++;
        RemoveEntryList(&Extension->ListEntry);
        RemoveEntryList(&Slot->ListEntry);
        Slot->ListEntry.Flink = Slot->ListEntry.Blink = 0;
        Extension->ListEntry.Flink = Extension->ListEntry.Blink = 0;
        ASSERT(SpdRingSrbPending == Extension->RingState);
        ASSERT(0 == Extension->Slot && 0 != Extension->Srb);
        Slot->RequestId = ((Generation + 1) << 16) | (UINT32)(Slot - Ioq->Slots);
        Slot->PreparingTicks = Now;
#if SPD_RING_LATENCY_DIAGNOSTICS
        SpdLatencyRecord(&Ioq->StorageUnit->Ring.Latency[SpdLatencyPending], Now - Extension->PendingTicks);
#endif
        SpdHistogramRecord(&Ioq->StorageUnit->Ring.PendingToPreparing, Now - Extension->PendingTicks);
        Slot->State = SpdSlotPreparing;
        Slot->CancelRequested = FALSE;
        Slot->MappingLease = Extension->MappingCandidate;
        Slot->Srb = Extension;
        Slot->DataLength = 0;
        Extension->Slot = Slot;
        Extension->RingState = SpdRingSrbPreparing;
        Slots[(*PCount)++] = Slot;
    }
    if (0 != *PCount)
    {
        SPD_RING_STATE *Ring = &Ioq->StorageUnit->Ring;
        Ring->PreparationClaimCopyBytes += CopyBytes;
        if (CopyBytes > CopyByteLimit)
        {
            Ring->PreparationOversizedClaims++;
            if (1 != CopyRequests)
                Ring->PreparationCopyBudgetViolations++;
        }
    }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Result;
}

UINT32 SpdIoqRingCommitBatch(SPD_IOQ *Ioq, SPD_WORK_SLOT **Slots,
    SPD_RING_REQUEST *Requests, UINT32 Count, BOOLEAN AbortAll, PVOID *AbortedSrbs,
    BOOLEAN *PQueueCleanup)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    UINT32 Aborted = 0;
    *PQueueCleanup = FALSE;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    UINT64 Now = KeQueryPerformanceCounter(0).QuadPart;
    for (UINT32 I = 0; I < Count; I++)
    {
        SPD_WORK_SLOT *Slot = Slots[I];
        SPD_SRB_EXTENSION *Extension = Slot->Srb;
        ASSERT(SpdSlotPreparing == Slot->State && Extension->Slot == Slot);
        if (Ioq->Stopped || AbortAll || Slot->CancelRequested)
        {
            if (Slot->MappingLease)
            {
                *PQueueCleanup = TRUE;
                /* Completion context owns unmapping; never do it under lock. */
                Slot->CancelRequested = TRUE;
                Slot->State = SpdSlotCompletionQueued;
                Extension->RingState = SpdRingSrbCompletionQueued;
                Slot->CqTicks = Now;
#if SPD_RING_LATENCY_DIAGNOSTICS
                Ioq->CompletionQueuedCount++;
#endif
                InsertTailList(&Ioq->CompletedSlots, &Slot->ListEntry);
                continue;
            }
            if (Slot->CancelRequested)
                InterlockedIncrement64(&Extension->StorageUnit->Ring.DeferredSrbAborts);
            AbortedSrbs[Aborted++] = Extension->Srb;
            SpdIoqRingRetireLogicalNoLock(Ioq, Extension);
            Extension->Srb = 0;
            Extension->Slot = 0;
            Extension->RingState = SpdRingSrbNone;
            Extension->RingAbortPending = FALSE;
            SpdIoqRingFreeSlotNoLock(Ioq, Slot);
        }
        else
        {
            Slot->Request = Requests[I].Request;
            Slot->Request.Hint = Slot->RequestId;
            Slot->DataLength = Requests[I].Data.Length;
            Slot->ReadyTicks = Now;
#if SPD_RING_LATENCY_DIAGNOSTICS
            SpdLatencyRecord(&Ioq->StorageUnit->Ring.Latency[SpdLatencyPrepare], Now - Slot->PreparingTicks);
#endif
            SpdHistogramRecord(&Ioq->StorageUnit->Ring.PreparingToReady, Now - Slot->PreparingTicks);
            Slot->State = SpdSlotReady;
            Extension->RingState = SpdRingSrbReady;
            InsertTailList(&Ioq->ReadySlots, &Slot->ListEntry);
        }
    }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Aborted;
}

UINT32 SpdIoqRingPublishBatch(SPD_IOQ *Ioq, UINT32 Limit, BOOLEAN *PInvalidHead)
{
    SPD_RING_STATE *Ring = &Ioq->StorageUnit->Ring;
    SPD_RING_HEADER *Header = Ring->SystemAddress;
    SPD_RING_REQUEST *Requests = (SPD_RING_REQUEST *)
        ((PUINT8)Ring->SystemAddress + Ring->RequestOffset);
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    UINT32 Count = 0;
    *PInvalidHead = FALSE;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    UINT64 Now = KeQueryPerformanceCounter(0).QuadPart;
    if (!Ioq->Stopped)
    {
        UINT32 Used = Ring->RequestTail -
            SpdRingLoadAcquire32(&Header->RequestHead.Value);
        if (Used > Ring->QueueDepth)
        {
            Ioq->Stopped = TRUE;
            *PInvalidHead = TRUE;
        }
        else
        {
            Limit = min(Limit, Ring->QueueDepth - Used);
            while (Count < Limit && !IsListEmpty(&Ioq->ReadySlots))
            {
                SPD_WORK_SLOT *Slot = CONTAINING_RECORD(
                    RemoveHeadList(&Ioq->ReadySlots), SPD_WORK_SLOT, ListEntry);
                SPD_RING_REQUEST *Entry = &Requests[
                    (Ring->RequestTail + Count) & (Ring->QueueDepth - 1)];
                ASSERT(SpdSlotReady == Slot->State && !Slot->CancelRequested);
                Entry->Request = Slot->Request;
                Entry->Data.BufferId = SpdIoctlTransactFlushKind == Slot->Request.Kind ?
                    SPD_RING_NO_BUFFER : (UINT32)(Slot - Ioq->Slots);
                Entry->Data.Length = Slot->DataLength;
                Entry->Data.UserAddress = (UINT64)(UINT_PTR)Slot->MappedUserAddress;
                Slot->PublishedTicks = Now;
#if SPD_RING_LATENCY_DIAGNOSTICS
                SpdLatencyRecord(&Ring->Latency[SpdLatencyReady], Now - Slot->ReadyTicks);
#endif
                SpdHistogramRecord(&Ring->ReadyToSq, Now - Slot->ReadyTicks);
                Slot->State = SpdSlotInFlight;
                Slot->ListEntry.Flink = Slot->ListEntry.Blink = 0;
                Slot->Srb->RingState = SpdRingSrbInFlight;
                Count++;
            }
            if (0 != Count)
            {
                Ring->RequestTail += Count;
                SpdRingStoreRelease32(&Header->RequestTail.Value, Ring->RequestTail);
            }
        }
    }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Count;
}

NTSTATUS SpdIoqRingEnqueueCompletions(SPD_IOQ *Ioq,
    const SPD_RING_COMPLETION *Completions, UINT32 Count, UINT32 *PAccepted)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    NTSTATUS Result = STATUS_SUCCESS;
    *PAccepted = 0;
#if SPD_RING_LATENCY_DIAGNOSTICS
    UINT64 LockBefore = KeQueryPerformanceCounter(0).QuadPart;
#endif
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
#if SPD_RING_LATENCY_DIAGNOSTICS
    SpdLatencyRecord(&Ioq->StorageUnit->Ring.Latency[SpdLatencyCompletionEnqueueLock], Start.QuadPart - LockBefore);
#endif
    UINT64 Now = KeQueryPerformanceCounter(0).QuadPart;
    if (Ioq->Stopped)
        Result = STATUS_CANCELLED;
    else for (UINT32 I = 0; I < Count; I++)
    {
        const SPD_RING_COMPLETION *Completion = &Completions[I];
        UINT32 Index = (UINT32)(Completion->Response.Hint & 0xffff);
        if (Index >= Ioq->SlotCount)
        {
            Ioq->Stopped = TRUE;
            Result = STATUS_INVALID_PARAMETER;
            break;
        }
        SPD_WORK_SLOT *Slot = &Ioq->Slots[Index];
        UINT32 BufferId = SpdIoctlTransactFlushKind == Slot->Request.Kind ?
            SPD_RING_NO_BUFFER : Index;
        if (SpdSlotInFlight != Slot->State ||
            Slot->RequestId != Completion->Response.Hint ||
            Slot->Request.Kind != Completion->Response.Kind ||
            BufferId != Completion->Data.BufferId ||
            (UINT64)(UINT_PTR)Slot->MappedUserAddress != Completion->Data.UserAddress ||
            Slot->DataLength != Completion->Data.Length || 0 == Slot->Srb)
        {
            /* Freeze claims/publication before releasing metadata authority.
             * Reset subsequently retires SRBs and quarantines published slots. */
            Ioq->Stopped = TRUE;
            Result = STATUS_INVALID_PARAMETER;
            break;
        }
        ASSERT(Slot->Srb->Slot == Slot && 0 != Slot->Srb->Srb);
        Slot->CqTicks = Now;
#if SPD_RING_LATENCY_DIAGNOSTICS
        SpdLatencyRecord(&Ioq->StorageUnit->Ring.Latency[SpdLatencyFlight], Now - Slot->PublishedTicks);
        Ioq->CompletionQueuedCount++;
        SpdLatencyRecord(&Ioq->StorageUnit->Ring.Latency[SpdLatencyCompletionBacklog], Ioq->CompletionQueuedCount);
#endif
        if (SpdIoctlTransactFlushKind == Slot->Request.Kind)
            SpdHistogramRecord(&Ioq->StorageUnit->Ring.FlushRoundTrip, Now - Slot->PublishedTicks);
        Slot->Response = Completion->Response;
        Slot->State = SpdSlotCompletionQueued;
        Slot->Srb->RingState = SpdRingSrbCompletionQueued;
        InsertTailList(&Ioq->CompletedSlots, &Slot->ListEntry);
        ++*PAccepted;
    }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Result;
}

/* Only terminal CLOSE calls this after kernel and cooperative user rundown. */
UINT32 SpdIoqRingClaimMappedCleanup(SPD_IOQ *Ioq,
    SPD_WORK_SLOT **Slots, UINT32 Limit)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    UINT32 Count = 0;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    ASSERT(Ioq->Stopped && Ioq->RingClosing);
    for (UINT32 I = 0; I < Ioq->SlotCount && Count < Limit; I++)
    {
        SPD_WORK_SLOT *Slot = &Ioq->Slots[I];
        if (0 == Slot->Srb || !Slot->MappingLease)
            continue;
        ASSERT(SpdSlotReady == Slot->State || SpdSlotInFlight == Slot->State ||
            SpdSlotCompletionQueued == Slot->State);
        if (SpdSlotReady == Slot->State || SpdSlotCompletionQueued == Slot->State)
            RemoveEntryList(&Slot->ListEntry);
#if SPD_RING_LATENCY_DIAGNOSTICS
        if (SpdSlotCompletionQueued == Slot->State)
            Ioq->CompletionQueuedCount--;
#endif
        Slot->ListEntry.Flink = Slot->ListEntry.Blink = 0;
        Slot->State = SpdSlotCompleting;
        Slot->Srb->RingState = SpdRingSrbCompleting;
        Slots[Count++] = Slot;
    }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Count;
}

UINT32 SpdIoqRingClaimCompletedBatch(SPD_IOQ *Ioq,
    SPD_WORK_SLOT **Slots, UINT32 Limit)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    UINT32 Count = 0;
#if SPD_RING_LATENCY_DIAGNOSTICS
    UINT64 LockBefore = KeQueryPerformanceCounter(0).QuadPart;
#endif
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
#if SPD_RING_LATENCY_DIAGNOSTICS
    SpdLatencyRecord(&Ioq->StorageUnit->Ring.Latency[SpdLatencyCompletionClaimLock], Start.QuadPart - LockBefore);
#endif
    while (!Ioq->Stopped && Count < Limit && !IsListEmpty(&Ioq->CompletedSlots))
    {
        SPD_WORK_SLOT *Slot = CONTAINING_RECORD(
            RemoveHeadList(&Ioq->CompletedSlots), SPD_WORK_SLOT, ListEntry);
        ASSERT(SpdSlotCompletionQueued == Slot->State && 0 != Slot->Srb);
        Slot->ListEntry.Flink = Slot->ListEntry.Blink = 0;
#if SPD_RING_LATENCY_DIAGNOSTICS
        Slot->CompletionClaimTicks = KeQueryPerformanceCounter(0).QuadPart;
        SpdLatencyRecord(&Ioq->StorageUnit->Ring.Latency[SpdLatencyCqQueue], Slot->CompletionClaimTicks - Slot->CqTicks);
        ASSERT(0 != Ioq->CompletionQueuedCount);
        Ioq->CompletionQueuedCount--;
#endif
        Slot->State = SpdSlotCompleting;
        Slot->Srb->RingState = SpdRingSrbCompleting;
        Slots[Count++] = Slot;
    }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Count;
}

UINT32 SpdIoqRingFinishCompletedBatch(SPD_IOQ *Ioq,
    SPD_WORK_SLOT **Slots, UCHAR *Statuses, UINT32 Count,
    PVOID *Srbs, UCHAR *FinalStatuses, SPD_RING_RETIRE_TIMING *Timings)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    UINT32 Retired = 0;
#if SPD_RING_LATENCY_DIAGNOSTICS
    UINT64 LockBefore = KeQueryPerformanceCounter(0).QuadPart;
#endif
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
#if SPD_RING_LATENCY_DIAGNOSTICS
    SpdLatencyRecord(&Ioq->StorageUnit->Ring.Latency[SpdLatencyCompletionRetireLock], Start.QuadPart - LockBefore);
#endif
    UINT64 Now = KeQueryPerformanceCounter(0).QuadPart;
    for (UINT32 I = 0; I < Count; I++)
    {
        SPD_WORK_SLOT *Slot = Slots[I];
        SPD_SRB_EXTENSION *Extension = Slot->Srb;
        UCHAR Status = Statuses[I];
#if SPD_RING_LATENCY_DIAGNOSTICS
        SpdLatencyRecord(&Ioq->StorageUnit->Ring.Latency[SpdLatencyCompletionRetireWait], Now - Slot->CompletionServiceEnd);
#endif
        SpdHistogramRecord(&Ioq->StorageUnit->Ring.CqToRetire, Now - Slot->CqTicks);
        ASSERT(SpdSlotCompleting == Slot->State && Extension->Slot == Slot);
        if (Slot->CancelRequested || Ioq->Stopped)
        {
            Status = SRB_STATUS_ABORTED;
            InterlockedIncrement64(&Extension->StorageUnit->Ring.DeferredSrbAborts);
        }
        Extension->Slot = 0;
        Extension->RingAbortPending = FALSE;
        if (SRB_STATUS_PENDING == Status)
        {
            /* This is a continuation of an accepted logical SRB, not a new
             * admission. Its next chunk precedes unclaimed FIFO entries. */
            Extension->PendingTicks = Now;
            Extension->RingState = SpdRingSrbPending;
            InsertHeadList(&Ioq->PendingList, &Extension->ListEntry);
        }
        else
        {
            Timings[Retired].CqTicks = Slot->CqTicks;
#if SPD_RING_LATENCY_DIAGNOSTICS
            Timings[Retired].AcceptedTicks = Extension->AcceptedTicks;
            Timings[Retired].RetiredTicks = Now;
#endif
            Srbs[Retired] = Extension->Srb;
            FinalStatuses[Retired++] = Status;
            SpdIoqRingRetireLogicalNoLock(Ioq, Extension);
            Extension->Srb = 0;
            Extension->RingState = SpdRingSrbNone;
        }
        SpdIoqRingFreeSlotNoLock(Ioq, Slot);
    }
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Retired;
}

BOOLEAN SpdIoqRingHasCompleted(SPD_IOQ *Ioq)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    BOOLEAN Result = !Ioq->Stopped && !IsListEmpty(&Ioq->CompletedSlots);
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Result;
}

ULONG SpdIoqRingLeasedCount(SPD_IOQ *Ioq)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    ULONG Count;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    Count = Ioq->ResetOwners;
    for (UINT32 I = 0; I < Ioq->SlotCount; I++)
        if (SpdSlotPreparing == Ioq->Slots[I].State ||
            SpdSlotCompleting == Ioq->Slots[I].State)
            Count++;
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Count;
}

BOOLEAN SpdIoqRingHasPending(SPD_IOQ *Ioq)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    BOOLEAN Result;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    Result = SpdIoqRingCanClaimNoLock(Ioq);
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Result;
}

BOOLEAN SpdIoqRingHasReady(SPD_IOQ *Ioq)
{
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER Start;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &Start);
    BOOLEAN Result = !Ioq->Stopped && !IsListEmpty(&Ioq->ReadySlots);
    SpdIoqReleaseSpinLock(Ioq, &Irql, Start);
    return Result;
}

NTSTATUS SpdIoqTryStartProcessingSrb(SPD_IOQ *Ioq,
    VOID (*Prepare)(PVOID SrbExtension, PVOID Context, PVOID DataBuffer),
    PVOID Context, PVOID DataBuffer)
{
    NTSTATUS Result;
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER LockStart;

    ASSERT(DISPATCH_LEVEL == KeGetCurrentIrql());

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);

    if (Ioq->Stopped || Ioq->NonblockingConsumer)
    {
        Result = STATUS_CANCELLED;
    }
    else if (IsListEmpty(&Ioq->PendingList))
        Result = STATUS_NOT_FOUND;
    else
    {
        PLIST_ENTRY PendingEntry = Ioq->PendingList.Flink;
        SPD_SRB_EXTENSION *SrbExtension =
            CONTAINING_RECORD(PendingEntry, SPD_SRB_EXTENSION, ListEntry);
        BOOLEAN Wake;
        ULONG Index;

        if (Ioq->NonblockingConsumer)
        {
            RemoveEntryList(&SrbExtension->ListEntry);
            Wake = FALSE;
        }
        else
        {
            Wake = !RemoveEntryList(&SrbExtension->ListEntry);
        }

        Prepare(SrbExtension, Context, DataBuffer);

        InsertTailList(&Ioq->ProcessList, &SrbExtension->ListEntry);
        Index = SpdHashMixPointer(SrbExtension) % Ioq->ProcessBucketCount;
#if DBG
        for (PVOID X = Ioq->ProcessBuckets[Index]; X; X = ((SPD_SRB_EXTENSION *)X)->HashNext)
            ASSERT(X != SrbExtension);
        ASSERT(0 == SrbExtension->HashNext);
#endif
        SrbExtension->HashNext = Ioq->ProcessBuckets[Index];
        Ioq->ProcessBuckets[Index] = SrbExtension;
        if (Wake)
            SpdQeventSetNoLock(&Ioq->PendingEvent);

        Result = STATUS_SUCCESS;
    }

    SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);

    return Result;
}

NTSTATUS SpdIoqStartProcessingSrb(SPD_IOQ *Ioq, PLARGE_INTEGER Timeout, PIRP CancellableIrp,
    VOID (*Prepare)(PVOID SrbExtension, PVOID Context, PVOID DataBuffer),
    PVOID Context, PVOID DataBuffer)
{
    NTSTATUS Result;
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER LockStart;

    Result = SpdQeventCancellableWait(&Ioq->PendingEvent, Timeout, CancellableIrp);
    if (STATUS_TIMEOUT == Result)
        return STATUS_TIMEOUT;
    if (STATUS_CANCELLED == Result || STATUS_THREAD_IS_TERMINATING == Result)
        return STATUS_CANCELLED;
    ASSERT(STATUS_SUCCESS == Result);

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);

    if (!Ioq->Stopped && !Ioq->NonblockingConsumer)
    {
        PLIST_ENTRY PendingEntry;

        PendingEntry = &Ioq->PendingList;
        if (PendingEntry->Flink != PendingEntry)
        {
            SPD_SRB_EXTENSION *SrbExtension =
                CONTAINING_RECORD(PendingEntry->Flink, SPD_SRB_EXTENSION, ListEntry);
            BOOLEAN Wake;
            ULONG Index;

            Wake = !RemoveEntryList(&SrbExtension->ListEntry);

            Prepare(SrbExtension, Context, DataBuffer);

            InsertTailList(&Ioq->ProcessList, &SrbExtension->ListEntry);
            Index = SpdHashMixPointer(SrbExtension) % Ioq->ProcessBucketCount;
#if DBG
            for (PVOID X = Ioq->ProcessBuckets[Index]; X; X = ((SPD_SRB_EXTENSION *)X)->HashNext)
                ASSERT(X != SrbExtension);
            ASSERT(0 == SrbExtension->HashNext);
#endif
            SrbExtension->HashNext = Ioq->ProcessBuckets[Index];
            Ioq->ProcessBuckets[Index] = SrbExtension;

            if (Wake)
                /* queue is not empty; wake up a waiter */
                SpdQeventSetNoLock(&Ioq->PendingEvent);

            Result = STATUS_SUCCESS;
        }
        else
            Result = STATUS_UNSUCCESSFUL;
    }
    else
    {
        /* queue is stopped; wake up a waiter */
        SpdQeventSetNoLock(&Ioq->PendingEvent);

        Result = STATUS_CANCELLED;
    }

    SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);

    return Result;
}

NTSTATUS SpdIoqEndProcessingSrbByExtension(SPD_IOQ *Ioq,
    PVOID SrbExtension0,
    UCHAR (*Complete)(PVOID SrbExtension, PVOID Context, PVOID DataBuffer),
    PVOID Context, PVOID DataBuffer)
{
    NTSTATUS Result;
    SPD_IOQ_LOCK_HANDLE Irql;
    LARGE_INTEGER LockStart;
    PVOID CompleteSrb = 0;
    UCHAR CompleteStatus = SRB_STATUS_SUCCESS;

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);

    if (!Ioq->Stopped)
    {
        SPD_SRB_EXTENSION *SrbExtension = SrbExtension0;
        ULONG Index;
        Result = STATUS_NOT_FOUND;

        Index = SpdHashMixPointer(SrbExtension) % Ioq->ProcessBucketCount;
        for (PVOID *P = &Ioq->ProcessBuckets[Index]; *P; P = &((SPD_SRB_EXTENSION *)(*P))->HashNext)
            if (*P == SrbExtension)
            {
                *P = SrbExtension->HashNext;
                SrbExtension->HashNext = 0;
                RemoveEntryList(&SrbExtension->ListEntry);

                UCHAR SrbStatus = Complete(SrbExtension, Context, DataBuffer);
                if (SRB_STATUS_PENDING == SrbStatus)
                {
#if defined(WINSPD_TEST_BUILD)
                    InterlockedIncrement(&SrbExtension->StorageUnit->TestLegacyContinuations);
#endif
                    /*
                     * If Complete returns PENDING we need to repost the SRB
                     * and we will also place it at the queue head, so that it
                     * gets picked up immediately after.
                     *
                     * This functionality supports splitting SRB's into chunks,
                     * which is required for I/O that exceeds our MaxTransferLength.
                     * See https://tinyurl.com/ychyv62s
                     */
                    InsertHeadList(&Ioq->PendingList, &SrbExtension->ListEntry);

                    /* The shared-ring KICK path schedules its producer after
                     * returning the response and buffer. */
                    if (!Ioq->NonblockingConsumer)
                        SpdQeventSetNoLock(&Ioq->PendingEvent);
                }
                else
                {
                    CompleteSrb = SrbExtension->Srb;
                    CompleteStatus = SrbStatus;
                    SrbExtension->Srb = 0;
                    SrbExtension->ListEntry.Flink =
                        SrbExtension->ListEntry.Blink = 0;
                    SrbExtension->RingState = SpdRingSrbNone;
                }

                Result = STATUS_SUCCESS;
                break;
            }
    }
    else
    {
        DbgPrint(DRIVER_NAME ": IOQ END hint=%p CANCELLED stopped=1\n",
            SrbExtension0);
        Result = STATUS_CANCELLED;
    }

    if (STATUS_NOT_FOUND == Result)
    {
        SPD_SRB_EXTENSION *SrbExtension = SrbExtension0;
        ULONG Index = SpdHashMixPointer(SrbExtension) %
            Ioq->ProcessBucketCount;

        DbgPrint(DRIVER_NAME ": IOQ END hint=%p NOT_FOUND bucket=%lu contents:",
            SrbExtension0, Index);
        for (PVOID P = Ioq->ProcessBuckets[Index]; P;
            P = ((SPD_SRB_EXTENSION *)P)->HashNext)
            DbgPrint(" %p(srb=%p)", P,
                ((SPD_SRB_EXTENSION *)P)->Srb);
        DbgPrint("\n");
    }

    SpdIoqReleaseSpinLock(Ioq, &Irql, LockStart);

    if (0 != CompleteSrb)
        SpdSrbComplete(Ioq->DeviceExtension, CompleteSrb, CompleteStatus);

    return Result;
}

NTSTATUS SpdIoqEndProcessingSrb(SPD_IOQ *Ioq, UINT64 Hint,
    UCHAR (*Complete)(PVOID SrbExtension, PVOID Context, PVOID DataBuffer),
    PVOID Context, PVOID DataBuffer)
{
    return SpdIoqEndProcessingSrbByExtension(Ioq, (PVOID)(UINT_PTR)Hint,
        Complete, Context, DataBuffer);
}
