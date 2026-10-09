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

static VOID SpdIoqAcquireSpinLock(
    SPD_IOQ *Ioq, KIRQL *PIrql, LARGE_INTEGER *PStart)
{
    LARGE_INTEGER Before = KeQueryPerformanceCounter(0);
    LARGE_INTEGER After;

    KeAcquireSpinLock(&Ioq->SpinLock, PIrql);
    After = KeQueryPerformanceCounter(0);
    InterlockedExchangeAdd64(&Ioq->SpinLockWaitTicks,
        After.QuadPart - Before.QuadPart);
    InterlockedIncrement64(&Ioq->SpinLockAcquisitions);
    *PStart = After;
}

static VOID SpdIoqReleaseSpinLock(
    SPD_IOQ *Ioq, KIRQL Irql, LARGE_INTEGER Start)
{
    LARGE_INTEGER End = KeQueryPerformanceCounter(0);
    InterlockedExchangeAdd64(&Ioq->SpinLockHoldTicks,
        End.QuadPart - Start.QuadPart);
    KeReleaseSpinLock(&Ioq->SpinLock, Irql);
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
    Ioq->ProcessBucketCount = BucketCount;

    *PIoq = Ioq;

    return STATUS_SUCCESS;
}

VOID SpdIoqDelete(SPD_IOQ *Ioq)
{
    SpdIoqReset(Ioq, FALSE, SpdIoqResetReasonDelete);
    SpdQeventFinalize(&Ioq->PendingEvent);
    SpdFree(Ioq, SpdTagIoq);
}

VOID SpdIoqSetNonblockingConsumer(SPD_IOQ *Ioq)
{
    KIRQL Irql;
    LARGE_INTEGER LockStart;

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);

    Ioq->NonblockingConsumer = TRUE;
    for (PLIST_ENTRY Entry = Ioq->PendingList.Flink;
        Entry != &Ioq->PendingList; Entry = Entry->Flink)
    {
        SPD_SRB_EXTENSION *SrbExtension = CONTAINING_RECORD(
            Entry, SPD_SRB_EXTENSION, ListEntry);
        SrbExtension->RingState = SpdRingSrbPending;
        SrbExtension->RingAbortPending = FALSE;
    }

    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);
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
    KIRQL Irql;
    LARGE_INTEGER LockStart;
#if defined(WINSPD_TEST_BUILD)
    SPD_STORAGE_UNIT *TestStorageUnit = 0;
#endif

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);

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
            SrbExtension->ListEntry.Flink =
                SrbExtension->ListEntry.Blink = 0;
            SrbExtension->RingState = SpdRingSrbNone;
            SrbExtension->RingAbortPending = FALSE;
            SrbExtension->Srb = 0;
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
            SpdSrbComplete(
                Ioq->DeviceExtension,
                Srb,
                SRB_STATUS_ABORTED);
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
            SrbExtension->ListEntry.Flink =
                SrbExtension->ListEntry.Blink = 0;
            SrbExtension->RingState = SpdRingSrbNone;
            SrbExtension->RingAbortPending = FALSE;
            PVOID Srb = SrbExtension->Srb;
            SrbExtension->Srb = 0;
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
            SpdSrbComplete(
                Ioq->DeviceExtension,
                Srb,
                SRB_STATUS_ABORTED);
            ProcessEntry = NextEntry;
        }
    }

    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);
#if defined(WINSPD_TEST_BUILD)
    if (0 != TestStorageUnit)
        SpdStorageUnitRingTestIoqResetComplete(TestStorageUnit);
#endif
}

BOOLEAN SpdIoqStopped(SPD_IOQ *Ioq)
{
    BOOLEAN Result;
    KIRQL Irql;
    LARGE_INTEGER LockStart;

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);

    Result = Ioq->Stopped;

    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);

    return Result;
}

NTSTATUS SpdIoqCancelSrb(SPD_IOQ *Ioq, PVOID Srb)
{
    NTSTATUS Result = STATUS_NOT_FOUND;
    KIRQL Irql;
    LARGE_INTEGER LockStart;

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);

    if (!Ioq->Stopped)
    {
        SPD_SRB_EXTENSION *SrbExtension = SpdSrbExtension(Srb);
        BOOLEAN Pending = FALSE;
        BOOLEAN Processing = FALSE;
        ULONG Index = 0;

        ASSERT(Srb == SrbExtension->Srb);

        if (Srb == SrbExtension->Srb &&
            0 != SrbExtension->ListEntry.Flink &&
            0 != SrbExtension->ListEntry.Blink)
        {
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
                SpdSrbComplete(Ioq->DeviceExtension, Srb,
                    SRB_STATUS_ABORTED);
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
    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);

    return Result;
}

NTSTATUS SpdIoqPostSrb(SPD_STORAGE_UNIT *StorageUnit, PVOID Srb)
{
    SPD_IOQ *Ioq = StorageUnit->Ioq;
    NTSTATUS Result = STATUS_CANCELLED;
    BOOLEAN ScheduleRingProducer = FALSE;
    KIRQL Irql;
    LARGE_INTEGER LockStart;

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);

    if (!Ioq->Stopped)
    {
        SPD_SRB_EXTENSION *SrbExtension = SpdSrbExtension(Srb);

        ASSERT(0 == SrbExtension->Srb);
        SrbExtension->Srb = Srb;
        SrbExtension->RingState = Ioq->NonblockingConsumer ?
            SpdRingSrbPending : SpdRingSrbNone;
        SrbExtension->RingAbortPending = FALSE;

        ASSERT(0 == SrbExtension->ListEntry.Flink && 0 == SrbExtension->ListEntry.Blink);
        InsertTailList(&Ioq->PendingList, &SrbExtension->ListEntry);

        if (!Ioq->NonblockingConsumer)
            SpdQeventSetNoLock(&Ioq->PendingEvent);
        else
            ScheduleRingProducer = TRUE;

        Result = STATUS_SUCCESS;
    }

    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);

#if defined(WINSPD_TEST_BUILD)
    if (NT_SUCCESS(Result) && ScheduleRingProducer)
        SpdStorageUnitRingTestIoqPostBarrier(StorageUnit);
#endif
    if (NT_SUCCESS(Result) && ScheduleRingProducer)
        SpdStorageUnitRingScheduleProducer(StorageUnit);

    return Result;
}

NTSTATUS SpdIoqRingClaimSrb(
    SPD_IOQ *Ioq, SPD_SRB_EXTENSION **PSrbExtension)
{
    NTSTATUS Result;
    KIRQL Irql;
    LARGE_INTEGER LockStart;

    *PSrbExtension = 0;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);
    if (Ioq->Stopped)
        Result = STATUS_CANCELLED;
    else if (IsListEmpty(&Ioq->PendingList))
        Result = STATUS_NOT_FOUND;
    else
    {
        PLIST_ENTRY PendingEntry = RemoveHeadList(&Ioq->PendingList);
        SPD_SRB_EXTENSION *SrbExtension = CONTAINING_RECORD(
            PendingEntry, SPD_SRB_EXTENSION, ListEntry);
        ULONG Index = SpdHashMixPointer(SrbExtension) %
            Ioq->ProcessBucketCount;

        InsertTailList(&Ioq->ProcessList, &SrbExtension->ListEntry);
#if DBG
        for (PVOID X = Ioq->ProcessBuckets[Index]; X;
            X = ((SPD_SRB_EXTENSION *)X)->HashNext)
            ASSERT(X != SrbExtension);
        ASSERT(0 == SrbExtension->HashNext);
#endif
        SrbExtension->HashNext = Ioq->ProcessBuckets[Index];
        Ioq->ProcessBuckets[Index] = SrbExtension;
        SrbExtension->RingState = SpdRingSrbPreparing;
        SrbExtension->RingAbortPending = FALSE;
        *PSrbExtension = SrbExtension;
        Result = STATUS_SUCCESS;
    }
    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);
    return Result;
}

VOID SpdIoqRingAcquireCommitLock(
    SPD_IOQ *Ioq, KIRQL *PIrql, LARGE_INTEGER *PStart)
{
    SpdIoqAcquireSpinLock(Ioq, PIrql, PStart);
}

VOID SpdIoqRingReleaseCommitLock(
    SPD_IOQ *Ioq, KIRQL Irql, LARGE_INTEGER Start)
{
    SpdIoqReleaseSpinLock(Ioq, Irql, Start);
}

/* Caller holds Ioq->SpinLock as part of Ring->Lock -> Ioq->SpinLock commit. */
BOOLEAN SpdIoqRingCommitPreparedSrbNoLock(
    SPD_IOQ *Ioq,
    SPD_SRB_EXTENSION *SrbExtension,
    BOOLEAN AbortAll,
    PVOID *PSrb,
    BOOLEAN *PAbortRequested)
{
    ASSERT(SpdRingSrbPreparing == SrbExtension->RingState);
    ASSERT(0 != SrbExtension->Srb);
    *PSrb = 0;
    *PAbortRequested = SrbExtension->RingAbortPending;

    if (AbortAll || SrbExtension->RingAbortPending)
    {
        SpdIoqRemoveProcessHashNoLock(Ioq, SrbExtension);
        RemoveEntryList(&SrbExtension->ListEntry);
        SrbExtension->ListEntry.Flink =
            SrbExtension->ListEntry.Blink = 0;
        *PSrb = SrbExtension->Srb;
        SrbExtension->Srb = 0;
        SrbExtension->RingState = SpdRingSrbNone;
        SrbExtension->RingAbortPending = FALSE;
        return FALSE;
    }

    SrbExtension->RingState = SpdRingSrbInFlight;
    return TRUE;
}

NTSTATUS SpdIoqRingClaimCompletion(
    SPD_IOQ *Ioq, UINT64 Hint, SPD_SRB_EXTENSION **PSrbExtension)
{
    NTSTATUS Result = STATUS_NOT_FOUND;
    PVOID HintPointer = (PVOID)(UINT_PTR)Hint;
    ULONG Index = SpdHashMixPointer(HintPointer) %
        Ioq->ProcessBucketCount;
    KIRQL Irql;
    LARGE_INTEGER LockStart;

    *PSrbExtension = 0;
    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);
    if (Ioq->Stopped)
        Result = STATUS_CANCELLED;
    else
    {
        for (PVOID P = Ioq->ProcessBuckets[Index]; P;
            P = ((SPD_SRB_EXTENSION *)P)->HashNext)
            if (P == HintPointer)
            {
                SPD_SRB_EXTENSION *SrbExtension = P;
                if (SpdRingSrbInFlight == SrbExtension->RingState)
                {
                    SrbExtension->RingState = SpdRingSrbCompleting;
                    *PSrbExtension = SrbExtension;
                    Result = STATUS_SUCCESS;
                }
                break;
            }
    }
    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);
    return Result;
}

NTSTATUS SpdIoqRingFinishCompletion(
    SPD_IOQ *Ioq, SPD_SRB_EXTENSION *SrbExtension, UCHAR SrbStatus)
{
    KIRQL Irql;
    LARGE_INTEGER LockStart;
    PVOID Srb = 0;
    BOOLEAN Complete = FALSE;

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);
    ASSERT(SpdRingSrbCompleting == SrbExtension->RingState);
    ASSERT(0 != SrbExtension->Srb);

    SpdIoqRemoveProcessHashNoLock(Ioq, SrbExtension);
    RemoveEntryList(&SrbExtension->ListEntry);
    SrbExtension->ListEntry.Flink =
        SrbExtension->ListEntry.Blink = 0;

    if (SrbExtension->RingAbortPending)
    {
        SrbStatus = SRB_STATUS_ABORTED;
        Srb = SrbExtension->Srb;
        SrbExtension->Srb = 0;
        SrbExtension->RingState = SpdRingSrbNone;
        SrbExtension->RingAbortPending = FALSE;
        InterlockedIncrement64(
            &SrbExtension->StorageUnit->Ring.DeferredSrbAborts);
        Complete = TRUE;
    }
    else if (SRB_STATUS_PENDING == SrbStatus)
    {
        SrbExtension->RingState = SpdRingSrbPending;
        InsertHeadList(&Ioq->PendingList, &SrbExtension->ListEntry);
        if (!Ioq->NonblockingConsumer)
            SpdQeventSetNoLock(&Ioq->PendingEvent);
    }
    else
    {
        Srb = SrbExtension->Srb;
        SrbExtension->Srb = 0;
        SrbExtension->RingState = SpdRingSrbNone;
        SrbExtension->RingAbortPending = FALSE;
        Complete = TRUE;
    }

    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);
    if (Complete)
        SpdSrbComplete(Ioq->DeviceExtension, Srb, SrbStatus);
    return STATUS_SUCCESS;
}

ULONG SpdIoqRingLeasedCount(SPD_IOQ *Ioq)
{
    ULONG Count = 0;
    KIRQL Irql;
    LARGE_INTEGER LockStart;

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);
    for (PLIST_ENTRY Entry = Ioq->ProcessList.Flink;
        Entry != &Ioq->ProcessList; Entry = Entry->Flink)
    {
        SPD_SRB_EXTENSION *SrbExtension = CONTAINING_RECORD(
            Entry, SPD_SRB_EXTENSION, ListEntry);
        if (SpdRingSrbPreparing == SrbExtension->RingState ||
            SpdRingSrbCompleting == SrbExtension->RingState)
            Count++;
    }
    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);
    return Count;
}

BOOLEAN SpdIoqRingHasPending(SPD_IOQ *Ioq)
{
    BOOLEAN HasPending;
    KIRQL Irql;
    LARGE_INTEGER LockStart;

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);
    HasPending = !Ioq->Stopped && !IsListEmpty(&Ioq->PendingList);
    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);
    return HasPending;
}

NTSTATUS SpdIoqTryStartProcessingSrb(SPD_IOQ *Ioq,
    VOID (*Prepare)(PVOID SrbExtension, PVOID Context, PVOID DataBuffer),
    PVOID Context, PVOID DataBuffer)
{
    NTSTATUS Result;
    KIRQL Irql;
    LARGE_INTEGER LockStart;

    ASSERT(DISPATCH_LEVEL == KeGetCurrentIrql());

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);

    if (Ioq->Stopped)
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

    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);

    return Result;
}

NTSTATUS SpdIoqStartProcessingSrb(SPD_IOQ *Ioq, PLARGE_INTEGER Timeout, PIRP CancellableIrp,
    VOID (*Prepare)(PVOID SrbExtension, PVOID Context, PVOID DataBuffer),
    PVOID Context, PVOID DataBuffer)
{
    NTSTATUS Result;
    KIRQL Irql;
    LARGE_INTEGER LockStart;

    Result = SpdQeventCancellableWait(&Ioq->PendingEvent, Timeout, CancellableIrp);
    if (STATUS_TIMEOUT == Result)
        return STATUS_TIMEOUT;
    if (STATUS_CANCELLED == Result || STATUS_THREAD_IS_TERMINATING == Result)
        return STATUS_CANCELLED;
    ASSERT(STATUS_SUCCESS == Result);

    SpdIoqAcquireSpinLock(Ioq, &Irql, &LockStart);

    if (!Ioq->Stopped)
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

    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);

    return Result;
}

NTSTATUS SpdIoqEndProcessingSrbByExtension(SPD_IOQ *Ioq,
    PVOID SrbExtension0,
    UCHAR (*Complete)(PVOID SrbExtension, PVOID Context, PVOID DataBuffer),
    PVOID Context, PVOID DataBuffer)
{
    NTSTATUS Result;
    KIRQL Irql;
    LARGE_INTEGER LockStart;

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
                RemoveEntryList(&SrbExtension->ListEntry);

                UCHAR SrbStatus = Complete(SrbExtension, Context, DataBuffer);
                if (SRB_STATUS_PENDING == SrbStatus)
                {
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
                    SpdSrbComplete(Ioq->DeviceExtension, SrbExtension->Srb, SrbStatus);

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

    SpdIoqReleaseSpinLock(Ioq, Irql, LockStart);

    return Result;
}

NTSTATUS SpdIoqEndProcessingSrb(SPD_IOQ *Ioq, UINT64 Hint,
    UCHAR (*Complete)(PVOID SrbExtension, PVOID Context, PVOID DataBuffer),
    PVOID Context, PVOID DataBuffer)
{
    return SpdIoqEndProcessingSrbByExtension(Ioq, (PVOID)(UINT_PTR)Hint,
        Complete, Context, DataBuffer);
}
