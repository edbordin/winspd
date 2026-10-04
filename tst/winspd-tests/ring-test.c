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
    HANDLE ReleaseSlowRead;
    HANDLE SlowReadStarted;
    SRWLOCK AsyncLock;
    SRWLOCK BufferLock;
    HANDLE AsyncThreads[16];
    UINT32 ObservedBufferIds[4];
    UINT32 ObservedBufferCount;
    UINT32 LastBufferId;
    LONG AsyncThreadCount;
    LONG AsyncMode;
    LONG HoldReads;
    LONG ReadCallbacks;
    LONG TestReadCalls;
    LONG WriteCallbacks;
    LONG Failure;
    UINT32 QueueDepth;
    UINT64 SlowReadBlockAddress;
    UINT64 LoggedPhysicalDriveMask;
    DWORD ExpectedDispatcherError;
} RING_TEST_STATE;

typedef struct _RING_TEST_ASYNC_RESPONSE
{
    RING_TEST_STATE *State;
    SPD_IOCTL_TRANSACT_RSP Response;
    HANDLE SentEvent;
    BOOLEAN Early;
    BOOLEAN Duplicate;
} RING_TEST_ASYNC_RESPONSE;

typedef struct _RING_TEST_READ_THREAD
{
    RING_TEST_STATE *State;
    HANDLE Finished;
    UINT64 BlockAddress;
    DWORD Error;
} RING_TEST_READ_THREAD;

enum
{
    RingTestAsyncNone,
    RingTestAsyncEarly,
    RingTestAsyncDeferred,
    RingTestAsyncDuplicate,
    RingTestAsyncEarlyTrue
};

static const GUID RingTestDiskInterfaceGuid =
    { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };
static HANDLE RingTestDebugLogHandle = INVALID_HANDLE_VALUE;

enum
{
    RingTestBlockAddress = 32,
    RingTestDataLength = 512
};

static BOOL ring_test_disk_has_serial(RING_TEST_STATE *State, DWORD Drive,
    PWSTR DiskPath, const char *ExpectedSerial)
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
    char Serial[256] = "<unavailable>";

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
            char *SerialValue = (char *)DescriptorBuffer.Bytes +
                Descriptor->SerialNumberOffset;
            size_t SerialLength = strnlen(SerialValue,
                BytesReturned - Descriptor->SerialNumberOffset);
            while (0 < SerialLength && ' ' == SerialValue[SerialLength - 1])
                SerialLength--;
            size_t CopyLength = min(SerialLength, sizeof Serial - 1);
            memcpy(Serial, SerialValue, CopyLength);
            Serial[CopyLength] = '\0';
            Result = 0 == _stricmp(Serial, ExpectedSerial);
        }
    }

    CloseHandle(DiskHandle);
    if (Drive < 64 &&
        0 == (State->LoggedPhysicalDriveMask & ((UINT64)1 << Drive)))
    {
        State->LoggedPhysicalDriveMask |= (UINT64)1 << Drive;
        tlib_printf("physical drive %lu opened, path=%ls serial=%s expected=%s\n",
            (unsigned long)Drive, DiskPath, Serial, ExpectedSerial);
    }
    return Result;
}

static BOOL ring_test_find_disk(RING_TEST_STATE *State, const GUID *Guid,
    BOOLEAN InterfaceOnly)
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
                BOOL DetailSuccess = SetupDiGetDeviceInterfaceDetailW(DeviceInfoSet,
                    &InterfaceData, DetailData, RequiredSize, 0,
                    &DeviceInfoData);
                BOOL InstanceSuccess = DetailSuccess &&
                    SetupDiGetDeviceInstanceIdW(DeviceInfoSet,
                        &DeviceInfoData, InstanceId, ARRAYSIZE(InstanceId), 0);
                if (InstanceSuccess &&
                    0 == _wcsnicmp(InstanceId, L"SCSI\\Disk&Ven_WinSpd", 20))
                {
                    tlib_printf("ring candidate: instance=%ls path=%ls\n",
                        InstanceId, DetailData->DevicePath);
                    if (L'\0' == State->DiskPath[0] &&
                        ARRAYSIZE(State->DiskPath) >
                        wcslen(DetailData->DevicePath))
                    {
                        /* The test requires no other present WinSpd LU, so
                         * this interface belongs to the unit just provisioned. */
                        wcscpy_s(State->DiskPath,
                            ARRAYSIZE(State->DiskPath),
                            DetailData->DevicePath);
                    }
                }
                free(DetailData);
            }
            SetupDiDestroyDeviceInfoList(DeviceInfoSet);
        }
        if (L'\0' != State->DiskPath[0])
            return TRUE;

        /* Manual ring tests must not make synchronous storage queries while
         * they are the only consumer of ring requests. */
        if (InterfaceOnly)
            return FALSE;

        /* Some Storport virtual disks become visible through PhysicalDrive
         * before SetupAPI reports their disk interface. Scan that namespace
         * as a fallback while PnP finishes publishing the interface. */
    for (DWORD Drive = 0; Drive < 64; Drive++)
        {
            if (0 > _snwprintf_s(CandidatePath,
                ARRAYSIZE(CandidatePath), _TRUNCATE,
                L"\\\\.\\PhysicalDrive%lu", (unsigned long)Drive))
                continue;
            if (ring_test_disk_has_serial(State, Drive, CandidatePath,
                    ExpectedSerial))
            {
                wcscpy_s(State->DiskPath, ARRAYSIZE(State->DiskPath),
                    CandidatePath);
                return TRUE;
            }
    }
    return FALSE;
}

static VOID ring_test_log_discovery_timeout(RING_TEST_STATE *State)
{
    DWORD DispatcherError = ERROR_SUCCESS;
    SPD_RING_HEADER *Header = State->StorageUnit->SharedRingHeader;

    SpdStorageUnitGetDispatcherError(State->StorageUnit,
        &DispatcherError);
    tlib_printf("ring discovery timeout: dispatcher=%lu callbackFailure=%ld readCallbacks=%ld testReads=%ld writeCallbacks=%ld\n",
        (unsigned long)DispatcherError,
        InterlockedCompareExchange(&State->Failure, 0, 0),
        InterlockedCompareExchange(&State->ReadCallbacks, 0, 0),
        InterlockedCompareExchange(&State->TestReadCalls, 0, 0),
        InterlockedCompareExchange(&State->WriteCallbacks, 0, 0));

    if (0 != Header)
    {
        UINT32 RequestHead =
            SpdRingLoadAcquire32(&Header->RequestHead.Value);
        UINT32 RequestTail =
            SpdRingLoadAcquire32(&Header->RequestTail.Value);
        UINT32 CompletionHead =
            SpdRingLoadAcquire32(&Header->CompletionHead.Value);
        UINT32 CompletionTail =
            SpdRingLoadAcquire32(&Header->CompletionTail.Value);

        tlib_printf("ring cursors req=%lu/%lu count=%lu cq=%lu/%lu count=%lu\n",
            (unsigned long)RequestHead, (unsigned long)RequestTail,
            (unsigned long)(RequestTail - RequestHead),
            (unsigned long)CompletionHead, (unsigned long)CompletionTail,
            (unsigned long)(CompletionTail - CompletionHead));
    }
}

static const GUID RingTestGuidLifecycle =
    { 0x51aeb043, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x01 } };
static const GUID RingTestGuidManualBulk =
    { 0x51aeb04f, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x0d } };
static const GUID RingTestGuidBatch =
    { 0x51aeb04a, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x0a } };
static const GUID RingTestGuidWorkerStop =
    { 0x51aeb04b, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x0b } };
static const GUID RingTestGuidWaitStop =
    { 0x51aeb04c, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x0c } };
static const GUID RingTestGuidWraparound =
    { 0x51aeb044, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x02 } };
static const GUID RingTestGuidAsync =
    { 0x51aeb045, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x03 } };
static const GUID RingTestGuidSaturation =
    { 0x51aeb046, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x04 } };
static const GUID RingTestGuidWaitCredit =
    { 0x51aeb047, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x05 } };
static const GUID RingTestGuidLifo =
    { 0x51aeb048, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x06 } };
static const GUID RingTestGuidOutOfOrder =
    { 0x51aeb049, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x07 } };
static const GUID RingTestGuidReadWrite =
    { 0x51aeb04a, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x08 } };
static const GUID RingTestGuidOwnerHint =
    { 0x51aeb04b, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x09 } };
static const GUID RingTestGuidOpenValidation =
    { 0x51aeb04c, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x0a } };
static const GUID RingTestGuidDuplicateResponse =
    { 0x51aeb04d, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x0b } };
static const GUID RingTestGuidEarlyTrue =
    { 0x51aeb04e, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x0c } };

static DWORD WINAPI ring_test_async_response_thread(PVOID Data)
{
    RING_TEST_ASYNC_RESPONSE *Work = Data;

    if (!Work->Early)
        Sleep(20);
    SpdStorageUnitSendResponse(Work->State->StorageUnit,
        &Work->Response, 0);
    if (Work->Duplicate)
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
    else if (0 == OperationContext->Request->Hint)
        InterlockedExchange(&State->Failure, 1);
    ReadCall = IsTestRead ? InterlockedIncrement(&State->TestReadCalls) : 0;
    if (IsTestRead && 0 != Buffer)
    {
        SPD_RING_HEADER *Header = StorageUnit->SharedRingHeader;
        UINT_PTR BufferAddress = (UINT_PTR)Buffer;
        UINT_PTR PoolAddress =
            (UINT_PTR)StorageUnit->SharedRingAddress +
            Header->BufferOffset;
        SIZE_T PoolBytes =
            (SIZE_T)Header->BufferCount * Header->BufferSize;

        if (BufferAddress < PoolAddress ||
            BufferAddress - PoolAddress >= PoolBytes)
            InterlockedExchange(&State->Failure, 1);
        else
        {
            SIZE_T BufferOffset = BufferAddress - PoolAddress;
            UINT32 BufferId = (UINT32)
                (BufferOffset / Header->BufferSize);
            if (0 != BufferOffset % Header->BufferSize)
                InterlockedExchange(&State->Failure, 1);
            AcquireSRWLockExclusive(&State->BufferLock);
            if (ReadCall <= ARRAYSIZE(State->ObservedBufferIds))
            {
                State->ObservedBufferIds[ReadCall - 1] = BufferId;
                if ((UINT32)ReadCall > State->ObservedBufferCount)
                    State->ObservedBufferCount = (UINT32)ReadCall;
            }
            ReleaseSRWLockExclusive(&State->BufferLock);
        }
    }
    if (State->SlowReadBlockAddress == BlockAddress)
    {
        SetEvent(State->SlowReadStarted);
        if (WAIT_OBJECT_0 != WaitForSingleObject(
                State->ReleaseSlowRead, 10000))
        {
            InterlockedExchange(&State->Failure, 1);
            return TRUE;
        }
    }
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
        if (RingTestAsyncEarly == AsyncMode ||
            RingTestAsyncEarlyTrue == AsyncMode)
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
        Work->Early = RingTestAsyncEarly == AsyncMode ||
            RingTestAsyncEarlyTrue == AsyncMode;
        Work->Duplicate = RingTestAsyncDuplicate == AsyncMode;
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
        return RingTestAsyncEarlyTrue != AsyncMode;
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

static BOOLEAN ring_test_write(SPD_STORAGE_UNIT *StorageUnit,
    PVOID Buffer, UINT64 BlockAddress, UINT32 BlockCount, BOOLEAN Flush,
    SPD_STORAGE_UNIT_STATUS *Status)
{
    RING_TEST_STATE *State = StorageUnit->UserContext;
    SIZE_T Length = (SIZE_T)BlockCount *
        StorageUnit->StorageUnitParams.BlockLength;

    UNREFERENCED_PARAMETER(Flush);
    memset(Status, 0, sizeof *Status);
    if (RingTestBlockAddress <= BlockAddress &&
        BlockAddress < RingTestBlockAddress + 16)
    {
        InterlockedIncrement(&State->WriteCallbacks);
        for (SIZE_T I = 0; Length > I; I++)
            if (0x3c != ((PUINT8)Buffer)[I])
            {
                InterlockedExchange(&State->Failure, 1);
                break;
            }
    }
    return TRUE;
}

static const SPD_STORAGE_UNIT_INTERFACE RingTestInterface =
{
    ring_test_read,
    ring_test_write,
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
    InitializeSRWLock(&State->BufferLock);
    State->Guid = *Guid;
    State->QueueDepth = QueueDepth;
    State->SlowReadBlockAddress = (UINT64)-1;
    State->ReleaseReads = CreateEventW(0, TRUE, FALSE, 0);
    State->TwoReadsStarted = CreateEventW(0, TRUE, FALSE, 0);
    State->ReleaseSlowRead = CreateEventW(0, TRUE, FALSE, 0);
    State->SlowReadStarted = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != State->ReleaseReads);
    ASSERT(0 != State->TwoReadsStarted);
    ASSERT(0 != State->ReleaseSlowRead);
    ASSERT(0 != State->SlowReadStarted);

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
    RingParams.Version = SPD_RING_VERSION_3;
    RingParams.QueueDepth = QueueDepth;
    RingParams.BufferSize = 4096;
    Error = SpdStorageUnitOpenSharedRing(State->StorageUnit, &RingParams);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(0 != State->StorageUnit->SharedRingHeader);
    ASSERT(SPD_RING_VERSION_3 ==
        State->StorageUnit->SharedRingHeader->Version);
    ASSERT(sizeof(SPD_RING_HEADER) ==
        State->StorageUnit->SharedRingHeader->HeaderSize);
    ASSERT(QueueDepth == State->StorageUnit->SharedRingHeader->QueueDepth);
    ASSERT(QueueDepth == State->StorageUnit->SharedRingHeader->BufferCount);
    ASSERT(4096 == State->StorageUnit->SharedRingHeader->BufferSize);
    ASSERT(0 == State->StorageUnit->SharedRingHeader->RequestOffset %
        SPD_RING_CACHE_LINE_SIZE);
    ASSERT(0 == State->StorageUnit->SharedRingHeader->CompletionOffset %
        SPD_RING_CACHE_LINE_SIZE);
    ASSERT(0 == State->StorageUnit->SharedRingHeader->BufferOffset % 4096);
    ASSERT(State->StorageUnit->SharedRingSize <=
        SPD_RING_MAX_SECTION_BYTES);

}

static VOID ring_test_start(RING_TEST_STATE *State, ULONG WorkerCount)
{
    DWORD Error = SpdStorageUnitStartDispatcher(State->StorageUnit,
        WorkerCount);
    ASSERT(ERROR_SUCCESS == Error);

    ULONGLONG Deadline = GetTickCount64() + 15000;
    while (!ring_test_find_disk(State, &State->Guid, FALSE) &&
        GetTickCount64() < Deadline)
        Sleep(100);
    if (L'\0' == State->DiskPath[0])
        ring_test_log_discovery_timeout(State);
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
        ASSERT(State->ExpectedDispatcherError == DispatcherError);
        ASSERT(0 == InterlockedCompareExchange(&State->Failure, 0, 0));
        SpdStorageUnitDelete(State->StorageUnit);
        State->StorageUnit = 0;
    }
    if (0 != State->TwoReadsStarted)
        CloseHandle(State->TwoReadsStarted);
    if (0 != State->ReleaseReads)
        CloseHandle(State->ReleaseReads);
    if (0 != State->SlowReadStarted)
        CloseHandle(State->SlowReadStarted);
    if (0 != State->ReleaseSlowRead)
        CloseHandle(State->ReleaseSlowRead);
}

static DWORD ring_test_disk_read_at(
    RING_TEST_STATE *State,
    UINT64 BlockAddress)
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

    Offset.QuadPart = (LONGLONG)BlockAddress * RingTestDataLength;
    if (!SetFilePointerEx(DiskHandle, Offset, 0, FILE_BEGIN))
        Error = GetLastError();
    else if (!ReadFile(DiskHandle, DataBuffer, sizeof DataBuffer,
        &BytesRead, 0))
        Error = GetLastError();
    else
        Error = sizeof DataBuffer == BytesRead ? ERROR_SUCCESS :
            ERROR_INVALID_DATA;
    if (ERROR_SUCCESS == Error &&
        0 != State->StorageUnit->DispatcherThread)
        for (UINT32 I = 0; sizeof DataBuffer > I; I++)
            if (0x5a != DataBuffer[I])
            {
                Error = ERROR_INVALID_DATA;
                break;
            }
    CloseHandle(DiskHandle);
    return Error;
}

static DWORD ring_test_disk_read(RING_TEST_STATE *State)
{
    return ring_test_disk_read_at(State, RingTestBlockAddress);
}

static DWORD ring_test_disk_write(RING_TEST_STATE *State)
{
    LARGE_INTEGER Offset;
    UINT8 DataBuffer[RingTestDataLength];
    DWORD BytesWritten;
    HANDLE DiskHandle;
    DWORD Error;

    memset(DataBuffer, 0x3c, sizeof DataBuffer);
    DiskHandle = CreateFileW(State->DiskPath, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, 0);
    if (INVALID_HANDLE_VALUE == DiskHandle)
        return GetLastError();

    Offset.QuadPart = (LONGLONG)RingTestBlockAddress * RingTestDataLength;
    if (!SetFilePointerEx(DiskHandle, Offset, 0, FILE_BEGIN))
        Error = GetLastError();
    else if (!WriteFile(DiskHandle, DataBuffer, sizeof DataBuffer,
        &BytesWritten, 0))
        Error = GetLastError();
    else
        Error = sizeof DataBuffer == BytesWritten ? ERROR_SUCCESS :
            ERROR_INVALID_DATA;
    CloseHandle(DiskHandle);
    return Error;
}

static DWORD ring_test_disk_read_large(RING_TEST_STATE *State)
{
    const DWORD DataLength = 8192;
    LARGE_INTEGER Offset;
    PUINT8 DataBuffer;
    DWORD BytesRead;
    HANDLE DiskHandle;
    DWORD Error;

    DataBuffer = VirtualAlloc(0, DataLength,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (0 == DataBuffer)
        return GetLastError();
    DiskHandle = CreateFileW(State->DiskPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, 0);
    if (INVALID_HANDLE_VALUE == DiskHandle)
    {
        Error = GetLastError();
        VirtualFree(DataBuffer, 0, MEM_RELEASE);
        return Error;
    }

    Offset.QuadPart = (LONGLONG)RingTestBlockAddress * RingTestDataLength;
    if (!SetFilePointerEx(DiskHandle, Offset, 0, FILE_BEGIN))
        Error = GetLastError();
    else if (!ReadFile(DiskHandle, DataBuffer, DataLength,
        &BytesRead, 0))
        Error = GetLastError();
    else
    {
        Error = DataLength == BytesRead ? ERROR_SUCCESS :
            ERROR_INVALID_DATA;
        for (DWORD I = 0; ERROR_SUCCESS == Error && DataLength > I; I++)
            if (0x5a != DataBuffer[I])
                Error = ERROR_INVALID_DATA;
    }
    CloseHandle(DiskHandle);
    VirtualFree(DataBuffer, 0, MEM_RELEASE);
    return Error;
}

static DWORD WINAPI ring_test_disk_read_thread(PVOID Data)
{
    RING_TEST_READ_THREAD *ReadThread = Data;
    UINT64 BlockAddress = 0 == ReadThread->BlockAddress ?
        RingTestBlockAddress : ReadThread->BlockAddress;
    ReadThread->Error = ring_test_disk_read_at(
        ReadThread->State, BlockAddress);
    SetEvent(ReadThread->Finished);
    return ReadThread->Error;
}

static DWORD ring_test_complete_one_manually(RING_TEST_STATE *State,
    PBOOLEAN PTestRead)
{
    SPD_RING_HEADER *Header = State->StorageUnit->SharedRingHeader;
    UINT32 RequestHead =
        SpdRingLoadAcquire32(&Header->RequestHead.Value);
    UINT32 RequestTail =
        SpdRingLoadAcquire32(&Header->RequestTail.Value);
    UINT32 CompletionHead =
        SpdRingLoadAcquire32(&Header->CompletionHead.Value);
    UINT32 CompletionTail =
        SpdRingLoadAcquire32(&Header->CompletionTail.Value);
    SPD_RING_REQUEST *Request;
    SPD_RING_COMPLETION *Completion;
    SPD_IOCTL_RING_KICK_PARAMS KickParams;

    *PTestRead = FALSE;
    if (RequestTail - RequestHead != 1 ||
        CompletionTail != CompletionHead)
        return ERROR_INVALID_DATA;

    Request = (SPD_RING_REQUEST *)
        ((PUINT8)State->StorageUnit->SharedRingAddress +
        Header->RequestOffset +
        (RequestHead & (Header->QueueDepth - 1)) * sizeof *Request);
    Completion = (SPD_RING_COMPLETION *)
        ((PUINT8)State->StorageUnit->SharedRingAddress +
        Header->CompletionOffset +
        (CompletionTail & (Header->QueueDepth - 1)) * sizeof *Completion);
    State->LastBufferId = Request->Data.BufferId;
    memset(&Completion->Response, 0, sizeof Completion->Response);
    Completion->Response.Hint = Request->Request.Hint;
    Completion->Response.Kind = Request->Request.Kind;
    Completion->Data = Request->Data;
    if (Request->Data.BufferId != SPD_RING_NO_BUFFER)
        memset((PUINT8)State->StorageUnit->SharedRingAddress +
            Header->BufferOffset +
            (SIZE_T)Request->Data.BufferId * Header->BufferSize +
            Request->Data.Offset, 0, Request->Data.Length);
    *PTestRead = SpdIoctlTransactReadKind == Request->Request.Kind &&
        RingTestBlockAddress <= Request->Request.Op.Read.BlockAddress &&
        Request->Request.Op.Read.BlockAddress < RingTestBlockAddress + 16;

    SpdRingStoreRelease32(&Header->RequestHead.Value,
        RequestHead + 1);
    SpdRingStoreRelease32(&Header->CompletionTail.Value,
        CompletionTail + 1);
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

    while (!ring_test_find_disk(State, &State->Guid, TRUE) &&
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

    if (L'\0' == State->DiskPath[0])
        ring_test_log_discovery_timeout(State);
    ASSERT(L'\0' != State->DiskPath[0]);
}

static DWORD ring_test_manual_complete_until_bulk_io(
    RING_TEST_STATE *State,
    UINT8 ExpectedKind,
    UINT64 ExpectedBlockAddress,
    const UINT8 *ExpectedWriteData,
    UINT32 ExpectedLength,
    UINT8 ReadFill)
{
    SPD_RING_HEADER *Header = State->StorageUnit->SharedRingHeader;
    SPD_RING_REQUEST *RequestRing = (SPD_RING_REQUEST *)
        ((PUINT8)State->StorageUnit->SharedRingAddress +
        Header->RequestOffset);
    SPD_RING_COMPLETION *CompletionRing = (SPD_RING_COMPLETION *)
        ((PUINT8)State->StorageUnit->SharedRingAddress +
        Header->CompletionOffset);

    for (;;)
    {
        SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
        UINT32 RequestHead;
        UINT32 RequestTail;
        UINT32 CompletionHead;
        UINT32 CompletionTail;
        SPD_RING_REQUEST *Request;
        SPD_RING_COMPLETION *Completion;
        PVOID Buffer = 0;
        UINT64 BlockAddress = 0;
        UINT32 BlockCount = 0;
        BOOLEAN IsTarget = FALSE;
        SPD_IOCTL_RING_KICK_PARAMS KickParams;
        DWORD Error;

        memset(&WaitParams, 0, sizeof WaitParams);
        WaitParams.MaxRequests = 1;
        Error = SpdIoctlRingWait(State->StorageUnit->Handle,
            State->StorageUnit->Btl, &WaitParams);
        if (ERROR_SUCCESS != Error)
            return Error;
        if (1 != WaitParams.Produced)
            return ERROR_INVALID_DATA;

        RequestHead =
            SpdRingLoadAcquire32(&Header->RequestHead.Value);
        RequestTail =
            SpdRingLoadAcquire32(&Header->RequestTail.Value);
        CompletionHead =
            SpdRingLoadAcquire32(&Header->CompletionHead.Value);
        CompletionTail =
            SpdRingLoadAcquire32(&Header->CompletionTail.Value);
        if (1 != RequestTail - RequestHead ||
            CompletionTail != CompletionHead)
            return ERROR_INVALID_DATA;

        Request = &RequestRing[
            RequestHead & (Header->QueueDepth - 1)];
        Completion = &CompletionRing[
            CompletionTail & (Header->QueueDepth - 1)];

        switch (Request->Request.Kind)
        {
        case SpdIoctlTransactReadKind:
            BlockAddress = Request->Request.Op.Read.BlockAddress;
            BlockCount = Request->Request.Op.Read.BlockCount;
            break;
        case SpdIoctlTransactWriteKind:
            BlockAddress = Request->Request.Op.Write.BlockAddress;
            BlockCount = Request->Request.Op.Write.BlockCount;
            break;
        case SpdIoctlTransactFlushKind:
            BlockAddress = Request->Request.Op.Flush.BlockAddress;
            BlockCount = Request->Request.Op.Flush.BlockCount;
            break;
        default:
            break;
        }

        IsTarget = Request->Request.Kind == ExpectedKind &&
            BlockAddress == ExpectedBlockAddress && 1 == BlockCount;
        if (SPD_RING_NO_BUFFER != Request->Data.BufferId)
        {
            if (Request->Data.BufferId >= Header->BufferCount ||
                0 != Request->Data.Offset ||
                Request->Data.Offset > Header->BufferSize ||
                Request->Data.Length >
                    Header->BufferSize - Request->Data.Offset ||
                0 != Request->Data.Flags)
                return ERROR_INVALID_DATA;

            Buffer = (PUINT8)State->StorageUnit->SharedRingAddress +
                Header->BufferOffset +
                (SIZE_T)Request->Data.BufferId * Header->BufferSize +
                Request->Data.Offset;

            if (IsTarget)
            {
                if (Request->Data.Length != ExpectedLength)
                    return ERROR_INVALID_DATA;
                if (SpdIoctlTransactWriteKind == ExpectedKind)
                {
                    if (0 != memcmp(Buffer, ExpectedWriteData,
                            ExpectedLength))
                        return ERROR_INVALID_DATA;
                }
                else if (SpdIoctlTransactReadKind == ExpectedKind)
                    memset(Buffer, ReadFill, Request->Data.Length);
                else
                    return ERROR_INVALID_PARAMETER;
            }
            else
                memset(Buffer, 0, Request->Data.Length);
        }
        else if (IsTarget)
            return ERROR_INVALID_DATA;

        memset(&Completion->Response, 0,
            sizeof Completion->Response);
        Completion->Response.Hint = Request->Request.Hint;
        Completion->Response.Kind = Request->Request.Kind;
        Completion->Data = Request->Data;

        SpdRingStoreRelease32(&Header->RequestHead.Value,
            RequestHead + 1);
        SpdRingStoreRelease32(&Header->CompletionTail.Value,
            CompletionTail + 1);

        memset(&KickParams, 0, sizeof KickParams);
        Error = SpdIoctlRingKick(State->StorageUnit->Handle,
            State->StorageUnit->Btl, &KickParams);
        if (ERROR_SUCCESS != Error)
            return Error;
        if (1 != KickParams.Consumed)
            return ERROR_INVALID_DATA;
        if (IsTarget)
            return ERROR_SUCCESS;
    }
}

static DWORD ring_test_complete_overlapped_bulk_io(
    RING_TEST_STATE *State,
    HANDLE DiskHandle,
    UINT8 Kind,
    PUINT8 Data,
    UINT8 ReadFill)
{
    OVERLAPPED Overlapped;
    LARGE_INTEGER Offset;
    DWORD Transferred = 0;
    BOOL Success;
    DWORD Error;

    memset(&Overlapped, 0, sizeof Overlapped);
    Overlapped.hEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (0 == Overlapped.hEvent)
        return GetLastError();
    Offset.QuadPart =
        (LONGLONG)RingTestBlockAddress * RingTestDataLength;
    Overlapped.Offset = Offset.LowPart;
    Overlapped.OffsetHigh = Offset.HighPart;

    if (SpdIoctlTransactWriteKind == Kind)
        Success = WriteFile(DiskHandle, Data, RingTestDataLength,
            &Transferred, &Overlapped);
    else
        Success = ReadFile(DiskHandle, Data, RingTestDataLength,
            &Transferred, &Overlapped);

    if (Success)
    {
        CloseHandle(Overlapped.hEvent);
        return ERROR_INVALID_DATA;
    }
    else
    {
        Error = GetLastError();
        if (ERROR_IO_PENDING != Error)
        {
            CloseHandle(Overlapped.hEvent);
            return Error;
        }
    }

    Error = ring_test_manual_complete_until_bulk_io(State, Kind,
        RingTestBlockAddress,
        SpdIoctlTransactWriteKind == Kind ? Data : 0,
        RingTestDataLength, ReadFill);
    if (ERROR_SUCCESS == Error &&
        !GetOverlappedResult(DiskHandle, &Overlapped, &Transferred, TRUE))
        Error = GetLastError();
    if (ERROR_SUCCESS == Error && RingTestDataLength != Transferred)
        Error = ERROR_INVALID_DATA;

    CloseHandle(Overlapped.hEvent);
    return Error;
}

static void ioctl_ring_manual_bulk_visibility_test(void)
{
    RING_TEST_STATE State;
    HANDLE DiskHandle;
    UINT8 WriteData[RingTestDataLength];
    UINT8 ReadData[RingTestDataLength];

    ring_test_create(&State, &RingTestGuidManualBulk, 2);
    ring_test_wait_for_disk_manual(&State);
    DiskHandle = CreateFileW(State.DiskPath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, 0);
    ASSERT(INVALID_HANDLE_VALUE != DiskHandle);

    memset(WriteData, 0x3c, sizeof WriteData);
    ASSERT(ERROR_SUCCESS == ring_test_complete_overlapped_bulk_io(
        &State, DiskHandle, SpdIoctlTransactWriteKind, WriteData, 0));

    memset(ReadData, 0, sizeof ReadData);
    ASSERT(ERROR_SUCCESS == ring_test_complete_overlapped_bulk_io(
        &State, DiskHandle, SpdIoctlTransactReadKind, ReadData, 0x5a));
    for (UINT32 I = 0; sizeof ReadData > I; I++)
        ASSERT(0x5a == ReadData[I]);

    CloseHandle(DiskHandle);
    ring_test_destroy(&State);
}

static void ioctl_ring_lifecycle_test(void)
{
    RING_TEST_STATE State;
    SPD_IOCTL_RING_OPEN_PARAMS RingParams;
    DWORD Error;

    ring_test_create(&State, &RingTestGuidLifecycle, 2);
    ASSERT(0 != State.StorageUnit->SharedRingAddress);
    ASSERT(0 < State.StorageUnit->SharedRingSize);
    Error = SpdIoctlSetTransactProcessId(State.StorageUnit->Handle,
        State.StorageUnit->Btl, GetCurrentProcessId() + 1);
    ASSERT(ERROR_BUSY == Error);
    SpdStorageUnitCloseSharedRing(State.StorageUnit);
    ASSERT(0 == State.StorageUnit->SharedRingAddress);
    memset(&RingParams, 0, sizeof RingParams);
    RingParams.Version = SPD_RING_VERSION_3;
    RingParams.QueueDepth = 2;
    RingParams.BufferSize = 4096;
    Error = SpdStorageUnitOpenSharedRing(State.StorageUnit, &RingParams);
    ASSERT(ERROR_BUSY == Error);
    ring_test_destroy(&State);
}

static void ioctl_ring_open_layout_validation_test(void)
{
    SPD_STORAGE_UNIT_PARAMS Params;
    SPD_IOCTL_RING_OPEN_PARAMS RingParams;
    SPD_STORAGE_UNIT *StorageUnit = 0;
    DWORD Error;

    memset(&Params, 0, sizeof Params);
    Params.Guid = RingTestGuidOpenValidation;
    memcpy(Params.ProductId, "RingTest", sizeof "RingTest");
    Params.BlockCount = 64;
    Params.BlockLength = 512;
    Params.MaxTransferLength = 4096;
    Error = SpdStorageUnitCreate(0, &Params, &RingTestInterface,
        &StorageUnit);
    ASSERT(ERROR_SUCCESS == Error);

    memset(&RingParams, 0, sizeof RingParams);
    RingParams.Version = SPD_RING_VERSION_3;
    RingParams.QueueDepth = 3;
    RingParams.BufferSize = 4096;
    ASSERT(ERROR_INVALID_PARAMETER ==
        SpdStorageUnitOpenSharedRing(StorageUnit, &RingParams));

    RingParams.QueueDepth = SPD_RING_MAX_QUEUE_DEPTH;
    RingParams.BufferSize = 1024 * 1024;
    ASSERT(ERROR_INVALID_PARAMETER ==
        SpdStorageUnitOpenSharedRing(StorageUnit, &RingParams));

    RingParams.QueueDepth = 2;
    RingParams.BufferSize = 2048;
    ASSERT(ERROR_INVALID_PARAMETER ==
        SpdStorageUnitOpenSharedRing(StorageUnit, &RingParams));

    RingParams.BufferSize = 4096;
    RingParams.Flags = 1;
    ASSERT(ERROR_INVALID_PARAMETER ==
        SpdStorageUnitOpenSharedRing(StorageUnit, &RingParams));
    RingParams.Flags = 0;
    RingParams.Reserved = 1;
    ASSERT(ERROR_INVALID_PARAMETER ==
        SpdStorageUnitOpenSharedRing(StorageUnit, &RingParams));
    RingParams.Reserved = 0;
    ASSERT(ERROR_SUCCESS ==
        SpdStorageUnitOpenSharedRing(StorageUnit, &RingParams));
    SPD_RING_HEADER *Header = StorageUnit->SharedRingHeader;
    UINT32 QueueDepth = Header->QueueDepth;
    UINT32 BufferCount = Header->BufferCount;
    UINT32 BufferSize = Header->BufferSize;
    UINT32 RequestOffset = Header->RequestOffset;
    Header->QueueDepth = 4;
    Header->BufferCount = 4;
    Header->BufferSize = 2048;
    ASSERT(ERROR_INVALID_PARAMETER ==
        SpdStorageUnitStartDispatcher(StorageUnit, 1));
    Header->QueueDepth = QueueDepth;
    Header->BufferCount = BufferCount;
    Header->BufferSize = BufferSize;
    Header->RequestOffset++;
    ASSERT(ERROR_INVALID_PARAMETER ==
        SpdStorageUnitStartDispatcher(StorageUnit, 1));
    Header->RequestOffset = RequestOffset;
    SpdStorageUnitDelete(StorageUnit);
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
    ASSERT(8 <= SpdRingLoadAcquire32(&Header->RequestTail.Value));
    ASSERT(8 <= SpdRingLoadAcquire32(&Header->RequestHead.Value));
    ASSERT(8 <= SpdRingLoadAcquire32(&Header->CompletionTail.Value));
    ASSERT(8 <= SpdRingLoadAcquire32(&Header->CompletionHead.Value));
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

static void ioctl_ring_batch_full_boundary_test(void)
{
    RING_TEST_STATE State;
    SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
    SPD_IOCTL_RING_KICK_PARAMS KickParams;
    SPD_RING_HEADER *Header;
    SPD_RING_REQUEST *RequestRing;
    SPD_RING_COMPLETION *CompletionRing;
    HANDLE DiskHandle;
    UINT8 DataBuffers[2][RingTestDataLength];
    OVERLAPPED Overlapped[2];
    DWORD BytesRead;
    DWORD Error;
    UINT32 RequestHead;
    UINT32 RequestTail;
    UINT32 CompletionHead;
    UINT32 CompletionTail;
    UINT32 BufferIds[2];

    ring_test_create(&State, &RingTestGuidBatch, 2);
    ring_test_wait_for_disk_manual(&State);
    DiskHandle = CreateFileW(State.DiskPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, 0);
    ASSERT(INVALID_HANDLE_VALUE != DiskHandle);
    memset(Overlapped, 0, sizeof Overlapped);
    for (UINT32 I = 0; ARRAYSIZE(Overlapped) > I; I++)
    {
        LARGE_INTEGER Offset;
        Overlapped[I].hEvent = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Overlapped[I].hEvent);
        Offset.QuadPart = (LONGLONG)(RingTestBlockAddress + 8 * I) *
            RingTestDataLength;
        Overlapped[I].Offset = Offset.LowPart;
        Overlapped[I].OffsetHigh = Offset.HighPart;
        if (ReadFile(DiskHandle, DataBuffers[I], RingTestDataLength,
                &BytesRead, &Overlapped[I]))
            Error = ERROR_SUCCESS;
        else
            Error = GetLastError();
        ASSERT(ERROR_IO_PENDING == Error);
    }

    memset(&WaitParams, 0, sizeof WaitParams);
    WaitParams.MaxRequests = 2;
    ASSERT(ERROR_SUCCESS == SpdIoctlRingWait(State.StorageUnit->Handle,
        State.StorageUnit->Btl, &WaitParams));
    ASSERT(2 == WaitParams.Produced);

    Header = State.StorageUnit->SharedRingHeader;
    RequestRing = (SPD_RING_REQUEST *)
        ((PUINT8)State.StorageUnit->SharedRingAddress +
        Header->RequestOffset);
    CompletionRing = (SPD_RING_COMPLETION *)
        ((PUINT8)State.StorageUnit->SharedRingAddress +
        Header->CompletionOffset);
    RequestHead = SpdRingLoadAcquire32(&Header->RequestHead.Value);
    RequestTail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    CompletionHead =
        SpdRingLoadAcquire32(&Header->CompletionHead.Value);
    CompletionTail =
        SpdRingLoadAcquire32(&Header->CompletionTail.Value);
    ASSERT(2 == RequestTail - RequestHead);
    ASSERT(CompletionHead == CompletionTail);
    for (UINT32 I = 0; 2 > I; I++)
    {
        SPD_RING_REQUEST *Request = &RequestRing[
            (RequestHead + I) & (Header->QueueDepth - 1)];
        SPD_RING_COMPLETION *Completion = &CompletionRing[
            (CompletionTail + I) & (Header->QueueDepth - 1)];
        ASSERT(SpdIoctlTransactReadKind == Request->Request.Kind);
        ASSERT(SPD_RING_NO_BUFFER != Request->Data.BufferId);
        BufferIds[I] = Request->Data.BufferId;
        memset((PUINT8)State.StorageUnit->SharedRingAddress +
            Header->BufferOffset +
            (SIZE_T)Request->Data.BufferId * Header->BufferSize +
            Request->Data.Offset, 0x5a, Request->Data.Length);
        memset(&Completion->Response, 0,
            sizeof Completion->Response);
        Completion->Response.Hint = Request->Request.Hint;
        Completion->Response.Kind = Request->Request.Kind;
        Completion->Data = Request->Data;
    }
    ASSERT(BufferIds[0] < Header->BufferCount);
    ASSERT(BufferIds[1] < Header->BufferCount);
    ASSERT(BufferIds[0] != BufferIds[1]);
    SpdRingStoreRelease32(&Header->RequestHead.Value, RequestTail);
    SpdRingStoreRelease32(&Header->CompletionTail.Value,
        CompletionTail + 2);
    ASSERT(2 == SpdRingLoadAcquire32(&Header->CompletionTail.Value) -
        SpdRingLoadAcquire32(&Header->CompletionHead.Value));

    memset(&KickParams, 0, sizeof KickParams);
    ASSERT(ERROR_SUCCESS == SpdIoctlRingKick(State.StorageUnit->Handle,
        State.StorageUnit->Btl, &KickParams));
    ASSERT(2 == KickParams.Consumed);
    for (UINT32 I = 0; 2 > I; I++)
    {
        ASSERT(GetOverlappedResult(DiskHandle, &Overlapped[I],
            &BytesRead, TRUE));
        ASSERT(RingTestDataLength == BytesRead);
        for (UINT32 J = 0; RingTestDataLength > J; J++)
            ASSERT(0x5a == DataBuffers[I][J]);
        CloseHandle(Overlapped[I].hEvent);
    }
    CloseHandle(DiskHandle);
    ring_test_destroy(&State);
}

static void ring_test_async_protocol_failure(
    const GUID *Guid,
    LONG AsyncMode)
{
    RING_TEST_STATE State;

    ring_test_create(&State, Guid, 2);
    ring_test_start(&State, 2);
    State.ExpectedDispatcherError = ERROR_INVALID_DATA;
    InterlockedExchange(&State.AsyncMode, AsyncMode);
    (void)ring_test_disk_read(&State);
    ring_test_join_async_threads(&State);
    ring_test_destroy(&State);
}

static void ioctl_ring_duplicate_response_test(void)
{
    ring_test_async_protocol_failure(
        &RingTestGuidDuplicateResponse, RingTestAsyncDuplicate);
}

static void ioctl_ring_callback_true_early_response_test(void)
{
    ring_test_async_protocol_failure(
        &RingTestGuidEarlyTrue, RingTestAsyncEarlyTrue);
}

static void ioctl_ring_lifo_buffer_reuse_test(void)
{
    RING_TEST_STATE State;
    UINT32 FirstBufferId = SPD_RING_NO_BUFFER;

    ring_test_create(&State, &RingTestGuidLifo, 4);
    ring_test_wait_for_disk_manual(&State);
    for (UINT32 I = 0; I < 3; I++)
    {
        RING_TEST_READ_THREAD Read;
        SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
        BOOLEAN TestRead;
        HANDLE Thread;

        memset(&Read, 0, sizeof Read);
        Read.State = &State;
        Read.Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Read.Finished);
        Thread = CreateThread(0, 0,
            ring_test_disk_read_thread, &Read, 0, 0);
        ASSERT(0 != Thread);

        memset(&WaitParams, 0, sizeof WaitParams);
        WaitParams.MaxRequests = 1;
        ASSERT(ERROR_SUCCESS == SpdIoctlRingWait(
            State.StorageUnit->Handle, State.StorageUnit->Btl,
            &WaitParams));
        ASSERT(1 == WaitParams.Produced);
        ASSERT(State.LastBufferId < State.QueueDepth);
        if (0 == I)
            FirstBufferId = State.LastBufferId;
        else
            ASSERT(FirstBufferId == State.LastBufferId);

        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(
            &State, &TestRead));
        ASSERT(TestRead);
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Thread, 10000));
        ASSERT(ERROR_SUCCESS == Read.Error);
        CloseHandle(Thread);
        CloseHandle(Read.Finished);
    }
    ring_test_destroy(&State);
}

static void ioctl_ring_buffer_owner_hint_test(void)
{
    RING_TEST_STATE State;
    RING_TEST_READ_THREAD Read;
    SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
    SPD_IOCTL_RING_KICK_PARAMS KickParams;
    SPD_RING_HEADER *Header;
    SPD_RING_REQUEST *Request;
    SPD_RING_COMPLETION *Completion;
    UINT32 RequestHead;
    UINT32 CompletionTail;
    HANDLE Thread;
    DWORD Error;

    ring_test_create(&State, &RingTestGuidOwnerHint, 2);
    ring_test_wait_for_disk_manual(&State);

    memset(&Read, 0, sizeof Read);
    Read.State = &State;
    Read.Finished = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != Read.Finished);
    Thread = CreateThread(0, 0,
        ring_test_disk_read_thread, &Read, 0, 0);
    ASSERT(0 != Thread);

    memset(&WaitParams, 0, sizeof WaitParams);
    WaitParams.MaxRequests = 1;
    ASSERT(ERROR_SUCCESS == SpdIoctlRingWait(
        State.StorageUnit->Handle, State.StorageUnit->Btl,
        &WaitParams));
    ASSERT(1 == WaitParams.Produced);

    Header = State.StorageUnit->SharedRingHeader;
    RequestHead = SpdRingLoadAcquire32(&Header->RequestHead.Value);
    CompletionTail =
        SpdRingLoadAcquire32(&Header->CompletionTail.Value);
    Request = (SPD_RING_REQUEST *)
        ((PUINT8)State.StorageUnit->SharedRingAddress +
        Header->RequestOffset +
        (RequestHead & (Header->QueueDepth - 1)) * sizeof *Request);
    Completion = (SPD_RING_COMPLETION *)
        ((PUINT8)State.StorageUnit->SharedRingAddress +
        Header->CompletionOffset +
        (CompletionTail & (Header->QueueDepth - 1)) * sizeof *Completion);
    memset(&Completion->Response, 0, sizeof Completion->Response);
    Completion->Response.Hint = Request->Request.Hint ^ 1;
    Completion->Response.Kind = Request->Request.Kind;
    Completion->Data = Request->Data;
    SpdRingStoreRelease32(&Header->RequestHead.Value, RequestHead + 1);
    SpdRingStoreRelease32(&Header->CompletionTail.Value,
        CompletionTail + 1);

    memset(&KickParams, 0, sizeof KickParams);
    Error = SpdIoctlRingKick(State.StorageUnit->Handle,
        State.StorageUnit->Btl, &KickParams);
    ASSERT(ERROR_SUCCESS != Error);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Thread, 10000));
    CloseHandle(Thread);
    CloseHandle(Read.Finished);
    ring_test_destroy(&State);
}

static void ioctl_ring_out_of_order_worker_test(void)
{
    RING_TEST_STATE State;
    RING_TEST_READ_THREAD Slow;
    RING_TEST_READ_THREAD Fast;
    HANDLE SlowThread;
    HANDLE FastThread;

    ring_test_create(&State, &RingTestGuidOutOfOrder, 4);
    ring_test_start(&State, 2);
    State.SlowReadBlockAddress = RingTestBlockAddress;

    memset(&Slow, 0, sizeof Slow);
    Slow.State = &State;
    Slow.BlockAddress = RingTestBlockAddress;
    Slow.Finished = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != Slow.Finished);
    SlowThread = CreateThread(0, 0,
        ring_test_disk_read_thread, &Slow, 0, 0);
    ASSERT(0 != SlowThread);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(
        State.SlowReadStarted, 5000));

    memset(&Fast, 0, sizeof Fast);
    Fast.State = &State;
    Fast.BlockAddress = RingTestBlockAddress + 1;
    Fast.Finished = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != Fast.Finished);
    FastThread = CreateThread(0, 0,
        ring_test_disk_read_thread, &Fast, 0, 0);
    ASSERT(0 != FastThread);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(FastThread, 5000));
    ASSERT(ERROR_SUCCESS == Fast.Error);
    ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Slow.Finished, 0));

    SetEvent(State.ReleaseSlowRead);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(SlowThread, 10000));
    ASSERT(ERROR_SUCCESS == Slow.Error);
    CloseHandle(SlowThread);
    CloseHandle(FastThread);
    CloseHandle(Slow.Finished);
    CloseHandle(Fast.Finished);
    ring_test_destroy(&State);
}

static void ioctl_ring_bulk_visibility_test(void)
{
    RING_TEST_STATE State;

    ring_test_create(&State, &RingTestGuidReadWrite, 4);
    ring_test_start(&State, 2);
    ASSERT(ERROR_SUCCESS == ring_test_disk_write(&State));
    ASSERT(0 < InterlockedCompareExchange(&State.WriteCallbacks, 0, 0));
    ASSERT(ERROR_SUCCESS == ring_test_disk_read(&State));
    ASSERT(ERROR_SUCCESS == ring_test_disk_read_large(&State));
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
    AcquireSRWLockShared(&State.BufferLock);
    ASSERT(2 == State.ObservedBufferCount);
    ASSERT(State.ObservedBufferIds[0] != State.ObservedBufferIds[1]);
    ReleaseSRWLockShared(&State.BufferLock);
    SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
    UINT32 RequestTail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    UINT32 RequestHead = SpdRingLoadAcquire32(&Header->RequestHead.Value);
    UINT32 CompletionTail =
        SpdRingLoadAcquire32(&Header->CompletionTail.Value);
    UINT32 CompletionHead =
        SpdRingLoadAcquire32(&Header->CompletionHead.Value);
    ASSERT(RequestTail - RequestHead <= State.QueueDepth);
    ASSERT(CompletionTail - CompletionHead <= State.QueueDepth);
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

static void ioctl_ring_stop_with_wait_blocked_test(void)
{
    RING_TEST_STATE State;

    ring_test_create(&State, &RingTestGuidWaitStop, 2);
    ring_test_start(&State, 2);
    /* Let the pump enter its next empty RING_WAIT after disk discovery. */
    Sleep(100);
    SpdStorageUnitShutdown(State.StorageUnit);
    ring_test_destroy(&State);
}

static void ioctl_ring_stop_with_workers_active_test(void)
{
    RING_TEST_STATE State;
    RING_TEST_READ_THREAD Reads[2];
    HANDLE Threads[2];

    ring_test_create(&State, &RingTestGuidWorkerStop, 2);
    ring_test_start(&State, 2);
    InterlockedExchange(&State.HoldReads, 1);
    memset(Reads, 0, sizeof Reads);
    memset(Threads, 0, sizeof Threads);
    for (UINT32 I = 0; ARRAYSIZE(Reads) > I; I++)
    {
        Reads[I].State = &State;
        Reads[I].Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Reads[I].Finished);
        Threads[I] = CreateThread(0, 0,
            ring_test_disk_read_thread, &Reads[I], 0, 0);
        ASSERT(0 != Threads[I]);
    }
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.TwoReadsStarted,
        5000));

    /* Ring shutdown must cancel WAIT while workers are still in callbacks. */
    SpdStorageUnitShutdown(State.StorageUnit);
    SetEvent(State.ReleaseReads);
    for (UINT32 I = 0; ARRAYSIZE(Reads) > I; I++)
    {
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Threads[I], 10000));
        ASSERT(ERROR_SUCCESS != Reads[I].Error);
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

    BOOLEAN WaitFailed = FALSE;
    while (CompletedTestReads < ARRAYSIZE(Reads))
    {
        SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
        BOOLEAN TestRead;
        DWORD Error;
        memset(&WaitParams, 0, sizeof WaitParams);
        WaitParams.MaxRequests = 1;
        Error = SpdIoctlRingWait(
            State.StorageUnit->Handle, State.StorageUnit->Btl,
            &WaitParams);
        if (ERROR_SUCCESS != Error)
        {
            tlib_printf("wait-credit RING_WAIT failed: error=%lu (0x%08lx), produced=%lu, completed=%lu\n",
                (unsigned long)Error, (unsigned long)Error,
                (unsigned long)WaitParams.Produced,
                (unsigned long)CompletedTestReads);
            ASSERT(ERROR_SUCCESS == Error);
            WaitFailed = TRUE;
            break;
        }
        ASSERT(1 == WaitParams.Produced);
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(
            &State, &TestRead));
        if (TestRead)
            CompletedTestReads++;
    }

    if (WaitFailed)
    {
        ring_test_destroy(&State);
        for (UINT32 I = 0; I < ARRAYSIZE(Reads); I++)
        {
            if (0 != Threads[I])
            {
                WaitForSingleObject(Threads[I], 10000);
                CloseHandle(Threads[I]);
            }
            if (0 != Reads[I].Finished)
                CloseHandle(Reads[I].Finished);
        }
        return;
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
    TEST(ioctl_ring_open_layout_validation_test);
    TEST(ioctl_ring_wraparound_test);
    TEST(ioctl_ring_batch_full_boundary_test);
    TEST(ioctl_ring_async_response_test);
    TEST(ioctl_ring_duplicate_response_test);
    TEST(ioctl_ring_callback_true_early_response_test);
    TEST(ioctl_ring_lifo_buffer_reuse_test);
    TEST(ioctl_ring_buffer_owner_hint_test);
    TEST(ioctl_ring_out_of_order_worker_test);
    TEST(ioctl_ring_manual_bulk_visibility_test);
    TEST(ioctl_ring_bulk_visibility_test);
    TEST(ioctl_ring_saturation_test);
    TEST(ioctl_ring_stop_with_wait_blocked_test);
    TEST(ioctl_ring_stop_with_workers_active_test);
    TEST(ioctl_ring_wait_credit_test);
}
