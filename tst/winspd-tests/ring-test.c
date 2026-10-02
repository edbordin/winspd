/**
 * @file ring-test.c
 *
 * SharedRing transport integration tests. These tests require the WinSpd
 * driver and should be run elevated, like the other ioctl tests.
 */

#include <winspd/winspd.h>
#include <tlib/testsuite.h>
#include <process.h>
#include <setupapi.h>
#include <ntddstor.h>
#include <stdio.h>

#pragma comment(lib, "setupapi.lib")

typedef struct _RING_TEST_STATE
{
    SPD_STORAGE_UNIT *StorageUnit;
    GUID Guid;
    WCHAR DiskPath[1024];
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
    LONG ReadCallbacks;
    LONG TestReadCalls;
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

static const GUID RingTestDiskInterfaceGuid =
    { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };
static HANDLE RingTestDebugLogHandle = INVALID_HANDLE_VALUE;

enum
{
    RingTestBlockAddress = 32,
    RingTestDataLength = 512
};

static BOOL ring_test_disk_has_serial(PWSTR DiskPath, const char *ExpectedSerial)
{
    STORAGE_PROPERTY_QUERY Query;
    union
    {
        LARGE_INTEGER Alignment;
        UINT8 Bytes[4096];
    } DescriptorBuffer;
    PSTORAGE_DEVICE_DESCRIPTOR Descriptor;
    HANDLE DiskHandle;
    DWORD BytesReturned;
    BOOL Result = FALSE;

    DiskHandle = CreateFileW(DiskPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, 0);
    if (INVALID_HANDLE_VALUE == DiskHandle)
        return FALSE;

    memset(&Query, 0, sizeof Query);
    Query.PropertyId = StorageDeviceProperty;
    Query.QueryType = PropertyStandardQuery;
    if (DeviceIoControl(DiskHandle, IOCTL_STORAGE_QUERY_PROPERTY,
        &Query, sizeof Query, DescriptorBuffer.Bytes,
        sizeof DescriptorBuffer.Bytes, &BytesReturned, 0) &&
        BytesReturned >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
    {
        Descriptor = (PSTORAGE_DEVICE_DESCRIPTOR)DescriptorBuffer.Bytes;
        if (0 != Descriptor->SerialNumberOffset &&
            Descriptor->SerialNumberOffset < BytesReturned)
        {
            char *Serial = (char *)DescriptorBuffer.Bytes +
                Descriptor->SerialNumberOffset;
            size_t SerialLength = strnlen(Serial,
                BytesReturned - Descriptor->SerialNumberOffset);
            while (0 < SerialLength && ' ' == Serial[SerialLength - 1])
                Serial[--SerialLength] = '\0';
            Result = 0 == _stricmp(Serial, ExpectedSerial);
        }
    }

    CloseHandle(DiskHandle);
    return Result;
}

static BOOL ring_test_find_disk(RING_TEST_STATE *State, const GUID *Guid)
{
    char ExpectedSerial[37];
    WCHAR CandidatePath[64];

    sprintf_s(ExpectedSerial, sizeof ExpectedSerial,
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        Guid->Data1, Guid->Data2, Guid->Data3,
        Guid->Data4[0], Guid->Data4[1], Guid->Data4[2], Guid->Data4[3],
        Guid->Data4[4], Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);

    HDEVINFO DeviceInfoSet = SetupDiGetClassDevsW(
            &RingTestDiskInterfaceGuid, 0, 0,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (INVALID_HANDLE_VALUE != DeviceInfoSet)
        {
            for (DWORD Index = 0;; Index++)
            {
                SP_DEVICE_INTERFACE_DATA InterfaceData;
                SP_DEVINFO_DATA DeviceInfoData;
                DWORD RequiredSize = 0;
                PSP_DEVICE_INTERFACE_DETAIL_DATA_W DetailData;
                WCHAR InstanceId[256];
                BOOL Found = FALSE;

                memset(&InterfaceData, 0, sizeof InterfaceData);
                InterfaceData.cbSize = sizeof InterfaceData;
                memset(&DeviceInfoData, 0, sizeof DeviceInfoData);
                DeviceInfoData.cbSize = sizeof DeviceInfoData;
                if (!SetupDiEnumDeviceInterfaces(DeviceInfoSet, 0,
                    &RingTestDiskInterfaceGuid, Index, &InterfaceData))
                    break;

                SetupDiGetDeviceInterfaceDetailW(DeviceInfoSet,
                    &InterfaceData, 0, 0, &RequiredSize, 0);
                if (0 == RequiredSize)
                    continue;
                DetailData = malloc(RequiredSize);
                if (0 == DetailData)
                    continue;
                memset(DetailData, 0, RequiredSize);
                DetailData->cbSize = sizeof *DetailData;
                if (SetupDiGetDeviceInterfaceDetailW(DeviceInfoSet,
                    &InterfaceData, DetailData, RequiredSize, 0,
                    &DeviceInfoData) &&
                    SetupDiGetDeviceInstanceIdW(DeviceInfoSet,
                        &DeviceInfoData, InstanceId, ARRAYSIZE(InstanceId), 0) &&
                    0 == _wcsnicmp(InstanceId,
                        L"SCSI\\Disk&Ven_WinSpd", 20))
                {
                    if (ARRAYSIZE(State->DiskPath) >
                        wcslen(DetailData->DevicePath))
                    {
                        /* The test requires no other present WinSpd LU, so
                         * this interface belongs to the unit just provisioned. */
                        wcscpy_s(State->DiskPath,
                            ARRAYSIZE(State->DiskPath),
                            DetailData->DevicePath);
                        Found = TRUE;
                    }
                }
                free(DetailData);
                if (Found)
                    break;
            }
            SetupDiDestroyDeviceInfoList(DeviceInfoSet);
        }
        if (L'\0' != State->DiskPath[0])
            return TRUE;

        /* Some Storport virtual disks become visible through PhysicalDrive
         * before SetupAPI reports their disk interface. Scan that namespace
         * as a fallback while PnP finishes publishing the interface. */
    for (DWORD Drive = 0; Drive < 64; Drive++)
        {
            if (0 > _snwprintf_s(CandidatePath,
                ARRAYSIZE(CandidatePath), _TRUNCATE,
                L"\\\\.\\PhysicalDrive%lu", (unsigned long)Drive))
                continue;
            if (ring_test_disk_has_serial(CandidatePath, ExpectedSerial))
            {
                wcscpy_s(State->DiskPath, ARRAYSIZE(State->DiskPath),
                    CandidatePath);
                return TRUE;
            }
    }
    return FALSE;
}

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
    BOOLEAN IsTestRead;

    UNREFERENCED_PARAMETER(Flush);
    InterlockedIncrement(&State->ReadCallbacks);
    IsTestRead = RingTestBlockAddress <= BlockAddress &&
        BlockAddress < RingTestBlockAddress + 16;
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
    ReadCall = IsTestRead ? InterlockedIncrement(&State->TestReadCalls) : 0;
    if (IsTestRead &&
        0 != InterlockedCompareExchange(&State->HoldReads, 0, 0))
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

    AsyncMode = IsTestRead ?
        InterlockedCompareExchange(&State->AsyncMode, 0, 0) :
        RingTestAsyncNone;
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
    State->Guid = *Guid;
    State->QueueDepth = QueueDepth;
    State->ReleaseReads = CreateEventW(0, TRUE, FALSE, 0);
    State->TwoReadsStarted = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != State->ReleaseReads);
    ASSERT(0 != State->TwoReadsStarted);

    memset(&Params, 0, sizeof Params);
    Params.Guid = *Guid;
    memcpy(Params.ProductId, "RingTest", sizeof "RingTest");
    Params.BlockCount = 64;
    Params.BlockLength = 512;
    Params.MaxTransferLength = 4096;
    Error = SpdStorageUnitCreate(0, &Params, &RingTestInterface,
        &State->StorageUnit);
    ASSERT(ERROR_SUCCESS == Error);
    State->StorageUnit->UserContext = State;
    SpdStorageUnitSetDebugLog(State->StorageUnit,
        1 << SpdIoctlTransactReadKind);
    if (INVALID_HANDLE_VALUE == RingTestDebugLogHandle)
    {
        RingTestDebugLogHandle = CreateFileW(
            L"artifacts\\ring-dispatcher.log", GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, 0, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, 0);
        if (INVALID_HANDLE_VALUE != RingTestDebugLogHandle)
            SpdDebugLogSetHandle(RingTestDebugLogHandle);
    }

    memset(&RingParams, 0, sizeof RingParams);
    RingParams.Version = SPD_RING_VERSION_1;
    RingParams.QueueDepth = QueueDepth;
    RingParams.BufferSize = 4096;
    Error = SpdStorageUnitOpenSharedRing(State->StorageUnit, &RingParams);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(0 != State->StorageUnit->SharedRingHeader);
    ASSERT(QueueDepth == State->StorageUnit->SharedRingHeader->QueueDepth);
    ASSERT(4096 == State->StorageUnit->SharedRingHeader->BufferSize);

}

static VOID ring_test_start(RING_TEST_STATE *State, ULONG WorkerCount)
{
    DWORD Error = SpdStorageUnitStartDispatcher(State->StorageUnit,
        WorkerCount);
    ASSERT(ERROR_SUCCESS == Error);

    ULONGLONG Deadline = GetTickCount64() + 15000;
    while (!ring_test_find_disk(State, &State->Guid) &&
        GetTickCount64() < Deadline)
        Sleep(100);
    if (L'\0' == State->DiskPath[0])
    {
        DWORD DispatcherError = ERROR_SUCCESS;
        SpdStorageUnitGetDispatcherError(State->StorageUnit,
            &DispatcherError);
        tlib_printf("ring discovery timeout: dispatcher=%lu callbackFailure=%ld readCallbacks=%ld testReads=%ld\n",
            (unsigned long)DispatcherError,
            InterlockedCompareExchange(&State->Failure, 0, 0),
            InterlockedCompareExchange(&State->ReadCallbacks, 0, 0),
            InterlockedCompareExchange(&State->TestReadCalls, 0, 0));
    }
    ASSERT(L'\0' != State->DiskPath[0]);
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
        if (ERROR_SUCCESS != DispatcherError)
            tlib_printf("ring dispatcher error=%lu\n",
                (unsigned long)DispatcherError);
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

static DWORD ring_test_disk_read(RING_TEST_STATE *State)
{
    LARGE_INTEGER Offset;
    UINT8 DataBuffer[RingTestDataLength];
    DWORD BytesRead;
    HANDLE DiskHandle;
    DWORD Error;

    DiskHandle = CreateFileW(State->DiskPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, 0);
    if (INVALID_HANDLE_VALUE == DiskHandle)
        return GetLastError();

    Offset.QuadPart = (LONGLONG)RingTestBlockAddress * RingTestDataLength;
    if (!SetFilePointerEx(DiskHandle, Offset, 0, FILE_BEGIN))
        Error = GetLastError();
    else if (!ReadFile(DiskHandle, DataBuffer, sizeof DataBuffer,
        &BytesRead, 0))
        Error = GetLastError();
    else
        Error = sizeof DataBuffer == BytesRead ? ERROR_SUCCESS :
            ERROR_INVALID_DATA;
    CloseHandle(DiskHandle);
    return Error;
}

static DWORD WINAPI ring_test_disk_read_thread(PVOID Data)
{
    RING_TEST_READ_THREAD *ReadThread = Data;
    ReadThread->Error = ring_test_disk_read(ReadThread->State);
    SetEvent(ReadThread->Finished);
    return ReadThread->Error;
}

static DWORD ring_test_complete_one_manually(RING_TEST_STATE *State,
    PBOOLEAN PTestRead)
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

    *PTestRead = FALSE;
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
    *PTestRead = SpdIoctlTransactReadKind == Request->Request.Kind &&
        RingTestBlockAddress <= Request->Request.Op.Read.BlockAddress &&
        Request->Request.Op.Read.BlockAddress < RingTestBlockAddress + 16;

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

static VOID ring_test_wait_for_disk_manual(RING_TEST_STATE *State)
{
    ULONGLONG Deadline = GetTickCount64() + 15000;

    while (!ring_test_find_disk(State, &State->Guid) &&
        GetTickCount64() < Deadline)
    {
        SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
        BOOLEAN TestRead;
        DWORD Error;

        memset(&WaitParams, 0, sizeof WaitParams);
        WaitParams.MaxRequests = 1;
        Error = SpdIoctlRingWait(State->StorageUnit->Handle,
            State->StorageUnit->Btl, &WaitParams);
        ASSERT(ERROR_SUCCESS == Error);
        if (0 == WaitParams.Produced)
            continue;
        ASSERT(1 == WaitParams.Produced);
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(
            State, &TestRead));
        ASSERT(!TestRead);
    }

    ASSERT(L'\0' != State->DiskPath[0]);
}

static void ioctl_ring_lifecycle_test(void)
{
    RING_TEST_STATE State;

    ring_test_create(&State, &RingTestGuidLifecycle, 2);
    ASSERT(0 != State.StorageUnit->SharedRingAddress);
    ASSERT(0 < State.StorageUnit->SharedRingSize);
    SpdStorageUnitCloseSharedRing(State.StorageUnit);
    ASSERT(0 == State.StorageUnit->SharedRingAddress);
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
        ASSERT(ERROR_SUCCESS == ring_test_disk_read(&State));
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
    ASSERT(ERROR_SUCCESS == ring_test_disk_read(&State));
    InterlockedExchange(&State.AsyncMode, RingTestAsyncDeferred);
    ASSERT(ERROR_SUCCESS == ring_test_disk_read(&State));
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
            ring_test_disk_read_thread, &Reads[I], 0, 0);
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
    UINT32 CompletedTestReads = 0;

    ring_test_create(&State, &RingTestGuidWaitCredit, 4);
    ring_test_wait_for_disk_manual(&State);
    memset(Reads, 0, sizeof Reads);
    memset(Threads, 0, sizeof Threads);
    for (UINT32 I = 0; I < ARRAYSIZE(Reads); I++)
    {
        Reads[I].State = &State;
        Reads[I].Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Reads[I].Finished);
        Threads[I] = CreateThread(0, 0,
            ring_test_disk_read_thread, &Reads[I], 0, 0);
        ASSERT(0 != Threads[I]);
    }

    while (CompletedTestReads < ARRAYSIZE(Reads))
    {
        SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
        BOOLEAN TestRead;
        memset(&WaitParams, 0, sizeof WaitParams);
        WaitParams.MaxRequests = 1;
        ASSERT(ERROR_SUCCESS == SpdIoctlRingWait(
            State.StorageUnit->Handle, State.StorageUnit->Btl,
            &WaitParams));
        ASSERT(1 == WaitParams.Produced);
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(
            &State, &TestRead));
        if (TestRead)
            CompletedTestReads++;
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
