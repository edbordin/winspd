/**
 * @file ring-test.c
 *
 * SharedRing transport integration tests. These tests require the WinSpd
 * driver and should be run elevated, like the other ioctl tests.
 */

#include <winspd/winspd.h>
#include <tlib/testsuite.h>
#include <process.h>

typedef struct _RING_TEST_STATE
{
    SPD_STORAGE_UNIT *StorageUnit;
    HANDLE ReleaseReads;
    HANDLE TwoReadsStarted;
    SRWLOCK AsyncLock;
    SRWLOCK TokenLock;
    HANDLE AsyncThreads[16];
    UINT64 TokensBySlot[4];
    BOOLEAN TokenSeen[4];
    LONG AsyncThreadCount;
    LONG AsyncMode;
    LONG HoldReads;
    LONG ReadCalls;
    LONG Failure;
    UINT32 QueueDepth;
} RING_TEST_STATE;

typedef struct _RING_TEST_ASYNC_RESPONSE
{
    RING_TEST_STATE *State;
    SPD_IOCTL_TRANSACT_RSP Response;
    HANDLE SentEvent;
    BOOLEAN Early;
} RING_TEST_ASYNC_RESPONSE;

typedef struct _RING_TEST_READ_THREAD
{
    RING_TEST_STATE *State;
    HANDLE Finished;
    DWORD Error;
} RING_TEST_READ_THREAD;

enum
{
    RingTestAsyncNone,
    RingTestAsyncEarly,
    RingTestAsyncDeferred
};

static const GUID RingTestGuidLifecycle =
    { 0x51aeb043, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x01 } };
static const GUID RingTestGuidWraparound =
    { 0x51aeb044, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x02 } };
static const GUID RingTestGuidAsync =
    { 0x51aeb045, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x03 } };
static const GUID RingTestGuidSaturation =
    { 0x51aeb046, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x04 } };
static const GUID RingTestGuidWaitCredit =
    { 0x51aeb047, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x05 } };

static DWORD WINAPI ring_test_async_response_thread(PVOID Data)
{
    RING_TEST_ASYNC_RESPONSE *Work = Data;

    if (!Work->Early)
        Sleep(20);
    SpdStorageUnitSendResponse(Work->State->StorageUnit,
        &Work->Response, 0);
    if (0 != Work->SentEvent)
        SetEvent(Work->SentEvent);
    free(Work);
    return ERROR_SUCCESS;
}

static BOOLEAN ring_test_read(SPD_STORAGE_UNIT *StorageUnit,
    PVOID Buffer, UINT64 BlockAddress, UINT32 BlockCount, BOOLEAN Flush,
    SPD_STORAGE_UNIT_STATUS *Status)
{
    RING_TEST_STATE *State = StorageUnit->UserContext;
    SPD_STORAGE_UNIT_OPERATION_CONTEXT *OperationContext;
    LONG AsyncMode;
    LONG ReadCall;

    UNREFERENCED_PARAMETER(BlockAddress);
    UNREFERENCED_PARAMETER(Flush);
    memset(Status, 0, sizeof *Status);
    OperationContext = SpdStorageUnitGetOperationContext();
    if (0 == OperationContext || 0 == OperationContext->Request)
        InterlockedExchange(&State->Failure, 1);
    else
    {
        UINT64 Token = OperationContext->Request->Hint;
        UINT32 Slot = (UINT32)Token;
        if (Slot >= State->QueueDepth || Slot >= ARRAYSIZE(State->TokensBySlot))
            InterlockedExchange(&State->Failure, 1);
        else
        {
            AcquireSRWLockExclusive(&State->TokenLock);
            if (State->TokenSeen[Slot] &&
                State->TokensBySlot[Slot] == Token)
                InterlockedExchange(&State->Failure, 1);
            State->TokensBySlot[Slot] = Token;
            State->TokenSeen[Slot] = TRUE;
            ReleaseSRWLockExclusive(&State->TokenLock);
        }
    }
    ReadCall = InterlockedIncrement(&State->ReadCalls);
    if (0 != InterlockedCompareExchange(&State->HoldReads, 0, 0))
    {
        if (2 <= ReadCall)
            SetEvent(State->TwoReadsStarted);
        if (WAIT_OBJECT_0 != WaitForSingleObject(State->ReleaseReads, 10000))
        {
            InterlockedExchange(&State->Failure, 1);
            return TRUE;
        }
    }
    if (0 != Buffer)
        memset(Buffer, 0x5a,
            (SIZE_T)BlockCount * StorageUnit->StorageUnitParams.BlockLength);

    AsyncMode = InterlockedCompareExchange(&State->AsyncMode, 0, 0);
    if (RingTestAsyncNone != AsyncMode)
    {
        RING_TEST_ASYNC_RESPONSE *Work;
        HANDLE Thread;
        HANDLE SentEvent = 0;

        if (0 == OperationContext || 0 == OperationContext->Response)
        {
            InterlockedExchange(&State->Failure, 1);
            return TRUE;
        }
        Work = calloc(1, sizeof *Work);
        if (0 == Work)
        {
            InterlockedExchange(&State->Failure, 1);
            return TRUE;
        }
        if (RingTestAsyncEarly == AsyncMode)
        {
            SentEvent = CreateEventW(0, FALSE, FALSE, 0);
            if (0 == SentEvent)
            {
                free(Work);
                InterlockedExchange(&State->Failure, 1);
                return TRUE;
            }
        }
        Work->State = State;
        Work->Response = *OperationContext->Response;
        Work->SentEvent = SentEvent;
        Work->Early = RingTestAsyncEarly == AsyncMode;
        Thread = CreateThread(0, 0,
            ring_test_async_response_thread, Work, 0, 0);
        if (0 == Thread)
        {
            if (0 != SentEvent)
                CloseHandle(SentEvent);
            free(Work);
            InterlockedExchange(&State->Failure, 1);
            return TRUE;
        }
        AcquireSRWLockExclusive(&State->AsyncLock);
        if (State->AsyncThreadCount < ARRAYSIZE(State->AsyncThreads))
            State->AsyncThreads[State->AsyncThreadCount++] = Thread;
        else
        {
            ReleaseSRWLockExclusive(&State->AsyncLock);
            CloseHandle(Thread);
            if (0 != SentEvent)
                CloseHandle(SentEvent);
            InterlockedExchange(&State->Failure, 1);
            return TRUE;
        }
        ReleaseSRWLockExclusive(&State->AsyncLock);

        if (0 != SentEvent)
        {
            if (WAIT_OBJECT_0 != WaitForSingleObject(SentEvent, 5000))
                InterlockedExchange(&State->Failure, 1);
            CloseHandle(SentEvent);
        }
        return FALSE;
    }

    return TRUE;
}

static BOOLEAN ring_test_flush(SPD_STORAGE_UNIT *StorageUnit,
    UINT64 BlockAddress, UINT32 BlockCount,
    SPD_STORAGE_UNIT_STATUS *Status)
{
    UNREFERENCED_PARAMETER(StorageUnit);
    UNREFERENCED_PARAMETER(BlockAddress);
    UNREFERENCED_PARAMETER(BlockCount);
    memset(Status, 0, sizeof *Status);
    return TRUE;
}

static const SPD_STORAGE_UNIT_INTERFACE RingTestInterface =
{
    ring_test_read,
    0,
    ring_test_flush,
};

static VOID ring_test_create(RING_TEST_STATE *State, const GUID *Guid,
    UINT32 QueueDepth)
{
    SPD_STORAGE_UNIT_PARAMS Params;
    SPD_IOCTL_RING_OPEN_PARAMS RingParams;
    DWORD Error;

    memset(State, 0, sizeof *State);
    InitializeSRWLock(&State->AsyncLock);
    InitializeSRWLock(&State->TokenLock);
    State->QueueDepth = QueueDepth;
    State->ReleaseReads = CreateEventW(0, TRUE, FALSE, 0);
    State->TwoReadsStarted = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != State->ReleaseReads);
    ASSERT(0 != State->TwoReadsStarted);

    memset(&Params, 0, sizeof Params);
    Params.Guid = *Guid;
    Params.BlockCount = 64;
    Params.BlockLength = 512;
    Params.MaxTransferLength = 4096;
    Error = SpdStorageUnitCreate(0, &Params, &RingTestInterface,
        &State->StorageUnit);
    ASSERT(ERROR_SUCCESS == Error);
    State->StorageUnit->UserContext = State;

    memset(&RingParams, 0, sizeof RingParams);
    RingParams.Version = SPD_RING_VERSION_1;
    RingParams.QueueDepth = QueueDepth;
    RingParams.BufferSize = 4096;
    Error = SpdStorageUnitOpenSharedRing(State->StorageUnit, &RingParams);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(0 != State->StorageUnit->SharedRingHeader);
    ASSERT(QueueDepth == State->StorageUnit->SharedRingHeader->QueueDepth);
    ASSERT(4096 == State->StorageUnit->SharedRingHeader->BufferSize);

    Error = SpdIoctlScsiInquiry(State->StorageUnit->Handle,
        State->StorageUnit->Btl, 0, 3000);
    ASSERT(ERROR_SUCCESS == Error);
}

static VOID ring_test_start(RING_TEST_STATE *State, ULONG WorkerCount)
{
    DWORD Error = SpdStorageUnitStartDispatcher(State->StorageUnit,
        WorkerCount);
    ASSERT(ERROR_SUCCESS == Error);
}

static VOID ring_test_join_async_threads(RING_TEST_STATE *State)
{
    AcquireSRWLockExclusive(&State->AsyncLock);
    for (LONG I = 0; I < State->AsyncThreadCount; I++)
    {
        WaitForSingleObject(State->AsyncThreads[I], INFINITE);
        CloseHandle(State->AsyncThreads[I]);
    }
    State->AsyncThreadCount = 0;
    ReleaseSRWLockExclusive(&State->AsyncLock);
}

static VOID ring_test_destroy(RING_TEST_STATE *State)
{
    DWORD DispatcherError = ERROR_SUCCESS;

    if (0 != State->StorageUnit)
    {
        SetEvent(State->ReleaseReads);
        ring_test_join_async_threads(State);
        SpdStorageUnitShutdown(State->StorageUnit);
        SpdStorageUnitWaitDispatcher(State->StorageUnit);
        SpdStorageUnitGetDispatcherError(State->StorageUnit,
            &DispatcherError);
        ASSERT(ERROR_SUCCESS == DispatcherError);
        ASSERT(0 == InterlockedCompareExchange(&State->Failure, 0, 0));
        SpdStorageUnitDelete(State->StorageUnit);
        State->StorageUnit = 0;
    }
    if (0 != State->TwoReadsStarted)
        CloseHandle(State->TwoReadsStarted);
    if (0 != State->ReleaseReads)
        CloseHandle(State->ReleaseReads);
}

static DWORD ring_test_scsi_read(RING_TEST_STATE *State)
{
    CDB Cdb;
    UINT8 DataBuffer[512];
    UINT32 DataLength = sizeof DataBuffer;
    UCHAR ScsiStatus;
    UCHAR Sense[32];
    DWORD Error;

    memset(&Cdb, 0, sizeof Cdb);
    Cdb.READ16.OperationCode = SCSIOP_READ16;
    Cdb.READ16.TransferLength[3] = 1;
    Error = SpdIoctlScsiExecute(State->StorageUnit->Handle,
        State->StorageUnit->Btl, &Cdb, +1, DataBuffer, &DataLength,
        &ScsiStatus, Sense);
    if (ERROR_SUCCESS == Error &&
        (SCSISTAT_GOOD != ScsiStatus || sizeof DataBuffer != DataLength))
        Error = ERROR_INVALID_DATA;
    return Error;
}

static DWORD WINAPI ring_test_scsi_read_thread(PVOID Data)
{
    RING_TEST_READ_THREAD *ReadThread = Data;
    ReadThread->Error = ring_test_scsi_read(ReadThread->State);
    SetEvent(ReadThread->Finished);
    return ReadThread->Error;
}

static DWORD ring_test_complete_one_manually(RING_TEST_STATE *State)
{
    SPD_RING_HEADER *Header = State->StorageUnit->SharedRingHeader;
    UINT64 SubmissionConsumer =
        SpdRingLoadCounter(&Header->SubmissionConsumer);
    UINT64 SubmissionProducer =
        SpdRingLoadCounter(&Header->SubmissionProducer);
    UINT64 CompletionConsumer =
        SpdRingLoadCounter(&Header->CompletionConsumer);
    UINT64 CompletionProducer =
        SpdRingLoadCounter(&Header->CompletionProducer);
    SPD_RING_REQUEST *Request;
    SPD_RING_COMPLETION *Completion;
    SPD_IOCTL_RING_KICK_PARAMS KickParams;

    if (SubmissionProducer - SubmissionConsumer != 1 ||
        CompletionProducer != CompletionConsumer)
        return ERROR_INVALID_DATA;

    Request = (SPD_RING_REQUEST *)
        ((PUINT8)State->StorageUnit->SharedRingAddress +
        Header->SubmissionOffset +
        (SubmissionConsumer % Header->QueueDepth) * sizeof *Request);
    Completion = (SPD_RING_COMPLETION *)
        ((PUINT8)State->StorageUnit->SharedRingAddress +
        Header->CompletionOffset +
        (CompletionProducer % Header->QueueDepth) * sizeof *Completion);
    memset(&Completion->Response, 0, sizeof Completion->Response);
    Completion->Response.Hint = Request->Request.Hint;
    Completion->Response.Kind = Request->Request.Kind;
    if (Request->Data.Slot != SPD_RING_NO_BUFFER)
        memset((PUINT8)State->StorageUnit->SharedRingAddress +
            Header->BufferOffset +
            (SIZE_T)Request->Data.Slot * Header->BufferSize +
            Request->Data.Offset, 0, Request->Data.Length);

    SpdRingStoreCounter(&Header->SubmissionConsumer,
        SubmissionConsumer + 1);
    SpdRingStoreCounter(&Header->CompletionProducer,
        CompletionProducer + 1);
    memset(&KickParams, 0, sizeof KickParams);
    {
        DWORD Error = SpdIoctlRingKick(State->StorageUnit->Handle,
            State->StorageUnit->Btl, &KickParams);
        if (ERROR_SUCCESS != Error)
            return Error;
    }
    return 1 == KickParams.Consumed ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

static void ioctl_ring_lifecycle_test(void)
{
    RING_TEST_STATE State;
    DWORD Error;

    ring_test_create(&State, &RingTestGuidLifecycle, 2);
    ASSERT(0 != State.StorageUnit->SharedRingAddress);
    ASSERT(0 < State.StorageUnit->SharedRingSize);
    SpdStorageUnitCloseSharedRing(State.StorageUnit);
    ASSERT(0 == State.StorageUnit->SharedRingAddress);

    SPD_IOCTL_RING_OPEN_PARAMS Params;
    memset(&Params, 0, sizeof Params);
    Params.Version = SPD_RING_VERSION_1;
    Params.QueueDepth = 2;
    Params.BufferSize = 4096;
    Error = SpdStorageUnitOpenSharedRing(State.StorageUnit, &Params);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(2 == State.StorageUnit->SharedRingHeader->QueueDepth);

    ring_test_start(&State, 1);
    ring_test_destroy(&State);
}

static void ioctl_ring_wraparound_test(void)
{
    RING_TEST_STATE State;
    SPD_RING_HEADER *Header;

    ring_test_create(&State, &RingTestGuidWraparound, 2);
    ring_test_start(&State, 2);
    Header = State.StorageUnit->SharedRingHeader;
    for (UINT32 I = 0; I < 8; I++)
        ASSERT(ERROR_SUCCESS == ring_test_scsi_read(&State));
    ASSERT(8 <= SpdRingLoadCounter(&Header->SubmissionProducer));
    ASSERT(8 <= SpdRingLoadCounter(&Header->SubmissionConsumer));
    ASSERT(8 <= SpdRingLoadCounter(&Header->CompletionProducer));
    ASSERT(8 <= SpdRingLoadCounter(&Header->CompletionConsumer));
    ring_test_destroy(&State);
}

static void ioctl_ring_async_response_test(void)
{
    RING_TEST_STATE State;

    ring_test_create(&State, &RingTestGuidAsync, 2);
    ring_test_start(&State, 2);
    InterlockedExchange(&State.AsyncMode, RingTestAsyncEarly);
    ASSERT(ERROR_SUCCESS == ring_test_scsi_read(&State));
    InterlockedExchange(&State.AsyncMode, RingTestAsyncDeferred);
    ASSERT(ERROR_SUCCESS == ring_test_scsi_read(&State));
    InterlockedExchange(&State.AsyncMode, RingTestAsyncNone);
    ring_test_join_async_threads(&State);
    ASSERT(0 == InterlockedCompareExchange(&State.Failure, 0, 0));
    ring_test_destroy(&State);
}

static void ioctl_ring_saturation_test(void)
{
    RING_TEST_STATE State;
    RING_TEST_READ_THREAD Reads[3];
    HANDLE Threads[3];

    ring_test_create(&State, &RingTestGuidSaturation, 2);
    ring_test_start(&State, 2);
    InterlockedExchange(&State.HoldReads, 1);
    memset(Reads, 0, sizeof Reads);
    memset(Threads, 0, sizeof Threads);
    for (UINT32 I = 0; I < ARRAYSIZE(Reads); I++)
    {
        Reads[I].State = &State;
        Reads[I].Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Reads[I].Finished);
        Threads[I] = CreateThread(0, 0,
            ring_test_scsi_read_thread, &Reads[I], 0, 0);
        ASSERT(0 != Threads[I]);
    }

    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.TwoReadsStarted, 5000));
    for (UINT32 I = 0; I < ARRAYSIZE(Reads); I++)
        ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Reads[I].Finished, 100));

    InterlockedExchange(&State.HoldReads, 0);
    SetEvent(State.ReleaseReads);
    for (UINT32 I = 0; I < ARRAYSIZE(Reads); I++)
    {
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Threads[I], 10000));
        ASSERT(ERROR_SUCCESS == Reads[I].Error);
        CloseHandle(Threads[I]);
        CloseHandle(Reads[I].Finished);
    }
    ring_test_destroy(&State);
}

static void ioctl_ring_wait_credit_test(void)
{
    RING_TEST_STATE State;
    RING_TEST_READ_THREAD Reads[4];
    HANDLE Threads[4];

    ring_test_create(&State, &RingTestGuidWaitCredit, 4);
    memset(Reads, 0, sizeof Reads);
    memset(Threads, 0, sizeof Threads);
    for (UINT32 I = 0; I < ARRAYSIZE(Reads); I++)
    {
        Reads[I].State = &State;
        Reads[I].Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Reads[I].Finished);
        Threads[I] = CreateThread(0, 0,
            ring_test_scsi_read_thread, &Reads[I], 0, 0);
        ASSERT(0 != Threads[I]);
    }

    for (UINT32 I = 0; I < ARRAYSIZE(Reads); I++)
    {
        SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
        memset(&WaitParams, 0, sizeof WaitParams);
        WaitParams.MaxRequests = 1;
        ASSERT(ERROR_SUCCESS == SpdIoctlRingWait(
            State.StorageUnit->Handle, State.StorageUnit->Btl,
            &WaitParams));
        ASSERT(1 == WaitParams.Produced);
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State));
    }

    for (UINT32 I = 0; I < ARRAYSIZE(Reads); I++)
    {
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Threads[I], 10000));
        ASSERT(ERROR_SUCCESS == Reads[I].Error);
        CloseHandle(Threads[I]);
        CloseHandle(Reads[I].Finished);
    }
    ASSERT(0 == InterlockedCompareExchange(&State.Failure, 0, 0));
    ring_test_destroy(&State);
}

void ring_tests(void)
{
    TEST(ioctl_ring_lifecycle_test);
    TEST(ioctl_ring_wraparound_test);
    TEST(ioctl_ring_async_response_test);
    TEST(ioctl_ring_saturation_test);
    TEST(ioctl_ring_wait_credit_test);
}
