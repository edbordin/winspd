/**
 * @file sys/driver.h
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

#ifndef WINSPD_SYS_DRIVER_H_INCLUDED
#define WINSPD_SYS_DRIVER_H_INCLUDED

#define WINSPD_SYS_INTERNAL

#define POOL_NX_OPTIN                   1
#include <ntifs.h>
#include <ntstrsafe.h>
#include <storport.h>
#include <ntddscsi.h>
#include <winspd/ioctl.h>
#include "srbcompat.h"

/* disable warnings */
#pragma warning(disable:4100)           /* unreferenced formal parameter */
#pragma warning(disable:4200)           /* zero-sized array in struct/union */

#define DRIVER_NAME                     SPD_IOCTL_DRIVER_NAME

/* debug */
#if DBG
enum
{
    spd_debug_bp_generic                = 0x00000001,   /* generic breakpoint switch */
    spd_debug_bp_drvrld                 = 0x00000002,   /* DriverEntry breakpoint switch */
    spd_debug_bp_tracing                = 0x00000004,   /* tracing functions breakpoint switch */
    spd_debug_bp_adapter                = 0x00000008,   /* adapter functions breakpoint switch */
    spd_debug_bp_ioctl                  = 0x00000010,   /* ioctl functions breakpoint switch */
    spd_debug_bp_io                     = 0x00000020,   /* io functions breakpoint switch */
    spd_debug_bp_irp                    = 0x00000040,   /* irp breakpoint switch */
    spd_debug_dp_generic                = 0x00010000,   /* generic DbgPrint switch */
    spd_debug_dp_drvrld                 = 0x00020000,   /* DriverEntry DbgPrint switch */
    spd_debug_dp_tracing                = 0x00040000,   /* tracing functions DbgPrint switch */
    spd_debug_dp_adapter                = 0x00080000,   /* adapter functions DbgPrint switch */
    spd_debug_dp_ioctl                  = 0x00100000,   /* ioctl functions DbgPrint switch */
    spd_debug_dp_io                     = 0x00200000,   /* io functions DbgPrint switch */
    spd_debug_dp_irp                    = 0x00400000,   /* irp DbgPrint switch */
    spd_debug_dp_srb                    = 0x80000000,   /* srb completion DbgPrint switch */
    spd_debug_dp_srberr                 = 0x40000000,   /* srb error completion DbgPrint switch */
    spd_debug_dp                        = 0xffff0000,
};
extern __declspec(selectany) int spd_debug =
    spd_debug_bp_drvrld | spd_debug_dp_srberr;
const char *AdapterControlSym(ULONG Control);
const char *SrbFunctionSym(ULONG Function);
const char *SrbStatusSym(ULONG Status);
const char *SrbStatusMaskSym(ULONG Status);
const char *CdbOperationCodeSym(ULONG OperationCode);
const char *SrbStringize(PVOID Srb, char Buffer[], size_t Size);
#endif

/* DEBUGBREAK */
#if DBG
#define DEBUGBREAK_EX(category)         \
    do                                  \
    {                                   \
        static int bp = 1;              \
        if (bp && (spd_debug & spd_debug_bp_ ## category) && !KD_DEBUGGER_NOT_PRESENT)\
            DbgBreakPoint();            \
    } while (0,0)
#else
#define DEBUGBREAK_EX(category)         do {} while (0,0)
#endif
#define DEBUGBREAK()                    DEBUGBREAK_EX(generic)

/* DEBUGLOG */
#if DBG
#define DEBUGLOG_COND(cond, fmt, ...)   \
    ((void)((cond) ?\
        DbgPrint("[%d] " DRIVER_NAME "!" __FUNCTION__ ": " fmt "\n", KeGetCurrentIrql(), __VA_ARGS__) :\
        0))
#else
#define DEBUGLOG_COND(cond, fmt, ...)   ((void)0)
#endif
#define DEBUGLOG_EX(category, fmt, ...) DEBUGLOG_COND(spd_debug & spd_debug_dp_ ## category, fmt, __VA_ARGS__)
#define DEBUGLOG(fmt, ...)              DEBUGLOG_EX(generic, fmt, __VA_ARGS__)

/* SPD_ENTER/SPD_LEAVE */
#if DBG
#define SPD_DEBUGLOG_(category, fmt, rfmt, ...)\
    ((void)((spd_debug & spd_debug_dp_ ## category) ?\
        DbgPrint(AbnormalTermination() ?\
            "[%d] " DRIVER_NAME "!" __FUNCTION__ "(" fmt ") = *AbnormalTermination*\n" :\
            "[%d] " DRIVER_NAME "!" __FUNCTION__ "(" fmt ")" rfmt "\n"\
            , KeGetCurrentIrql(), __VA_ARGS__) :\
        0))
#else
#define SPD_DEBUGLOG_(category, fmt, rfmt, ...)\
    ((void)0)
#endif
#define SPD_ENTER_(category, ...)       \
    DEBUGBREAK_EX(category);            \
    try                                 \
    {                                   \
        __VA_ARGS__
#define SPD_LEAVE_(...)                 \
    goto spd_leave_label;               \
    spd_leave_label:;                   \
    }                                   \
    finally                             \
    {                                   \
        __VA_ARGS__;                    \
    }
#define SPD_ENTER(category, ...)        \
    SPD_ENTER_(category, __VA_ARGS__)
#define SPD_LEAVE_NOLOG(category)\
    SPD_LEAVE_()
#define SPD_LEAVE(category, fmt, rfmt, ...)\
    SPD_LEAVE_(SPD_DEBUGLOG_(category, fmt, rfmt, __VA_ARGS__))
#define SPD_RETURN(...)                 \
    do                                  \
    {                                   \
        __VA_ARGS__;                    \
        goto spd_leave_label;           \
    } while (0,0)

/* memory allocation */
#if defined(_ARM64_) || defined(_ARM64)
#define SpdAllocNonPaged(Size, Tag)     ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, Tag)
#else
#define SpdAllocNonPaged(Size, Tag)     ExAllocatePoolWithTag(NonPagedPool, Size, Tag)
#endif
#define SpdFree(Pointer, Tag)           ExFreePoolWithTag(Pointer, Tag)
#define SpdTagStorageUnit               'SdpS'
#define SpdTagIoq                       'QdpS'

/* hash mix */
/* Based on the MurmurHash3 fmix32/fmix64 function:
 * See: https://code.google.com/p/smhasher/source/browse/trunk/MurmurHash3.cpp?r=152#68
 */
static inline
UINT32 SpdHashMix32(UINT32 h)
{
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}
static inline
UINT64 SpdHashMix64(UINT64 k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}
static inline
ULONG SpdHashMixPointer(PVOID Pointer)
{
#if _WIN64
    return (ULONG)SpdHashMix64((UINT64)Pointer);
#else
    return (ULONG)SpdHashMix32((UINT32)Pointer);
#endif
}

/* IRP dispatch */
DRIVER_DISPATCH SpdDispatchPnp;
extern PDRIVER_DISPATCH StorPortDispatchPnp;

/* virtual miniport functions */
HW_INITIALIZE_TRACING SpdHwInitializeTracing;
HW_CLEANUP_TRACING SpdHwCleanupTracing;
VIRTUAL_HW_FIND_ADAPTER SpdHwFindAdapter;
HW_INITIALIZE SpdHwInitialize;
HW_FREE_ADAPTER_RESOURCES SpdHwFreeAdapterResources;
HW_RESET_BUS SpdHwResetBus;
HW_ADAPTER_CONTROL SpdHwAdapterControl;
HW_PROCESS_SERVICE_REQUEST SpdHwProcessServiceRequest;
HW_COMPLETE_SERVICE_IRP SpdHwCompleteServiceIrp;
HW_STARTIO SpdHwStartIo;

/* I/O */
FORCEINLINE VOID SpdSrbComplete(PVOID DeviceExtension, PVOID Srb, UCHAR SrbStatus)
{
    ASSERT(SRB_STATUS_PENDING != SRB_STATUS(SrbStatus));
    SrbSetSrbStatus(Srb, SrbStatus);
#if DBG
    {
        char buf[1024];
        DEBUGLOG_COND(
            (spd_debug & spd_debug_dp_srb) ||
                ((SRB_STATUS_SUCCESS != SRB_STATUS(SrbStatus)) && (spd_debug & spd_debug_dp_srberr)),
            "%p, Srb=%p {%s}", DeviceExtension, Srb, SrbStringize(Srb, buf, sizeof buf));
    }
#endif
    StorPortNotification(RequestComplete, DeviceExtension, Srb);
}
UCHAR SpdSrbExecuteScsi(PVOID DeviceExtension, PVOID Srb);
UCHAR SpdSrbPostFlush(PVOID DeviceExtension, PVOID Srb);
BOOLEAN SpdSrbIsFlush(PVOID Srb);
VOID SpdSrbExecuteScsiPrepare(PVOID SrbExtension, PVOID Context, PVOID DataBuffer);
UCHAR SpdSrbExecuteScsiComplete(PVOID SrbExtension, PVOID Context, PVOID DataBuffer);
UCHAR SpdSrbAbortCommand(PVOID DeviceExtension, PVOID Srb);
UCHAR SpdSrbResetBus(PVOID DeviceExtension, PVOID Srb);
UCHAR SpdSrbResetDevice(PVOID DeviceExtension, PVOID Srb);
UCHAR SpdSrbResetLogicalUnit(PVOID DeviceExtension, PVOID Srb);
UCHAR SpdSrbFlush(PVOID DeviceExtension, PVOID Srb);
UCHAR SpdSrbShutdown(PVOID DeviceExtension, PVOID Srb);
UCHAR SpdSrbPnp(PVOID DeviceExtension, PVOID Srb);
UCHAR SpdSrbWmi(PVOID DeviceExtension, PVOID Srb);
NTSTATUS SpdNtStatusFromStorStatus(ULONG StorStatus);

/* PnP capabilities */
#define SpdPnpSetAdapterCapabilities(D) \
    do                                  \
    {                                   \
        (D)->Removable = 1;             \
        (D)->SilentInstall = 1;         \
        (D)->SurpriseRemovalOK = 1;     \
    } while (0,0)
#define SpdPnpSetDeviceCapabilities(D)  \
    do                                  \
    {                                   \
        (D)->Removable = 1;             \
        (D)->SilentInstall = 1;         \
        (D)->SurpriseRemovalOK = 1;     \
    } while (0,0)

/*
 * Queued Events
 *
 * Queued Events are an implementation of SynchronizationEvent's using
 * a KQUEUE, originally from WinFsp. For a discussion see:
 * https://github.com/billziss-gh/winfsp/wiki/Queued-Events
 */
typedef struct _SPD_QEVENT
{
    KQUEUE Queue;
    LIST_ENTRY DummyEntry;
    KSPIN_LOCK SpinLock;
} SPD_QEVENT;
static inline
VOID SpdQeventInitialize(SPD_QEVENT *Qevent, ULONG ThreadCount)
{
    KeInitializeQueue(&Qevent->Queue, ThreadCount);
    RtlZeroMemory(&Qevent->DummyEntry, sizeof Qevent->DummyEntry);
    KeInitializeSpinLock(&Qevent->SpinLock);
}
static inline
VOID SpdQeventFinalize(SPD_QEVENT *Qevent)
{
    KeRundownQueue(&Qevent->Queue);
}
static inline
VOID SpdQeventSetNoLock(SPD_QEVENT *Qevent)
{
    ASSERT(KeGetCurrentIrql() == DISPATCH_LEVEL);
    if (0 == KeReadStateQueue(&Qevent->Queue))
        KeInsertQueue(&Qevent->Queue, &Qevent->DummyEntry);
}
static inline
VOID SpdQeventSet(SPD_QEVENT *Qevent)
{
    KIRQL Irql;
    KeAcquireSpinLock(&Qevent->SpinLock, &Irql);
    SpdQeventSetNoLock(Qevent);
    KeReleaseSpinLock(&Qevent->SpinLock, Irql);
}
static inline
NTSTATUS SpdQeventWait(SPD_QEVENT *Qevent,
    KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER PTimeout)
{
    PLIST_ENTRY ListEntry;
    KeRemoveQueueEx(&Qevent->Queue, WaitMode, Alertable, PTimeout, &ListEntry, 1);
    if (ListEntry == &Qevent->DummyEntry)
        return STATUS_SUCCESS;
    return (NTSTATUS)(UINT_PTR)ListEntry;
}
static inline
NTSTATUS SpdQeventCancellableWait(SPD_QEVENT *Qevent,
    PLARGE_INTEGER PTimeout, PIRP Irp)
{
    NTSTATUS Result;
    UINT64 ExpirationTime = 0, InterruptTime;
    if (0 != PTimeout && 0 > PTimeout->QuadPart)
        ExpirationTime = KeQueryInterruptTime() - PTimeout->QuadPart;
retry:
    Result = SpdQeventWait(Qevent, KernelMode, TRUE, PTimeout);
    if (STATUS_ALERTED == Result)
    {
        if (PsIsThreadTerminating(PsGetCurrentThread()))
            return STATUS_THREAD_IS_TERMINATING;
        if (0 != Irp && Irp->Cancel)
            return STATUS_CANCELLED;
        if (0 != ExpirationTime)
        {
            InterruptTime = KeQueryInterruptTime();
            if (ExpirationTime <= InterruptTime)
                return STATUS_TIMEOUT;
            PTimeout->QuadPart = (INT64)InterruptTime - (INT64)ExpirationTime;
        }
        goto retry;
    }
    return Result;
}

/* Fixed storage; stage histograms can have concurrent thread writers. */
#define SPD_HISTOGRAM_BUCKETS 64
typedef struct
{
    UINT64 Bins[SPD_HISTOGRAM_BUCKETS];
} SPD_HISTOGRAM;

FORCEINLINE VOID SpdHistogramRecord(SPD_HISTOGRAM *Histogram, UINT64 Value)
{
    ULONG Bucket = 0;
    while (Value > 1 && Bucket + 1 < SPD_HISTOGRAM_BUCKETS)
    {
        Value = (Value >> 1) + (Value & 1);
        Bucket++;
    }
    InterlockedIncrement64((volatile LONG64 *)&Histogram->Bins[Bucket]);
}
VOID SpdHistogramLog(const SPD_HISTOGRAM *Histogram, PCSTR Name);

/* Optional latency diagnostics support parallel stage writers without per-I/O
 * logging/allocation. Read/log only after callback rundown. */
#ifndef SPD_RING_LATENCY_DIAGNOSTICS
#define SPD_RING_LATENCY_DIAGNOSTICS 0
#endif
typedef enum
{
    SpdLatencyPending, SpdLatencyPrepare, SpdLatencyReady, SpdLatencyFlight,
    SpdLatencyCqQueue, SpdLatencyCompletionBatchWait, SpdLatencyCompletionService,
    SpdLatencyCompletionRetireWait, SpdLatencyNotifyWait, SpdLatencyNotifyService,
    SpdLatencyKernelTotal, SpdLatencyCqNotify, SpdLatencyCompletionEnqueueLock,
    SpdLatencyCompletionClaimLock, SpdLatencyCompletionRetireLock, SpdLatencyPreparationDispatch, SpdLatencyProducerDispatch,
    SpdLatencyCompletionDispatch, SpdLatencyKick, SpdLatencyCompletionBacklog,
    SpdLatencyCqDepth, SpdLatencyCount
} SPD_LATENCY_STAGE;
typedef struct
{
    SPD_HISTOGRAM Histogram;
    UINT64 Count, Total, Maximum;
} SPD_LATENCY_METRIC;
FORCEINLINE VOID SpdLatencyRecord(SPD_LATENCY_METRIC *Metric, UINT64 Value)
{
    SpdHistogramRecord(&Metric->Histogram, Value);
    InterlockedIncrement64((volatile LONG64 *)&Metric->Count);
    InterlockedExchangeAdd64((volatile LONG64 *)&Metric->Total, Value);
    UINT64 Previous = InterlockedCompareExchange64((volatile LONG64 *)&Metric->Maximum, 0, 0);
    while (Value > Previous)
    {
        UINT64 Observed = InterlockedCompareExchange64(
            (volatile LONG64 *)&Metric->Maximum, Value, Previous);
        if (Observed == Previous)
            break;
        Previous = Observed;
    }
}
VOID SpdLatencyLog(const SPD_LATENCY_METRIC *Metric, PCSTR Name, BOOLEAN IsDepth);
typedef struct
{
    UINT64 CqTicks, AcceptedTicks, RetiredTicks;
} SPD_RING_RETIRE_TIMING;

/* I/O queue */
typedef struct
{
    PVOID DeviceExtension;
    struct _SPD_STORAGE_UNIT *StorageUnit;
    KSPIN_LOCK SpinLock;
    volatile LONG64 SpinLockWaitTicks;
    volatile LONG64 SpinLockHoldTicks;
    volatile LONG64 SpinLockAcquisitions;
    SPD_HISTOGRAM SpinLockWaitHistogram, SpinLockHoldHistogram;
    UINT64 AcceptedSrbs, RetiredSrbs;
#if SPD_RING_LATENCY_DIAGNOSTICS
    UINT32 CompletionQueuedCount;
#endif
    BOOLEAN Stopped;
    BOOLEAN RingClosing;
    ULONG ResetOwners;
    BOOLEAN NonblockingConsumer;
    BOOLEAN FlushInProgress;
    ULONG ActiveSrbs;
    SPD_QEVENT PendingEvent;
    LIST_ENTRY PendingList, ProcessList;
    /* Shared transport metadata, all protected by SpinLock. ProcessList and
     * ProcessBuckets below belong exclusively to the legacy transport. */
    struct _SPD_WORK_SLOT *Slots;
    UINT32 SlotCount;
    LIST_ENTRY FreeSlots, ReadySlots, CompletedSlots;
    ULONG ProcessBucketCount;
    PVOID ProcessBuckets[];
} SPD_IOQ;

typedef enum
{
    SpdIoqResetReasonDelete,
    SpdIoqResetReasonBusReset,
    SpdIoqResetReasonDeviceReset,
    SpdIoqResetReasonLuReset,
    SpdIoqResetReasonRingFailure,
    SpdIoqResetReasonRingStop,
    SpdIoqResetReasonRingClose,
    SpdIoqResetReasonRemoval
} SPD_IOQ_RESET_REASON;

NTSTATUS SpdIoqCreate(PVOID DeviceExtension, SPD_IOQ **PIoq);
VOID SpdIoqDelete(SPD_IOQ *Ioq);
NTSTATUS SpdIoqRingAttachSlots(SPD_IOQ *Ioq,
    struct _SPD_WORK_SLOT *Slots, UINT32 Count);
struct _SPD_WORK_SLOT *SpdIoqRingDetachSlots(SPD_IOQ *Ioq);
VOID SpdIoqRingBeginClose(SPD_IOQ *Ioq);
VOID SpdIoqReset(SPD_IOQ *Ioq, BOOLEAN Stop,
    SPD_IOQ_RESET_REASON Reason);
BOOLEAN SpdIoqStopped(SPD_IOQ *Ioq);
VOID SpdIoqLogDiagnostics(SPD_IOQ *Ioq);
VOID SpdStorageUnitRingTerminalReset(struct _SPD_STORAGE_UNIT *StorageUnit);
NTSTATUS SpdIoqCancelSrb(SPD_IOQ *Ioq, PVOID Srb);
#if defined(WINSPD_TEST_BUILD)
NTSTATUS SpdIoqTestCancel(SPD_IOQ *Ioq, UINT32 Kind,
    UINT64 BlockAddress, UINT32 ExpectedState);
BOOLEAN SpdSrbTestMatchesRange(PVOID Srb, UINT32 Kind, UINT64 BlockAddress);
NTSTATUS SpdIoqTestExhaustGeneration(SPD_IOQ *Ioq);
NTSTATUS SpdIoqTestPromoteFlush(SPD_IOQ *Ioq, UINT64 BlockAddress);
NTSTATUS SpdIoqTestExpectChunked(SPD_IOQ *Ioq, UINT64 BlockAddress);
NTSTATUS SpdIoqTestExpectFence(SPD_IOQ *Ioq);
NTSTATUS SpdIoqTestExpectPending(SPD_IOQ *Ioq, UINT32 Kind, UINT64 BlockAddress);
NTSTATUS SpdIoqTestSetChunkLimit(SPD_IOQ *Ioq, UINT32 Limit);
NTSTATUS SpdIoqTestCopyBudget(SPD_IOQ *Ioq);
NTSTATUS SpdSrbTestNonCdbHelpers(struct _SPD_STORAGE_UNIT *StorageUnit);
#endif
struct _SPD_STORAGE_UNIT;
struct _SPD_SRB_EXTENSION;
VOID SpdStorageUnitRingTestPrepareCopyBarrier(
    struct _SPD_STORAGE_UNIT *StorageUnit);
VOID SpdStorageUnitRingTestCompletionCopyBarrier(
    struct _SPD_STORAGE_UNIT *StorageUnit);
VOID SpdStorageUnitRingTestResetRetireBarrier(
    struct _SPD_STORAGE_UNIT *StorageUnit);
/*
 * Caller must hold a live StorageUnit reference
 * for the duration of SpdIoqPostSrb().
 */
NTSTATUS SpdIoqPostSrb(struct _SPD_STORAGE_UNIT *StorageUnit, PVOID Srb);
NTSTATUS SpdIoqTryStartProcessingSrb(SPD_IOQ *Ioq,
    VOID (*Prepare)(PVOID SrbExtension, PVOID Context, PVOID DataBuffer),
    PVOID Context, PVOID DataBuffer);
NTSTATUS SpdIoqRingClaimBatch(SPD_IOQ *Ioq,
    struct _SPD_WORK_SLOT **Slots, UINT32 Limit, UINT32 CopyByteLimit, UINT32 *PCount);
UINT32 SpdIoqRingCommitBatch(SPD_IOQ *Ioq,
    struct _SPD_WORK_SLOT **Slots, SPD_RING_REQUEST *Requests, UINT32 Count,
    BOOLEAN AbortAll, PVOID *AbortedSrbs, BOOLEAN *PQueueCleanup);
UINT32 SpdIoqRingPublishBatch(SPD_IOQ *Ioq, UINT32 Limit,
    BOOLEAN *PInvalidHead);
NTSTATUS SpdIoqRingEnqueueCompletions(SPD_IOQ *Ioq,
    const SPD_RING_COMPLETION *Completions, UINT32 Count, UINT32 *PAccepted);
UINT32 SpdIoqRingClaimCompletedBatch(SPD_IOQ *Ioq,
    struct _SPD_WORK_SLOT **Slots, UINT32 Limit);
UINT32 SpdIoqRingFinishCompletedBatch(SPD_IOQ *Ioq,
    struct _SPD_WORK_SLOT **Slots, UCHAR *Statuses, UINT32 Count,
    PVOID *Srbs, UCHAR *FinalStatuses, SPD_RING_RETIRE_TIMING *Timings);
BOOLEAN SpdIoqRingHasCompleted(SPD_IOQ *Ioq);
UINT32 SpdIoqRingClaimMappedCleanup(SPD_IOQ *Ioq,
    struct _SPD_WORK_SLOT **Slots, UINT32 Limit);
ULONG SpdIoqRingLeasedCount(SPD_IOQ *Ioq);
BOOLEAN SpdIoqRingHasPending(SPD_IOQ *Ioq);
BOOLEAN SpdIoqRingHasReady(SPD_IOQ *Ioq);
UINT32 SpdSrbPreparationCopyBytes(struct _SPD_SRB_EXTENSION *Extension);
NTSTATUS SpdIoqStartProcessingSrb(SPD_IOQ *Ioq, PLARGE_INTEGER Timeout, PIRP CancellableIrp,
    VOID (*Prepare)(PVOID SrbExtension, PVOID Context, PVOID DataBuffer),
    PVOID Context, PVOID DataBuffer);
NTSTATUS SpdIoqEndProcessingSrb(SPD_IOQ *Ioq, UINT64 Hint,
    UCHAR (*Complete)(PVOID SrbExtension, PVOID Context, PVOID DataBuffer),
    PVOID Context, PVOID DataBuffer);
NTSTATUS SpdIoqEndProcessingSrbByExtension(SPD_IOQ *Ioq,
    PVOID SrbExtension,
    UCHAR (*Complete)(PVOID SrbExtension, PVOID Context, PVOID DataBuffer),
    PVOID Context, PVOID DataBuffer);
typedef enum
{
    SpdRingSrbNone = 0,
    SpdRingSrbPending,
    SpdRingSrbPreparing,
    SpdRingSrbReady,
    SpdRingSrbInFlight,
    SpdRingSrbCompletionQueued,
    SpdRingSrbCompleting,
    /* Detached from admission/processing; owned by a reset retirement batch. */
    SpdRingSrbRetiring
} SPD_RING_SRB_STATE;

typedef struct _SPD_SRB_EXTENSION
{
    struct _SPD_STORAGE_UNIT *StorageUnit;
    LIST_ENTRY ListEntry;
    PVOID HashNext;
    PVOID Srb;
    PVOID SystemDataBuffer;
    PMDL OriginalMdl; /* Borrowed from Storport; never unlock/free. */
    BOOLEAN MappingCandidate;
    ULONG SystemDataLength;
    ULONG ChunkOffset;
    UINT64 PendingTicks;
#if SPD_RING_LATENCY_DIAGNOSTICS
    UINT64 AcceptedTicks;
#endif
    SPD_RING_SRB_STATE RingState;
    BOOLEAN RingAbortPending;
    BOOLEAN RingIsFlush;
    BOOLEAN ExecutionStarted;
    struct _SPD_WORK_SLOT *Slot;
} SPD_SRB_EXTENSION;
#define SpdSrbExtension(Srb)            ((SPD_SRB_EXTENSION *)SrbGetMiniportContext(Srb))

typedef enum
{
    SpdSlotFree,
    SpdSlotPreparing,
    SpdSlotReady,
    SpdSlotInFlight,
    SpdSlotCompletionQueued,
    SpdSlotCompleting
} SPD_SLOT_STATE;

typedef struct _SPD_WORK_SLOT
{
    LIST_ENTRY ListEntry;
    SPD_SRB_EXTENSION *Srb;
    UINT64 RequestId;
    SPD_SLOT_STATE State;
    BOOLEAN CancelRequested;
    BOOLEAN MappingLease; /* Cleanup must run outside the metadata lock. */
    PMDL MappingMdl;
    PVOID MappedUserAddress;
    SPD_IOCTL_TRANSACT_REQ Request;
    SPD_IOCTL_TRANSACT_RSP Response;
    UINT32 DataLength;
    UINT64 PreparingTicks, ReadyTicks, PublishedTicks, CqTicks;
#if SPD_RING_LATENCY_DIAGNOSTICS
    UINT64 CompletionClaimTicks, CompletionServiceEnd;
#endif
} SPD_WORK_SLOT;

#ifndef SPD_RING_MAP_THRESHOLD_BYTES
#define SPD_RING_MAP_THRESHOLD_BYTES 0
#endif
#ifndef SPD_RING_MAP_ORIGIN_DIAGNOSTICS
#define SPD_RING_MAP_ORIGIN_DIAGNOSTICS 0
#endif

/* Change only IOQ lock acquisition, preserving all metadata ownership rules. */
#ifndef SPD_IOQ_QUEUED_SPIN_LOCK
#define SPD_IOQ_QUEUED_SPIN_LOCK 0
#endif
#if SPD_IOQ_QUEUED_SPIN_LOCK != 0 && SPD_IOQ_QUEUED_SPIN_LOCK != 1
#error IOQ queued spin lock selector must be 0 or 1.
#endif

#define SPD_RING_COMPLETION_LIMIT 32
#ifndef SPD_RING_COMPLETION_CLAIM_LIMIT
#define SPD_RING_COMPLETION_CLAIM_LIMIT 32
#endif
#if SPD_RING_COMPLETION_CLAIM_LIMIT < 1 || SPD_RING_COMPLETION_CLAIM_LIMIT > 32
#error Completion claim limit must be between 1 and 32.
#endif
#define SPD_RING_PUBLISH_LIMIT 32

/* Keep execution-context experiments independent of queue/slot ownership. */
#ifndef SPD_RING_THREAD_PRIORITY
#define SPD_RING_THREAD_PRIORITY 8
#endif
/* Restrict this experiment to Normal and Delayed worker priorities. */
#if SPD_RING_THREAD_PRIORITY != 8 && SPD_RING_THREAD_PRIORITY != 12
#error Ring thread priority must be 8 (Normal) or 12 (Delayed).
#endif
#ifndef SPD_RING_PREPARATION_THREAD
#define SPD_RING_PREPARATION_THREAD 0
#endif
#ifndef SPD_RING_COMPLETION_THREAD
#define SPD_RING_COMPLETION_THREAD 0
#endif
#ifndef SPD_RING_PREPARATION_THREADS
#define SPD_RING_PREPARATION_THREADS 1
#endif
#ifndef SPD_RING_COMPLETION_THREADS
#define SPD_RING_COMPLETION_THREADS 1
#endif
#if SPD_RING_PREPARATION_THREADS < 1 || SPD_RING_PREPARATION_THREADS > 4 || SPD_RING_COMPLETION_THREADS < 1 || SPD_RING_COMPLETION_THREADS > 4
#error Ring thread counts must be between 1 and 4 per stage.
#endif
#if (SPD_RING_PREPARATION_THREAD != 0 && SPD_RING_PREPARATION_THREAD != 1) || (SPD_RING_COMPLETION_THREAD != 0 && SPD_RING_COMPLETION_THREAD != 1)
#error Ring thread selectors must be 0 (work item) or 1 (dedicated thread).
#endif
#if (SPD_RING_PREPARATION_THREAD || SPD_RING_COMPLETION_THREAD) && NTDDI_VERSION < NTDDI_WIN10_FE
#error Dedicated ring threads require NTDDI_WIN10_FE or later for Storport thread APIs.
#endif
typedef struct _SPD_RING_THREAD SPD_RING_THREAD;

#ifndef SPD_RING_CLAIM_LIMIT
#define SPD_RING_CLAIM_LIMIT 32
#endif
/* Claim and commit limits are deliberately independent tuning parameters. */
#ifndef SPD_RING_PREPARE_COPY_LIMIT
#define SPD_RING_PREPARE_COPY_LIMIT (1024 * 1024)
#endif
#ifndef SPD_RING_READY_COMMIT_LIMIT
#define SPD_RING_READY_COMMIT_LIMIT 8
#endif
#ifndef SPD_RING_READY_COPY_LIMIT
#define SPD_RING_READY_COPY_LIMIT (256 * 1024)
#endif
#ifndef SPD_RING_PREPARE_TIME_US
#define SPD_RING_PREPARE_TIME_US 100
#endif
#define SPD_SLOT_GENERATION_MAX 0x0000ffffffffffffULL

#if SPD_RING_CLAIM_LIMIT < 1 || SPD_RING_CLAIM_LIMIT > 32
#error Preparation claim limit must be between 1 and 32.
#endif
#if SPD_RING_PREPARE_COPY_LIMIT < 1 || SPD_RING_READY_COMMIT_LIMIT < 1 || SPD_RING_READY_COPY_LIMIT < 1 || SPD_RING_PREPARE_TIME_US < 1
#error Preparation copy, commit and time budgets must be positive.
#endif

/* Each stage state owns one lifetime reference while ACTIVE/RERUN. */
#define SPD_RING_PRODUCER_IDLE     0L
#define SPD_RING_PRODUCER_ACTIVE   1L
#define SPD_RING_PRODUCER_RERUN    2L

typedef struct
{
    /* System VA owned by Mdl via MmGetSystemAddressForMdlSafe. */
    PVOID SystemAddress;
    SIZE_T SectionSize;
    UINT32 RequestOffset;
    UINT32 CompletionOffset;
    UINT32 BufferOffset;
    UINT32 QueueDepth;
    UINT32 BufferCount;
    UINT32 BufferSize;
    UINT32 RequestTail;
    UINT32 CompletionHead;
    KSPIN_LOCK Lock;
    PVOID volatile WaitIrp;
    PVOID ProducerWorker;
    /* A device work item hands off retries while Storport still owns Worker. */
    PIO_WORKITEM ProducerRetryWorker;
    BOOLEAN ProducerRetryQueued;
    volatile LONG ProducerState;
#if DBG
    volatile LONG ProducerExecuting;
    volatile LONG ProducerLiveReferences;
#endif
    PVOID PreparationWorker;
    SPD_RING_THREAD *PreparationThread;
    /* A device work item hands off retries while Storport still owns Worker. */
    PIO_WORKITEM PreparationRetryWorker;
    BOOLEAN PreparationRetryQueued;
    volatile LONG PreparationState;
#if DBG
    volatile LONG PreparationExecuting;
    volatile LONG PreparationLiveReferences;
#endif
    PVOID CompletionWorker;
    SPD_RING_THREAD *CompletionThread;
    /* A device work item hands off retries while Storport still owns Worker. */
    PIO_WORKITEM CompletionRetryWorker;
    BOOLEAN CompletionRetryQueued;
    volatile LONG CompletionState;
#if DBG
    volatile LONG CompletionExecuting;
    volatile LONG CompletionLiveReferences;
#endif
#if defined(WINSPD_TEST_BUILD)
    PKEVENT TestIoqPostEnteredEvent;
    PKEVENT TestIoqPostReleaseEvent;
    PKEVENT TestIoqPostResetDoneEvent;
    PKEVENT TestProducerPrepareEnteredEvent;
    PKEVENT TestProducerPrepareReleaseEvent;
    PKEVENT TestCompletionCopyEnteredEvent;
    PKEVENT TestCompletionCopyReleaseEvent;
    PKEVENT TestCompletionClaimEnteredEvent;
    PKEVENT TestCompletionClaimReleaseEvent;
    PKEVENT TestPrepareCommitEnteredEvent;
    PKEVENT TestPrepareCommitReleaseEvent;
    PKEVENT TestReadyEnteredEvent;
    PKEVENT TestReadyReleaseEvent;
    PKEVENT TestPreparationClaimReleaseEvent;
    volatile LONG TestIoqPostBarrierActive;
#endif
    volatile LONG KickActive;
    BOOLEAN Stopping;
    BOOLEAN Failed;
    ULONG ActiveCalls;
    KEVENT IdleEvent;
    volatile LONG CloseState;
    KEVENT CloseDoneEvent;
    ULONG ProcessId;
    volatile LONG64 SqFullEvents;
    volatile LONG64 BufferPoolExhaustions;
    volatile LONG64 WaitSubmissions;
    volatile LONG64 WaitCompletions;
    volatile LONG64 ProducerWorkerRuns;
    volatile LONG64 ProducerWorkerReruns;
    volatile LONG64 ProducerContinuations;
    volatile LONG64 ProducerBusyDeferrals;
    volatile LONG64 PreparationWorkerRuns;
    volatile LONG64 PreparationWorkerReruns;
    volatile LONG64 PreparationContinuations;
    volatile LONG64 PreparationBusyDeferrals;
    volatile LONG64 PreparationClaimed;
    volatile LONG64 PreparationCommits;
    volatile LONG64 PreparationCallbackTicks;
    volatile LONG PreparationMaxClaim;
    volatile LONG64 CompletionWorkerRuns;
    volatile LONG64 CompletionWorkerReruns;
    volatile LONG64 CompletionContinuations;
    volatile LONG64 CompletionBusyDeferrals;
    volatile LONG64 CompletionClaimed;
    volatile LONG64 CompletionCallbackTicks;
    volatile LONG CompletionMaxClaim;
    volatile LONG PreparationOwnedBatches, CompletionOwnedBatches;
    volatile LONG PreparationMaxOwnedBatches, CompletionMaxOwnedBatches;
    volatile LONG64 ProducerProduced;
    volatile LONG64 ProducerBatchLe1;
    volatile LONG64 ProducerBatch2To4;
    volatile LONG64 ProducerBatch5To16;
    volatile LONG64 ProducerBatch17To64;
    volatile LONG64 ProducerBatchOver64;
    volatile LONG64 PreparationCopyTicks;
    volatile LONG64 MapAttempts, MapSuccesses, MapFallbacks, MapUnmaps;
    volatile LONG64 MapOriginSamples, MapOriginUser, MapOriginKernel;
    volatile LONG64 MapOriginUnknown, MapOriginSelf;
    volatile LONG64 MapBytes, MapTicks, UnmapTicks;
    volatile LONG MapSourceFlagsOr;
    volatile LONG64 CompletionCopyTicks;
    volatile LONG64 DeferredSrbAborts;
    UINT64 PreparationClaimCopyBytes, PreparationOversizedClaims, PreparationCopyBudgetViolations;
    volatile LONG64 NonCdbFlushSubmissions;
    volatile LONG64 ShutdownSubmissions;
    volatile LONG ProducerMaxBatch;
    SPD_HISTOGRAM PendingToPreparing, PreparingToReady, ReadyToSq;
    SPD_HISTOGRAM CqToRetire, CqToComplete, FlushRoundTrip;
    SPD_HISTOGRAM PreparationDuration, CompletionDuration;
    UINT64 PreparationClaimSizes[SPD_RING_CLAIM_LIMIT + 1];
    UINT64 PreparationCommitSizes[SPD_RING_CLAIM_LIMIT + 1];
    UINT64 CompletionClaimSizes[SPD_RING_COMPLETION_CLAIM_LIMIT + 1];
#if SPD_RING_LATENCY_DIAGNOSTICS
    SPD_LATENCY_METRIC Latency[SpdLatencyCount];
    volatile LONG64 PreparationQueuedTicks, ProducerQueuedTicks, CompletionQueuedTicks;
    volatile LONG64 KickCalls, KickBusy;
#endif

    /* Ring lifetime and mapping state. */
    HANDLE SectionHandle;
    PMDL Mdl;
    PVOID UserAddress;
    ULONG UserProcessId;
} SPD_RING_STATE;

typedef enum
{
    SpdServiceIrpCompleteNow,
    SpdServiceIrpDeferred
} SPD_SERVICE_IRP_DISPOSITION;

/* storage units */
typedef struct _SPD_STORAGE_UNIT SPD_STORAGE_UNIT;
typedef struct _SPD_DEVICE_EXTENSION
{
    KSPIN_LOCK SpinLock;
    PDEVICE_OBJECT DeviceObject;        /* adapter device */
    ULONG StorageUnitCount, StorageUnitCapacity;
    ULONG RundownWorkers;
    /* Immutable after HwInitialize; report queried and successfully applied
     * completion performance options separately from the requested build mode. */
    ULONG CompletionPerfMode, CompletionPerfVersion;
    ULONG CompletionPerfQueryStatus, CompletionPerfApplyStatus;
    ULONG CompletionPerfSupported, CompletionPerfRequested, CompletionPerfApplied;
    KEVENT RundownIdleEvent;
    /* Removed units reserve their address until physical ring rundown. */
    SPD_STORAGE_UNIT *RetiringUnits[SPD_IOCTL_STORAGE_UNIT_MAX_CAPACITY];
    SPD_STORAGE_UNIT *StorageUnits[];
} SPD_DEVICE_EXTENSION;
typedef struct _SPD_STORAGE_UNIT
{
    /* fields protected by SPD_DEVICE_EXTENSION::SpinLock */
    ULONG RefCount;
    /* fields below are read-only after construction */
    SPD_IOCTL_STORAGE_UNIT_PARAMS StorageUnitParams;
    CHAR SerialNumber[36];
    ULONG OwnerProcessId;
    PEPROCESS OwnerProcess;
    UINT32 Btl;
    SPD_IOQ *Ioq;
    /* fields not protected */
    PDEVICE_OBJECT DeviceObject;        /* disk device */
    ULONG TransactProcessId;
    /* Set once during OPEN under Ring.Lock; references live until deletion. */
    PEPROCESS RingProcess;
    PFILE_OBJECT RingFileObject;
    PIO_WORKITEM RundownWorker;
#if defined(WINSPD_TEST_BUILD)
    UINT32 TestChunkLimit;
    volatile LONG TestLegacyContinuations;
    PKEVENT TestOwnerRundownDoneEvent;
#endif
    SPD_RING_STATE Ring;
} SPD_STORAGE_UNIT;
NTSTATUS SpdDeviceExtensionInit(SPD_DEVICE_EXTENSION *DeviceExtension, PVOID BusInformation);
VOID SpdDeviceExtensionFini(SPD_DEVICE_EXTENSION *DeviceExtension);
SPD_DEVICE_EXTENSION *SpdDeviceExtensionAcquire(VOID);
VOID SpdDeviceExtensionRelease(SPD_DEVICE_EXTENSION *DeviceExtension);
NTSTATUS SpdStorageUnitProvision(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    SPD_IOCTL_STORAGE_UNIT_PARAMS *StorageUnitParams,
    ULONG ProcessId,
    PUINT32 PBtl);
NTSTATUS SpdStorageUnitUnprovision(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    PGUID Guid, ULONG Index,
    ULONG ProcessId);
NTSTATUS SpdStorageUnitRingOpen(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId,
    PFILE_OBJECT FileObject,
    SPD_IOCTL_RING_OPEN_PARAMS *Params);
VOID SpdStorageUnitRingClose(
    SPD_STORAGE_UNIT *StorageUnit,
    BOOLEAN UserProcessExiting);
NTSTATUS SpdStorageUnitRingStop(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId);
SPD_SERVICE_IRP_DISPOSITION SpdStorageUnitRingWait(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId,
    SPD_IOCTL_RING_WAIT_PARAMS *Params,
    PIRP Irp);
VOID SpdStorageUnitRingScheduleProducer(SPD_STORAGE_UNIT *StorageUnit);
VOID SpdStorageUnitRingSchedulePreparation(SPD_STORAGE_UNIT *StorageUnit);
VOID SpdStorageUnitRingScheduleCompletion(SPD_STORAGE_UNIT *StorageUnit);
#if defined(WINSPD_TEST_BUILD)
VOID SpdStorageUnitRingTestIoqPostBarrier(
    SPD_STORAGE_UNIT *StorageUnit);
VOID SpdStorageUnitRingTestIoqResetComplete(
    SPD_STORAGE_UNIT *StorageUnit);
#endif
VOID SpdStorageUnitRingStopForRemoval(SPD_STORAGE_UNIT *StorageUnit);
NTSTATUS SpdStorageUnitRingKick(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId,
    SPD_IOCTL_RING_KICK_PARAMS *Params);
SPD_STORAGE_UNIT *SpdStorageUnitReferenceByBtl(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    UINT32 Btl);
SPD_STORAGE_UNIT *SpdStorageUnitReferenceForClose(
    SPD_DEVICE_EXTENSION *DeviceExtension, UINT32 Btl);
VOID SpdStorageUnitDereference(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    SPD_STORAGE_UNIT *StorageUnit);
ULONG SpdStorageUnitGetUseBitmap(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    PULONG PProcessId,
    UINT8 Bitmap[32]);
NTSTATUS SpdStorageUnitGlobalSetDevice(
    PDEVICE_OBJECT DeviceObject);
SPD_STORAGE_UNIT *SpdStorageUnitGlobalReferenceByDevice(
    PDEVICE_OBJECT DeviceObject);
VOID SpdStorageUnitGlobalDereference(
    SPD_STORAGE_UNIT *StorageUnit);
static inline
SPD_STORAGE_UNIT *SpdStorageUnitReference(PVOID DeviceExtension, PVOID Srb)
{
    UCHAR PathId, TargetId, Lun;

    SrbGetPathTargetLun(Srb, &PathId, &TargetId, &Lun);
    return SpdStorageUnitReferenceByBtl(DeviceExtension, SPD_IOCTL_BTL(PathId, TargetId, Lun));
}
extern ERESOURCE SpdGlobalDeviceResource;
extern KMUTEX SpdGlobalDeviceLifecycleMutex;
extern SPD_DEVICE_EXTENSION *SpdGlobalDeviceExtension;  /* protected by SpdGlobalDeviceResource */
extern ULONG SpdStorageUnitCapacity;                    /* read-only after DriverLoad */
#define SPD_INDEX_FROM_BTL(Btl)         SPD_IOCTL_BTL_T(Btl)
#define SPD_BTL_FROM_INDEX(Idx)         SPD_IOCTL_BTL(0, Idx, 0)

/* utility */
NTSTATUS SpdRegistryGetValue(PUNICODE_STRING Path, PUNICODE_STRING ValueName,
    PKEY_VALUE_PARTIAL_INFORMATION ValueInformation, PULONG PValueInformationLength);
VOID SpdMakeDeviceIoControlRequest(
    ULONG ControlCode,
    PDEVICE_OBJECT DeviceObject,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    PIO_STATUS_BLOCK IoStatus);
NTSTATUS SpdGetScsiAddress(
    PDEVICE_OBJECT DeviceObject,
    PSCSI_ADDRESS ScsiAddress);

/*
 * Fixes
 */

/* RtlEqualMemory: this is defined as memcmp, which does not exist on Win7 x86! */
#undef RtlEqualMemory
static inline
LOGICAL RtlEqualMemory(const VOID *Source1, const VOID *Source2, SIZE_T Length)
{
    return Length == RtlCompareMemory(Source1, Source2, Length);
}

#endif
