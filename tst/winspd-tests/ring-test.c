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
#include <ntddscsi.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "setupapi.lib")

typedef struct _RING_TEST_STATE
{
    SPD_STORAGE_UNIT *StorageUnit;
    GUID Guid;
    WCHAR DiskPath[1024];
    HANDLE ManualDiskHandle;
    HANDLE ManualWaitEvent;
    OVERLAPPED ManualWaitOverlapped;
    SPD_IOCTL_RING_WAIT_PARAMS ManualWaitParams;
    BOOLEAN ManualWaitOutstanding;
    HANDLE ReleaseReads;
    HANDLE TwoReadsStarted;
    HANDLE ReleaseSlowRead;
    HANDLE SlowReadStarted;
#if defined(WINSPD_TEST_BUILD)
    HANDLE PumpHookEntered;
    HANDLE PumpHookRelease;
    HANDLE IoqPostEntered;
    HANDLE IoqPostRelease;
    HANDLE IoqPostResetDone;
    HANDLE ProducerPrepareEntered;
    HANDLE ProducerPrepareRelease;
    HANDLE CompletionCopyEntered;
    HANDLE CompletionCopyRelease;
    HANDLE CompletionClaimEntered;
    HANDLE CompletionClaimRelease;
    HANDLE PrepareCommitEntered;
    HANDLE PrepareCommitRelease;
    HANDLE ReadyEntered;
    HANDLE ReadyRelease;
    LONG PumpHookRuns;
#endif
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
    LONG FlushCallbacks;
    LONG FlushError;
    LONG HoldFlush;
    HANDLE FlushStarted;
    HANDLE ReleaseFlush;
    UINT64 LastFlushBlockAddress;
    UINT32 LastFlushBlockCount;
    LONG VerifyChunkOffsets;
    LONG MappingChecks;
    LONG MappedReadCallbacks;
    LONG MappedWriteCallbacks;
    LONG ReadOnlyMappings;
    LONG Failure;
    UINT32 QueueDepth;
    UINT64 SlowReadBlockAddress;
    UINT64 LoggedPhysicalDriveMask;
    DWORD ExpectedDispatcherError;
    UINT32 BufferCount;
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

typedef struct _RING_TEST_OPEN_THREAD
{
    RING_TEST_STATE *State;
    HANDLE Finished;
    DWORD DesiredAccess;
    HANDLE DiskHandle;
    DWORD Error;
} RING_TEST_OPEN_THREAD;

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
    RingTestDataLength = 512,
    RingTestWaitTimeoutMs = 15000
};

static DWORD ring_test_stop_pending_wait(RING_TEST_STATE *State);
static DWORD ring_test_complete_one_manually(
    RING_TEST_STATE *State,
    PBOOLEAN PTestRead);
static DWORD ring_test_manual_complete_until_bulk_io(
    RING_TEST_STATE *State,
    UINT8 ExpectedKind,
    UINT64 ExpectedBlockAddress,
    const UINT8 *ExpectedWriteData,
    UINT32 ExpectedLength,
    UINT8 ReadFill);
#if defined(WINSPD_TEST_BUILD)
static void ring_test_owner_process_termination_test(void);

static void ring_test_owner_process_termination_case(UINT32 Published);
static DWORD ring_test_owned_control(RING_TEST_STATE *State, UINT16 Code,
    UINT32 Kind, UINT64 BlockAddress, UINT32 ExpectedState);
#endif

static DWORD ring_test_begin_wait(RING_TEST_STATE *State)
{
    DWORD BytesTransferred = 0;
    DWORD Error;
    BOOL Success;

    if (State->ManualWaitOutstanding)
        return ERROR_SUCCESS;

    memset(&State->ManualWaitParams, 0,
        sizeof State->ManualWaitParams);
    State->ManualWaitParams.Base.Size =
        sizeof State->ManualWaitParams;
    State->ManualWaitParams.Base.Code = SPD_IOCTL_RING_WAIT;
    State->ManualWaitParams.Btl = State->StorageUnit->Btl;
    memset(&State->ManualWaitOverlapped, 0,
        sizeof State->ManualWaitOverlapped);
    State->ManualWaitEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (0 == State->ManualWaitEvent)
        return GetLastError();
    State->ManualWaitOverlapped.hEvent = State->ManualWaitEvent;

    Success = DeviceIoControl(
        State->StorageUnit->Handle,
        IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &State->ManualWaitParams,
        sizeof State->ManualWaitParams,
        &State->ManualWaitParams,
        sizeof State->ManualWaitParams,
        &BytesTransferred,
        &State->ManualWaitOverlapped);
    if (!Success && ERROR_IO_PENDING != (Error = GetLastError()))
    {
        CloseHandle(State->ManualWaitEvent);
        State->ManualWaitEvent = 0;
        return Error;
    }

    State->ManualWaitOutstanding = TRUE;
    return ERROR_SUCCESS;
}

static DWORD ring_test_stop_handle(HANDLE Device, UINT32 Btl)
{
    SPD_IOCTL_RING_CLOSE_PARAMS Params;
    DWORD BytesTransferred = 0;

    memset(&Params, 0, sizeof Params);
    Params.Base.Size = sizeof Params;
    Params.Base.Code = SPD_IOCTL_RING_STOP;
    Params.Btl = Btl;
    if (!DeviceIoControl(Device,
            IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
            &Params, sizeof Params, 0, 0, &BytesTransferred, 0))
        return GetLastError();
    return ERROR_SUCCESS;
}

static DWORD ring_test_stop_ring(RING_TEST_STATE *State)
{
    return ring_test_stop_handle(State->StorageUnit->Handle, State->StorageUnit->Btl);
}

static DWORD ring_test_submit_second_wait(RING_TEST_STATE *State)
{
    SPD_IOCTL_RING_WAIT_PARAMS Params;
    OVERLAPPED Overlapped;
    HANDLE Event;
    DWORD BytesTransferred = 0;
    DWORD Error;
    DWORD WaitResult;

    memset(&Params, 0, sizeof Params);
    Params.Base.Size = sizeof Params;
    Params.Base.Code = SPD_IOCTL_RING_WAIT;
    Params.Btl = State->StorageUnit->Btl;
    memset(&Overlapped, 0, sizeof Overlapped);
    Event = CreateEventW(0, TRUE, FALSE, 0);
    if (0 == Event)
        return GetLastError();
    Overlapped.hEvent = Event;

    if (DeviceIoControl(State->StorageUnit->Handle,
            IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
            &Params, sizeof Params, &Params, sizeof Params,
            &BytesTransferred, &Overlapped))
        Error = ERROR_SUCCESS;
    else
        Error = GetLastError();

    if (ERROR_IO_PENDING == Error)
    {
        WaitResult = WaitForSingleObject(Event, RingTestWaitTimeoutMs);
        if (WAIT_OBJECT_0 == WaitResult)
        {
            if (!GetOverlappedResult(State->StorageUnit->Handle,
                    &Overlapped, &BytesTransferred, FALSE))
                Error = GetLastError();
            else
                Error = ERROR_SUCCESS;
        }
        else
        {
            /* Stop releases both the state-owned WAIT and this unexpected
             * second operation before either stack buffer can go out of
             * scope. Wait for the second OVERLAPPED to become terminal. */
            (void)ring_test_stop_pending_wait(State);
            WaitResult = WaitForSingleObject(Event, INFINITE);
            if (WAIT_OBJECT_0 == WaitResult)
            {
                if (!GetOverlappedResult(State->StorageUnit->Handle,
                        &Overlapped, &BytesTransferred, FALSE))
                    tlib_printf("second RING_WAIT stopped with error=%lu\n",
                        (unsigned long)GetLastError());
            }
            else
                tlib_printf("second RING_WAIT completion wait failed error=%lu\n",
                    (unsigned long)GetLastError());
            Error = ERROR_TIMEOUT;
        }
    }

    /* The stack-backed parameters remain live until any pending IOCTL ends. */
    CloseHandle(Event);
    return Error;
}

static UINT32 ring_test_request_available(RING_TEST_STATE *State)
{
    SPD_RING_HEADER *Header = State->StorageUnit->SharedRingHeader;
    UINT32 Head = SpdRingLoadAcquire32(&Header->RequestHead.Value);
    UINT32 Tail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    return Tail - Head;
}

static DWORD ring_test_stop_pending_wait(RING_TEST_STATE *State)
{
    SPD_IOCTL_RING_CLOSE_PARAMS StopParams;
    DWORD StopBytes = 0;
    DWORD StopError;
    OVERLAPPED StopOverlapped;
    HANDLE StopEvent;

    memset(&StopParams, 0, sizeof StopParams);
    StopParams.Base.Size = sizeof StopParams;
    StopParams.Base.Code = SPD_IOCTL_RING_STOP;
    StopParams.Btl = State->StorageUnit->Btl;
    memset(&StopOverlapped, 0, sizeof StopOverlapped);
    StopEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (0 == StopEvent)
        StopError = GetLastError();
    else
    {
        StopOverlapped.hEvent = StopEvent;
        if (!DeviceIoControl(State->StorageUnit->Handle,
                IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
                &StopParams, sizeof StopParams, 0, 0,
                &StopBytes, &StopOverlapped))
        {
            StopError = GetLastError();
            if (ERROR_IO_PENDING == StopError)
            {
                DWORD StopWait = WaitForSingleObject(
                    StopEvent, INFINITE);
                if (WAIT_OBJECT_0 == StopWait &&
                    GetOverlappedResult(State->StorageUnit->Handle,
                        &StopOverlapped, &StopBytes, FALSE))
                    StopError = ERROR_SUCCESS;
                else
                    StopError = WAIT_FAILED == StopWait ?
                        GetLastError() : ERROR_GEN_FAILURE;
            }
        }
        else
            StopError = ERROR_SUCCESS;
    }
    if (ERROR_SUCCESS != StopError)
    {
        tlib_printf("manual RING_WAIT timeout: RING_STOP failed error=%lu\n",
            (unsigned long)StopError);
        ASSERT(ERROR_SUCCESS == StopError);
    }
    if (0 != StopEvent)
        CloseHandle(StopEvent);

    /* Keep the state-owned OVERLAPPED and buffer alive until the IRP exits. */
    if (!GetOverlappedResult(State->StorageUnit->Handle,
            &State->ManualWaitOverlapped, &StopBytes, TRUE))
        tlib_printf("manual RING_WAIT stopped with error=%lu\n",
            (unsigned long)GetLastError());

    CloseHandle(State->ManualWaitEvent);
    State->ManualWaitEvent = 0;
    State->ManualWaitOutstanding = FALSE;
    return WAIT_TIMEOUT;
}

static DWORD ring_test_finish_wait(
    RING_TEST_STATE *State,
    SPD_IOCTL_RING_WAIT_PARAMS *Params,
    DWORD TimeoutMs)
{
    SPD_RING_HEADER *Header = State->StorageUnit->SharedRingHeader;
    DWORD BytesTransferred = 0;
    DWORD Error;
    DWORD WaitResult;

    if (!State->ManualWaitOutstanding)
        return ERROR_INVALID_STATE;

    WaitResult = WaitForSingleObject(State->ManualWaitEvent, TimeoutMs);
    if (WAIT_OBJECT_0 != WaitResult)
    {
        if (0 != Header)
            tlib_printf("manual RING_WAIT timed out: req=%lu/%lu cq=%lu/%lu\n",
                (unsigned long)SpdRingLoadAcquire32(
                    &Header->RequestHead.Value),
                (unsigned long)SpdRingLoadAcquire32(
                    &Header->RequestTail.Value),
                (unsigned long)SpdRingLoadAcquire32(
                    &Header->CompletionHead.Value),
                (unsigned long)SpdRingLoadAcquire32(
                    &Header->CompletionTail.Value));
        return ring_test_stop_pending_wait(State);
    }

    if (!GetOverlappedResult(State->StorageUnit->Handle,
            &State->ManualWaitOverlapped, &BytesTransferred, FALSE))
        Error = GetLastError();
    else if (sizeof State->ManualWaitParams != BytesTransferred)
        Error = ERROR_INVALID_DATA;
    else
    {
        *Params = State->ManualWaitParams;
        Error = ERROR_SUCCESS;
    }

    CloseHandle(State->ManualWaitEvent);
    State->ManualWaitEvent = 0;
    State->ManualWaitOutstanding = FALSE;
    return Error;
}

static DWORD ring_test_wait(
    RING_TEST_STATE *State,
    SPD_IOCTL_RING_WAIT_PARAMS *Params,
    DWORD TimeoutMs)
{
    DWORD Error;

    if (!State->ManualWaitOutstanding)
    {
        Error = ring_test_begin_wait(State);
        if (ERROR_SUCCESS != Error)
            return Error;
    }

    return ring_test_finish_wait(State, Params, TimeoutMs);
}

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
    {
        if ((DWORD)-1 == Drive)
            tlib_printf("disk interface candidate path=%ls open_error=%lu expected=%s\n",
                DiskPath, (unsigned long)GetLastError(), ExpectedSerial);
        return FALSE;
    }

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
    else if ((DWORD)-1 == Drive)
        tlib_printf("disk interface candidate path=%ls serial=%s expected=%s match=%u\n",
            DiskPath, Serial, ExpectedSerial, (unsigned)Result);
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
                    /* The manual consumer cannot issue a synchronous
                     * storage-property query while it owns RING_WAIT. Its
                     * test environment therefore requires a single WinSpd
                     * disk. Dispatcher tests can verify the requested GUID
                     * from the device serial and must do so. */
                    if ((InterfaceOnly || ring_test_disk_has_serial(State,
                            (DWORD)-1, DetailData->DevicePath,
                            ExpectedSerial)) &&
                        L'\0' == State->DiskPath[0] &&
                        ARRAYSIZE(State->DiskPath) >
                            wcslen(DetailData->DevicePath))
                    {
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

    tlib_printf("ring discovery timeout tick=%I64u\n",
        GetTickCount64());
    SpdStorageUnitGetDispatcherError(State->StorageUnit,
        &DispatcherError);
    tlib_printf("ring discovery timeout: dispatcher=%lu callbackFailure=%ld readCallbacks=%ld testReads=%ld writeCallbacks=%ld\n",
        (unsigned long)DispatcherError,
        InterlockedCompareExchange(&State->Failure, 0, 0),
        InterlockedCompareExchange(&State->ReadCallbacks, 0, 0),
        InterlockedCompareExchange(&State->TestReadCalls, 0, 0),
        InterlockedCompareExchange(&State->WriteCallbacks, 0, 0));

    /* A failed dispatcher may already have unprovisioned the device and
     * unmapped the ring. Diagnostics must not dereference that mapping. */
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
static const GUID RingTestGuidBufferExhaustion =
    { 0x51aeb04d, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x0d } };
static const GUID RingTestGuidCqSaturation =
    { 0x51aeb04e, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x0e } };
static const GUID RingTestGuidWaitNotification =
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
static const GUID RingTestGuidPublishBeforeWait =
    { 0x51aeb050, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x10 } };
static const GUID RingTestGuidPostCancelRace =
    { 0x51aeb051, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x11 } };
static const GUID RingTestGuidProducerConcurrency =
    { 0x51aeb052, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x12 } };
static const GUID RingTestGuidWaitTeardown =
    { 0x51aeb053, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x13 } };
static const GUID RingTestGuidOwnerDeath =
    { 0x51aeb054, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x14 } };
static const GUID RingTestGuidLeaseCancelWrite =
    { 0x51aeb055, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x15 } };
static const GUID RingTestGuidLeaseResetWrite =
    { 0x51aeb056, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x16 } };
static const GUID RingTestGuidLeaseStopWrite =
    { 0x51aeb057, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x17 } };
static const GUID RingTestGuidLeaseCancelRead =
    { 0x51aeb058, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x18 } };
static const GUID RingTestGuidLeaseResetRead =
    { 0x51aeb059, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x19 } };
static const GUID RingTestGuidBatchAbortCompact =
    { 0x51aeb05a, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x1a } };
static const GUID RingTestGuidFullSqResume =
    { 0x51aeb05b, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x1b } };
static const GUID RingTestGuidChunkOffsets =
    { 0x51aeb05c, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x1c } };

#if defined(WINSPD_TEST_BUILD)
static VOID ring_test_pause_pump(PVOID Context)
{
    RING_TEST_STATE *State = Context;

    if (0 != InterlockedCompareExchange(&State->PumpHookRuns, 1, 0))
        return;
    SetEvent(State->PumpHookEntered);
    if (WAIT_OBJECT_0 != WaitForSingleObject(State->PumpHookRelease,
            RingTestWaitTimeoutMs))
        InterlockedExchange(&State->Failure, 1);
}
#endif

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

static UINT8 ring_test_byte_pattern(UINT64 Offset)
{
    /* Include higher address bits: a 4 KiB chunk must not repeat the first
     * chunk's 256-byte pattern and hide a broken copy offset. */
    return (UINT8)(Offset ^ (Offset >> 8) ^ (Offset >> 16) ^ (Offset >> 24));
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
    if (IsTestRead && 0 != Buffer && 0 != StorageUnit->SharedRingHeader)
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
        {
            if (0 == Header->MapThresholdBytes)
                InterlockedExchange(&State->Failure, 1);
        }
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
    if (0 != Buffer && State->MappingChecks)
    {
        UINT_PTR Pool = (UINT_PTR)StorageUnit->SharedRingAddress + StorageUnit->SharedRingHeader->BufferOffset;
        SIZE_T Bytes = (SIZE_T)StorageUnit->SharedRingHeader->BufferCount * StorageUnit->SharedRingHeader->BufferSize;
        if ((UINT_PTR)Buffer < Pool || (UINT_PTR)Buffer - Pool >= Bytes)
            InterlockedIncrement(&State->MappedReadCallbacks);
    }
    if (State->SlowReadBlockAddress == BlockAddress)
    {
        tlib_printf("out-of-order slow callback entered block=%I64u\n",
            BlockAddress);
        fflush(stdout);
        SetEvent(State->SlowReadStarted);
        DWORD SlowGateResult = WaitForSingleObject(
            State->ReleaseSlowRead, 10000);
        tlib_printf("out-of-order slow callback gate result=%lu block=%I64u\n",
            (unsigned long)SlowGateResult, BlockAddress);
        fflush(stdout);
        if (WAIT_OBJECT_0 != SlowGateResult)
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
    {
        SIZE_T Length = (SIZE_T)BlockCount *
            StorageUnit->StorageUnitParams.BlockLength;
        if (0 != InterlockedCompareExchange(
                &State->VerifyChunkOffsets, 0, 0))
        {
            UINT8 *Bytes = Buffer;
            UINT64 Start = BlockAddress *
                StorageUnit->StorageUnitParams.BlockLength;
            for (SIZE_T I = 0; Length > I; I++)
                Bytes[I] = ring_test_byte_pattern(Start + I);
        }
        else
            memset(Buffer, 0x5a, Length);
    }

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
        /* Only the explicit negative-test mode returns synchronous TRUE. */
        return RingTestAsyncEarlyTrue == AsyncMode;
    }

    return TRUE;
}

static BOOLEAN ring_test_flush(SPD_STORAGE_UNIT *StorageUnit,
    UINT64 BlockAddress, UINT32 BlockCount,
    SPD_STORAGE_UNIT_STATUS *Status)
{
    RING_TEST_STATE *State = StorageUnit->UserContext;

    memset(Status, 0, sizeof *Status);
    State->LastFlushBlockAddress = BlockAddress;
    State->LastFlushBlockCount = BlockCount;
    InterlockedIncrement(&State->FlushCallbacks);
    if (0 != InterlockedCompareExchange(&State->HoldFlush, 0, 0))
    {
        SetEvent(State->FlushStarted);
        if (WAIT_OBJECT_0 != WaitForSingleObject(State->ReleaseFlush, 10000))
            InterlockedExchange(&State->Failure, 1);
    }
    if (0 != InterlockedCompareExchange(&State->FlushError, 0, 0))
    {
        Status->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        Status->SenseKey = SCSI_SENSE_HARDWARE_ERROR;
        Status->ASC = SCSI_ADSENSE_INTERNAL_TARGET_FAILURE;
    }
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
    if (State->MappingChecks && 0 != Buffer)
    {
        UINT_PTR Pool = (UINT_PTR)StorageUnit->SharedRingAddress + StorageUnit->SharedRingHeader->BufferOffset;
        SIZE_T Bytes = (SIZE_T)StorageUnit->SharedRingHeader->BufferCount * StorageUnit->SharedRingHeader->BufferSize;
        if ((UINT_PTR)Buffer < Pool || (UINT_PTR)Buffer - Pool >= Bytes)
        {
            InterlockedIncrement(&State->MappedWriteCallbacks);
            __try
            {
                *(volatile UINT8 *)Buffer = *(volatile UINT8 *)Buffer;
                InterlockedExchange(&State->Failure, 1);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                InterlockedIncrement(&State->ReadOnlyMappings);
            }
        }
    }
    if (RingTestBlockAddress <= BlockAddress &&
        BlockAddress < RingTestBlockAddress + 32)
    {
        InterlockedIncrement(&State->WriteCallbacks);
        UINT64 Start = BlockAddress *
            StorageUnit->StorageUnitParams.BlockLength;
        BOOLEAN VerifyOffsets = 0 != InterlockedCompareExchange(
            &State->VerifyChunkOffsets, 0, 0);
        for (SIZE_T I = 0; Length > I; I++)
        {
            UINT8 Expected = VerifyOffsets ?
                ring_test_byte_pattern(Start + I) : 0x3c;
            if (Expected != ((PUINT8)Buffer)[I])
            {
                InterlockedExchange(&State->Failure, 1);
                break;
            }
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

static VOID ring_test_create_transport(RING_TEST_STATE *State,
    const GUID *Guid, UINT32 QueueDepth, UINT32 BufferCount,
    BOOLEAN IoqPostBarrier, BOOLEAN CacheSupported, UINT32 MaxTransferLength, BOOLEAN SharedRing)
{
    SPD_STORAGE_UNIT_PARAMS Params;
    SPD_IOCTL_RING_OPEN_PARAMS RingParams;
    DWORD Error;

    memset(State, 0, sizeof *State);
    InitializeSRWLock(&State->AsyncLock);
    InitializeSRWLock(&State->BufferLock);
    State->Guid = *Guid;
    State->QueueDepth = QueueDepth;
    State->BufferCount = BufferCount;
    State->SlowReadBlockAddress = (UINT64)-1;
    State->ReleaseReads = CreateEventW(0, TRUE, FALSE, 0);
    State->TwoReadsStarted = CreateEventW(0, TRUE, FALSE, 0);
    State->ReleaseSlowRead = CreateEventW(0, TRUE, FALSE, 0);
    State->SlowReadStarted = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != State->ReleaseReads);
    ASSERT(0 != State->TwoReadsStarted);
    ASSERT(0 != State->ReleaseSlowRead);
    ASSERT(0 != State->SlowReadStarted);
#if defined(WINSPD_TEST_BUILD)
    if (IoqPostBarrier)
    {
        State->IoqPostEntered = CreateEventW(0, TRUE, FALSE,
            L"Global\\WinSpdSharedRingTestPostEntered");
        State->IoqPostRelease = CreateEventW(0, TRUE, TRUE,
            L"Global\\WinSpdSharedRingTestPostRelease");
        State->IoqPostResetDone = CreateEventW(0, TRUE, FALSE,
            L"Global\\WinSpdSharedRingTestPostResetDone");
        ASSERT(0 != State->IoqPostEntered);
        ASSERT(0 != State->IoqPostRelease);
        ASSERT(0 != State->IoqPostResetDone);
        ASSERT(ResetEvent(State->IoqPostEntered));
        ASSERT(SetEvent(State->IoqPostRelease));
        ASSERT(ResetEvent(State->IoqPostResetDone));
    }
    State->ProducerPrepareEntered = CreateEventW(0, TRUE, FALSE,
        L"Global\\WinSpdSharedRingTestPrepareEntered");
    State->ProducerPrepareRelease = CreateEventW(0, TRUE, TRUE,
        L"Global\\WinSpdSharedRingTestPrepareRelease");
    State->CompletionCopyEntered = CreateEventW(0, TRUE, FALSE,
        L"Global\\WinSpdSharedRingTestCompletionEntered");
    State->CompletionCopyRelease = CreateEventW(0, TRUE, TRUE,
        L"Global\\WinSpdSharedRingTestCompletionRelease");
    ASSERT(0 != State->ProducerPrepareEntered);
    ASSERT(0 != State->ProducerPrepareRelease);
    ASSERT(0 != State->CompletionCopyEntered);
    ASSERT(0 != State->CompletionCopyRelease);
    ASSERT(ResetEvent(State->ProducerPrepareEntered));
    ASSERT(SetEvent(State->ProducerPrepareRelease));
    ASSERT(ResetEvent(State->CompletionCopyEntered));
    ASSERT(SetEvent(State->CompletionCopyRelease));
    State->CompletionClaimEntered = CreateEventW(0, TRUE, FALSE,
        L"Global\\WinSpdSharedRingTestCompletionClaimEntered");
    State->CompletionClaimRelease = CreateEventW(0, TRUE, TRUE,
        L"Global\\WinSpdSharedRingTestCompletionClaimRelease");
    ASSERT(0 != State->CompletionClaimEntered && 0 != State->CompletionClaimRelease);
    ASSERT(ResetEvent(State->CompletionClaimEntered));
    ASSERT(SetEvent(State->CompletionClaimRelease));
    State->PrepareCommitEntered = CreateEventW(0, TRUE, FALSE,
        L"Global\\WinSpdSharedRingTestPrepareCommitEntered");
    State->PrepareCommitRelease = CreateEventW(0, TRUE, TRUE,
        L"Global\\WinSpdSharedRingTestPrepareCommitRelease");
    ASSERT(0 != State->PrepareCommitEntered && 0 != State->PrepareCommitRelease);
    ASSERT(ResetEvent(State->PrepareCommitEntered));
    ASSERT(SetEvent(State->PrepareCommitRelease));
    State->ReadyEntered = CreateEventW(0, TRUE, FALSE,
        L"Global\\WinSpdSharedRingTestReadyEntered");
    State->ReadyRelease = CreateEventW(0, TRUE, TRUE,
        L"Global\\WinSpdSharedRingTestReadyRelease");
    ASSERT(0 != State->ReadyEntered && 0 != State->ReadyRelease);
    ASSERT(ResetEvent(State->ReadyEntered));
    ASSERT(SetEvent(State->ReadyRelease));
#else
    UNREFERENCED_PARAMETER(IoqPostBarrier);
#endif

    memset(&Params, 0, sizeof Params);
    Params.Guid = *Guid;
    memcpy(Params.ProductId, "RingTest", sizeof "RingTest");
    Params.BlockCount = MaxTransferLength > 16384 ? (UINT64)MaxTransferLength * 32 / 512 : 64;
    Params.BlockLength = 512;
    Params.MaxTransferLength = MaxTransferLength;
    Params.CacheSupported = CacheSupported;
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

    if (!SharedRing)
        return;

    memset(&RingParams, 0, sizeof RingParams);
    RingParams.Version = SPD_RING_VERSION;
    RingParams.QueueDepth = QueueDepth;
    RingParams.BufferCount = State->BufferCount;
    RingParams.BufferSize = MaxTransferLength;
    Error = SpdStorageUnitOpenSharedRing(State->StorageUnit, &RingParams);
    if (ERROR_SUCCESS != Error)
        tlib_printf("shared ring OPEN failed: error=%lu\n", (unsigned long)Error);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(0 != State->StorageUnit->SharedRingHeader);
    ASSERT(SPD_RING_VERSION ==
        State->StorageUnit->SharedRingHeader->Version);
    ASSERT(sizeof(SPD_RING_HEADER) ==
        State->StorageUnit->SharedRingHeader->HeaderSize);
    ASSERT(QueueDepth == State->StorageUnit->SharedRingHeader->QueueDepth);
    ASSERT(State->BufferCount ==
        State->StorageUnit->SharedRingHeader->BufferCount);
    ASSERT(MaxTransferLength == State->StorageUnit->SharedRingHeader->BufferSize);
    ASSERT(0 == State->StorageUnit->SharedRingHeader->RequestOffset %
        SPD_RING_CACHE_LINE_SIZE);
    ASSERT(0 == State->StorageUnit->SharedRingHeader->CompletionOffset %
        SPD_RING_CACHE_LINE_SIZE);
    ASSERT(0 == State->StorageUnit->SharedRingHeader->BufferOffset % 4096);
    ASSERT(State->StorageUnit->SharedRingSize <=
        SPD_RING_MAX_SECTION_BYTES);

}

static VOID ring_test_create_with_limits(RING_TEST_STATE *State,
    const GUID *Guid, UINT32 QueueDepth, UINT32 BufferCount,
    BOOLEAN IoqPostBarrier, BOOLEAN CacheSupported, UINT32 MaxTransferLength)
{
    ring_test_create_transport(State, Guid, QueueDepth, BufferCount,
        IoqPostBarrier, CacheSupported, MaxTransferLength, TRUE);
}

static VOID ring_test_create_with_options(RING_TEST_STATE *State,
    const GUID *Guid, UINT32 QueueDepth, UINT32 BufferCount,
    BOOLEAN IoqPostBarrier, BOOLEAN CacheSupported)
{
    ring_test_create_with_limits(State, Guid, QueueDepth, BufferCount,
        IoqPostBarrier, CacheSupported, 4096);
}

static VOID ring_test_create_with_buffer_count(RING_TEST_STATE *State,
    const GUID *Guid, UINT32 QueueDepth, UINT32 BufferCount,
    BOOLEAN IoqPostBarrier)
{
    ring_test_create_with_options(State, Guid, QueueDepth, BufferCount,
        IoqPostBarrier, FALSE);
}

static VOID ring_test_create(RING_TEST_STATE *State, const GUID *Guid,
    UINT32 QueueDepth)
{
    ring_test_create_with_buffer_count(State, Guid, QueueDepth,
        SPD_RING_DEFAULT_LUN_QUEUE_DEPTH, FALSE);
}

static VOID ring_test_destroy(RING_TEST_STATE *State);

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
    {
        ring_test_log_discovery_timeout(State);
        /* Preserve the dispatcher summary and close the ring before the
         * assertion terminates this test process. */
        ring_test_destroy(State);
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
        InterlockedExchange(&State->HoldFlush, 0);
        InterlockedExchange(&State->FlushError, 0);
        if (0 != State->ReleaseFlush)
            SetEvent(State->ReleaseFlush);
        SetEvent(State->ReleaseReads);
        ring_test_join_async_threads(State);
        if (State->ManualWaitOutstanding)
        {
            /* Retire the test's WAIT through production IRP cancellation.
             * Shutdown/Delete own transport STOP/CLOSE and callback rundown. */
            BOOL Cancelled = CancelIoEx(State->StorageUnit->Handle, &State->ManualWaitOverlapped);
            ASSERT(Cancelled || ERROR_NOT_FOUND == GetLastError());
            SPD_IOCTL_RING_WAIT_PARAMS UnusedParams;
            (void)ring_test_finish_wait(State, &UnusedParams, RingTestWaitTimeoutMs);
        }
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
        /* A cache-enabled disk can generate new flush I/O on handle close.
         * The manual consumer has finished: close handles after production
         * teardown, rather than waiting for an unserviced flush to time out. */
        if (0 != State->ManualDiskHandle)
        {
            CloseHandle(State->ManualDiskHandle);
            State->ManualDiskHandle = 0;
        }
    }
    if (0 != State->TwoReadsStarted)
        CloseHandle(State->TwoReadsStarted);
    if (0 != State->ReleaseReads)
        CloseHandle(State->ReleaseReads);
    if (0 != State->SlowReadStarted)
        CloseHandle(State->SlowReadStarted);
    if (0 != State->ReleaseSlowRead)
        CloseHandle(State->ReleaseSlowRead);
    if (0 != State->FlushStarted)
        CloseHandle(State->FlushStarted);
    if (0 != State->ReleaseFlush)
        CloseHandle(State->ReleaseFlush);
#if defined(WINSPD_TEST_BUILD)
    if (0 != State->PumpHookEntered)
        CloseHandle(State->PumpHookEntered);
    if (0 != State->PumpHookRelease)
        CloseHandle(State->PumpHookRelease);
    if (0 != State->IoqPostEntered)
        CloseHandle(State->IoqPostEntered);
    if (0 != State->IoqPostRelease)
        CloseHandle(State->IoqPostRelease);
    if (0 != State->IoqPostResetDone)
        CloseHandle(State->IoqPostResetDone);
    if (0 != State->ProducerPrepareEntered)
        CloseHandle(State->ProducerPrepareEntered);
    if (0 != State->ProducerPrepareRelease)
        CloseHandle(State->ProducerPrepareRelease);
    if (0 != State->CompletionCopyEntered)
        CloseHandle(State->CompletionCopyEntered);
    if (0 != State->CompletionCopyRelease)
        CloseHandle(State->CompletionCopyRelease);
    if (0 != State->CompletionClaimEntered)
        CloseHandle(State->CompletionClaimEntered);
    if (0 != State->CompletionClaimRelease)
        CloseHandle(State->CompletionClaimRelease);
    if (0 != State->PrepareCommitEntered)
        CloseHandle(State->PrepareCommitEntered);
    if (0 != State->PrepareCommitRelease)
        CloseHandle(State->PrepareCommitRelease);
    if (0 != State->ReadyEntered)
        CloseHandle(State->ReadyEntered);
    if (0 != State->ReadyRelease)
        CloseHandle(State->ReadyRelease);
#endif
}

static DWORD ring_test_disk_read_at(
    RING_TEST_STATE *State,
    UINT64 BlockAddress)
{
    LARGE_INTEGER Offset;
    UINT8 DataBuffer[RingTestDataLength];
    DWORD BytesRead;
    HANDLE DiskHandle;
    BOOLEAN OwnHandle = FALSE;
    DWORD Error;

    Offset.QuadPart = (LONGLONG)BlockAddress * RingTestDataLength;
    DiskHandle = State->ManualDiskHandle;
    if (0 == DiskHandle)
    {
        DiskHandle = CreateFileW(State->DiskPath, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, 0);
        if (INVALID_HANDLE_VALUE == DiskHandle)
            return GetLastError();
        OwnHandle = TRUE;
    }

    if (!OwnHandle)
    {
        OVERLAPPED Overlapped;
        DWORD WaitResult;

        memset(&Overlapped, 0, sizeof Overlapped);
        Overlapped.hEvent = CreateEventW(0, TRUE, FALSE, 0);
        if (0 == Overlapped.hEvent)
            Error = GetLastError();
        else
        {
            Overlapped.Offset = Offset.LowPart;
            Overlapped.OffsetHigh = Offset.HighPart;
            if (ReadFile(DiskHandle, DataBuffer, sizeof DataBuffer,
                    &BytesRead, &Overlapped))
                Error = ERROR_SUCCESS;
            else if (ERROR_IO_PENDING != (Error = GetLastError()))
                ;
            else
            {
                WaitResult = WaitForSingleObject(
                    Overlapped.hEvent, INFINITE);
                if (WAIT_OBJECT_0 != WaitResult)
                    Error = WAIT_FAILED == WaitResult ?
                        GetLastError() : ERROR_GEN_FAILURE;
                else if (!GetOverlappedResult(DiskHandle,
                        &Overlapped, &BytesRead, FALSE))
                    Error = GetLastError();
                else
                    Error = ERROR_SUCCESS;
            }
            CloseHandle(Overlapped.hEvent);
        }
    }
    else if (!SetFilePointerEx(DiskHandle, Offset, 0, FILE_BEGIN))
        Error = GetLastError();
    else if (!ReadFile(DiskHandle, DataBuffer, sizeof DataBuffer,
        &BytesRead, 0))
        Error = GetLastError();
    else
        Error = sizeof DataBuffer == BytesRead ? ERROR_SUCCESS :
            ERROR_INVALID_DATA;
    if (ERROR_SUCCESS == Error && sizeof DataBuffer != BytesRead)
        Error = ERROR_INVALID_DATA;
    if (ERROR_SUCCESS == Error &&
        0 != State->StorageUnit->DispatcherThread)
        for (UINT32 I = 0; sizeof DataBuffer > I; I++)
            if (0x5a != DataBuffer[I])
            {
                Error = ERROR_INVALID_DATA;
                break;
            }
    if (OwnHandle)
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
    {
        Error = GetLastError();
        tlib_printf("large ring READ failed error=%lu\n",
            (unsigned long)Error);
    }
    else
    {
        Error = DataLength == BytesRead ? ERROR_SUCCESS :
            ERROR_INVALID_DATA;
        if (ERROR_SUCCESS != Error)
            tlib_printf("large ring READ length mismatch bytes=%lu expected=%lu\n",
                (unsigned long)BytesRead, (unsigned long)DataLength);
        for (DWORD I = 0; ERROR_SUCCESS == Error && DataLength > I; I++)
            if (0x5a != DataBuffer[I])
            {
                Error = ERROR_INVALID_DATA;
                tlib_printf("large ring READ data mismatch offset=%lu value=%02x expected=5a\n",
                    (unsigned long)I, DataBuffer[I]);
            }
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

#if defined(WINSPD_TEST_BUILD)
static DWORD ring_test_manual_handle_read(
    RING_TEST_STATE *State,
    UINT64 BlockAddress)
{
    OVERLAPPED Overlapped;
    LARGE_INTEGER Offset;
    UINT8 Buffer[RingTestDataLength];
    DWORD BytesRead = 0;
    DWORD Error;
    BOOL Success;

    memset(&Overlapped, 0, sizeof Overlapped);
    Overlapped.hEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (0 == Overlapped.hEvent)
        return GetLastError();
    Offset.QuadPart = (LONGLONG)BlockAddress * RingTestDataLength;
    Overlapped.Offset = Offset.LowPart;
    Overlapped.OffsetHigh = Offset.HighPart;
    Success = ReadFile(State->ManualDiskHandle, Buffer, sizeof Buffer,
        &BytesRead, &Overlapped);
    if (Success)
        Error = ERROR_SUCCESS;
    else
    {
        Error = GetLastError();
        if (ERROR_IO_PENDING == Error)
        {
            if (WAIT_OBJECT_0 != WaitForSingleObject(Overlapped.hEvent,
                    RingTestWaitTimeoutMs))
                Error = ERROR_TIMEOUT;
            else if (!GetOverlappedResult(State->ManualDiskHandle,
                    &Overlapped, &BytesRead, FALSE))
                Error = GetLastError();
            else
                Error = ERROR_SUCCESS;
        }
    }
    if (ERROR_SUCCESS == Error && sizeof Buffer != BytesRead)
        Error = ERROR_INVALID_DATA;
    for (DWORD I = 0; ERROR_SUCCESS == Error && sizeof Buffer > I; I++)
        if (0x5a != Buffer[I])
            Error = ERROR_INVALID_DATA;
    CloseHandle(Overlapped.hEvent);
    return Error;
}

static DWORD WINAPI ring_test_manual_handle_read_thread(PVOID Data)
{
    RING_TEST_READ_THREAD *ReadThread = Data;
    ReadThread->Error = ring_test_manual_handle_read(
        ReadThread->State, 0 == ReadThread->BlockAddress ?
            RingTestBlockAddress : ReadThread->BlockAddress);
    SetEvent(ReadThread->Finished);
    return ReadThread->Error;
}

static DWORD ring_test_retire_manual_wait(RING_TEST_STATE *State)
{
    RING_TEST_READ_THREAD Read;
    HANDLE Thread;
    DWORD WaitResult;
    DWORD Error;

    if (!State->ManualWaitOutstanding)
        return ERROR_SUCCESS;

    memset(&Read, 0, sizeof Read);
    Read.State = State;
    Read.BlockAddress = RingTestBlockAddress + 15;
    Read.Finished = CreateEventW(0, TRUE, FALSE, 0);
    if (0 == Read.Finished)
        return GetLastError();
    Thread = CreateThread(0, 0,
        ring_test_manual_handle_read_thread, &Read, 0, 0);
    if (0 == Thread)
    {
        Error = GetLastError();
        CloseHandle(Read.Finished);
        return Error;
    }

    Error = ring_test_manual_complete_until_bulk_io(State,
        SpdIoctlTransactReadKind, Read.BlockAddress, 0,
        RingTestDataLength, 0x5a);

    if (State->ManualWaitOutstanding)
    {
        SPD_IOCTL_RING_WAIT_PARAMS UnusedParams;
        if (ERROR_SUCCESS == Error)
            Error = ring_test_finish_wait(State, &UnusedParams, 0);
        else
            (void)ring_test_stop_pending_wait(State);
    }
    if (ERROR_SUCCESS != Error)
        SetEvent(State->ReleaseReads);
    WaitResult = WaitForSingleObject(Thread, INFINITE);
    if (WAIT_OBJECT_0 != WaitResult && ERROR_SUCCESS == Error)
        Error = ERROR_TIMEOUT;
    else if (ERROR_SUCCESS == Error && ERROR_SUCCESS != Read.Error)
        Error = Read.Error;
    if (ERROR_SUCCESS != Error)
    {
        SPD_RING_HEADER *Header = State->StorageUnit->SharedRingHeader;
        tlib_printf("manual wait retirement failed error=%lu read_error=%lu req=%lu/%lu cq=%lu/%lu\n",
            (unsigned long)Error, (unsigned long)Read.Error,
            (unsigned long)SpdRingLoadAcquire32(
                &Header->RequestHead.Value),
            (unsigned long)SpdRingLoadAcquire32(
                &Header->RequestTail.Value),
            (unsigned long)SpdRingLoadAcquire32(
                &Header->CompletionHead.Value),
            (unsigned long)SpdRingLoadAcquire32(
                &Header->CompletionTail.Value));
    }
    CloseHandle(Thread);
    CloseHandle(Read.Finished);
    return Error;
}
#endif

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
    if (0 == RequestTail - RequestHead ||
        RequestTail - RequestHead > Header->QueueDepth ||
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
            (SIZE_T)Request->Data.BufferId * Header->BufferSize,
            0, Request->Data.Length);
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

static DWORD WINAPI ring_test_open_disk_thread(PVOID Data)
{
    RING_TEST_OPEN_THREAD *Open = Data;
    STORAGE_PROPERTY_QUERY Query;
    union
    {
        LARGE_INTEGER Alignment;
        UINT8 Bytes[4096];
    } DescriptorBuffer;
    DWORD BytesReturned = 0;
    char ExpectedSerial[37];
    char ActualSerial[256] = "<unavailable>";

    Open->DiskHandle = CreateFileW(Open->State->DiskPath,
        Open->DesiredAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, 0);
    Open->Error = INVALID_HANDLE_VALUE == Open->DiskHandle ?
        GetLastError() : ERROR_SUCCESS;

    sprintf_s(ExpectedSerial, sizeof ExpectedSerial,
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        Open->State->Guid.Data1, Open->State->Guid.Data2,
        Open->State->Guid.Data3, Open->State->Guid.Data4[0],
        Open->State->Guid.Data4[1], Open->State->Guid.Data4[2],
        Open->State->Guid.Data4[3], Open->State->Guid.Data4[4],
        Open->State->Guid.Data4[5], Open->State->Guid.Data4[6],
        Open->State->Guid.Data4[7]);

    if (ERROR_SUCCESS == Open->Error)
    {
        OVERLAPPED Overlapped;
        BOOL Success;

        memset(&Query, 0, sizeof Query);
        Query.PropertyId = StorageDeviceProperty;
        Query.QueryType = PropertyStandardQuery;
        memset(&Overlapped, 0, sizeof Overlapped);
        Overlapped.hEvent = CreateEventW(0, TRUE, FALSE, 0);
        if (0 == Overlapped.hEvent)
            Open->Error = GetLastError();
        else
        {
            Success = DeviceIoControl(Open->DiskHandle,
                IOCTL_STORAGE_QUERY_PROPERTY, &Query, sizeof Query,
                DescriptorBuffer.Bytes, sizeof DescriptorBuffer.Bytes,
                &BytesReturned, &Overlapped);
            if (!Success)
            {
                Open->Error = GetLastError();
                if (ERROR_IO_PENDING == Open->Error)
                {
                    DWORD WaitResult = WaitForSingleObject(
                        Overlapped.hEvent, INFINITE);
                    if (WAIT_OBJECT_0 != WaitResult)
                        Open->Error = WAIT_FAILED == WaitResult ?
                            GetLastError() : ERROR_GEN_FAILURE;
                    else if (!GetOverlappedResult(Open->DiskHandle,
                            &Overlapped, &BytesReturned, FALSE))
                        Open->Error = GetLastError();
                    else
                        Open->Error = ERROR_SUCCESS;
                }
            }
            CloseHandle(Overlapped.hEvent);
        }
    }

    if (ERROR_SUCCESS == Open->Error &&
        BytesReturned >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
    {
        PSTORAGE_DEVICE_DESCRIPTOR Descriptor =
            (PSTORAGE_DEVICE_DESCRIPTOR)DescriptorBuffer.Bytes;
        if (0 != Descriptor->SerialNumberOffset &&
            Descriptor->SerialNumberOffset < BytesReturned)
        {
            char *SerialValue = (char *)DescriptorBuffer.Bytes +
                Descriptor->SerialNumberOffset;
            size_t SerialLength = strnlen(SerialValue,
                BytesReturned - Descriptor->SerialNumberOffset);
            while (0 < SerialLength && ' ' == SerialValue[SerialLength - 1])
                SerialLength--;
            size_t CopyLength = min(SerialLength,
                sizeof ActualSerial - 1);
            memcpy(ActualSerial, SerialValue, CopyLength);
            ActualSerial[CopyLength] = '\0';
            if (0 != _stricmp(ActualSerial, ExpectedSerial))
                Open->Error = ERROR_NOT_FOUND;
        }
        else
            Open->Error = ERROR_NOT_FOUND;
    }
    else if (ERROR_SUCCESS == Open->Error)
        Open->Error = ERROR_INVALID_DATA;

    tlib_printf("manual disk probe: path=%ls serial=%s expected=%s error=%lu\n",
        Open->State->DiskPath, ActualSerial, ExpectedSerial,
        (unsigned long)Open->Error);
    if (ERROR_SUCCESS != Open->Error &&
        INVALID_HANDLE_VALUE != Open->DiskHandle)
    {
        CloseHandle(Open->DiskHandle);
        Open->DiskHandle = INVALID_HANDLE_VALUE;
    }
    SetEvent(Open->Finished);
    return Open->Error;
}

static DWORD ring_test_open_disk_manual(
    RING_TEST_STATE *State,
    DWORD DesiredAccess)
{
    RING_TEST_OPEN_THREAD Open;
    HANDLE Thread;
    ULONGLONG Deadline = GetTickCount64() + RingTestWaitTimeoutMs;
    DWORD Error = ERROR_SUCCESS;

    if (0 != State->ManualDiskHandle)
        return ERROR_SUCCESS;

    memset(&Open, 0, sizeof Open);
    Open.State = State;
    Open.DesiredAccess = DesiredAccess;
    Open.DiskHandle = INVALID_HANDLE_VALUE;
    Open.Finished = CreateEventW(0, TRUE, FALSE, 0);
    if (0 == Open.Finished)
        return GetLastError();
    Thread = CreateThread(0, 0, ring_test_open_disk_thread,
        &Open, 0, 0);
    if (0 == Thread)
    {
        Error = GetLastError();
        CloseHandle(Open.Finished);
        return Error;
    }

    while (WAIT_OBJECT_0 != WaitForSingleObject(Open.Finished, 0))
    {
        SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
        BOOLEAN TestRead;
        HANDLE WaitHandles[2];
        DWORD Remaining = (DWORD)(Deadline > GetTickCount64() ?
            Deadline - GetTickCount64() : 0);
        DWORD WaitResult;

        if (0 == Remaining)
        {
            Error = ERROR_TIMEOUT;
            break;
        }

        Error = ring_test_begin_wait(State);
        if (ERROR_SUCCESS != Error)
            break;

        WaitHandles[0] = Open.Finished;
        WaitHandles[1] = State->ManualWaitEvent;
        WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles),
            WaitHandles, FALSE, Remaining);
        if (WAIT_OBJECT_0 == WaitResult)
        {
            if (WAIT_OBJECT_0 != WaitForSingleObject(
                    State->ManualWaitEvent, 0))
                break;
        }
        else if (WAIT_OBJECT_0 + 1 != WaitResult)
        {
            Error = WAIT_TIMEOUT == WaitResult ? ERROR_TIMEOUT :
                GetLastError();
            break;
        }

        memset(&WaitParams, 0, sizeof WaitParams);
        Error = ring_test_finish_wait(State, &WaitParams, 0);
        if (ERROR_SUCCESS != Error)
            break;
        if (0 == ring_test_request_available(State))
        {
            Error = ERROR_INVALID_DATA;
            break;
        }
        Error = ring_test_complete_one_manually(State, &TestRead);
        if (ERROR_SUCCESS != Error)
            break;
        ASSERT(!TestRead);
    }

    if (ERROR_SUCCESS != Error && State->ManualWaitOutstanding)
    {
        SPD_IOCTL_RING_WAIT_PARAMS UnusedParams;
        (void)ring_test_finish_wait(State, &UnusedParams, 0);
    }

    if (WAIT_OBJECT_0 != WaitForSingleObject(Open.Finished, 0))
    {
        /* RING_STOP above releases the service IRP blocking CreateFile. */
        SpdStorageUnitShutdown(State->StorageUnit);
        WaitForSingleObject(Open.Finished, INFINITE);
    }
    WaitForSingleObject(Thread, INFINITE);
    CloseHandle(Thread);
    CloseHandle(Open.Finished);

    if (ERROR_SUCCESS == Error)
        Error = Open.Error;
    if (ERROR_SUCCESS == Error)
        State->ManualDiskHandle = Open.DiskHandle;
    else if (INVALID_HANDLE_VALUE != Open.DiskHandle)
        CloseHandle(Open.DiskHandle);

    return Error;
}

static VOID ring_test_wait_for_disk_manual(RING_TEST_STATE *State)
{
    ULONGLONG Deadline = GetTickCount64() + 15000;
    DWORD WaitError = ERROR_SUCCESS;

    while (GetTickCount64() < Deadline)
    {
        /* SetupAPI can still list the previous unit while PnP removes its
         * interface. Readiness requires a successfully opened, serial-checked
         * LU handle, with discovery I/O serviced by the manual consumer. */
        if (ring_test_find_disk(State, &State->Guid, TRUE))
        {
            WaitError = ring_test_open_disk_manual(State, GENERIC_READ | GENERIC_WRITE);
            if (ERROR_SUCCESS == WaitError)
                break;
            if (ERROR_FILE_NOT_FOUND != WaitError && ERROR_PATH_NOT_FOUND != WaitError &&
                ERROR_NOT_FOUND != WaitError && ERROR_NOT_READY != WaitError &&
                ERROR_DEV_NOT_EXIST != WaitError && ERROR_DEVICE_NOT_CONNECTED != WaitError)
                break;
            State->DiskPath[0] = L'\0';
        }
        if (!State->ManualWaitOutstanding)
        {
            WaitError = ring_test_begin_wait(State);
            if (ERROR_SUCCESS != WaitError)
                break;
        }
        ULONGLONG Now = GetTickCount64();
        if (Now >= Deadline)
            break;
        DWORD WaitResult = WaitForSingleObject(State->ManualWaitEvent,
            (DWORD)min(50, Deadline - Now));
        /* A polling interval is not a transport timeout. Keep the same WAIT
         * registered while rechecking PnP; never STOP a healthy ring here. */
        if (WAIT_TIMEOUT == WaitResult)
            continue;
        if (WAIT_OBJECT_0 != WaitResult)
        {
            WaitError = GetLastError();
            break;
        }
        SPD_IOCTL_RING_WAIT_PARAMS WaitParams = { 0 };
        WaitError = ring_test_finish_wait(State, &WaitParams, 0);
        if (ERROR_SUCCESS != WaitError)
            break;
        if (0 != ring_test_request_available(State))
        {
            BOOLEAN TestRead;
            WaitError = ring_test_complete_one_manually(State, &TestRead);
            if (ERROR_SUCCESS != WaitError)
                break;
            ASSERT(!TestRead);
        }
    }

    if (0 == State->ManualDiskHandle)
    {
        ring_test_log_discovery_timeout(State);
        if (ERROR_SUCCESS == WaitError)
            WaitError = ERROR_TIMEOUT;
        tlib_printf("manual disk discovery failed: error=%lu\n", (unsigned long)WaitError);
        ring_test_destroy(State);
        ASSERT(ERROR_SUCCESS == WaitError);
    }
    ASSERT(L'\0' != State->DiskPath[0]);
    ASSERT(0 != State->ManualDiskHandle);
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
        Error = ring_test_wait(State, &WaitParams,
            RingTestWaitTimeoutMs);
        if (ERROR_SUCCESS != Error)
            return Error;
        if (0 == ring_test_request_available(State))
            return ERROR_INVALID_DATA;

        RequestHead =
            SpdRingLoadAcquire32(&Header->RequestHead.Value);
        RequestTail =
            SpdRingLoadAcquire32(&Header->RequestTail.Value);
        CompletionHead =
            SpdRingLoadAcquire32(&Header->CompletionHead.Value);
        CompletionTail =
            SpdRingLoadAcquire32(&Header->CompletionTail.Value);
        if (0 == RequestTail - RequestHead ||
            RequestTail - RequestHead > Header->QueueDepth ||
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
                Request->Data.Length > Header->BufferSize)
                return ERROR_INVALID_DATA;

            Buffer = (PUINT8)State->StorageUnit->SharedRingAddress +
                Header->BufferOffset +
                (SIZE_T)Request->Data.BufferId * Header->BufferSize;

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
    DiskHandle = State.ManualDiskHandle;
    ASSERT(INVALID_HANDLE_VALUE != DiskHandle);

    memset(WriteData, 0x3c, sizeof WriteData);
    ASSERT(ERROR_SUCCESS == ring_test_complete_overlapped_bulk_io(
        &State, DiskHandle, SpdIoctlTransactWriteKind, WriteData, 0));

    memset(ReadData, 0, sizeof ReadData);
    ASSERT(ERROR_SUCCESS == ring_test_complete_overlapped_bulk_io(
        &State, DiskHandle, SpdIoctlTransactReadKind, ReadData, 0x5a));
    for (UINT32 I = 0; sizeof ReadData > I; I++)
        ASSERT(0x5a == ReadData[I]);

    ring_test_destroy(&State);
}

/* Submit guest commands through the disk LU on a separate handle from the
 * transport's IOCP-associated adapter handle. */
static DWORD ring_test_scsi_range(RING_TEST_STATE *State, HANDLE DiskHandle,
    UCHAR OperationCode, UINT64 BlockAddress, UINT32 BlockCount,
    UCHAR *ScsiStatus, UCHAR Sense[32])
{
    CDB Cdb;
    BOOLEAN Extended = SCSIOP_SYNCHRONIZE_CACHE16 == OperationCode;
    UINT32 AddressBytes = Extended ? 8 : 4;
    UINT32 CountBytes = Extended ? 4 : 2;
    UINT32 CountOffset = Extended ? 10 : 7;

    memset(&Cdb, 0, sizeof Cdb);
    Cdb.AsByte[0] = OperationCode;
    for (UINT32 I = 0; I < AddressBytes; I++)
        Cdb.AsByte[2 + I] = (UINT8)(BlockAddress >>
            (8 * (AddressBytes - 1 - I)));
    for (UINT32 I = 0; I < CountBytes; I++)
        Cdb.AsByte[CountOffset + I] = (UINT8)(BlockCount >>
            (8 * (CountBytes - 1 - I)));
    *ScsiStatus = 0xff;
    memset(Sense, 0, 32);
    return SpdIoctlScsiExecute(DiskHandle, State->StorageUnit->Btl,
        &Cdb, 0, 0, 0, ScsiStatus, Sense);
}

typedef struct
{
    RING_TEST_STATE *State;
    HANDLE DiskHandle;
    HANDLE Finished;
    UCHAR OperationCode;
    BOOLEAN FileBuffers;
    UCHAR ScsiStatus;
    UCHAR Sense[32];
    DWORD Error;
} RING_TEST_FLUSH_THREAD;

static DWORD WINAPI ring_test_flush_thread(PVOID Context)
{
    RING_TEST_FLUSH_THREAD *Work = Context;
    if (Work->FileBuffers)
        Work->Error = FlushFileBuffers(Work->DiskHandle) ? ERROR_SUCCESS : GetLastError();
    else
        Work->Error = ring_test_scsi_range(Work->State, Work->DiskHandle,
            Work->OperationCode, 0, 0, &Work->ScsiStatus, Work->Sense);
    SetEvent(Work->Finished);
    return Work->Error;
}

static void ring_test_zero_count_flush_dotest(UCHAR OperationCode)
{
    static const GUID Guid =
        { 0x924a6c97, 0xaebe, 0x4a80, { 0xaf, 0x55, 0xec, 0x74, 0x06, 0xba, 0x96, 0x59 } };
    RING_TEST_STATE State;
    RING_TEST_FLUSH_THREAD Work;
    HANDLE DiskHandle, Thread;
    UCHAR ScsiStatus, Sense[32];
    DWORD Error, Started, Premature, Finished;
    LONG Before;

    ring_test_create_with_options(&State, &Guid, 8,
        SPD_RING_DEFAULT_LUN_QUEUE_DEPTH, FALSE, TRUE);
    ring_test_start(&State, 2);
    DiskHandle = CreateFileW(State.DiskPath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, 0);
    ASSERT(INVALID_HANDLE_VALUE != DiskHandle);

    /* Successful completion must wait for the backend callback to return. */
    State.FlushStarted = CreateEventW(0, TRUE, FALSE, 0);
    State.ReleaseFlush = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != State.FlushStarted);
    ASSERT(0 != State.ReleaseFlush);
    memset(&Work, 0, sizeof Work);
    Work.State = &State;
    Work.DiskHandle = DiskHandle;
    Work.OperationCode = OperationCode;
    Work.Finished = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != Work.Finished);
    Before = InterlockedCompareExchange(&State.FlushCallbacks, 0, 0);
    InterlockedExchange(&State.HoldFlush, 1);
    Thread = CreateThread(0, 0, ring_test_flush_thread, &Work, 0, 0);
    ASSERT(0 != Thread);
    Started = WaitForSingleObject(State.FlushStarted, 5000);
    Premature = WaitForSingleObject(Work.Finished, 50);
    SetEvent(State.ReleaseFlush);
    Finished = WaitForSingleObject(Thread, RingTestWaitTimeoutMs);
    InterlockedExchange(&State.HoldFlush, 0);
    /* Do not release the thread's event/context if a timeout leaves it live. */
    ASSERT(WAIT_OBJECT_0 == Finished);
    CloseHandle(Thread);
    CloseHandle(Work.Finished);
    if (WAIT_OBJECT_0 != Started)
    {
        DWORD DispatcherError;
        SpdStorageUnitGetDispatcherError(State.StorageUnit, &DispatcherError);
        tlib_printf("FLUSH callback not reached: commandError=%lu scsiStatus=%u callbacks=%ld dispatcher=%lu\n",
            (unsigned long)Work.Error, (unsigned)Work.ScsiStatus,
            InterlockedCompareExchange(&State.FlushCallbacks, 0, 0),
            (unsigned long)DispatcherError);
    }
    ASSERT(WAIT_OBJECT_0 == Started);
    ASSERT(WAIT_TIMEOUT == Premature);
    ASSERT(ERROR_SUCCESS == Work.Error);
    ASSERT(SCSISTAT_GOOD == Work.ScsiStatus);
    ASSERT(Before + 1 == InterlockedCompareExchange(&State.FlushCallbacks, 0, 0));
    ASSERT(0 == State.LastFlushBlockAddress);
    ASSERT(0 == State.LastFlushBlockCount);

    /* Zero count also covers the remaining medium from a nonzero/last LBA. */
    const UINT64 Addresses[] = { RingTestBlockAddress, 63 };
    for (UINT32 I = 0; I < ARRAYSIZE(Addresses); I++)
    {
        Before = InterlockedCompareExchange(&State.FlushCallbacks, 0, 0);
        Error = ring_test_scsi_range(&State, DiskHandle, OperationCode,
            Addresses[I], 0, &ScsiStatus, Sense);
        ASSERT(ERROR_SUCCESS == Error);
        ASSERT(SCSISTAT_GOOD == ScsiStatus);
        ASSERT(Before + 1 == InterlockedCompareExchange(&State.FlushCallbacks, 0, 0));
        ASSERT(Addresses[I] == State.LastFlushBlockAddress);
        ASSERT(0 == State.LastFlushBlockCount);
    }

    Before = InterlockedCompareExchange(&State.FlushCallbacks, 0, 0);
    Error = ring_test_scsi_range(&State, DiskHandle, OperationCode,
        63, 1, &ScsiStatus, Sense);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(SCSISTAT_GOOD == ScsiStatus);
    ASSERT(Before + 1 == InterlockedCompareExchange(&State.FlushCallbacks, 0, 0));
    ASSERT(63 == State.LastFlushBlockAddress);
    ASSERT(1 == State.LastFlushBlockCount);

    /* Invalid starting LBA, end beyond capacity, and arithmetic overflow must
     * fail before reaching the backend. */
    const UINT64 InvalidAddresses[] = { 64, 63, (UINT64)-1 };
    const UINT32 InvalidCounts[] = { 0, 2, 1 };
    for (UINT32 I = 0; I < ARRAYSIZE(InvalidAddresses); I++)
    {
        Before = InterlockedCompareExchange(&State.FlushCallbacks, 0, 0);
        Error = ring_test_scsi_range(&State, DiskHandle, OperationCode,
            InvalidAddresses[I], InvalidCounts[I], &ScsiStatus, Sense);
        ASSERT(ERROR_SUCCESS == Error);
        ASSERT(SCSISTAT_CHECK_CONDITION == ScsiStatus);
        ASSERT(SCSI_SENSE_ILLEGAL_REQUEST == ((PSENSE_DATA)Sense)->SenseKey);
        ASSERT(SCSI_ADSENSE_ILLEGAL_BLOCK == ((PSENSE_DATA)Sense)->AdditionalSenseCode);
        ASSERT(Before == InterlockedCompareExchange(&State.FlushCallbacks, 0, 0));
    }

    InterlockedExchange(&State.FlushError, 1);
    Before = InterlockedCompareExchange(&State.FlushCallbacks, 0, 0);
    Error = ring_test_scsi_range(&State, DiskHandle, OperationCode,
        0, 0, &ScsiStatus, Sense);
    InterlockedExchange(&State.FlushError, 0);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(SCSISTAT_CHECK_CONDITION == ScsiStatus);
    ASSERT(SCSI_SENSE_HARDWARE_ERROR == ((PSENSE_DATA)Sense)->SenseKey);
    ASSERT(SCSI_ADSENSE_INTERNAL_TARGET_FAILURE == ((PSENSE_DATA)Sense)->AdditionalSenseCode);
    ASSERT(Before + 1 == InterlockedCompareExchange(&State.FlushCallbacks, 0, 0));

    /* Keep zero-length READ/WRITE no-ops separate from FLUSH forwarding. */
    LONG Reads = InterlockedCompareExchange(&State.TestReadCalls, 0, 0);
    LONG Writes = InterlockedCompareExchange(&State.WriteCallbacks, 0, 0);
    Error = ring_test_scsi_range(&State, DiskHandle, SCSIOP_READ,
        RingTestBlockAddress, 0, &ScsiStatus, Sense);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(SCSISTAT_GOOD == ScsiStatus);
    Error = ring_test_scsi_range(&State, DiskHandle, SCSIOP_WRITE,
        RingTestBlockAddress, 0, &ScsiStatus, Sense);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(SCSISTAT_GOOD == ScsiStatus);
    ASSERT(Reads == InterlockedCompareExchange(&State.TestReadCalls, 0, 0));
    ASSERT(Writes == InterlockedCompareExchange(&State.WriteCallbacks, 0, 0));

    CloseHandle(DiskHandle);
    ring_test_destroy(&State);
}

static void ioctl_ring_zero_count_flush10_test(void)
{
    ring_test_zero_count_flush_dotest(SCSIOP_SYNCHRONIZE_CACHE);
}

static void ioctl_ring_zero_count_flush16_test(void)
{
    ring_test_zero_count_flush_dotest(SCSIOP_SYNCHRONIZE_CACHE16);
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
    RingParams.Version = SPD_RING_VERSION;
    RingParams.QueueDepth = 2;
    RingParams.BufferCount = SPD_RING_DEFAULT_LUN_QUEUE_DEPTH;
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
    RingParams.Version = SPD_RING_VERSION + 1;
    RingParams.QueueDepth = 2;
    RingParams.BufferCount = SPD_RING_DEFAULT_LUN_QUEUE_DEPTH;
    RingParams.BufferSize = 4096;
    ASSERT(ERROR_NOT_SUPPORTED ==
        SpdStorageUnitOpenSharedRing(StorageUnit, &RingParams));

    memset(&RingParams, 0, sizeof RingParams);
    RingParams.Version = SPD_RING_VERSION;
    RingParams.QueueDepth = 3;
    RingParams.BufferCount = SPD_RING_DEFAULT_LUN_QUEUE_DEPTH;
    RingParams.BufferSize = 4096;
    ASSERT(ERROR_INVALID_PARAMETER ==
        SpdStorageUnitOpenSharedRing(StorageUnit, &RingParams));

    RingParams.QueueDepth = SPD_RING_MAX_QUEUE_DEPTH;
    RingParams.BufferCount = 256;
    RingParams.BufferSize = 1024 * 1024;
    ASSERT(ERROR_INVALID_PARAMETER ==
        SpdStorageUnitOpenSharedRing(StorageUnit, &RingParams));

    RingParams.QueueDepth = 2;
    RingParams.BufferCount = SPD_RING_DEFAULT_LUN_QUEUE_DEPTH;
    RingParams.BufferSize = 2048;
    ASSERT(ERROR_INVALID_PARAMETER ==
        SpdStorageUnitOpenSharedRing(StorageUnit, &RingParams));

    RingParams.BufferSize = 4096;
    RingParams.BufferCount = 0;
    ASSERT(ERROR_INVALID_PARAMETER ==
        SpdStorageUnitOpenSharedRing(StorageUnit, &RingParams));
    /* BufferCount is independent of the power-of-two queue depth. */
    RingParams.BufferCount = 3;
    ASSERT(ERROR_SUCCESS ==
        SpdStorageUnitOpenSharedRing(StorageUnit, &RingParams));
    ASSERT(2 == StorageUnit->SharedRingHeader->QueueDepth);
    ASSERT(3 == StorageUnit->SharedRingHeader->BufferCount);
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
    DWORD Error;
    DWORD DispatcherError = ERROR_SUCCESS;

    ring_test_create(&State, &RingTestGuidAsync, 2);
    ring_test_start(&State, 2);
    InterlockedExchange(&State.AsyncMode, RingTestAsyncEarly);
    Error = ring_test_disk_read(&State);
    if (ERROR_SUCCESS != Error)
    {
        SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
        SpdStorageUnitGetDispatcherError(State.StorageUnit,
            &DispatcherError);
        tlib_printf("async early read failed error=%lu dispatcher=%lu failure=%ld readCallbacks=%ld testReads=%ld req=%lu/%lu cq=%lu/%lu\n",
            (unsigned long)Error,
            (unsigned long)DispatcherError,
            InterlockedCompareExchange(&State.Failure, 0, 0),
            InterlockedCompareExchange(&State.ReadCallbacks, 0, 0),
            InterlockedCompareExchange(&State.TestReadCalls, 0, 0),
            (unsigned long)SpdRingLoadAcquire32(
                &Header->RequestHead.Value),
            (unsigned long)SpdRingLoadAcquire32(
                &Header->RequestTail.Value),
            (unsigned long)SpdRingLoadAcquire32(
                &Header->CompletionHead.Value),
            (unsigned long)SpdRingLoadAcquire32(
                &Header->CompletionTail.Value));
        fflush(stdout);
        State.ExpectedDispatcherError = DispatcherError;
        ring_test_destroy(&State);
        ASSERT(ERROR_SUCCESS == Error);
        return;
    }
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
    tlib_printf("batch stage: manual discovery begin\n");
    fflush(stdout);
    ring_test_wait_for_disk_manual(&State);
    tlib_printf("batch stage: manual discovery complete path=%ls\n",
        State.DiskPath);
    fflush(stdout);
    DiskHandle = State.ManualDiskHandle;
    ASSERT(INVALID_HANDLE_VALUE != DiskHandle);
    tlib_printf("batch stage: reusing manually opened disk handle\n");
    fflush(stdout);

    Header = State.StorageUnit->SharedRingHeader;
    RequestRing = (SPD_RING_REQUEST *)
        ((PUINT8)State.StorageUnit->SharedRingAddress +
        Header->RequestOffset);
    CompletionRing = (SPD_RING_COMPLETION *)
        ((PUINT8)State.StorageUnit->SharedRingAddress +
        Header->CompletionOffset);

    /*
     * Disk open and the serial query can leave one service request in
     * flight when their helper thread returns. Retire a completed wait and
     * drain any request it published before taking the target-read baseline.
     */
    if (State.ManualWaitOutstanding &&
        WAIT_OBJECT_0 == WaitForSingleObject(State.ManualWaitEvent, 0))
    {
        memset(&WaitParams, 0, sizeof WaitParams);
        Error = ring_test_finish_wait(&State, &WaitParams, 0);
        ASSERT(ERROR_SUCCESS == Error);
        ASSERT(0 != ring_test_request_available(&State));
    }

    RequestHead = SpdRingLoadAcquire32(&Header->RequestHead.Value);
    RequestTail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    CompletionHead =
        SpdRingLoadAcquire32(&Header->CompletionHead.Value);
    CompletionTail =
        SpdRingLoadAcquire32(&Header->CompletionTail.Value);
    while (RequestHead != RequestTail)
    {
        BOOLEAN TestRead;

        ASSERT(RequestTail - RequestHead <= Header->QueueDepth);
        ASSERT(CompletionTail == CompletionHead);
        ASSERT(1 == RequestTail - RequestHead);
        Error = ring_test_complete_one_manually(&State, &TestRead);
        ASSERT(ERROR_SUCCESS == Error);
        ASSERT(!TestRead);

        RequestHead = SpdRingLoadAcquire32(&Header->RequestHead.Value);
        RequestTail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
        CompletionHead =
            SpdRingLoadAcquire32(&Header->CompletionHead.Value);
        CompletionTail =
            SpdRingLoadAcquire32(&Header->CompletionTail.Value);
    }
    ASSERT(CompletionHead == CompletionTail);

    /* Snapshot an empty request ring before submitting the two target SRBs. */
    RequestHead = SpdRingLoadAcquire32(&Header->RequestHead.Value);
    RequestTail = RequestHead;
    CompletionHead =
        SpdRingLoadAcquire32(&Header->CompletionHead.Value);
    CompletionTail = CompletionHead;

    Error = ring_test_begin_wait(&State);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(ERROR_BUSY == ring_test_submit_second_wait(&State));
    ASSERT(State.ManualWaitOutstanding);

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
        tlib_printf("batch stage: READ[%lu] submit result=%lu\n",
            (unsigned long)I, (unsigned long)Error);
        fflush(stdout);
        ASSERT(ERROR_IO_PENDING == Error);
    }
    tlib_printf("batch stage: reads submitted\n");
    fflush(stdout);

    /*
     * WAIT is notification-only. Arm it before requests arrive, then inspect
     * the SQ and wait for both independently posted SRBs to fill this ring.
     */
    ULONGLONG Deadline = GetTickCount64() + RingTestWaitTimeoutMs;
    DWORD WaitResult = WaitForSingleObject(
        State.ManualWaitEvent, RingTestWaitTimeoutMs);
    ASSERT(WAIT_OBJECT_0 == WaitResult);
    memset(&WaitParams, 0, sizeof WaitParams);
    Error = ring_test_finish_wait(&State, &WaitParams, 0);
    ASSERT(ERROR_SUCCESS == Error);

    RequestTail =
        SpdRingLoadAcquire32(&Header->RequestTail.Value);
    while (RequestTail - RequestHead < Header->QueueDepth &&
        GetTickCount64() < Deadline)
    {
        Sleep(1);
        RequestTail =
            SpdRingLoadAcquire32(&Header->RequestTail.Value);
    }

    ASSERT(2 == RequestTail - RequestHead);
    ASSERT(RequestTail - RequestHead <= Header->QueueDepth);
    ASSERT(CompletionHead == CompletionTail);
    for (UINT32 I = 0; 2 > I; I++)
    {
        SPD_RING_REQUEST *Request = &RequestRing[
            (RequestHead + I) & (Header->QueueDepth - 1)];
        SPD_RING_COMPLETION *Completion = &CompletionRing[
            (CompletionTail + I) & (Header->QueueDepth - 1)];
        tlib_printf("batch request[%lu]: kind=%u hint=%I64u block=%I64u count=%lu buffer=%lu length=%lu\n",
            (unsigned long)I,
            (unsigned)Request->Request.Kind,
            Request->Request.Hint,
            (SpdIoctlTransactReadKind == Request->Request.Kind ?
                Request->Request.Op.Read.BlockAddress : 0),
            (unsigned long)(SpdIoctlTransactReadKind ==
                Request->Request.Kind ?
                Request->Request.Op.Read.BlockCount : 0),
            (unsigned long)Request->Data.BufferId,
            (unsigned long)Request->Data.Length);
        ASSERT(SpdIoctlTransactReadKind == Request->Request.Kind);
        ASSERT(SPD_RING_NO_BUFFER != Request->Data.BufferId);
        BufferIds[I] = Request->Data.BufferId;
        memset((PUINT8)State.StorageUnit->SharedRingAddress +
            Header->BufferOffset +
            (SIZE_T)Request->Data.BufferId * Header->BufferSize,
            0x5a, Request->Data.Length);
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

    /*
     * If Storport has already aborted either SRB, its overlapped read will
     * have completed before RING_KICK. Capture that distinction before a
     * failed kick resets the IOQ and completes any still-pending reads.
     */
    for (UINT32 I = 0; ARRAYSIZE(Overlapped) > I; I++)
    {
        DWORD PendingBytes = 0;
        if (GetOverlappedResult(DiskHandle, &Overlapped[I],
                &PendingBytes, FALSE))
            tlib_printf("batch READ[%lu] completed before kick bytes=%lu\n",
                (unsigned long)I, (unsigned long)PendingBytes);
        else
            tlib_printf("batch READ[%lu] pre-kick status error=%lu\n",
                (unsigned long)I, (unsigned long)GetLastError());
    }

    memset(&KickParams, 0, sizeof KickParams);
    Error = SpdIoctlRingKick(State.StorageUnit->Handle,
        State.StorageUnit->Btl, &KickParams);
    if (ERROR_SUCCESS != Error)
        tlib_printf("batch RING_KICK failed error=%lu consumed=%lu\n",
            (unsigned long)Error, (unsigned long)KickParams.Consumed);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(2 == KickParams.Consumed);
    for (UINT32 I = 0; 2 > I; I++)
    {
        DWORD WaitResult = WaitForSingleObject(
            Overlapped[I].hEvent, 30000);
        BOOL ReadCompleted = WAIT_OBJECT_0 == WaitResult &&
            GetOverlappedResult(DiskHandle, &Overlapped[I],
                &BytesRead, FALSE);

        if (!ReadCompleted)
        {
            Error = WAIT_OBJECT_0 == WaitResult ? GetLastError() :
                WAIT_TIMEOUT == WaitResult ? ERROR_TIMEOUT :
                GetLastError();
            SpdStorageUnitShutdown(State.StorageUnit);
            tlib_printf("batch READ[%lu] completion failed error=%lu bytes=%lu kickConsumed=%lu req=%lu/%lu cq=%lu/%lu\n",
                (unsigned long)I,
                (unsigned long)Error,
                (unsigned long)BytesRead,
                (unsigned long)KickParams.Consumed,
                (unsigned long)SpdRingLoadAcquire32(
                    &Header->RequestHead.Value),
                (unsigned long)SpdRingLoadAcquire32(
                    &Header->RequestTail.Value),
                (unsigned long)SpdRingLoadAcquire32(
                    &Header->CompletionHead.Value),
                    (unsigned long)SpdRingLoadAcquire32(
                        &Header->CompletionTail.Value));
            tlib_printf("batch READ[%lu] timeout cleanup requested ring shutdown\n",
                (unsigned long)I);
            CancelIoEx(DiskHandle, &Overlapped[I]);
            CancelIoEx(DiskHandle, &Overlapped[1 - I]);
            WaitForSingleObject(Overlapped[I].hEvent, 5000);
            WaitForSingleObject(Overlapped[1 - I].hEvent, 5000);
            CloseHandle(Overlapped[I].hEvent);
            CloseHandle(Overlapped[1 - I].hEvent);
            ring_test_destroy(&State);
            ASSERT(FALSE);
        }
        ASSERT(RingTestDataLength == BytesRead);
        for (UINT32 J = 0; RingTestDataLength > J; J++)
            ASSERT(0x5a == DataBuffers[I][J]);
        CloseHandle(Overlapped[I].hEvent);
    }
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
        ASSERT(ERROR_SUCCESS == ring_test_wait(&State, &WaitParams,
            RingTestWaitTimeoutMs));
        ASSERT(0 != ring_test_request_available(&State));
        ASSERT(State.LastBufferId < State.BufferCount);
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
    ASSERT(ERROR_SUCCESS == ring_test_wait(&State, &WaitParams,
        RingTestWaitTimeoutMs));
    ASSERT(0 != ring_test_request_available(&State));

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
    DWORD SlowWaitResult = WaitForSingleObject(SlowThread, 10000);
    if (WAIT_OBJECT_0 != SlowWaitResult)
    {
        SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
        DWORD DispatcherError = ERROR_SUCCESS;
        SpdStorageUnitGetDispatcherError(State.StorageUnit,
            &DispatcherError);
        tlib_printf("out-of-order slow read stalled: wait=%lu dispatcher=%lu failure=%ld readCallbacks=%ld testReads=%ld req=%lu/%lu cq=%lu/%lu\n",
            (unsigned long)SlowWaitResult,
            (unsigned long)DispatcherError,
            InterlockedCompareExchange(&State.Failure, 0, 0),
            InterlockedCompareExchange(&State.ReadCallbacks, 0, 0),
            InterlockedCompareExchange(&State.TestReadCalls, 0, 0),
            (unsigned long)SpdRingLoadAcquire32(
                &Header->RequestHead.Value),
            (unsigned long)SpdRingLoadAcquire32(
                &Header->RequestTail.Value),
            (unsigned long)SpdRingLoadAcquire32(
                &Header->CompletionHead.Value),
            (unsigned long)SpdRingLoadAcquire32(
                &Header->CompletionTail.Value));
        fflush(stdout);
    }
    ASSERT(WAIT_OBJECT_0 == SlowWaitResult);
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
    DWORD Error;

    ring_test_create(&State, &RingTestGuidReadWrite, 4);
    ring_test_start(&State, 2);
    ASSERT(ERROR_SUCCESS == ring_test_disk_write(&State));
    ASSERT(0 < InterlockedCompareExchange(&State.WriteCallbacks, 0, 0));
    ASSERT(ERROR_SUCCESS == ring_test_disk_read(&State));
    Error = ring_test_disk_read_large(&State);
    if (ERROR_SUCCESS != Error)
    {
        tlib_printf("ring large-read test failed error=%lu callbacks read=%ld write=%ld failure=%ld\n",
            (unsigned long)Error,
            InterlockedCompareExchange(&State.ReadCallbacks, 0, 0),
            InterlockedCompareExchange(&State.WriteCallbacks, 0, 0),
            InterlockedCompareExchange(&State.Failure, 0, 0));
        ring_test_destroy(&State);
    }
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(0 == InterlockedCompareExchange(&State.Failure, 0, 0));
    ring_test_destroy(&State);
}

static void ring_test_saturation_case(const GUID *Guid,
    UINT32 BufferCount)
{
    RING_TEST_STATE State;
    RING_TEST_READ_THREAD Reads[3];
    HANDLE Threads[3];

    ring_test_create_with_buffer_count(&State, Guid, 2, BufferCount, FALSE);
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

static void ioctl_ring_saturation_test(void)
{
    /* The two-entry SQ fills; the third posted request resumes after the pump
     * drains the SQ and registers WAIT again. */
    ring_test_saturation_case(&RingTestGuidSaturation,
        SPD_RING_DEFAULT_LUN_QUEUE_DEPTH);
}

static void ioctl_ring_buffer_exhaustion_test(void)
{
    /* Two shared buffers back three outstanding reads. The third request
     * waits for KICK to return a buffer, then resumes without ring failure. */
    ring_test_saturation_case(&RingTestGuidBufferExhaustion, 2);
}

static void ioctl_ring_cq_saturation_test(void)
{
    enum { ReadCount = 16 };
    RING_TEST_STATE State;
    RING_TEST_READ_THREAD Reads[ReadCount];
    HANDLE Threads[ReadCount];
    ULONGLONG Deadline;

    ring_test_create(&State, &RingTestGuidCqSaturation, 2);
    ring_test_start(&State, ReadCount);
    InterlockedExchange(&State.HoldReads, 1);
    memset(Reads, 0, sizeof Reads);
    memset(Threads, 0, sizeof Threads);
    for (UINT32 I = 0; I < ReadCount; I++)
    {
        Reads[I].State = &State;
        Reads[I].BlockAddress = RingTestBlockAddress + I;
        Reads[I].Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Reads[I].Finished);
        Threads[I] = CreateThread(0, 0,
            ring_test_disk_read_thread, &Reads[I], 0, 0);
        ASSERT(0 != Threads[I]);
    }

    Deadline = GetTickCount64() + 10000;
    while (ReadCount > (UINT32)InterlockedCompareExchange(
            &State.TestReadCalls, 0, 0) &&
        GetTickCount64() < Deadline)
        Sleep(1);
    ASSERT(ReadCount == (UINT32)InterlockedCompareExchange(
        &State.TestReadCalls, 0, 0));

    SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
    UINT32 RequestTail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    UINT32 RequestHead = SpdRingLoadAcquire32(&Header->RequestHead.Value);
    UINT32 CompletionTail =
        SpdRingLoadAcquire32(&Header->CompletionTail.Value);
    UINT32 CompletionHead =
        SpdRingLoadAcquire32(&Header->CompletionHead.Value);
    ASSERT(RequestTail - RequestHead <= State.QueueDepth);
    ASSERT(CompletionTail - CompletionHead <= State.QueueDepth);

    /* Releasing a burst larger than the CQ must eventually retire every
     * response even when the pump has to retry publication after KICK. */
    InterlockedExchange(&State.HoldReads, 0);
    SetEvent(State.ReleaseReads);
    ASSERT(WAIT_OBJECT_0 == WaitForMultipleObjects(ReadCount,
        Threads, TRUE, 15000));
    for (UINT32 I = 0; I < ReadCount; I++)
    {
        ASSERT(ERROR_SUCCESS == Reads[I].Error);
        CloseHandle(Threads[I]);
        CloseHandle(Reads[I].Finished);
    }
    ASSERT(0 == InterlockedCompareExchange(&State.Failure, 0, 0));
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

static void ioctl_ring_wait_notification_test(void)
{
    RING_TEST_STATE State;
    RING_TEST_READ_THREAD Reads[4];
    HANDLE Threads[4];
    UINT32 CompletedTestReads = 0;

    ring_test_create(&State, &RingTestGuidWaitNotification, 4);
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
        Error = ring_test_wait(&State, &WaitParams,
            RingTestWaitTimeoutMs);
        if (ERROR_SUCCESS != Error)
        {
            tlib_printf("notification RING_WAIT failed: error=%lu (0x%08lx), sq_available=%lu, completed=%lu\n",
                (unsigned long)Error, (unsigned long)Error,
                (unsigned long)ring_test_request_available(&State),
                (unsigned long)CompletedTestReads);
            ASSERT(ERROR_SUCCESS == Error);
            WaitFailed = TRUE;
            break;
        }
        ASSERT(0 != ring_test_request_available(&State));
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(
            &State, &TestRead));
        if (TestRead)
            CompletedTestReads++;

        /* A second WAIT installed after SQ publication must complete from
         * the DPC's post-registration recheck. */
        if (1 == CompletedTestReads)
        {
            ULONGLONG Deadline = GetTickCount64() +
                RingTestWaitTimeoutMs;
            while (0 == ring_test_request_available(&State) &&
                GetTickCount64() < Deadline)
                Sleep(1);
            ASSERT(0 != ring_test_request_available(&State));
            ASSERT(ERROR_SUCCESS == ring_test_begin_wait(&State));
            ASSERT(ERROR_SUCCESS == ring_test_finish_wait(&State,
                &WaitParams, RingTestWaitTimeoutMs));
            ASSERT(0 != ring_test_request_available(&State));
            ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(
                &State, &TestRead));
            ASSERT(TestRead);
            CompletedTestReads++;
        }
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

static void ioctl_ring_flush_file_buffers_test(void)
{
    const GUID Guid = { 0x51aeb09a, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x5a } };
    RING_TEST_STATE State;
    RING_TEST_FLUSH_THREAD Work = { 0 };
    ring_test_create(&State, &Guid, 16);
    ring_test_start(&State, 4);
    HANDLE Disk = CreateFileW(State.DiskPath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    State.FlushStarted = CreateEventW(0, TRUE, FALSE, 0);
    State.ReleaseFlush = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != State.FlushStarted && 0 != State.ReleaseFlush);
    InterlockedExchange(&State.HoldFlush, 1);
    Work.DiskHandle = Disk;
    Work.FileBuffers = TRUE;
    Work.Finished = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != Work.Finished);
    HANDLE Thread = CreateThread(0, 0, ring_test_flush_thread, &Work, 0, 0);
    ASSERT(0 != Thread);
    DWORD Started = WaitForSingleObject(State.FlushStarted, RingTestWaitTimeoutMs);
    DWORD Finished = WaitForSingleObject(Work.Finished, 0);
    ASSERT(SetEvent(State.ReleaseFlush));
    ASSERT(WAIT_OBJECT_0 == Started && WAIT_TIMEOUT == Finished);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Work.Finished, RingTestWaitTimeoutMs));
    ASSERT(ERROR_SUCCESS == Work.Error);
#if defined(WINSPD_TEST_BUILD)
    ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_NONCDB_SEEN, 0, 0, 0));
#endif
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Thread, RingTestWaitTimeoutMs));
    InterlockedExchange(&State.HoldFlush, 0);
    CloseHandle(Thread);
    CloseHandle(Work.Finished);
    CloseHandle(Disk);
    ring_test_destroy(&State);
}

#if defined(WINSPD_TEST_BUILD)
typedef struct _RING_TEST_STOP_THREAD
{
    RING_TEST_STATE *State;
    HANDLE Finished;
    DWORD Error;
} RING_TEST_STOP_THREAD;

typedef struct _RING_TEST_COMPLETE_THREAD
{
    RING_TEST_STATE *State;
    HANDLE Finished;
    DWORD Error;
    BOOLEAN TestRead;
} RING_TEST_COMPLETE_THREAD;

static HANDLE ring_test_open_overlapped_disk(
    RING_TEST_STATE *State, DWORD Access)
{
    return CreateFileW(State->DiskPath, Access,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, 0);
}

typedef struct
{
    SCSI_PASS_THROUGH_DIRECT Base;
    __declspec(align(16)) UINT8 Sense[32];
} RING_TEST_SCSI_PACKET;

typedef struct
{
    OVERLAPPED Overlapped;
    RING_TEST_SCSI_PACKET Packet;
} RING_TEST_SCSI_IO;

/* Ownership tests submit explicit commands instead of disk-class READ/WRITE
 * IRPs: disk.sys may retry an aborted SRB as a new request. Keep the packet
 * and payload alive until its overlapped completion, as required by SPTD. */
static DWORD ring_test_begin_scsi_io(
    HANDLE DiskHandle, BOOLEAN Write, UINT64 BlockAddress,
    PVOID Buffer, DWORD Length, RING_TEST_SCSI_IO *Io)
{
    DWORD Transferred = 0;
    UINT32 Blocks = Length / RingTestDataLength;
    BOOL Success;

    memset(Io, 0, sizeof *Io);
    Io->Overlapped.hEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (0 == Io->Overlapped.hEvent)
        return GetLastError();
    Io->Packet.Base.Length = sizeof Io->Packet.Base;
    Io->Packet.Base.CdbLength = 16;
    Io->Packet.Base.SenseInfoLength = sizeof Io->Packet.Sense;
    Io->Packet.Base.SenseInfoOffset = FIELD_OFFSET(RING_TEST_SCSI_PACKET, Sense);
    Io->Packet.Base.TimeOutValue = 10;
    Io->Packet.Base.DataIn = Write ? SCSI_IOCTL_DATA_OUT : SCSI_IOCTL_DATA_IN;
    Io->Packet.Base.DataTransferLength = Length;
    Io->Packet.Base.DataBuffer = Buffer;
    Io->Packet.Base.Cdb[0] = Write ? SCSIOP_WRITE16 : SCSIOP_READ16;
    for (UINT32 I = 0; I < 8; I++)
        Io->Packet.Base.Cdb[2 + I] = (UINT8)(BlockAddress >> (8 * (7 - I)));
    for (UINT32 I = 0; I < 4; I++)
        Io->Packet.Base.Cdb[10 + I] = (UINT8)(Blocks >> (8 * (3 - I)));
    Success = DeviceIoControl(DiskHandle, IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &Io->Packet, sizeof Io->Packet, &Io->Packet, sizeof Io->Packet,
        &Transferred, &Io->Overlapped);
    if (Success)
        return ERROR_SUCCESS;
    DWORD Error = GetLastError();
    return ERROR_IO_PENDING == Error ? ERROR_SUCCESS : Error;
}

static DWORD ring_test_finish_overlapped_io(
    HANDLE DiskHandle, OVERLAPPED *Overlapped, DWORD Timeout)
{
    DWORD Transferred = 0;
    if (WAIT_OBJECT_0 != WaitForSingleObject(Overlapped->hEvent, Timeout))
        return ERROR_TIMEOUT;
    return GetOverlappedResult(DiskHandle, Overlapped,
        &Transferred, FALSE) ? ERROR_SUCCESS : GetLastError();
}

static DWORD ring_test_begin_file_io(HANDLE DiskHandle, BOOLEAN Write, UINT64 BlockAddress,
    PVOID Buffer, DWORD Length, OVERLAPPED *Io)
{
    UINT64 Offset = BlockAddress * RingTestDataLength;
    DWORD Transferred;
    memset(Io, 0, sizeof *Io);
    Io->hEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (0 == Io->hEvent)
        return GetLastError();
    Io->Offset = (DWORD)Offset;
    Io->OffsetHigh = (DWORD)(Offset >> 32);
    if (Write ? WriteFile(DiskHandle, Buffer, Length, &Transferred, Io) :
            ReadFile(DiskHandle, Buffer, Length, &Transferred, Io))
        return ERROR_SUCCESS;
    DWORD Error = GetLastError();
    return ERROR_IO_PENDING == Error ? ERROR_SUCCESS : Error;
}

static DWORD ring_test_begin_file_read(HANDLE DiskHandle, UINT64 BlockAddress,
    PVOID Buffer, DWORD Length, OVERLAPPED *Io)
{
    return ring_test_begin_file_io(DiskHandle, FALSE, BlockAddress, Buffer, Length, Io);
}

static DWORD ring_test_finish_scsi_io(HANDLE DiskHandle,
    RING_TEST_SCSI_IO *Io, DWORD Timeout)
{
    DWORD Error = ring_test_finish_overlapped_io(DiskHandle,
        &Io->Overlapped, Timeout);
    if (ERROR_SUCCESS == Error && SCSISTAT_GOOD != Io->Packet.Base.ScsiStatus)
        Error = ERROR_IO_DEVICE;
    return Error;
}

static BOOLEAN ring_test_scsi_aborted(DWORD Error)
{
    /* Pass-through maps SRB_STATUS_ABORTED to ERROR_IO_DEVICE on this stack.
     * The ownership tests also require successful cancellation of the selected
     * worker-owned SRB and verify that its descriptor was never published. */
    return ERROR_OPERATION_ABORTED == Error || ERROR_CANCELLED == Error ||
        ERROR_IO_DEVICE == Error;
}

static DWORD WINAPI ring_test_stop_ring_thread(PVOID Data)
{
    RING_TEST_STOP_THREAD *Work = Data;
    Work->Error = ring_test_stop_ring(Work->State);
    SetEvent(Work->Finished);
    return Work->Error;
}

static DWORD ring_test_owned_control(RING_TEST_STATE *State, UINT16 Code,
    UINT32 Kind, UINT64 BlockAddress, UINT32 ExpectedState)
{
    HANDLE CommandHandle;
    OVERLAPPED Overlapped;
    SPD_IOCTL_TEST_CANCEL_PARAMS Params;
    DWORD Error, BytesReturned;
    Error = SpdIoctlOpenDevice(L"" SPD_IOCTL_HARDWARE_ID, &CommandHandle);
    if (ERROR_SUCCESS != Error)
        return Error;
    memset(&Overlapped, 0, sizeof Overlapped);
    Overlapped.hEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (0 == Overlapped.hEvent)
    {
        Error = GetLastError();
        CloseHandle(CommandHandle);
        return Error;
    }
    memset(&Params, 0, sizeof Params);
    Params.Base.Size = sizeof Params;
    Params.Base.Code = Code;
    Params.Btl = State->StorageUnit->Btl;
    Params.Kind = Kind;
    Params.BlockAddress = BlockAddress;
    Params.ExpectedState = ExpectedState;
    if (DeviceIoControl(CommandHandle, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
            &Params, sizeof Params, 0, 0, &BytesReturned, &Overlapped))
        Error = ERROR_SUCCESS;
    else if (ERROR_IO_PENDING == (Error = GetLastError()))
        Error = ring_test_finish_overlapped_io(CommandHandle, &Overlapped,
            RingTestWaitTimeoutMs);
    CloseHandle(Overlapped.hEvent);
    CloseHandle(CommandHandle);
    return Error;
}

static DWORD ring_test_cancel_owned_srb(RING_TEST_STATE *State,
    UINT32 Kind, UINT64 BlockAddress, UINT32 ExpectedState)
{
    return ring_test_owned_control(State, SPD_IOCTL_TEST_CANCEL,
        Kind, BlockAddress, ExpectedState);
}

static DWORD ring_test_reset_device(RING_TEST_STATE *State)
{
    return ring_test_owned_control(State, SPD_IOCTL_TEST_RESET, 0, 0, 0);
}

static DWORD WINAPI ring_test_complete_one_thread(PVOID Data)
{
    RING_TEST_COMPLETE_THREAD *Work = Data;
    Work->Error = ring_test_complete_one_manually(
        Work->State, &Work->TestRead);
    SetEvent(Work->Finished);
    return Work->Error;
}

static DWORD ring_test_wait_for_sq_count(
    RING_TEST_STATE *State, UINT32 Expected)
{
    SPD_RING_HEADER *Header = State->StorageUnit->SharedRingHeader;
    ULONGLONG Deadline = GetTickCount64() + RingTestWaitTimeoutMs;
    UINT32 Available;

    do
    {
        UINT32 Head = SpdRingLoadAcquire32(&Header->RequestHead.Value);
        UINT32 Tail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
        Available = Tail - Head;
        if (Available >= Expected)
            return Available;
        Sleep(1);
    } while (GetTickCount64() < Deadline);
    return Available;
}

static DWORD ring_test_write_prepare_lease_case(
    const GUID *Guid, LONG Action)
{
    RING_TEST_STATE State;
    HANDLE DiskHandle;
    RING_TEST_SCSI_IO Overlapped;
    UINT8 WriteData[RingTestDataLength];
    SPD_RING_HEADER *Header;
    UINT32 TailBefore;
    DWORD Error;

    ring_test_create(&State, Guid, 16);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    ASSERT(0 == ring_test_request_available(&State));
    DiskHandle = ring_test_open_overlapped_disk(&State,
        GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != DiskHandle);
    memset(WriteData, 0x3c, sizeof WriteData);
    ASSERT(ResetEvent(State.ProducerPrepareEntered));
    ASSERT(ResetEvent(State.ProducerPrepareRelease));
    Header = State.StorageUnit->SharedRingHeader;
    TailBefore = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    Error = ring_test_begin_scsi_io(DiskHandle, TRUE,
        RingTestBlockAddress, WriteData, sizeof WriteData, &Overlapped);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(
        State.ProducerPrepareEntered, RingTestWaitTimeoutMs));
    ASSERT(TailBefore == SpdRingLoadAcquire32(&Header->RequestTail.Value));

    if (0 == Action)
    {
        ASSERT(CancelIoEx(DiskHandle, &Overlapped.Overlapped));
        ASSERT(ERROR_SUCCESS == ring_test_cancel_owned_srb(&State,
            SpdIoctlTransactWriteKind, RingTestBlockAddress,
            SPD_TEST_CANCEL_PREPARING));
    }
    else if (1 == Action)
    {
        /* Reset must return while the producer still owns the lease. */
        ASSERT(ERROR_SUCCESS == ring_test_reset_device(&State));
    }
    else
    {
        RING_TEST_STOP_THREAD Stop;
        HANDLE StopThread;
        memset(&Stop, 0, sizeof Stop);
        Stop.State = &State;
        Stop.Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Stop.Finished);
        StopThread = CreateThread(0, 0,
            ring_test_stop_ring_thread, &Stop, 0, 0);
        ASSERT(0 != StopThread);
        ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Stop.Finished, 100));
        ASSERT(TailBefore == SpdRingLoadAcquire32(
            &Header->RequestTail.Value));
        ASSERT(SetEvent(State.ProducerPrepareRelease));
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(
            Stop.Finished, RingTestWaitTimeoutMs));
        ASSERT(ERROR_SUCCESS == Stop.Error);
        WaitForSingleObject(StopThread, RingTestWaitTimeoutMs);
        CloseHandle(StopThread);
        CloseHandle(Stop.Finished);
    }

    /* Cancellation flags an owned lease; retirement waits for the copy owner
     * to return. Release that owner before waiting for final I/O completion. */
    if (2 != Action)
        ASSERT(SetEvent(State.ProducerPrepareRelease));
    Error = ring_test_finish_scsi_io(
        DiskHandle, &Overlapped, RingTestWaitTimeoutMs);
    tlib_printf("prepare lease action=%ld completion error=%lu tail=%lu/%lu\n",
        Action, (unsigned long)Error, (unsigned long)TailBefore,
        (unsigned long)SpdRingLoadAcquire32(&Header->RequestTail.Value));
    ASSERT(ring_test_scsi_aborted(Error));
    ASSERT(TailBefore == SpdRingLoadAcquire32(&Header->RequestTail.Value));
    CloseHandle(Overlapped.Overlapped.hEvent);
    CloseHandle(DiskHandle);
    ring_test_destroy(&State);
    return ERROR_SUCCESS;
}

static void ioctl_ring_cancel_during_write_prepare_test(void)
{
    ASSERT(ERROR_SUCCESS == ring_test_write_prepare_lease_case(
        &RingTestGuidLeaseCancelWrite, 0));
}

static void ioctl_ring_reset_during_write_prepare_test(void)
{
    ASSERT(ERROR_SUCCESS == ring_test_write_prepare_lease_case(
        &RingTestGuidLeaseResetWrite, 1));
}

static void ioctl_ring_stop_during_write_prepare_test(void)
{
    ASSERT(ERROR_SUCCESS == ring_test_write_prepare_lease_case(
        &RingTestGuidLeaseStopWrite, 2));
}

static void ring_test_read_completion_lease_case(
    const GUID *Guid, UINT32 Action)
{
    RING_TEST_STATE State;
    HANDLE DiskHandle;
    RING_TEST_SCSI_IO Overlapped;
    UINT8 ReadData[RingTestDataLength];
    RING_TEST_COMPLETE_THREAD Complete;
    HANDLE CompleteThread;
    RING_TEST_STOP_THREAD Stop = { 0 };
    HANDLE StopThread = 0;
    SPD_RING_HEADER *Header;
    DWORD Error;

    ring_test_create(&State, Guid, 4);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    ASSERT(0 == ring_test_request_available(&State));
    DiskHandle = ring_test_open_overlapped_disk(&State,
        GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != DiskHandle);
    ASSERT(ResetEvent(State.CompletionCopyEntered));
    ASSERT(ResetEvent(State.CompletionCopyRelease));
    Error = ring_test_begin_scsi_io(DiskHandle, FALSE,
        RingTestBlockAddress, ReadData, sizeof ReadData, &Overlapped);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(1 == ring_test_wait_for_sq_count(&State, 1));

    memset(&Complete, 0, sizeof Complete);
    Complete.State = &State;
    Complete.Finished = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != Complete.Finished);
    CompleteThread = CreateThread(0, 0,
        ring_test_complete_one_thread, &Complete, 0, 0);
    ASSERT(0 != CompleteThread);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(
        State.CompletionCopyEntered, RingTestWaitTimeoutMs));
    /* KICK acknowledges CQ consumption while the READ copy remains parked in
     * its completion worker. The I/O must still belong to that copy owner. */
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Complete.Finished, RingTestWaitTimeoutMs));
    ASSERT(ERROR_SUCCESS == Complete.Error);
    ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Overlapped.Overlapped.hEvent, 0));
    if (1 == Action)
        ASSERT(ERROR_SUCCESS == ring_test_reset_device(&State));
    else if (2 == Action)
    {
        ASSERT(ERROR_SUCCESS == ring_test_begin_wait(&State));
        Stop.State = &State;
        Stop.Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Stop.Finished);
        StopThread = CreateThread(0, 0, ring_test_stop_ring_thread, &Stop, 0, 0);
        ASSERT(0 != StopThread);
        SPD_IOCTL_RING_WAIT_PARAMS Wait;
        DWORD WaitError = ring_test_finish_wait(&State, &Wait, RingTestWaitTimeoutMs);
        ASSERT(ERROR_OPERATION_ABORTED == WaitError || ERROR_CANCELLED == WaitError);
        ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Stop.Finished, 0));
    }
    else
    {
        ASSERT(CancelIoEx(DiskHandle, &Overlapped.Overlapped));
        ASSERT(ERROR_SUCCESS == ring_test_cancel_owned_srb(&State,
            SpdIoctlTransactReadKind, RingTestBlockAddress,
            SPD_TEST_CANCEL_COMPLETING));
    }
    ASSERT(SetEvent(State.CompletionCopyRelease));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(
        Complete.Finished, RingTestWaitTimeoutMs));
    ASSERT(ERROR_SUCCESS == Complete.Error);
    ASSERT(Complete.TestRead);
    Error = ring_test_finish_scsi_io(
        DiskHandle, &Overlapped, RingTestWaitTimeoutMs);
    ASSERT(ring_test_scsi_aborted(Error));
    Header = State.StorageUnit->SharedRingHeader;
    ASSERT(0 == SpdRingLoadAcquire32(&Header->CompletionTail.Value) -
        SpdRingLoadAcquire32(&Header->CompletionHead.Value));
    ASSERT(0 == InterlockedCompareExchange(&State.Failure, 0, 0));
    WaitForSingleObject(CompleteThread, RingTestWaitTimeoutMs);
    CloseHandle(CompleteThread);
    CloseHandle(Complete.Finished);
    CloseHandle(Overlapped.Overlapped.hEvent);
    if (0 != StopThread)
    {
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Stop.Finished, RingTestWaitTimeoutMs));
        ASSERT(ERROR_SUCCESS == Stop.Error);
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(StopThread, RingTestWaitTimeoutMs));
        CloseHandle(StopThread);
        CloseHandle(Stop.Finished);
    }
    CloseHandle(DiskHandle);
    ring_test_destroy(&State);
}

static void ioctl_ring_cancel_during_read_copy_test(void)
{
    ring_test_read_completion_lease_case(
        &RingTestGuidLeaseCancelRead, FALSE);
}

static void ioctl_ring_reset_during_read_copy_test(void)
{
    ring_test_read_completion_lease_case(
        &RingTestGuidLeaseResetRead, TRUE);
}

static void ioctl_ring_stop_during_read_copy_test(void)
{
    const GUID Guid = { 0x51aeb0a4, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x64 } };
    ring_test_read_completion_lease_case(&Guid, 2);
}

static void ioctl_ring_batch_abort_compaction_test(void)
{
    enum { RequestCount = 8, CancelledCount = 1 };
    RING_TEST_STATE State;
    HANDLE DiskHandle;
    OVERLAPPED Overlapped[RequestCount];
    RING_TEST_SCSI_IO CancelledIo;
    UINT8 ReadData[RequestCount][RingTestDataLength];
    DWORD BytesRead;
    SPD_RING_HEADER *Header;
    UINT32 BaseHead;
    UINT32 BaseTail;

    ring_test_create(&State, &RingTestGuidBatchAbortCompact, 16);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    ASSERT(0 == ring_test_request_available(&State));
    DiskHandle = ring_test_open_overlapped_disk(&State,
        GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != DiskHandle);
    Header = State.StorageUnit->SharedRingHeader;
    BaseHead = SpdRingLoadAcquire32(&Header->RequestHead.Value);
    BaseTail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    ASSERT(BaseHead == BaseTail);
    ASSERT(ResetEvent(State.ProducerPrepareEntered));
    ASSERT(ResetEvent(State.ProducerPrepareRelease));

    for (UINT32 I = 0; I < RequestCount; I++)
    {
        /* The cancelled command uses pass-through to prevent class retries;
         * surviving reads use the file path to permit concurrent dispatch. */
        DWORD Error = 0 == I ?
            ring_test_begin_scsi_io(DiskHandle, FALSE, RingTestBlockAddress,
                ReadData[I], sizeof ReadData[I], &CancelledIo) :
            ring_test_begin_file_read(DiskHandle, RingTestBlockAddress + I,
                ReadData[I], sizeof ReadData[I], &Overlapped[I]);
        ASSERT(ERROR_SUCCESS == Error);
        if (0 == I)
            ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(
                State.ProducerPrepareEntered, RingTestWaitTimeoutMs));
    }
    ASSERT(CancelIoEx(DiskHandle, &CancelledIo.Overlapped));
    ASSERT(ERROR_SUCCESS == ring_test_cancel_owned_srb(&State,
        SpdIoctlTransactReadKind, RingTestBlockAddress,
        SPD_TEST_CANCEL_PREPARING));
    ASSERT(SetEvent(State.ProducerPrepareRelease));
    ASSERT(RequestCount - CancelledCount ==
        ring_test_wait_for_sq_count(&State,
            RequestCount - CancelledCount));
    ASSERT(RequestCount - CancelledCount ==
        SpdRingLoadAcquire32(&Header->RequestTail.Value) -
        SpdRingLoadAcquire32(&Header->RequestHead.Value));

    for (UINT32 I = 0; I < RequestCount - CancelledCount; I++)
    {
        BOOLEAN TestRead;
        ASSERT(0 != ring_test_request_available(&State));
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(
            &State, &TestRead));
        ASSERT(TestRead);
    }
    for (UINT32 I = 0; I < RequestCount; I++)
    {
        DWORD Error = 0 == I ?
            ring_test_finish_scsi_io(DiskHandle, &CancelledIo,
                RingTestWaitTimeoutMs) :
            ring_test_finish_overlapped_io(DiskHandle, &Overlapped[I],
                RingTestWaitTimeoutMs);
        if (0 == I)
            ASSERT(ring_test_scsi_aborted(Error));
        else
        {
            ASSERT(ERROR_SUCCESS == Error);
            ASSERT(GetOverlappedResult(DiskHandle, &Overlapped[I],
                &BytesRead, FALSE));
            ASSERT(RingTestDataLength == BytesRead);
        }
        CloseHandle(0 == I ? CancelledIo.Overlapped.hEvent : Overlapped[I].hEvent);
    }
    ASSERT(0 == InterlockedCompareExchange(&State.Failure, 0, 0));
    CloseHandle(DiskHandle);
    ring_test_destroy(&State);
}

/* Park the publisher, then park preparation of another request. Publishing
 * and completing the first request must not depend on the preparation callback. */
static void ioctl_ring_independent_preparation_test(void)
{
    const GUID Guid =
        { 0x51aeb070, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x30 } };
    RING_TEST_STATE State;
    OVERLAPPED Io[2];
    UINT8 Data[2][RingTestDataLength];
    BOOLEAN TestRead;
    ring_test_create(&State, &Guid, 4);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    ASSERT(ResetEvent(State.ReadyEntered));
    ASSERT(ResetEvent(State.ReadyRelease));
    ASSERT(ERROR_SUCCESS == ring_test_begin_file_read(Disk,
        RingTestBlockAddress, Data[0], sizeof Data[0], &Io[0]));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.ReadyEntered, RingTestWaitTimeoutMs));
    ASSERT(ResetEvent(State.ProducerPrepareEntered));
    ASSERT(ResetEvent(State.ProducerPrepareRelease));
    ASSERT(ERROR_SUCCESS == ring_test_begin_file_read(Disk,
        RingTestBlockAddress + 1, Data[1], sizeof Data[1], &Io[1]));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.ProducerPrepareEntered, RingTestWaitTimeoutMs));
    ASSERT(SetEvent(State.ReadyRelease));
    ASSERT(1 == ring_test_wait_for_sq_count(&State, 1));
    ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
    ASSERT(TestRead);
    ASSERT(ERROR_SUCCESS == ring_test_finish_overlapped_io(Disk, &Io[0], RingTestWaitTimeoutMs));
    ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Io[1].hEvent, 0));
    ASSERT(SetEvent(State.ProducerPrepareRelease));
    ASSERT(1 == ring_test_wait_for_sq_count(&State, 1));
    ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
    ASSERT(TestRead);
    ASSERT(ERROR_SUCCESS == ring_test_finish_overlapped_io(Disk, &Io[1], RingTestWaitTimeoutMs));
    for (UINT32 I = 0; I < ARRAYSIZE(Io); I++)
        CloseHandle(Io[I].hEvent);
    CloseHandle(Disk);
    ring_test_destroy(&State);
}

/* Gather pending I/O behind one parked preparation, then pause a non-final
 * ready commit. Its published prefix must complete while the suffix is owned
 * by preparation. Reset must retire both ownership classes exactly once. */
static void ring_test_partial_prepare_commit_case(BOOLEAN Reset)
{
    enum { RequestCount = 49 };
    const GUID Guid =
        { 0x51aeb071, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x31 } };
    RING_TEST_STATE State;
    OVERLAPPED Io[RequestCount];
    UINT8 Data[RequestCount][RingTestDataLength];
    BOOLEAN TestRead;
    ring_test_create(&State, &Guid, 64);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    ASSERT(ResetEvent(State.ProducerPrepareEntered));
    ASSERT(ResetEvent(State.ProducerPrepareRelease));
    ASSERT(ResetEvent(State.PrepareCommitEntered));
    ASSERT(ResetEvent(State.PrepareCommitRelease));
    for (UINT32 I = 0; I < RequestCount; I++)
    {
        ASSERT(ERROR_SUCCESS == ring_test_begin_file_read(Disk,
            RingTestBlockAddress + I % 16, Data[I], sizeof Data[I], &Io[I]));
        if (0 == I)
            ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.ProducerPrepareEntered, RingTestWaitTimeoutMs));
    }
    ASSERT(SetEvent(State.ProducerPrepareRelease));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.PrepareCommitEntered, RingTestWaitTimeoutMs));
    ASSERT(0 != ring_test_wait_for_sq_count(&State, 1));
    if (Reset)
        ASSERT(ERROR_SUCCESS == ring_test_reset_device(&State));
    else
    {
        /* A published request must complete while at least one claimed suffix
         * remains parked. Exact prefix size depends on concurrent claim order. */
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
        ASSERT(TestRead);
        HANDLE Events[RequestCount];
        for (UINT32 I = 0; I < RequestCount; I++)
            Events[I] = Io[I].hEvent;
        DWORD Wait = WaitForMultipleObjects(RequestCount, Events, FALSE, RingTestWaitTimeoutMs);
        ASSERT(Wait >= WAIT_OBJECT_0 && Wait < WAIT_OBJECT_0 + RequestCount);
        UINT32 Pending = 0;
        for (UINT32 I = 0; I < RequestCount; I++)
            if (WAIT_TIMEOUT == WaitForSingleObject(Io[I].hEvent, 0))
                Pending++;
        ASSERT(0 != Pending);
    }
    ASSERT(SetEvent(State.PrepareCommitRelease));
    if (!Reset)
    {
        for (UINT32 I = 1; I < RequestCount; I++)
        {
            ASSERT(0 != ring_test_wait_for_sq_count(&State, 1));
            ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
        }
    }
    for (UINT32 I = 0; I < RequestCount; I++)
    {
        DWORD Error = ring_test_finish_overlapped_io(Disk, &Io[I], RingTestWaitTimeoutMs);
        if (Reset)
            ASSERT(ERROR_SUCCESS != Error && ERROR_TIMEOUT != Error);
        else
            ASSERT(ERROR_SUCCESS == Error);
        CloseHandle(Io[I].hEvent);
    }
    CloseHandle(Disk);
    ring_test_destroy(&State);
}

static void ioctl_ring_partial_prepare_commit_test(void) { ring_test_partial_prepare_commit_case(FALSE); }
static void ioctl_ring_reset_partial_prepare_test(void) { ring_test_partial_prepare_commit_case(TRUE); }

static void ring_test_ready_slot_case(UINT32 Action)
{
    const GUID Guid =
        { 0x51aeb066, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x26 } };
    RING_TEST_STATE State;
    RING_TEST_SCSI_IO Io;
    UINT8 Data[RingTestDataLength] = { 0 };
    RING_TEST_STOP_THREAD Stop;
    HANDLE StopThread = 0;
    SPD_IOCTL_RING_WAIT_PARAMS Wait;
    ring_test_create(&State, &Guid, 4);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
    UINT32 Tail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    ASSERT(ResetEvent(State.ReadyEntered));
    ASSERT(ResetEvent(State.ReadyRelease));
    ASSERT(ERROR_SUCCESS == ring_test_begin_scsi_io(Disk, TRUE,
        RingTestBlockAddress, Data, sizeof Data, &Io));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.ReadyEntered, RingTestWaitTimeoutMs));
    ASSERT(Tail == SpdRingLoadAcquire32(&Header->RequestTail.Value));
    if (0 == Action)
    {
        ASSERT(ERROR_SUCCESS == ring_test_cancel_owned_srb(&State,
            SpdIoctlTransactWriteKind, RingTestBlockAddress, SPD_TEST_CANCEL_READY));
    }
    else if (1 == Action)
        ASSERT(ERROR_SUCCESS == ring_test_reset_device(&State));
    else
    {
        ASSERT(ERROR_SUCCESS == ring_test_begin_wait(&State));
        memset(&Stop, 0, sizeof Stop);
        Stop.State = &State;
        Stop.Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Stop.Finished);
        StopThread = CreateThread(0, 0, ring_test_stop_ring_thread, &Stop, 0, 0);
        ASSERT(0 != StopThread);
        /* WAIT cancellation proves STOP has closed publication. No timing
         * assumption about when the stop thread entered the kernel is needed. */
        DWORD Error = ring_test_finish_wait(&State, &Wait, RingTestWaitTimeoutMs);
        ASSERT(ERROR_OPERATION_ABORTED == Error || ERROR_CANCELLED == Error);
    }
    ASSERT(ring_test_scsi_aborted(ring_test_finish_scsi_io(Disk, &Io, RingTestWaitTimeoutMs)));
    ASSERT(Tail == SpdRingLoadAcquire32(&Header->RequestTail.Value));
    ASSERT(SetEvent(State.ReadyRelease));
    if (0 != StopThread)
    {
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Stop.Finished, RingTestWaitTimeoutMs));
        ASSERT(ERROR_SUCCESS == Stop.Error);
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(StopThread, RingTestWaitTimeoutMs));
        CloseHandle(StopThread);
        CloseHandle(Stop.Finished);
    }
    CloseHandle(Io.Overlapped.hEvent);
    CloseHandle(Disk);
    ring_test_destroy(&State);
}

static void ioctl_ring_cancel_ready_slot_test(void) { ring_test_ready_slot_case(0); }
static void ioctl_ring_reset_ready_slot_test(void) { ring_test_ready_slot_case(1); }
static void ioctl_ring_stop_ready_slot_test(void) { ring_test_ready_slot_case(2); }

static void ioctl_ring_kick_during_completion_copy_test(void)
{
    enum { RequestCount = 3 };
    const GUID Guid =
        { 0x51aeb073, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x33 } };
    RING_TEST_STATE State;
    OVERLAPPED Io[RequestCount];
    UINT8 Data[RequestCount][RingTestDataLength];
    BOOLEAN TestRead;
    ring_test_create_with_buffer_count(&State, &Guid, 4, RequestCount, FALSE);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    for (UINT32 I = 0; I < RequestCount; I++)
        ASSERT(ERROR_SUCCESS == ring_test_begin_file_read(Disk,
            RingTestBlockAddress + I, Data[I], sizeof Data[I], &Io[I]));
    ASSERT(RequestCount == ring_test_wait_for_sq_count(&State, RequestCount));
    ASSERT(ResetEvent(State.CompletionCopyEntered));
    ASSERT(ResetEvent(State.CompletionCopyRelease));
    ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
    ASSERT(TestRead);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.CompletionCopyEntered, RingTestWaitTimeoutMs));
    /* These KICKs must consume CQ entries while the single completion worker
     * still owns the first copy. No READ may complete until that owner leaves. */
    for (UINT32 I = 1; I < RequestCount; I++)
    {
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
        ASSERT(TestRead);
    }
    SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
    ASSERT(SpdRingLoadAcquire32(&Header->CompletionHead.Value) ==
        SpdRingLoadAcquire32(&Header->CompletionTail.Value));
    for (UINT32 I = 0; I < RequestCount; I++)
        ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Io[I].hEvent, 0));
    ASSERT(SetEvent(State.CompletionCopyRelease));
    for (UINT32 I = 0; I < RequestCount; I++)
    {
        ASSERT(ERROR_SUCCESS == ring_test_finish_overlapped_io(Disk, &Io[I], RingTestWaitTimeoutMs));
        CloseHandle(Io[I].hEvent);
    }
    CloseHandle(Disk);
    ring_test_destroy(&State);
}

/* Run explicitly with at least two threads in the stage under test. Uses the
 * production transport/discovery helpers and tagged disk I/O, not a queue model.
 * SCSI pass-through is untagged and Storport can serialize it before the IOQ. */
static void ring_multi_owned_batches_case(BOOLEAN Preparation, UINT32 Action)
{
    GUID Guid = { 0x51aeb0b0, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x70 } };
    Guid.Data1 += Action + (Preparation ? 0 : 8);
    RING_TEST_STATE State;
    OVERLAPPED Io[2];
    UINT8 Data[2][RingTestDataLength];
    RING_TEST_STOP_THREAD Stop = { 0 };
    HANDLE StopThread = 0;
    HANDLE Parallel = CreateEventW(0, TRUE, FALSE, Preparation ?
        L"Global\\WinSpdSharedRingTestParallelPrepare" :
        L"Global\\WinSpdSharedRingTestParallelComplete");
    ASSERT(0 != Parallel);
    ASSERT(ResetEvent(Parallel));
    ring_test_create_with_buffer_count(&State, &Guid, 4, 4, FALSE);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    HANDLE Entered = Preparation ? State.ProducerPrepareEntered : State.CompletionCopyEntered;
    HANDLE Release = Preparation ? State.ProducerPrepareRelease : State.CompletionCopyRelease;
    /* Discovery can itself overlap batches; require fresh evidence for the
     * two deliberately parked requests below. */
    ASSERT(ResetEvent(Parallel));
    ASSERT(ResetEvent(Entered));
    ASSERT(ResetEvent(Release));
    for (UINT32 I = 0; I < 2; I++)
    {
        memset(Data[I], Preparation ? 0x3c : 0xa5, sizeof Data[I]);
        ASSERT(ERROR_SUCCESS == ring_test_begin_file_io(Disk, Preparation,
            RingTestBlockAddress + I, Data[I], sizeof Data[I], &Io[I]));
        if (!Preparation)
        {
            BOOLEAN TestRead;
            ASSERT(1 == ring_test_wait_for_sq_count(&State, 1));
            ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
            ASSERT(TestRead);
        }
        if (0 == I)
            ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Entered, RingTestWaitTimeoutMs));
    }
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Parallel, RingTestWaitTimeoutMs));
    for (UINT32 I = 0; I < 2; I++)
        ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Io[I].hEvent, 0));
    if (1 == Action)
        ASSERT(ERROR_SUCCESS == ring_test_reset_device(&State));
    else if (3 == Action)
        for (UINT32 I = 0; I < 2; I++)
        {
            ASSERT(CancelIoEx(Disk, &Io[I]));
            ASSERT(ERROR_SUCCESS == ring_test_cancel_owned_srb(&State,
                Preparation ? SpdIoctlTransactWriteKind : SpdIoctlTransactReadKind,
                RingTestBlockAddress + I,
                Preparation ? SPD_TEST_CANCEL_PREPARING : SPD_TEST_CANCEL_COMPLETING));
        }
    else if (2 == Action)
    {
        SPD_IOCTL_RING_WAIT_PARAMS Wait;
        ASSERT(ERROR_SUCCESS == ring_test_begin_wait(&State));
        Stop.State = &State;
        Stop.Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Stop.Finished);
        StopThread = CreateThread(0, 0, ring_test_stop_ring_thread, &Stop, 0, 0);
        ASSERT(0 != StopThread);
        DWORD Error = ring_test_finish_wait(&State, &Wait, RingTestWaitTimeoutMs);
        ASSERT(ERROR_OPERATION_ABORTED == Error || ERROR_CANCELLED == Error);
        ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Stop.Finished, 0));
    }
    ASSERT(SetEvent(Release));
    if (Preparation && 0 == Action)
    {
        ASSERT(2 == ring_test_wait_for_sq_count(&State, 2));
        for (UINT32 I = 0; I < 2; I++)
        {
            BOOLEAN TestRead;
            ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
            ASSERT(!TestRead);
        }
    }
    for (UINT32 I = 0; I < 2; I++)
    {
        DWORD Error = ring_test_finish_overlapped_io(Disk, &Io[I], RingTestWaitTimeoutMs);
        if (0 == Action)
        {
            ASSERT(ERROR_SUCCESS == Error);
            if (!Preparation)
                for (UINT32 J = 0; J < sizeof Data[I]; J++)
                    ASSERT(0 == Data[I][J]);
        }
        else
            ASSERT(ERROR_SUCCESS != Error && ERROR_TIMEOUT != Error);
        CloseHandle(Io[I].hEvent);
    }
    if (0 != StopThread)
    {
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Stop.Finished, RingTestWaitTimeoutMs));
        ASSERT(ERROR_SUCCESS == Stop.Error);
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(StopThread, RingTestWaitTimeoutMs));
        CloseHandle(StopThread);
        CloseHandle(Stop.Finished);
    }
    CloseHandle(Disk);
    ring_test_destroy(&State);
    CloseHandle(Parallel);
}

static void ring_multi_prepare_test(void) { ring_multi_owned_batches_case(TRUE, 0); }
static void ring_multi_prepare_reset_test(void) { ring_multi_owned_batches_case(TRUE, 1); }
static void ring_multi_prepare_stop_test(void) { ring_multi_owned_batches_case(TRUE, 2); }
static void ring_multi_prepare_cancel_test(void) { ring_multi_owned_batches_case(TRUE, 3); }
static void ring_multi_completion_test(void) { ring_multi_owned_batches_case(FALSE, 0); }
static void ring_multi_completion_reset_test(void) { ring_multi_owned_batches_case(FALSE, 1); }
static void ring_multi_completion_stop_test(void) { ring_multi_owned_batches_case(FALSE, 2); }
static void ring_multi_completion_cancel_test(void) { ring_multi_owned_batches_case(FALSE, 3); }

static void ring_test_completion_queued_case(UINT32 Action)
{
    GUID Guid =
        { 0x51aeb072, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x32 } };
    Guid.Data1 = 0x51aeb080 + Action;
    Guid.Data4[7] = (UINT8)(0x40 + Action);
    RING_TEST_STATE State;
    RING_TEST_SCSI_IO Io;
    RING_TEST_STOP_THREAD Stop;
    HANDLE StopThread = 0;
    SPD_IOCTL_RING_WAIT_PARAMS Wait;
    UINT8 Data[RingTestDataLength];
    BOOLEAN TestRead;
    ring_test_create(&State, &Guid, 4);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    ASSERT(ResetEvent(State.CompletionClaimEntered));
    ASSERT(ResetEvent(State.CompletionClaimRelease));
    ASSERT(ERROR_SUCCESS == ring_test_begin_scsi_io(Disk, FALSE,
        RingTestBlockAddress, Data, sizeof Data, &Io));
    ASSERT(1 == ring_test_wait_for_sq_count(&State, 1));
    ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
    ASSERT(TestRead);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.CompletionClaimEntered, RingTestWaitTimeoutMs));
    ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Io.Overlapped.hEvent, 0));
    if (0 == Action)
        ASSERT(ERROR_SUCCESS == ring_test_cancel_owned_srb(&State,
            SpdIoctlTransactReadKind, RingTestBlockAddress, SPD_TEST_CANCEL_COMPLETION_QUEUED));
    else if (1 == Action)
        ASSERT(ERROR_SUCCESS == ring_test_reset_device(&State));
    else
    {
        ASSERT(ERROR_SUCCESS == ring_test_begin_wait(&State));
        memset(&Stop, 0, sizeof Stop);
        Stop.State = &State;
        Stop.Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Stop.Finished);
        StopThread = CreateThread(0, 0, ring_test_stop_ring_thread, &Stop, 0, 0);
        ASSERT(0 != StopThread);
        DWORD Error = ring_test_finish_wait(&State, &Wait, RingTestWaitTimeoutMs);
        ASSERT(ERROR_OPERATION_ABORTED == Error || ERROR_CANCELLED == Error);
        ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Stop.Finished, 0));
    }
    /* Queue ownership permits immediate retirement. The still-parked callback
     * owns a lifetime reference, but has no slot or SRB to dereference. */
    ASSERT(ring_test_scsi_aborted(ring_test_finish_scsi_io(Disk, &Io, RingTestWaitTimeoutMs)));
    ASSERT(SetEvent(State.CompletionClaimRelease));
    if (0 != StopThread)
    {
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Stop.Finished, RingTestWaitTimeoutMs));
        ASSERT(ERROR_SUCCESS == Stop.Error);
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(StopThread, RingTestWaitTimeoutMs));
        CloseHandle(StopThread);
        CloseHandle(Stop.Finished);
    }
    CloseHandle(Io.Overlapped.hEvent);
    CloseHandle(Disk);
    ring_test_destroy(&State);
}

static void ioctl_ring_cancel_queued_completion_test(void) { ring_test_completion_queued_case(0); }
static void ioctl_ring_reset_queued_completion_test(void) { ring_test_completion_queued_case(1); }
static void ioctl_ring_stop_queued_completion_test(void) { ring_test_completion_queued_case(2); }

typedef struct
{
    HANDLE Device, Finished;
    UINT32 Btl;
    SPD_IOCTL_RING_KICK_PARAMS Params;
    DWORD Error;
} RING_TEST_KICK_THREAD;

static DWORD WINAPI ring_test_kick_thread(PVOID Context)
{
    RING_TEST_KICK_THREAD *Work = Context;
    Work->Error = SpdIoctlRingKick(Work->Device, Work->Btl, &Work->Params);
    SetEvent(Work->Finished);
    return Work->Error;
}

static void ioctl_ring_failure_freezes_publication_test(void)
{
    const GUID Guid = { 0x51aeb0a5, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x65 } };
    RING_TEST_STATE State;
    OVERLAPPED Io[3];
    UINT8 Data[3][RingTestDataLength];
    RING_TEST_KICK_THREAD Kick = { 0 };
    ring_test_create(&State, &Guid, 4);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    ASSERT(ERROR_SUCCESS == ring_test_begin_file_read(Disk,
        RingTestBlockAddress, Data[0], sizeof Data[0], &Io[0]));
    ASSERT(1 == ring_test_wait_for_sq_count(&State, 1));
    ASSERT(ResetEvent(State.ReadyEntered) && ResetEvent(State.ReadyRelease));
    ASSERT(ERROR_SUCCESS == ring_test_begin_file_read(Disk,
        RingTestBlockAddress + 1, Data[1], sizeof Data[1], &Io[1]));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.ReadyEntered, RingTestWaitTimeoutMs));
    SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
    UINT32 Tail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    UINT32 Head = SpdRingLoadAcquire32(&Header->RequestHead.Value);
    UINT32 CqHead = SpdRingLoadAcquire32(&Header->CompletionHead.Value);
    SPD_RING_REQUEST *Requests = (SPD_RING_REQUEST *)
        ((PUINT8)State.StorageUnit->SharedRingAddress + Header->RequestOffset);
    SPD_RING_COMPLETION *Completions = (SPD_RING_COMPLETION *)
        ((PUINT8)State.StorageUnit->SharedRingAddress + Header->CompletionOffset);
    SPD_RING_REQUEST *Request = &Requests[Head & (Header->QueueDepth - 1)];
    SPD_RING_COMPLETION *Bad = &Completions[CqHead & (Header->QueueDepth - 1)];
    memset(Bad, 0, sizeof *Bad);
    Bad->Response.Hint = Request->Request.Hint;
    Bad->Response.Kind = SpdIoctlTransactWriteKind; /* The published request is READ. */
    Bad->Data = Request->Data;
    ASSERT(ResetEvent(State.CompletionClaimEntered) && ResetEvent(State.CompletionClaimRelease));
    SpdRingStoreRelease32(&Header->RequestHead.Value, Head + 1);
    SpdRingStoreRelease32(&Header->CompletionTail.Value, CqHead + 1);
    Kick.Device = State.StorageUnit->Handle;
    Kick.Btl = State.StorageUnit->Btl;
    Kick.Finished = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != Kick.Finished);
    HANDLE Thread = CreateThread(0, 0, ring_test_kick_thread, &Kick, 0, 0);
    ASSERT(0 != Thread);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.CompletionClaimEntered, RingTestWaitTimeoutMs));
    ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Kick.Finished, 0));
    /* Validation has failed, but KICK's terminal reset has not run. Release a
     * ready producer and submit another real tagged READ. Admission must reject
     * it, and publication must remain frozen throughout this interval. */
    ASSERT(SetEvent(State.ReadyRelease));
    DWORD Error = ring_test_begin_file_read(Disk,
        RingTestBlockAddress + 2, Data[2], sizeof Data[2], &Io[2]);
    BOOLEAN Started = ERROR_SUCCESS == Error;
    ULONGLONG Deadline = GetTickCount64() + RingTestWaitTimeoutMs;
    while (Started && WAIT_TIMEOUT == WaitForSingleObject(Io[2].hEvent, 1) &&
        Tail == SpdRingLoadAcquire32(&Header->RequestTail.Value) && GetTickCount64() < Deadline)
        ;
    UINT32 After = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    DWORD ThirdFinished = Started ? WaitForSingleObject(Io[2].hEvent, 0) : WAIT_OBJECT_0;
    ASSERT(SetEvent(State.CompletionClaimRelease));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Thread, RingTestWaitTimeoutMs));
    ASSERT(ERROR_SUCCESS != Kick.Error && 0 == Kick.Params.Consumed);
    ASSERT(Tail == After && CqHead == SpdRingLoadAcquire32(&Header->CompletionHead.Value));
    ASSERT(WAIT_OBJECT_0 == ThirdFinished);
    for (UINT32 I = 0; I < 3; I++)
    {
        if (I < 2 || Started)
        {
            Error = ring_test_finish_overlapped_io(Disk, &Io[I], RingTestWaitTimeoutMs);
            ASSERT(ERROR_SUCCESS != Error && ERROR_TIMEOUT != Error);
        }
        CloseHandle(Io[I].hEvent);
    }
    CloseHandle(Thread);
    CloseHandle(Kick.Finished);
    CloseHandle(Disk);
    ring_test_destroy(&State);
}

static void ioctl_ring_invalid_slot_completion_test(void)
{
    const GUID Guid =
        { 0x51aeb069, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x29 } };
    for (UINT32 Fault = 0; Fault < 6; Fault++)
    {
        RING_TEST_STATE State;
        OVERLAPPED Io[2];
        UINT8 Data[2][RingTestDataLength];
        ring_test_create(&State, &Guid, 4);
        ring_test_wait_for_disk_manual(&State);
        ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
        HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ);
        ASSERT(INVALID_HANDLE_VALUE != Disk);
        for (UINT32 I = 0; I < 2; I++)
            ASSERT(ERROR_SUCCESS == ring_test_begin_file_read(Disk,
                RingTestBlockAddress + I, Data[I], sizeof Data[I], &Io[I]));
        ASSERT(2 == ring_test_wait_for_sq_count(&State, 2));
        SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
        UINT32 RequestHead = SpdRingLoadAcquire32(&Header->RequestHead.Value);
        UINT32 CompletionHead = SpdRingLoadAcquire32(&Header->CompletionHead.Value);
        ASSERT(CompletionHead == SpdRingLoadAcquire32(&Header->CompletionTail.Value));
        SPD_RING_REQUEST *Requests = (SPD_RING_REQUEST *)
            ((PUINT8)State.StorageUnit->SharedRingAddress + Header->RequestOffset);
        SPD_RING_COMPLETION *Completions = (SPD_RING_COMPLETION *)
            ((PUINT8)State.StorageUnit->SharedRingAddress + Header->CompletionOffset);
        for (UINT32 I = 0; I < 2; I++)
        {
            SPD_RING_REQUEST *Request = &Requests[(RequestHead + I) & (Header->QueueDepth - 1)];
            SPD_RING_COMPLETION *Response = &Completions[(CompletionHead + I) & (Header->QueueDepth - 1)];
            memset(Response, 0, sizeof *Response);
            Response->Response.Hint = Request->Request.Hint;
            Response->Response.Kind = Request->Request.Kind;
            Response->Data = Request->Data;
        }
        SPD_RING_COMPLETION *Bad = &Completions[(CompletionHead + 1) & (Header->QueueDepth - 1)];
        switch (Fault)
        {
        case 0: Bad->Response.Hint ^= 1ULL << 16; break;
        case 1: Bad->Response.Kind = SpdIoctlTransactWriteKind; break;
        case 2: Bad->Data.BufferId = (Bad->Data.BufferId + 1) % Header->BufferCount; break;
        case 3: Bad->Data.Length++; break;
        case 4: Bad->Response.Hint |= 0xffff; break;
        case 5: Bad->Data.UserAddress ^= 4096; break;
        }
        SpdRingStoreRelease32(&Header->RequestHead.Value, RequestHead + 2);
        SpdRingStoreRelease32(&Header->CompletionTail.Value, CompletionHead + 2);
        SPD_IOCTL_RING_KICK_PARAMS Kick = { 0 };
        ASSERT(ERROR_SUCCESS != SpdIoctlRingKick(State.StorageUnit->Handle,
            State.StorageUnit->Btl, &Kick));
        ASSERT(CompletionHead + 1 == SpdRingLoadAcquire32(&Header->CompletionHead.Value));
        /* A valid CQ prefix transfers ownership to the completion queue. A
         * protocol failure may reset it before its asynchronous copy finishes;
         * CQ consumption alone no longer promises successful SRB completion. */
        DWORD FirstError = ring_test_finish_overlapped_io(Disk, &Io[0], RingTestWaitTimeoutMs);
        ASSERT(ERROR_TIMEOUT != FirstError);
        DWORD Error = ring_test_finish_overlapped_io(Disk, &Io[1], RingTestWaitTimeoutMs);
        ASSERT(ERROR_SUCCESS != Error && ERROR_TIMEOUT != Error);
        CloseHandle(Io[0].hEvent);
        CloseHandle(Io[1].hEvent);
        CloseHandle(Disk);
        ring_test_destroy(&State);
    }
}

static void ring_test_slot_token_case(BOOLEAN Stale)
{
    const GUID Guid =
        { 0x51aeb067, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x27 } };
    RING_TEST_STATE State;
    RING_TEST_SCSI_IO Io;
    enum { SlotCount = 2 };
    SPD_RING_COMPLETION Seen[SlotCount] = { 0 };
    SPD_RING_COMPLETION Old = { 0 };
    UINT8 Data[RingTestDataLength];
    BOOLEAN TestRead, Reused = FALSE;
    /* Queue depth and payload-slot count are independent. Configure the two
     * leases tracked here explicitly; do not assume the default buffer count
     * or discovery's previous allocation order. */
    ring_test_create_with_buffer_count(&State, &Guid, 2, SlotCount, FALSE);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
    for (UINT32 I = 0; I < ARRAYSIZE(Seen) + 1; I++)
    {
        ASSERT(ERROR_SUCCESS == ring_test_begin_scsi_io(Disk, FALSE,
            RingTestBlockAddress, Data, sizeof Data, &Io));
        ASSERT(1 == ring_test_wait_for_sq_count(&State, 1));
        UINT32 Head = SpdRingLoadAcquire32(&Header->RequestHead.Value);
        SPD_RING_REQUEST *Requests = (SPD_RING_REQUEST *)
            ((PUINT8)State.StorageUnit->SharedRingAddress + Header->RequestOffset);
        SPD_RING_REQUEST Request = Requests[Head & (Header->QueueDepth - 1)];
        UINT32 Index = Request.Data.BufferId;
        ASSERT(Index < ARRAYSIZE(Seen));
        ASSERT(Index == (Request.Request.Hint & 0xffff));
        if (0 != Seen[Index].Response.Hint)
        {
            Old = Seen[Index];
            ASSERT((Request.Request.Hint >> 16) > (Old.Response.Hint >> 16));
            Reused = TRUE;
            break;
        }
        Seen[Index].Response.Hint = Request.Request.Hint;
        Seen[Index].Response.Kind = Request.Request.Kind;
        Seen[Index].Data = Request.Data;
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
        ASSERT(ERROR_SUCCESS == ring_test_finish_scsi_io(Disk, &Io, RingTestWaitTimeoutMs));
        CloseHandle(Io.Overlapped.hEvent);
    }
    ASSERT(Reused);
    if (Stale)
    {
        UINT32 Head = SpdRingLoadAcquire32(&Header->CompletionHead.Value);
        UINT32 Tail = SpdRingLoadAcquire32(&Header->CompletionTail.Value);
        ASSERT(Head == Tail);
        SPD_RING_COMPLETION *Responses = (SPD_RING_COMPLETION *)
            ((PUINT8)State.StorageUnit->SharedRingAddress + Header->CompletionOffset);
        Responses[Tail & (Header->QueueDepth - 1)] = Old;
        SpdRingStoreRelease32(&Header->RequestHead.Value,
            SpdRingLoadAcquire32(&Header->RequestHead.Value) + 1);
        SpdRingStoreRelease32(&Header->CompletionTail.Value, Tail + 1);
        SPD_IOCTL_RING_KICK_PARAMS Kick = { 0 };
        ASSERT(ERROR_SUCCESS != SpdIoctlRingKick(State.StorageUnit->Handle,
            State.StorageUnit->Btl, &Kick));
        ASSERT(0 == Kick.Consumed);
        ASSERT(Head == SpdRingLoadAcquire32(&Header->CompletionHead.Value));
        ASSERT(ring_test_scsi_aborted(ring_test_finish_scsi_io(Disk, &Io, RingTestWaitTimeoutMs)));
    }
    else
    {
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
        ASSERT(ERROR_SUCCESS == ring_test_finish_scsi_io(Disk, &Io, RingTestWaitTimeoutMs));
    }
    CloseHandle(Io.Overlapped.hEvent);
    CloseHandle(Disk);
    ring_test_destroy(&State);
}

static void ioctl_ring_slot_generation_reuse_test(void) { ring_test_slot_token_case(FALSE); }
static void ioctl_ring_stale_slot_completion_test(void) { ring_test_slot_token_case(TRUE); }

static void ioctl_ring_slot_generation_exhaustion_test(void)
{
    const GUID Guid =
        { 0x51aeb068, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x28 } };
    RING_TEST_STATE State;
    RING_TEST_SCSI_IO Io;
    UINT8 Data[RingTestDataLength];
    ring_test_create(&State, &Guid, 2);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
    UINT32 Tail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State,
        SPD_IOCTL_TEST_EXHAUST_GENERATION, 0, 0, 0));
    DWORD Error = ring_test_begin_scsi_io(Disk, FALSE, RingTestBlockAddress,
        Data, sizeof Data, &Io);
    if (ERROR_SUCCESS == Error)
        Error = ring_test_finish_scsi_io(Disk, &Io, RingTestWaitTimeoutMs);
    ASSERT(ERROR_SUCCESS != Error && ERROR_TIMEOUT != Error);
    ASSERT(Tail == SpdRingLoadAcquire32(&Header->RequestTail.Value));
    CloseHandle(Io.Overlapped.hEvent);
    CloseHandle(Disk);
    ring_test_destroy(&State);
}

static void ring_test_published_io_case(BOOLEAN Reset)
{
    const GUID Guid =
        { 0x51aeb065, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x25 } };
    RING_TEST_STATE State;
    RING_TEST_SCSI_IO Io;
    UINT8 Data[RingTestDataLength];
    HANDLE DiskHandle;
    BOOLEAN TestRead;

    ring_test_create(&State, &Guid, 4);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    DiskHandle = ring_test_open_overlapped_disk(&State,
        GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != DiskHandle);
    ASSERT(ERROR_SUCCESS == ring_test_begin_scsi_io(DiskHandle, FALSE,
        RingTestBlockAddress, Data, sizeof Data, &Io));
    ASSERT(1 == ring_test_wait_for_sq_count(&State, 1));
    if (Reset)
    {
        SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
        UINT32 CompletionHead = SpdRingLoadAcquire32(&Header->CompletionHead.Value);
        UINT32 RequestTail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
        ASSERT(ERROR_SUCCESS == ring_test_reset_device(&State));
        ASSERT(ring_test_scsi_aborted(ring_test_finish_scsi_io(DiskHandle,
            &Io, RingTestWaitTimeoutMs)));
        /* A backend may finish the old operation after logical retirement.
         * Its late CQ entry must not be consumed or reopen the session. */
        ASSERT(ERROR_SUCCESS != ring_test_complete_one_manually(&State, &TestRead));
        ASSERT(CompletionHead == SpdRingLoadAcquire32(&Header->CompletionHead.Value));
        CloseHandle(Io.Overlapped.hEvent);
        DWORD Error = ring_test_begin_scsi_io(DiskHandle, FALSE,
            RingTestBlockAddress, Data, sizeof Data, &Io);
        if (ERROR_SUCCESS == Error)
            Error = ring_test_finish_scsi_io(DiskHandle, &Io, RingTestWaitTimeoutMs);
        ASSERT(ERROR_SUCCESS != Error && ERROR_TIMEOUT != Error);
        ASSERT(RequestTail == SpdRingLoadAcquire32(&Header->RequestTail.Value));
        goto exit;
    }
    ASSERT(ERROR_NOT_SUPPORTED == ring_test_cancel_owned_srb(&State,
        SpdIoctlTransactReadKind, RingTestBlockAddress, SPD_TEST_CANCEL_INFLIGHT));
    ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Io.Overlapped.hEvent, 0));
    ASSERT(1 == ring_test_request_available(&State));
    ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
    ASSERT(TestRead);
    ASSERT(ERROR_SUCCESS == ring_test_finish_scsi_io(DiskHandle, &Io,
        RingTestWaitTimeoutMs));
exit:
    CloseHandle(Io.Overlapped.hEvent);
    CloseHandle(DiskHandle);
    ring_test_destroy(&State);
}

static void ioctl_ring_abort_published_io_test(void)
{
    ring_test_published_io_case(FALSE);
}

static void ioctl_ring_reset_published_io_test(void)
{
    ring_test_published_io_case(TRUE);
}

static void ioctl_ring_stop_retires_large_batch_test(void)
{
    enum { RequestCount = 40, QueueDepth = 64 };
    const GUID Guid =
        { 0x51aeb064, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x24 } };
    RING_TEST_STATE State;
    OVERLAPPED Io[RequestCount];
    UINT8 Data[RequestCount][RingTestDataLength];
    HANDLE DiskHandle;

    ring_test_create(&State, &Guid, QueueDepth);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    ASSERT(0 == ring_test_request_available(&State));
    DiskHandle = ring_test_open_overlapped_disk(&State, GENERIC_READ);
    ASSERT(INVALID_HANDLE_VALUE != DiskHandle);
    for (UINT32 I = 0; I < RequestCount; I++)
        ASSERT(ERROR_SUCCESS == ring_test_begin_file_read(DiskHandle,
            RingTestBlockAddress + I % 16, Data[I], sizeof Data[I], &Io[I]));
    ASSERT(RequestCount == ring_test_wait_for_sq_count(&State, RequestCount));

    /* All SRBs are published, with no backend currently using their buffers.
     * STOP must retire more than one bounded group without requiring CQ work,
     * while keeping the mapping alive until our cooperative CLOSE. */
    ASSERT(ERROR_SUCCESS == ring_test_stop_ring(&State));
    for (UINT32 I = 0; I < RequestCount; I++)
    {
        DWORD Error = ring_test_finish_overlapped_io(DiskHandle, &Io[I],
            RingTestWaitTimeoutMs);
        ASSERT(ERROR_SUCCESS != Error && ERROR_TIMEOUT != Error);
        CloseHandle(Io[I].hEvent);
    }
    CloseHandle(DiskHandle);
    ring_test_destroy(&State);
}

static void ioctl_ring_full_sq_pending_resume_test(void)
{
    enum { QueueDepth = 8, RequestCount = 10 };
    RING_TEST_STATE State;
    HANDLE DiskHandle;
    OVERLAPPED Overlapped[RequestCount];
    UINT8 ReadData[RequestCount][RingTestDataLength];
    SPD_RING_HEADER *Header;

    ring_test_create(&State, &RingTestGuidFullSqResume, QueueDepth);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    ASSERT(0 == ring_test_request_available(&State));
    DiskHandle = ring_test_open_overlapped_disk(&State,
        GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != DiskHandle);
    Header = State.StorageUnit->SharedRingHeader;
    ASSERT(ResetEvent(State.ProducerPrepareEntered));
    ASSERT(ResetEvent(State.ProducerPrepareRelease));
    for (UINT32 I = 0; I < RequestCount; I++)
    {
        DWORD Error = ring_test_begin_file_read(DiskHandle,
            RingTestBlockAddress + I, ReadData[I], sizeof ReadData[I],
            &Overlapped[I]);
        ASSERT(ERROR_SUCCESS == Error);
        if (0 == I)
            ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(
                State.ProducerPrepareEntered, RingTestWaitTimeoutMs));
    }
    ASSERT(SetEvent(State.ProducerPrepareRelease));
    ASSERT(QueueDepth == ring_test_wait_for_sq_count(&State, QueueDepth));
    Sleep(100);
    ASSERT(QueueDepth ==
        SpdRingLoadAcquire32(&Header->RequestTail.Value) -
        SpdRingLoadAcquire32(&Header->RequestHead.Value));

    for (UINT32 I = 0; I < RequestCount; I++)
    {
        ULONGLONG Deadline = GetTickCount64() + RingTestWaitTimeoutMs;
        while (0 == ring_test_request_available(&State) &&
            GetTickCount64() < Deadline)
            Sleep(1);
        ASSERT(0 != ring_test_request_available(&State));
        BOOLEAN TestRead;
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(
            &State, &TestRead));
        ASSERT(TestRead);
    }
    for (UINT32 I = 0; I < RequestCount; I++)
    {
        DWORD BytesRead = 0;
        ASSERT(ERROR_SUCCESS == ring_test_finish_overlapped_io(
            DiskHandle, &Overlapped[I], RingTestWaitTimeoutMs));
        ASSERT(GetOverlappedResult(DiskHandle, &Overlapped[I],
            &BytesRead, FALSE));
        ASSERT(RingTestDataLength == BytesRead);
        CloseHandle(Overlapped[I].hEvent);
    }
    CloseHandle(DiskHandle);
    ring_test_destroy(&State);
}

/* These are real tagged I/Os. A Debug control changes the pending probe's
 * CDB to FLUSH, bypassing only Storport's pass-through serialization. No queue,
 * ownership transition, continuation, cancellation or teardown is simulated. */
static void ring_test_flush_fence_case(UINT32 Action)
{
    enum { FirstLength = 12 * 1024, ChunkLength = 4096, FenceLba = 32, LaterLba = 48 };
    GUID Guid = { 0x51aeb090, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x50 } };
    Guid.Data1 += Action;
    HANDLE ClaimRelease = CreateEventW(0, TRUE, TRUE,
        L"Global\\WinSpdSharedRingTestPreparationClaimRelease");
    ASSERT(0 != ClaimRelease);
    ASSERT(SetEvent(ClaimRelease));
    RING_TEST_STATE State;
    OVERLAPPED First, Fence, Later, SecondFence;
    UINT8 FirstData[FirstLength], FenceData[512] = { 0 }, LaterData[512], SecondData[512] = { 0 };
    BOOLEAN TestRead;
    for (UINT32 I = 0; I < sizeof FirstData; I++)
        FirstData[I] = ring_test_byte_pattern(I);
    memset(LaterData, 0xa5, sizeof LaterData);
    ring_test_create_with_limits(&State, &Guid, 16, 16, FALSE, TRUE, 16384);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_CHUNK_LIMIT, 0, 0, ChunkLength));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    ASSERT(ResetEvent(State.ProducerPrepareEntered));
    ASSERT(ResetEvent(State.ProducerPrepareRelease));
    ASSERT(ERROR_SUCCESS == ring_test_begin_file_io(Disk, TRUE, 0, FirstData, sizeof FirstData, &First));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.ProducerPrepareEntered, RingTestWaitTimeoutMs));
    /* Verify this is one logical SRB with multiple chunks, not several smaller
     * SRBs split by disk.sys that would make the fence test pass accidentally. */
    ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_EXPECT_CHUNKED,
        SpdIoctlTransactWriteKind, 0, 0));
    ASSERT(ResetEvent(ClaimRelease));
    ASSERT(ERROR_SUCCESS == ring_test_begin_file_read(Disk, FenceLba, FenceData, sizeof FenceData, &Fence));
    ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_PROMOTE_FLUSH,
        SpdIoctlTransactReadKind, FenceLba, 0));
    if (8 == Action)
    {
        ASSERT(ERROR_SUCCESS == ring_test_begin_file_read(Disk, FenceLba + 1, SecondData, sizeof SecondData, &SecondFence));
        ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_PROMOTE_FLUSH,
            SpdIoctlTransactReadKind, FenceLba + 1, 0));
    }
    ASSERT(ERROR_SUCCESS == ring_test_begin_file_io(Disk, TRUE, LaterLba, LaterData, sizeof LaterData, &Later));
    ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_EXPECT_PENDING,
        SpdIoctlTransactWriteKind, LaterLba, 0));
    if (1 == Action)
    {
        ASSERT(CancelIoEx(Disk, &Fence));
        ASSERT(ERROR_SUCCESS == ring_test_cancel_owned_srb(&State, SpdIoctlTransactFlushKind,
            FenceLba, SPD_TEST_CANCEL_PENDING));
    }
    ASSERT(SetEvent(ClaimRelease));
    ASSERT(SetEvent(State.ProducerPrepareRelease));
    SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
    if (1 != Action)
    {
        for (UINT32 Chunk = 0; Chunk < FirstLength / ChunkLength; Chunk++)
        {
            ASSERT(1 == ring_test_wait_for_sq_count(&State, 1));
            UINT32 Head = SpdRingLoadAcquire32(&Header->RequestHead.Value);
            SPD_RING_REQUEST *Request = (SPD_RING_REQUEST *)((PUINT8)State.StorageUnit->SharedRingAddress +
                Header->RequestOffset) + (Head & (Header->QueueDepth - 1));
            ASSERT(SpdIoctlTransactWriteKind == Request->Request.Kind);
            ASSERT(Chunk * ChunkLength / 512 == Request->Request.Op.Write.BlockAddress);
            ASSERT(ChunkLength / 512 == Request->Request.Op.Write.BlockCount);
            ASSERT(ChunkLength == Request->Data.Length);
            ASSERT(Request->Data.BufferId < Header->BufferCount);
            PVOID Payload = (PUINT8)State.StorageUnit->SharedRingAddress + Header->BufferOffset +
                (SIZE_T)Request->Data.BufferId * Header->BufferSize;
            ASSERT(0 == memcmp(Payload, FirstData + Chunk * ChunkLength, ChunkLength));
            ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_EXPECT_CHUNKED,
                SpdIoctlTransactWriteKind, 0, 0));
            if (Chunk + 1 == FirstLength / ChunkLength)
            {
                if (2 == Action)
                {
                    ASSERT(ResetEvent(State.ProducerPrepareEntered));
                    ASSERT(ResetEvent(State.ProducerPrepareRelease));
                }
                if (3 == Action)
                {
                    ASSERT(ResetEvent(State.ReadyEntered));
                    ASSERT(ResetEvent(State.ReadyRelease));
                }
            }
            ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
            ASSERT(!TestRead);
        }
        ASSERT(ERROR_SUCCESS == ring_test_finish_overlapped_io(Disk, &First, RingTestWaitTimeoutMs));
        if (2 == Action || 3 == Action)
        {
            ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(2 == Action ? State.ProducerPrepareEntered :
                State.ReadyEntered, RingTestWaitTimeoutMs));
            ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_EXPECT_FENCE, 0, 0, 0));
            ASSERT(CancelIoEx(Disk, &Fence));
            ASSERT(ERROR_SUCCESS == ring_test_cancel_owned_srb(&State, SpdIoctlTransactFlushKind,
                FenceLba, 2 == Action ? SPD_TEST_CANCEL_PREPARING : SPD_TEST_CANCEL_READY));
            if (2 == Action)
                ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_EXPECT_FENCE, 0, 0, 0));
            ASSERT(SetEvent(2 == Action ? State.ProducerPrepareRelease : State.ReadyRelease));
        }
        else
        {
            UINT32 FlushCount = 8 == Action ? 2 : 1;
            for (UINT32 I = 0; I < FlushCount; I++)
            {
                ASSERT(1 == ring_test_wait_for_sq_count(&State, 1));
                UINT32 Head = SpdRingLoadAcquire32(&Header->RequestHead.Value);
                SPD_RING_REQUEST *Request = (SPD_RING_REQUEST *)((PUINT8)State.StorageUnit->SharedRingAddress +
                    Header->RequestOffset) + (Head & (Header->QueueDepth - 1));
                ASSERT(SpdIoctlTransactFlushKind == Request->Request.Kind);
                ASSERT(FenceLba + I == Request->Request.Op.Flush.BlockAddress);
                ASSERT(0 == Request->Request.Op.Flush.BlockCount);
                ASSERT(0 == Request->Data.Length && SPD_RING_NO_BUFFER == Request->Data.BufferId);
                ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_EXPECT_FENCE, 0, 0, 0));
                ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_EXPECT_PENDING,
                    SpdIoctlTransactWriteKind, LaterLba, 0));
                if (4 == Action)
                {
                    ASSERT(ERROR_SUCCESS != ring_test_cancel_owned_srb(&State, SpdIoctlTransactFlushKind,
                        FenceLba, SPD_TEST_CANCEL_INFLIGHT));
                    ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_EXPECT_FENCE, 0, 0, 0));
                }
                if (5 == Action || 7 == Action)
                {
                    ASSERT(ERROR_SUCCESS == (5 == Action ? ring_test_reset_device(&State) : ring_test_stop_ring(&State)));
                    break;
                }
                ASSERT(ResetEvent(State.CompletionClaimEntered));
                ASSERT(ResetEvent(State.CompletionClaimRelease));
                ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
                ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.CompletionClaimEntered, RingTestWaitTimeoutMs));
                ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_EXPECT_FENCE, 0, 0, 0));
                ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_EXPECT_PENDING,
                    SpdIoctlTransactWriteKind, LaterLba, 0));
                if (6 == Action)
                {
                    ASSERT(CancelIoEx(Disk, &Fence));
                    ASSERT(ERROR_SUCCESS == ring_test_cancel_owned_srb(&State, SpdIoctlTransactFlushKind,
                        FenceLba, SPD_TEST_CANCEL_COMPLETION_QUEUED));
                }
                ASSERT(SetEvent(State.CompletionClaimRelease));
                if (0 == I && 6 != Action)
                    ASSERT(ERROR_SUCCESS == ring_test_finish_overlapped_io(Disk, &Fence, RingTestWaitTimeoutMs));
                if (1 == I)
                    ASSERT(ERROR_SUCCESS == ring_test_finish_overlapped_io(Disk, &SecondFence, RingTestWaitTimeoutMs));
            }
        }
    }
    if (5 != Action && 7 != Action)
    {
        BOOLEAN SawLater = FALSE;
        UINT32 WriteChunks = 0;
        do
        {
            ASSERT(0 != ring_test_wait_for_sq_count(&State, 1));
            UINT32 Head = SpdRingLoadAcquire32(&Header->RequestHead.Value);
            SPD_RING_REQUEST *Request = (SPD_RING_REQUEST *)((PUINT8)State.StorageUnit->SharedRingAddress +
                Header->RequestOffset) + (Head & (Header->QueueDepth - 1));
            ASSERT(SpdIoctlTransactFlushKind != Request->Request.Kind);
            if (SpdIoctlTransactWriteKind == Request->Request.Kind)
            {
                if (LaterLba == Request->Request.Op.Write.BlockAddress)
                    SawLater = TRUE;
                else
                    WriteChunks++;
            }
            ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
        } while (!SawLater || (1 == Action && WriteChunks < FirstLength / ChunkLength));
        ASSERT(ERROR_SUCCESS == ring_test_finish_overlapped_io(Disk, &First, RingTestWaitTimeoutMs));
        ASSERT(ERROR_SUCCESS == ring_test_finish_overlapped_io(Disk, &Later, RingTestWaitTimeoutMs));
    }
    else
        ASSERT(ERROR_SUCCESS != ring_test_finish_overlapped_io(Disk, &Later, RingTestWaitTimeoutMs));
    DWORD FenceError = ring_test_finish_overlapped_io(Disk, &Fence, RingTestWaitTimeoutMs);
    if (1 == Action || 2 == Action || 3 == Action || 5 == Action || 6 == Action || 7 == Action)
        ASSERT(ERROR_SUCCESS != FenceError && ERROR_TIMEOUT != FenceError);
    else
        ASSERT(ERROR_SUCCESS == FenceError);
    CloseHandle(First.hEvent);
    CloseHandle(Fence.hEvent);
    CloseHandle(Later.hEvent);
    if (8 == Action)
        CloseHandle(SecondFence.hEvent);
    ring_test_destroy(&State);
    CloseHandle(Disk);
    CloseHandle(ClaimRelease);
}

static void ioctl_ring_flush_fence_multichunk_test(void) { ring_test_flush_fence_case(0); }
static void ioctl_ring_cancel_pending_flush_test(void) { ring_test_flush_fence_case(1); }
static void ioctl_ring_cancel_preparing_flush_test(void) { ring_test_flush_fence_case(2); }
static void ioctl_ring_cancel_ready_flush_test(void) { ring_test_flush_fence_case(3); }
static void ioctl_ring_abort_published_flush_test(void) { ring_test_flush_fence_case(4); }
static void ioctl_ring_reset_published_flush_test(void) { ring_test_flush_fence_case(5); }
static void ioctl_ring_cancel_queued_flush_test(void) { ring_test_flush_fence_case(6); }
static void ioctl_ring_stop_published_flush_test(void) { ring_test_flush_fence_case(7); }
static void ioctl_ring_two_flush_fences_test(void) { ring_test_flush_fence_case(8); }

static void ioctl_ring_noncdb_flush_helpers_test(void)
{
    const GUID Guid = { 0x51aeb099, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x59 } };
    RING_TEST_STATE State;
    ring_test_create(&State, &Guid, 4);
    ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_NONCDB_HELPERS, 0, 0, 0));
    ring_test_destroy(&State);
}


typedef struct
{
    UINT32 Btl;
    HANDLE Device;
    HANDLE Entered, Finished;
    DWORD Error;
} RING_TEST_CLOSE_THREAD;

static DWORD WINAPI ring_test_close_thread(PVOID Context)
{
    RING_TEST_CLOSE_THREAD *Work = Context;
    ASSERT(SetEvent(Work->Entered));
    Work->Error = SpdIoctlRingClose(Work->Device, Work->Btl);
    SetEvent(Work->Finished);
    return Work->Error;
}

/* Session controls must identify the OPEN file object, including after BTL reuse. */
static VOID ring_test_reject_foreign_controls(HANDLE Device, UINT32 Btl)
{
    SPD_IOCTL_RING_KICK_PARAMS Kick = { 0 };
    SPD_IOCTL_RING_WAIT_PARAMS Wait = { 0 };
    ASSERT(ERROR_ACCESS_DENIED == ring_test_stop_handle(Device, Btl));
    ASSERT(ERROR_ACCESS_DENIED == SpdIoctlRingClose(Device, Btl));
    ASSERT(ERROR_ACCESS_DENIED == SpdIoctlRingKick(Device, Btl, &Kick));
    ASSERT(ERROR_ACCESS_DENIED == SpdIoctlRingWait(Device, Btl, &Wait));
}

static void ioctl_ring_session_handle_test(void)
{
    const GUID Guid = { 0x51aeb0a2, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x62 } };
    RING_TEST_STATE State;
    HANDLE Foreign, Old;
    UINT32 Btl;
    ring_test_create(&State, &Guid, 4);
    Btl = State.StorageUnit->Btl;
    ASSERT(ERROR_SUCCESS == SpdIoctlOpenDevice(L"" SPD_IOCTL_HARDWARE_ID, &Foreign));
    ring_test_reject_foreign_controls(Foreign, Btl);
    CloseHandle(Foreign);
    ASSERT(DuplicateHandle(GetCurrentProcess(), State.StorageUnit->Handle,
        GetCurrentProcess(), &Old, 0, FALSE, DUPLICATE_SAME_ACCESS));
    ring_test_destroy(&State);
    ring_test_create(&State, &Guid, 4);
    ASSERT(Btl == State.StorageUnit->Btl);
    ring_test_reject_foreign_controls(Old, Btl);
    CloseHandle(Old);
    ring_test_start(&State, 2);
    ASSERT(ERROR_SUCCESS == ring_test_disk_read(&State));
    ring_test_destroy(&State);
}

static void ioctl_ring_unprovision_callback_test(void)
{
    const GUID Guid = { 0x51aeb0a3, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x63 } };
    const GUID ReplacementGuid = { 0x51aeb0a4, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x64 } };
    RING_TEST_STATE State, Replacement;
    OVERLAPPED Io;
    UINT8 Data[RingTestDataLength];
    MEMORY_BASIC_INFORMATION Info;
    UINT32 Btl;
    HANDLE Done = CreateEventW(0, TRUE, FALSE,
        L"Global\\WinSpdSharedRingTestOwnerRundownDone");
    ASSERT(0 != Done && ResetEvent(Done));
    ring_test_create(&State, &Guid, 4);
    ring_test_start(&State, 2);
    State.SlowReadBlockAddress = RingTestBlockAddress;
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    ASSERT(ERROR_SUCCESS == ring_test_begin_file_read(Disk,
        RingTestBlockAddress, Data, sizeof Data, &Io));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.SlowReadStarted, RingTestWaitTimeoutMs));
    Btl = State.StorageUnit->Btl;
    State.ExpectedDispatcherError = ERROR_OPERATION_ABORTED;
    PVOID Mapping = State.StorageUnit->SharedRingAddress;
    ASSERT(ERROR_SUCCESS == SpdIoctlUnprovision(State.StorageUnit->Handle, &Guid));
    ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Done, 0));
    ASSERT(sizeof Info == VirtualQuery(Mapping, &Info, sizeof Info));
    ASSERT(MEM_COMMIT == Info.State);
    /* The removed mapping remains owned by this callback; its address is reserved. */
    ring_test_create(&Replacement, &ReplacementGuid, 4);
    ASSERT(Btl != Replacement.StorageUnit->Btl);
    ASSERT(SetEvent(State.ReleaseSlowRead));
    SpdStorageUnitWaitDispatcher(State.StorageUnit);
    ASSERT(ERROR_SUCCESS == SpdIoctlRingClose(State.StorageUnit->Handle, Btl));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Done, RingTestWaitTimeoutMs));
    DWORD Transferred;
    (void)GetOverlappedResult(Disk, &Io, &Transferred, TRUE);
    CloseHandle(Io.hEvent);
    CloseHandle(Disk);
    ring_test_destroy(&State);
    ring_test_start(&Replacement, 2);
    ASSERT(ERROR_SUCCESS == ring_test_disk_read(&Replacement));
    ring_test_destroy(&Replacement);
    CloseHandle(Done);
}

typedef struct
{
    SPD_STORAGE_UNIT *Unit;
    SPD_IOCTL_RING_OPEN_PARAMS Params;
    DWORD Error;
} RING_TEST_SESSION_OPEN_THREAD;

static DWORD WINAPI ring_test_session_open_thread(PVOID Context)
{
    RING_TEST_SESSION_OPEN_THREAD *Work = Context;
    Work->Error = SpdStorageUnitOpenSharedRing(Work->Unit, &Work->Params);
    return Work->Error;
}

static void ioctl_ring_open_owner_change_test(void)
{
    const GUID Guid = { 0x51aeb0a7, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x67 } };
    RING_TEST_STATE State;
    RING_TEST_SESSION_OPEN_THREAD Work = { 0 };
    ring_test_create_transport(&State, &Guid, 4, 128, FALSE, FALSE, 4096, FALSE);
    HANDLE Entered = CreateEventW(0, TRUE, FALSE,
        L"Global\\WinSpdSharedRingTestOpenCommitEntered");
    HANDLE Release = CreateEventW(0, TRUE, FALSE,
        L"Global\\WinSpdSharedRingTestOpenCommitRelease");
    ASSERT(0 != Entered && 0 != Release);
    ASSERT(ResetEvent(Entered) && ResetEvent(Release));
    Work.Unit = State.StorageUnit;
    Work.Params.Version = SPD_RING_VERSION;
    Work.Params.QueueDepth = 4;
    Work.Params.BufferCount = 128;
    Work.Params.BufferSize = 4096;
    HANDLE Thread = CreateThread(0, 0, ring_test_session_open_thread, &Work, 0, 0);
    ASSERT(0 != Thread);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Entered, RingTestWaitTimeoutMs));
    DWORD Changed = SpdIoctlSetTransactProcessId(State.StorageUnit->Handle,
        State.StorageUnit->Btl, GetCurrentProcessId() + 1);
    ASSERT(SetEvent(Release));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Thread, RingTestWaitTimeoutMs));
    ASSERT(ERROR_SUCCESS == Changed && ERROR_ACCESS_DENIED == Work.Error);
    ASSERT(0 == State.StorageUnit->SharedRingAddress);
    ASSERT(ERROR_SUCCESS == SpdIoctlSetTransactProcessId(State.StorageUnit->Handle,
        State.StorageUnit->Btl, GetCurrentProcessId()));
    ASSERT(ERROR_SUCCESS == SpdStorageUnitOpenSharedRing(State.StorageUnit, &Work.Params));
    CloseHandle(Thread);
    CloseHandle(Entered);
    CloseHandle(Release);
    ring_test_start(&State, 2);
    ASSERT(ERROR_SUCCESS == ring_test_disk_read(&State));
    ring_test_destroy(&State);
}

static DWORD WINAPI ring_test_reset_thread(PVOID Context)
{
    RING_TEST_STOP_THREAD *Work = Context;
    Work->Error = ring_test_reset_device(Work->State);
    SetEvent(Work->Finished);
    return Work->Error;
}

static void ioctl_ring_reset_close_rundown_test(void)
{
    const GUID Guid = { 0x51aeb0a6, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x66 } };
    RING_TEST_STATE State;
    RING_TEST_SCSI_IO Io;
    UINT8 Data[RingTestDataLength];
    RING_TEST_STOP_THREAD Reset = { 0 };
    RING_TEST_CLOSE_THREAD Close = { 0 };
    ring_test_create(&State, &Guid, 4);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    ASSERT(ERROR_SUCCESS == ring_test_begin_scsi_io(Disk, FALSE,
        RingTestBlockAddress, Data, sizeof Data, &Io));
    ASSERT(1 == ring_test_wait_for_sq_count(&State, 1));
    HANDLE Entered = CreateEventW(0, TRUE, FALSE,
        L"Global\\WinSpdSharedRingTestResetRetireEntered");
    HANDLE Release = CreateEventW(0, TRUE, FALSE,
        L"Global\\WinSpdSharedRingTestResetRetireRelease");
    ASSERT(0 != Entered && 0 != Release);
    ASSERT(ResetEvent(Entered) && ResetEvent(Release));
    Reset.State = &State;
    Reset.Finished = CreateEventW(0, TRUE, FALSE, 0);
    Close.Device = State.StorageUnit->Handle;
    Close.Btl = State.StorageUnit->Btl;
    Close.Entered = CreateEventW(0, TRUE, FALSE, 0);
    Close.Finished = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != Reset.Finished && 0 != Close.Entered && 0 != Close.Finished);
    HANDLE ResetThread = CreateThread(0, 0, ring_test_reset_thread, &Reset, 0, 0);
    ASSERT(0 != ResetThread);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Entered, RingTestWaitTimeoutMs));
    ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Reset.Finished, 0));
    HANDLE CloseThread = CreateThread(0, 0, ring_test_close_thread, &Close, 0, 0);
    ASSERT(0 != CloseThread);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Close.Entered, RingTestWaitTimeoutMs));
    DWORD Early = WaitForSingleObject(Close.Finished, 200);
    ASSERT(SetEvent(Release));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(ResetThread, RingTestWaitTimeoutMs));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(CloseThread, RingTestWaitTimeoutMs));
    ASSERT(WAIT_TIMEOUT == Early);
    ASSERT(ERROR_SUCCESS == Reset.Error && ERROR_SUCCESS == Close.Error);
    ASSERT(ring_test_scsi_aborted(ring_test_finish_scsi_io(Disk, &Io, RingTestWaitTimeoutMs)));
    CloseHandle(Io.Overlapped.hEvent);
    CloseHandle(Disk);
    CloseHandle(ResetThread);
    CloseHandle(CloseThread);
    CloseHandle(Reset.Finished);
    CloseHandle(Close.Entered);
    CloseHandle(Close.Finished);
    CloseHandle(Entered);
    CloseHandle(Release);
    ring_test_destroy(&State);
}

static void ioctl_ring_concurrent_close_test(void)
{
    const GUID Guid = { 0x51aeb0a1, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x61 } };
    RING_TEST_STATE State;
    RING_TEST_SCSI_IO Io;
    RING_TEST_CLOSE_THREAD Close[2] = { 0 };
    HANDLE Threads[2];
    UINT8 Data[RingTestDataLength] = { 0 };
    SPD_IOCTL_RING_WAIT_PARAMS Wait;
    ring_test_create(&State, &Guid, 4);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    ASSERT(ResetEvent(State.ProducerPrepareEntered));
    ASSERT(ResetEvent(State.ProducerPrepareRelease));
    ASSERT(ERROR_SUCCESS == ring_test_begin_scsi_io(Disk, TRUE,
        RingTestBlockAddress, Data, sizeof Data, &Io));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.ProducerPrepareEntered, RingTestWaitTimeoutMs));
    ASSERT(ERROR_SUCCESS == ring_test_begin_wait(&State));
    for (UINT32 I = 0; I < ARRAYSIZE(Close); I++)
    {
        Close[I].Btl = State.StorageUnit->Btl;
        Close[I].Device = State.StorageUnit->Handle;
        Close[I].Entered = CreateEventW(0, TRUE, FALSE, 0);
        Close[I].Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Close[I].Entered && 0 != Close[I].Finished);
        Threads[I] = CreateThread(0, 0, ring_test_close_thread, &Close[I], 0, 0);
        ASSERT(0 != Threads[I]);
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Close[I].Entered, RingTestWaitTimeoutMs));
    }
    DWORD Error = ring_test_finish_wait(&State, &Wait, RingTestWaitTimeoutMs);
    ASSERT(ERROR_OPERATION_ABORTED == Error || ERROR_CANCELLED == Error);
    for (UINT32 I = 0; I < ARRAYSIZE(Close); I++)
        ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Close[I].Finished, 0));
    ASSERT(SetEvent(State.ProducerPrepareRelease));
    for (UINT32 I = 0; I < ARRAYSIZE(Close); I++)
    {
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Close[I].Finished, RingTestWaitTimeoutMs));
        ASSERT(ERROR_SUCCESS == Close[I].Error);
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Threads[I], RingTestWaitTimeoutMs));
        CloseHandle(Threads[I]);
        CloseHandle(Close[I].Entered);
        CloseHandle(Close[I].Finished);
    }
    ASSERT(ring_test_scsi_aborted(ring_test_finish_scsi_io(Disk, &Io, RingTestWaitTimeoutMs)));
    /* Update SDK mapping fields through its normal close path after the two
     * kernel CLOSE calls; this also checks terminal CLOSE idempotency. */
    SpdStorageUnitCloseSharedRing(State.StorageUnit);
    CloseHandle(Io.Overlapped.hEvent);
    ring_test_destroy(&State);
    CloseHandle(Disk);
}

static void ioctl_ring_mixed_copy_budgets_test(void)
{
    enum { Count = 9, Maximum = 2 * 1024 * 1024, Cancelled = 0, StrideBlocks = Maximum / 256 };
    static const DWORD Lengths[Count] = { 512, 256 * 1024, 512 * 1024, 512 * 1024,
        Maximum, Maximum, 4096, 4096, 128 * 1024 };
    static const BOOLEAN Writes[Count] = { TRUE, TRUE, FALSE, TRUE, FALSE, TRUE, FALSE, TRUE, TRUE };
    const GUID Guid = { 0x51aeb0a2, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x62 } };
    RING_TEST_STATE State;
    OVERLAPPED Io[Count];
    RING_TEST_SCSI_IO CancelledIo;
    UINT8 *Data[Count];
    ring_test_create_with_limits(&State, &Guid, 16, 16, FALSE, FALSE, Maximum);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ | GENERIC_WRITE);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    ASSERT(ResetEvent(State.ProducerPrepareEntered));
    ASSERT(ResetEvent(State.ProducerPrepareRelease));
    for (UINT32 I = 0; I < Count; I++)
    {
        Data[I] = VirtualAlloc(0, Lengths[I], MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        ASSERT(0 != Data[I]);
        for (DWORD J = 0; J < Lengths[I]; J++)
            Data[I][J] = Writes[I] ? ring_test_byte_pattern((UINT64)I * 0x100000 + J) : 0xe5;
        /* Cancel an explicit command so disk.sys cannot retry it as a new
         * SRB. The surviving file I/Os retain normal tagged batch admission. */
        DWORD Error = Cancelled == I ? ring_test_begin_scsi_io(Disk, Writes[I],
            (UINT64)I * StrideBlocks, Data[I], Lengths[I], &CancelledIo) :
            ring_test_begin_file_io(Disk, Writes[I], (UINT64)I * StrideBlocks,
                Data[I], Lengths[I], &Io[I]);
        ASSERT(ERROR_SUCCESS == Error);
        if (0 == I)
            ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.ProducerPrepareEntered, RingTestWaitTimeoutMs));
    }
    ASSERT(CancelIoEx(Disk, &CancelledIo.Overlapped));
    ASSERT(ERROR_SUCCESS == ring_test_cancel_owned_srb(&State, SpdIoctlTransactWriteKind,
        (UINT64)Cancelled * StrideBlocks, SPD_TEST_CANCEL_PREPARING));
    ASSERT(SetEvent(State.ProducerPrepareRelease));
    SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
    ULONGLONG Deadline = GetTickCount64() + RingTestWaitTimeoutMs;
    UINT32 Completed;
    do
    {
        Completed = 0;
        for (UINT32 I = 0; I < Count; I++)
            if (WAIT_OBJECT_0 == WaitForSingleObject(Cancelled == I ? CancelledIo.Overlapped.hEvent : Io[I].hEvent, 0))
                Completed++;
        if (Count == Completed)
            break;
        if (0 == ring_test_request_available(&State))
        {
            Sleep(1);
            continue;
        }
        UINT32 Head = SpdRingLoadAcquire32(&Header->RequestHead.Value);
        SPD_RING_REQUEST *Request = (SPD_RING_REQUEST *)((PUINT8)State.StorageUnit->SharedRingAddress +
            Header->RequestOffset) + (Head & (Header->QueueDepth - 1));
        if (SpdIoctlTransactWriteKind == Request->Request.Kind)
        {
            UINT64 Lba = Request->Request.Op.Write.BlockAddress;
            UINT32 Index = (UINT32)(Lba / StrideBlocks);
            UINT32 Offset = (UINT32)(Lba % StrideBlocks) * 512;
            ASSERT(Index < Count && Index != Cancelled && Writes[Index]);
            ASSERT(Request->Data.BufferId < Header->BufferCount);
            ASSERT(Offset + Request->Data.Length <= Lengths[Index]);
            PVOID Payload = (PUINT8)State.StorageUnit->SharedRingAddress + Header->BufferOffset +
                (SIZE_T)Request->Data.BufferId * Header->BufferSize;
            ASSERT(0 == memcmp(Payload, Data[Index] + Offset, Request->Data.Length));
        }
        else
            ASSERT(SpdIoctlTransactReadKind == Request->Request.Kind);
        BOOLEAN TestRead;
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(&State, &TestRead));
    } while (GetTickCount64() < Deadline);
    ASSERT(Count == Completed);
    for (UINT32 I = 0; I < Count; I++)
    {
        DWORD Error = Cancelled == I ? ring_test_finish_scsi_io(Disk, &CancelledIo, RingTestWaitTimeoutMs) :
            ring_test_finish_overlapped_io(Disk, &Io[I], RingTestWaitTimeoutMs);
        if (Cancelled == I)
            ASSERT(ERROR_SUCCESS != Error && ERROR_TIMEOUT != Error);
        else
        {
            ASSERT(ERROR_SUCCESS == Error);
            DWORD Bytes;
            ASSERT(GetOverlappedResult(Disk, &Io[I], &Bytes, FALSE));
            ASSERT(Lengths[I] == Bytes);
            if (!Writes[I])
                for (UINT32 J = 0; J < Lengths[I]; J++)
                    ASSERT(0 == Data[I][J]);
        }
        CloseHandle(Cancelled == I ? CancelledIo.Overlapped.hEvent : Io[I].hEvent);
        ASSERT(VirtualFree(Data[I], 0, MEM_RELEASE));
    }
    ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_COPY_BUDGET, 0, 0, 0));
    ring_test_destroy(&State);
    CloseHandle(Disk);
}

static void ioctl_legacy_production_lifecycle_test(void)
{
    enum { Length = 12 * 1024 };
    const GUID Guid = { 0x51aeb0a3, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x63 } };
    RING_TEST_STATE State;
    UINT8 WriteData[Length], ReadData[Length];
    DWORD Transferred;
    LARGE_INTEGER Offset;
    ring_test_create_transport(&State, &Guid, 16, 16, FALSE, FALSE, 16384, FALSE);
    State.ExpectedDispatcherError = ERROR_OPERATION_ABORTED; /* Legacy shutdown cancels TRANSACT. */
    ring_test_start(&State, 4);
    ASSERT(0 == State.StorageUnit->SharedRingAddress);
    ULONGLONG Deadline = GetTickCount64() + RingTestWaitTimeoutMs;
    DWORD ConfigureError;
    do
    {
        ConfigureError = ring_test_owned_control(&State, SPD_IOCTL_TEST_CHUNK_LIMIT, 0, 0, 4096);
        if (ERROR_SUCCESS == ConfigureError)
            break;
        ASSERT(ERROR_BUSY == ConfigureError);
        Sleep(1);
    } while (GetTickCount64() < Deadline);
    ASSERT(ERROR_SUCCESS == ConfigureError);
    InterlockedExchange(&State.VerifyChunkOffsets, 1);
    for (UINT32 I = 0; I < Length; I++)
        WriteData[I] = ring_test_byte_pattern((UINT64)RingTestBlockAddress * 512 + I);
    HANDLE Disk = CreateFileW(State.DiskPath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    Offset.QuadPart = (UINT64)RingTestBlockAddress * 512;
    ASSERT(SetFilePointerEx(Disk, Offset, 0, FILE_BEGIN));
    InterlockedExchange(&State.WriteCallbacks, 0);
    ASSERT(WriteFile(Disk, WriteData, sizeof WriteData, &Transferred, 0));
    ASSERT(Length == Transferred && 3 == State.WriteCallbacks);
    ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State, SPD_IOCTL_TEST_LEGACY_CHUNKS, 0, 0, 0));
    ASSERT(SetFilePointerEx(Disk, Offset, 0, FILE_BEGIN));
    InterlockedExchange(&State.ReadCallbacks, 0);
    ASSERT(ReadFile(Disk, ReadData, sizeof ReadData, &Transferred, 0));
    ASSERT(Length == Transferred && 3 == State.ReadCallbacks);
    ASSERT(0 == memcmp(WriteData, ReadData, sizeof WriteData));
    LONG Before = State.FlushCallbacks;
    ASSERT(FlushFileBuffers(Disk));
    ASSERT(State.FlushCallbacks > Before);
    CloseHandle(Disk);
    ring_test_destroy(&State);
}

/* Keep the requester separate from the backend, as in the real WSL path.
 * Backend-originated I/O intentionally falls back to copying to avoid an exit
 * dependency between its own pending SRB and process rundown. */
static void ring_mapping_io_child(void)
{
    WCHAR Path[1024], Text[32];
    ASSERT(0 != GetEnvironmentVariableW(L"WINSPD_MAP_TEST_PATH", Path, ARRAYSIZE(Path)));
    ASSERT(0 != GetEnvironmentVariableW(L"WINSPD_MAP_TEST_LENGTH", Text, ARRAYSIZE(Text)));
    DWORD Length = wcstoul(Text, 0, 10);
    ASSERT(Length > 0 && Length <= 256 * 1024);
    ASSERT(0 != GetEnvironmentVariableW(L"WINSPD_MAP_TEST_TERMINAL", Text, ARRAYSIZE(Text)));
    UINT32 Mode = wcstoul(Text, 0, 10); /* data, terminal read, cancelled SCSI */
    BOOLEAN Terminal = 0 != Mode;
    UINT8 *Data = VirtualAlloc(0, Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT(0 != Data);
    HANDLE Disk = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | (2 == Mode ? FILE_FLAG_OVERLAPPED : 0), 0);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    if (2 == Mode)
    {
        HANDLE Ready = OpenEventW(EVENT_MODIFY_STATE, FALSE,
            L"Global\\WinSpdSharedRingTestMapRequesterReady");
        HANDLE Start = OpenEventW(SYNCHRONIZE, FALSE,
            L"Global\\WinSpdSharedRingTestMapRequesterStart");
        ASSERT(0 != Ready && 0 != Start && SetEvent(Ready));
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Start, RingTestWaitTimeoutMs));
        CloseHandle(Ready);
        CloseHandle(Start);
    }
    LARGE_INTEGER Offset;
    DWORD Transferred = 0;
    Offset.QuadPart = (LONGLONG)RingTestBlockAddress * RingTestDataLength;
    if (!Terminal)
    {
        for (DWORD I = 0; I < Length; I++)
            Data[I] = ring_test_byte_pattern(Offset.QuadPart + I);
        ASSERT(SetFilePointerEx(Disk, Offset, 0, FILE_BEGIN));
        ASSERT(WriteFile(Disk, Data, Length, &Transferred, 0));
        ASSERT(Length == Transferred);
    }
    memset(Data, 0xcc, Length);
    ASSERT(SetFilePointerEx(Disk, Offset, 0, FILE_BEGIN));
    DWORD Error;
    if (2 == Mode)
    {
        RING_TEST_SCSI_IO Io;
        ASSERT(ERROR_SUCCESS == ring_test_begin_scsi_io(Disk, FALSE,
            RingTestBlockAddress, Data, Length, &Io));
        Error = ring_test_finish_scsi_io(Disk, &Io, RingTestWaitTimeoutMs);
        CloseHandle(Io.Overlapped.hEvent);
        tlib_printf("mapped SCSI cancellation error=%lu status=%u\n",
            (unsigned long)Error, Io.Packet.Base.ScsiStatus);
        ASSERT(ring_test_scsi_aborted(Error));
    }
    else
    {
        BOOL Success = ReadFile(Disk, Data, Length, &Transferred, 0);
        Error = Success ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(Disk);
    if (Terminal)
        ASSERT(ERROR_SUCCESS != Error);
    else
    {
        ASSERT(ERROR_SUCCESS == Error && Length == Transferred);
        for (DWORD I = 0; I < Length; I++)
            ASSERT(Data[I] == ring_test_byte_pattern(Offset.QuadPart + I));
    }
    VirtualFree(Data, 0, MEM_RELEASE);
    WCHAR DoneName[128];
    if (GetEnvironmentVariableW(L"WINSPD_MAP_TEST_DONE_EVENT", DoneName, ARRAYSIZE(DoneName)))
    {
        HANDLE Done = OpenEventW(EVENT_MODIFY_STATE, FALSE, DoneName);
        ASSERT(0 != Done && SetEvent(Done));
        CloseHandle(Done);
    }
}

static HANDLE ring_test_start_mapping_requester(RING_TEST_STATE *State,
    DWORD Length, UINT32 Mode)
{
    WCHAR FileName[MAX_PATH], CommandLine[MAX_PATH + 64], Text[32];
    const WCHAR *Names[] = { L"WINSPD_MAP_TEST_PATH", L"WINSPD_MAP_TEST_LENGTH",
        L"WINSPD_MAP_TEST_TERMINAL" };
    WCHAR Previous[3][1024];
    DWORD Present[3];
    for (UINT32 I = 0; I < ARRAYSIZE(Names); I++)
        Present[I] = GetEnvironmentVariableW(Names[I], Previous[I], ARRAYSIZE(Previous[I]));
    ASSERT(SetEnvironmentVariableW(Names[0], State->DiskPath));
    _snwprintf_s(Text, ARRAYSIZE(Text), _TRUNCATE, L"%lu", (unsigned long)Length);
    ASSERT(SetEnvironmentVariableW(Names[1], Text));
    _snwprintf_s(Text, ARRAYSIZE(Text), _TRUNCATE, L"%u", Mode);
    ASSERT(SetEnvironmentVariableW(Names[2], Text));
    ASSERT(0 != GetModuleFileNameW(0, FileName, ARRAYSIZE(FileName)));
    _snwprintf_s(CommandLine, ARRAYSIZE(CommandLine), _TRUNCATE,
        L"\"%s\" +ring_mapping_io_child", FileName);
    STARTUPINFOW StartupInfo = { sizeof StartupInfo };
    PROCESS_INFORMATION ProcessInfo;
    BOOL Success = CreateProcessW(FileName, CommandLine, 0, 0, FALSE, 0,
        0, 0, &StartupInfo, &ProcessInfo);
    for (UINT32 I = 0; I < ARRAYSIZE(Names); I++)
        SetEnvironmentVariableW(Names[I], Present[I] ? Previous[I] : 0);
    ASSERT(Success);
    CloseHandle(ProcessInfo.hThread);
    return ProcessInfo.hProcess;
}

static void ring_test_finish_mapping_requester(HANDLE Process)
{
    DWORD Wait = WaitForSingleObject(Process, RingTestWaitTimeoutMs);
    DWORD ExitCode = STILL_ACTIVE;
    GetExitCodeProcess(Process, &ExitCode);
    CloseHandle(Process);
    ASSERT(WAIT_OBJECT_0 == Wait && 0 == ExitCode);
}

static void ioctl_ring_mapped_payload_test(void)
{
    const GUID Guid = { 0x51aeb0b1, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x61 } };
    const DWORD Lengths[] = { 4096, 65536, 131072, 65536 + 512, 131072 };
    RING_TEST_STATE State;
    ring_test_create_with_limits(&State, &Guid, 8, 8, FALSE, TRUE, 256 * 1024);
    ring_test_start(&State, 2);
    State.MappingChecks = 1;
    State.VerifyChunkOffsets = 1;
    for (UINT32 N = 0; N < ARRAYSIZE(Lengths); N++)
    {
        DWORD Length = Lengths[N];
        BOOLEAN Multichunk = N == ARRAYSIZE(Lengths) - 1;
        if (Multichunk)
            ASSERT(ERROR_SUCCESS == ring_test_owned_control(&State,
                SPD_IOCTL_TEST_CHUNK_LIMIT, 0, 0, 16384));
        State.MappedReadCallbacks = State.MappedWriteCallbacks = State.ReadOnlyMappings = 0;
        ring_test_finish_mapping_requester(ring_test_start_mapping_requester(&State, Length, FALSE));
        UINT32 Threshold = State.StorageUnit->SharedRingHeader->MapThresholdBytes;
        BOOLEAN ExpectedMapped = Threshold && Length >= Threshold && 0 == Length % 4096;
        tlib_printf("payload length=%lu threshold=%u mapped-read=%ld mapped-write=%ld readonly=%ld\n",
            (unsigned long)Length, Threshold, State.MappedReadCallbacks,
            State.MappedWriteCallbacks, State.ReadOnlyMappings);
        ASSERT(ExpectedMapped == (State.MappedReadCallbacks > 0));
        ASSERT(ExpectedMapped == (State.MappedWriteCallbacks > 0));
        ASSERT(State.MappedWriteCallbacks == State.ReadOnlyMappings);
        if (ExpectedMapped && Multichunk)
            ASSERT(State.MappedReadCallbacks >= 8 && State.MappedWriteCallbacks >= 8);
        ASSERT(0 == State.Failure);
    }
    ring_test_destroy(&State);
}

static void ring_test_mapped_terminal_case(BOOLEAN Reset)
{
    const GUID Guid = { 0x51aeb0b2, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x61 } };
    RING_TEST_STATE State;
    ring_test_create_with_limits(&State, &Guid, 8, 8, FALSE, TRUE, 256 * 1024);
    ring_test_start(&State, 2);
    State.MappingChecks = 1;
    State.SlowReadBlockAddress = RingTestBlockAddress;
    BOOLEAN MappingEnabled = 0 != State.StorageUnit->SharedRingHeader->MapThresholdBytes;
    HANDLE Requester = ring_test_start_mapping_requester(&State, 131072, TRUE);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.SlowReadStarted, RingTestWaitTimeoutMs));
    if (Reset)
    {
        State.ExpectedDispatcherError = ERROR_OPERATION_ABORTED;
        ASSERT(ERROR_SUCCESS == ring_test_reset_device(&State));
    }
    else
        ASSERT(ERROR_SUCCESS == ring_test_stop_ring(&State));
    /* STOP/reset must retain SRB pages while the callback owns its alias. */
    BOOLEAN Retained = WAIT_TIMEOUT == WaitForSingleObject(Requester, 0);
    ASSERT(SetEvent(State.ReleaseSlowRead));
    SpdStorageUnitShutdown(State.StorageUnit);
    SpdStorageUnitWaitDispatcher(State.StorageUnit);
    DWORD DispatcherError;
    SpdStorageUnitGetDispatcherError(State.StorageUnit, &DispatcherError);
    /* Match the production contract: backend access has ended; CLOSE releases
     * retained SRBs. Do not wait for those SRBs before issuing CLOSE. */
    SpdStorageUnitCloseSharedRing(State.StorageUnit);
    ring_test_finish_mapping_requester(Requester);
    /* The pump can observe terminal KICK cancellation before Shutdown, or
     * Shutdown can stop it first. Both are valid cooperative rundown results. */
    ASSERT(ERROR_SUCCESS == DispatcherError || ERROR_OPERATION_ABORTED == DispatcherError);
    State.ExpectedDispatcherError = DispatcherError;
    if (MappingEnabled)
    {
        ASSERT(Retained);
        ASSERT(State.MappedReadCallbacks > 0);
    }
    ASSERT(0 == State.Failure);
    ring_test_destroy(&State);
}

static void ioctl_ring_mapped_stop_test(void) { ring_test_mapped_terminal_case(FALSE); }
static void ioctl_ring_mapped_reset_test(void) { ring_test_mapped_terminal_case(TRUE); }

static void ioctl_ring_mapped_cancel_states_test(void)
{
    for (UINT32 Stage = 0; Stage < 4; Stage++)
    {
        GUID Guid = { 0x51aeb0b5, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x61 } };
        Guid.Data1 += Stage;
        RING_TEST_STATE State;
        ring_test_create_with_limits(&State, &Guid, 8, 8, FALSE, TRUE, 256 * 1024);
        ring_test_start(&State, 2);
        State.MappingChecks = 1;
        HANDLE Entered[] = { State.ProducerPrepareEntered, State.ReadyEntered,
            State.CompletionClaimEntered, State.CompletionCopyEntered };
        HANDLE Release[] = { State.ProducerPrepareRelease, State.ReadyRelease,
            State.CompletionClaimRelease, State.CompletionCopyRelease };
        const UINT32 CancelState[] = { SPD_TEST_CANCEL_PREPARING, SPD_TEST_CANCEL_READY,
            SPD_TEST_CANCEL_COMPLETION_QUEUED, SPD_TEST_CANCEL_COMPLETING };
        HANDLE RequesterReady = CreateEventW(0, TRUE, FALSE,
            L"Global\\WinSpdSharedRingTestMapRequesterReady");
        HANDLE RequesterStart = CreateEventW(0, TRUE, FALSE,
            L"Global\\WinSpdSharedRingTestMapRequesterStart");
        ASSERT(0 != RequesterReady && 0 != RequesterStart);
        ASSERT(ResetEvent(RequesterReady) && ResetEvent(RequesterStart));
        HANDLE Requester = ring_test_start_mapping_requester(&State, 131072, 2);
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(RequesterReady, RingTestWaitTimeoutMs));
        ASSERT(ResetEvent(Entered[Stage]) && ResetEvent(Release[Stage]));
        ASSERT(SetEvent(RequesterStart));
        DWORD Started = WaitForSingleObject(Entered[Stage], RingTestWaitTimeoutMs);
        DWORD CancelError = ring_test_cancel_owned_srb(&State,
            SpdIoctlTransactReadKind, RingTestBlockAddress, CancelState[Stage]);
        tlib_printf("mapped cancel stage=%u entered=%lu cancel=%lu\n",
            Stage, (unsigned long)Started, (unsigned long)CancelError);
        SetEvent(Release[Stage]);
        ring_test_finish_mapping_requester(Requester);
        CloseHandle(RequesterReady);
        CloseHandle(RequesterStart);
        LONG MappedReads = State.MappedReadCallbacks;
        BOOLEAN MappingEnabled = State.StorageUnit->SharedRingHeader->MapThresholdBytes != 0;
        ring_test_destroy(&State);
        ASSERT(WAIT_OBJECT_0 == Started && ERROR_SUCCESS == CancelError);
        if (MappingEnabled && Stage >= 2)
            ASSERT(MappedReads > 0);
        ASSERT(0 == State.Failure);
    }
}

static void ioctl_ring_mapped_requester_kill_test(void)
{
    const GUID Guid = { 0x51aeb0b4, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x61 } };
    RING_TEST_STATE State;
    ring_test_create_with_limits(&State, &Guid, 8, 8, FALSE, TRUE, 256 * 1024);
    ring_test_start(&State, 2);
    State.MappingChecks = 1;
    State.SlowReadBlockAddress = RingTestBlockAddress;
    BOOLEAN MappingEnabled = 0 != State.StorageUnit->SharedRingHeader->MapThresholdBytes;
    HANDLE Requester = ring_test_start_mapping_requester(&State, 131072, TRUE);
    DWORD Started = WaitForSingleObject(State.SlowReadStarted, RingTestWaitTimeoutMs);
    BOOL Killed = TerminateProcess(Requester, ERROR_PROCESS_ABORTED);
    /* Cancelling the original request cannot revoke a published buffer while
     * the backend still owns it. Let the backend finish before waiting for
     * requester termination (Windows may wait for its pending I/O). */
    BOOLEAN Retained = WAIT_TIMEOUT == WaitForSingleObject(Requester, 0);
    LONG MappedReads = State.MappedReadCallbacks;
    SetEvent(State.ReleaseSlowRead);
    DWORD Wait = WaitForSingleObject(Requester, RingTestWaitTimeoutMs);
    DWORD ExitCode = STILL_ACTIVE;
    GetExitCodeProcess(Requester, &ExitCode);
    CloseHandle(Requester);
    ring_test_destroy(&State);
    ASSERT(WAIT_OBJECT_0 == Started && Killed);
    ASSERT(WAIT_OBJECT_0 == Wait && ERROR_PROCESS_ABORTED == ExitCode);
    if (MappingEnabled)
        ASSERT(Retained && MappedReads > 0);
    ASSERT(0 == State.Failure);
}

static void ioctl_ring_multichunk_read_write_test(void)
{
    enum { TransferLength = 16 * 1024 };
    RING_TEST_STATE State;
    HANDLE DiskHandle;
    UINT8 WriteData[TransferLength];
    UINT8 ReadData[TransferLength];
    LARGE_INTEGER Offset;
    DWORD Transferred;
    DWORD ReadCount;
    DWORD WriteCount;

    ring_test_create(&State, &RingTestGuidChunkOffsets, 64);
    ring_test_start(&State, 8);
    InterlockedExchange(&State.VerifyChunkOffsets, 1);
    for (UINT32 I = 0; TransferLength > I; I++)
        WriteData[I] = ring_test_byte_pattern((UINT64)RingTestBlockAddress *
            RingTestDataLength + I);

    DiskHandle = CreateFileW(State.DiskPath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, 0);
    ASSERT(INVALID_HANDLE_VALUE != DiskHandle);
    Offset.QuadPart = (LONGLONG)RingTestBlockAddress *
        RingTestDataLength;
    ASSERT(SetFilePointerEx(DiskHandle, Offset, 0, FILE_BEGIN));
    InterlockedExchange(&State.WriteCallbacks, 0);
    ASSERT(WriteFile(DiskHandle, WriteData, sizeof WriteData,
        &Transferred, 0));
    ASSERT(TransferLength == Transferred);
    WriteCount = InterlockedCompareExchange(&State.WriteCallbacks, 0, 0);
    ASSERT(4 == WriteCount);

    ASSERT(SetFilePointerEx(DiskHandle, Offset, 0, FILE_BEGIN));
    InterlockedExchange(&State.ReadCallbacks, 0);
    ASSERT(ReadFile(DiskHandle, ReadData, sizeof ReadData,
        &Transferred, 0));
    ASSERT(TransferLength == Transferred);
    ReadCount = InterlockedCompareExchange(&State.ReadCallbacks, 0, 0);
    ASSERT(4 == ReadCount);
    for (UINT32 I = 0; TransferLength > I; I++)
        ASSERT(ring_test_byte_pattern((UINT64)RingTestBlockAddress * RingTestDataLength + I) ==
            ReadData[I]);

    CloseHandle(DiskHandle);
    ASSERT(0 == InterlockedCompareExchange(&State.Failure, 0, 0));
    ring_test_destroy(&State);
}

static void ioctl_ring_publish_before_wait_test(void)
{
    RING_TEST_STATE State;
    RING_TEST_READ_THREAD Read;
    HANDLE ReadThread;
    SPD_RING_HEADER *Header;
    ULONGLONG Deadline;
    DWORD Error;

    ring_test_create(&State, &RingTestGuidPublishBeforeWait, 2);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    State.PumpHookEntered = CreateEventW(0, TRUE, FALSE, 0);
    State.PumpHookRelease = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != State.PumpHookEntered);
    ASSERT(0 != State.PumpHookRelease);
    State.StorageUnit->SharedRingTestPumpHook = ring_test_pause_pump;
    State.StorageUnit->SharedRingTestPumpContext = &State;

    Error = SpdStorageUnitStartDispatcher(State.StorageUnit, 2);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(
        State.PumpHookEntered, RingTestWaitTimeoutMs));

    memset(&Read, 0, sizeof Read);
    Read.State = &State;
    Read.BlockAddress = RingTestBlockAddress;
    Read.Finished = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != Read.Finished);
    ReadThread = CreateThread(0, 0,
        ring_test_manual_handle_read_thread, &Read, 0, 0);
    ASSERT(0 != ReadThread);

    Header = State.StorageUnit->SharedRingHeader;
    Deadline = GetTickCount64() + RingTestWaitTimeoutMs;
    while (0 == ring_test_request_available(&State) &&
        GetTickCount64() < Deadline)
        Sleep(1);
    UINT32 Available = ring_test_request_available(&State);
    ASSERT(1 == Available);
    SPD_RING_REQUEST *Request = (SPD_RING_REQUEST *)
        ((PUINT8)State.StorageUnit->SharedRingAddress +
        Header->RequestOffset +
        (SpdRingLoadAcquire32(&Header->RequestHead.Value) &
            (Header->QueueDepth - 1)) * sizeof(SPD_RING_REQUEST));
    ASSERT(SpdIoctlTransactReadKind == Request->Request.Kind);
    ASSERT(RingTestBlockAddress ==
        Request->Request.Op.Read.BlockAddress);

    /* The SQE is visible while the pump is between its drain and WAIT arm. */
    ASSERT(SetEvent(State.PumpHookRelease));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(
        Read.Finished, RingTestWaitTimeoutMs));
    ASSERT(ERROR_SUCCESS == Read.Error);
    SpdStorageUnitGetDispatcherError(State.StorageUnit, &Error);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(0 == InterlockedCompareExchange(&State.Failure, 0, 0));

    CloseHandle(ReadThread);
    CloseHandle(Read.Finished);
    ring_test_destroy(&State);
}

static DWORD WINAPI ring_test_post_cancel_shutdown_thread(PVOID Context)
{
    SPD_STORAGE_UNIT *StorageUnit = Context;

    SpdStorageUnitShutdown(StorageUnit);
    SpdStorageUnitWaitDispatcher(StorageUnit);
    return 0;
}

static void ioctl_ring_post_cancel_race_test(void)
{
    RING_TEST_STATE State;
    RING_TEST_READ_THREAD Read;
    HANDLE ReadThread;
    HANDLE ShutdownThread;
    DWORD ResetWaitResult;
    DWORD ReadWaitResult;
    DWORD ShutdownWaitResult;
    DWORD Error;

    ring_test_create_with_buffer_count(&State,
        &RingTestGuidPostCancelRace, 2,
        SPD_RING_DEFAULT_LUN_QUEUE_DEPTH, TRUE);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));

    /* Earlier discovery SRBs passed through while the test release event
     * was signalled. Arm the named kernel barrier for this one READ. */
    ASSERT(ResetEvent(State.IoqPostEntered));
    ASSERT(ResetEvent(State.IoqPostRelease));
    Error = SpdStorageUnitStartDispatcher(State.StorageUnit, 2);
    ASSERT(ERROR_SUCCESS == Error);

    memset(&Read, 0, sizeof Read);
    Read.State = &State;
    Read.BlockAddress = RingTestBlockAddress;
    Read.Finished = CreateEventW(0, TRUE, FALSE, 0);
    ASSERT(0 != Read.Finished);
    ReadThread = CreateThread(0, 0,
        ring_test_manual_handle_read_thread, &Read, 0, 0);
    ASSERT(0 != ReadThread);
    if (WAIT_OBJECT_0 != WaitForSingleObject(State.IoqPostEntered,
            RingTestWaitTimeoutMs))
    {
        SetEvent(State.IoqPostRelease);
        WaitForSingleObject(ReadThread, RingTestWaitTimeoutMs);
        CloseHandle(ReadThread);
        CloseHandle(Read.Finished);
        ring_test_destroy(&State);
        ASSERT(FALSE);
        return;
    }

    /* Ring close resets the IOQ, then waits for the test hook's ring
     * rundown reference. Let shutdown run independently so this thread can
     * release the hook as soon as the reset completes the SRB. */
    ShutdownThread = CreateThread(0, 0,
        ring_test_post_cancel_shutdown_thread,
        State.StorageUnit, 0, 0);
    if (0 == ShutdownThread)
    {
        SetEvent(State.IoqPostRelease);
        SpdStorageUnitShutdown(State.StorageUnit);
        SpdStorageUnitWaitDispatcher(State.StorageUnit);
        WaitForSingleObject(ReadThread, RingTestWaitTimeoutMs);
        CloseHandle(ReadThread);
        CloseHandle(Read.Finished);
        ring_test_destroy(&State);
        ASSERT(0 != ShutdownThread);
        return;
    }

    /* The reset acknowledgment means SpdIoqReset has issued completion for
     * the paused SRB. Storport may deliver the user-mode read result only
     * after the StartIo callback is released, so wait on this test hook. */
    ResetWaitResult = WaitForSingleObject(
        State.IoqPostResetDone, RingTestWaitTimeoutMs);
    ASSERT(SetEvent(State.IoqPostRelease));
    ShutdownWaitResult = WaitForSingleObject(
        ShutdownThread, RingTestWaitTimeoutMs);
    ASSERT(WAIT_OBJECT_0 == ShutdownWaitResult);
    CloseHandle(ShutdownThread);

    ReadWaitResult = WaitForSingleObject(
        Read.Finished, RingTestWaitTimeoutMs);
    ASSERT(WAIT_OBJECT_0 == ResetWaitResult);
    ASSERT(WAIT_OBJECT_0 == ReadWaitResult);
    ASSERT(ERROR_SUCCESS != Read.Error);
    ASSERT(0 == InterlockedCompareExchange(&State.Failure, 0, 0));
    CloseHandle(ReadThread);
    CloseHandle(Read.Finished);
    ring_test_destroy(&State);
}

static void ioctl_ring_producer_concurrency_test(void)
{
    RING_TEST_STATE State;
    RING_TEST_READ_THREAD Reads[16];
    HANDLE Threads[ARRAYSIZE(Reads)];

    ring_test_create_with_buffer_count(&State,
        &RingTestGuidProducerConcurrency, 2,
        SPD_RING_DEFAULT_LUN_QUEUE_DEPTH, FALSE);
    ring_test_start(&State, ARRAYSIZE(Reads));
    memset(Reads, 0, sizeof Reads);
    memset(Threads, 0, sizeof Threads);
    for (UINT32 I = 0; ARRAYSIZE(Reads) > I; I++)
    {
        Reads[I].State = &State;
        Reads[I].BlockAddress = RingTestBlockAddress + I;
        Reads[I].Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Reads[I].Finished);
        Threads[I] = CreateThread(0, 0,
            ring_test_disk_read_thread, &Reads[I], 0, 0);
        ASSERT(0 != Threads[I]);
    }

    ASSERT(WAIT_OBJECT_0 == WaitForMultipleObjects(
        ARRAYSIZE(Threads), Threads, TRUE, RingTestWaitTimeoutMs));
    for (UINT32 I = 0; ARRAYSIZE(Reads) > I; I++)
    {
        ASSERT(ERROR_SUCCESS == Reads[I].Error);
        CloseHandle(Threads[I]);
        CloseHandle(Reads[I].Finished);
    }
    SPD_RING_HEADER *Header = State.StorageUnit->SharedRingHeader;
    UINT32 RequestTail = SpdRingLoadAcquire32(&Header->RequestTail.Value);
    UINT32 RequestHead = SpdRingLoadAcquire32(&Header->RequestHead.Value);
    UINT32 CompletionTail =
        SpdRingLoadAcquire32(&Header->CompletionTail.Value);
    UINT32 CompletionHead =
        SpdRingLoadAcquire32(&Header->CompletionHead.Value);
    ASSERT(RequestTail - RequestHead <= State.QueueDepth);
    ASSERT(CompletionTail - CompletionHead <= State.QueueDepth);
    ASSERT(0 == InterlockedCompareExchange(&State.Failure, 0, 0));
    /* The Debug driver asserts ProducerExecuting <= 1 on every worker
     * section, and RingClose checks its live refs and ActiveCalls. */
    ring_test_destroy(&State);
}

static void ioctl_ring_wait_teardown_race_test(void)
{
    RING_TEST_STATE State;
    SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
    DWORD BytesTransferred = 0;
    DWORD WaitError;
    BOOL Success;

    ring_test_create(&State, &RingTestGuidWaitTeardown, 2);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    ASSERT(ERROR_SUCCESS == ring_test_begin_wait(&State));
    Sleep(10); /* Verify STOP also releases an already parked WAIT. */
    ASSERT(ERROR_SUCCESS == ring_test_stop_ring(&State));
    memset(&WaitParams, 0, sizeof WaitParams);
    WaitError = ring_test_finish_wait(&State, &WaitParams,
        RingTestWaitTimeoutMs);
    ASSERT(ERROR_OPERATION_ABORTED == WaitError ||
        ERROR_CANCELLED == WaitError);
    ring_test_destroy(&State);

    /* Individual cancellation must release a retained WAIT without STOP. */
    ring_test_create(&State, &RingTestGuidWaitTeardown, 2);
    ring_test_wait_for_disk_manual(&State);
    ASSERT(ERROR_SUCCESS == ring_test_retire_manual_wait(&State));
    ASSERT(ERROR_SUCCESS == ring_test_begin_wait(&State));
    Success = CancelIoEx(State.StorageUnit->Handle,
        &State.ManualWaitOverlapped);
    DWORD CancelError = Success ? ERROR_SUCCESS : GetLastError();
    ASSERT(Success || ERROR_NOT_FOUND == CancelError);
    DWORD EventWait = WaitForSingleObject(State.ManualWaitEvent,
        RingTestWaitTimeoutMs);
    ASSERT(WAIT_OBJECT_0 == EventWait);
    if (WAIT_OBJECT_0 == EventWait)
    {
        if (!GetOverlappedResult(State.StorageUnit->Handle,
                &State.ManualWaitOverlapped, &BytesTransferred, FALSE))
            WaitError = GetLastError();
        else
            WaitError = ERROR_SUCCESS;
        State.ManualWaitOutstanding = FALSE;
        CloseHandle(State.ManualWaitEvent);
        State.ManualWaitEvent = 0;
    }
    ASSERT(ERROR_SUCCESS == ring_test_stop_ring(&State));
    ASSERT(ERROR_OPERATION_ABORTED == WaitError ||
        ERROR_CANCELLED == WaitError || ERROR_SUCCESS == WaitError);
    ring_test_destroy(&State);

    /* Also close the ring while callbacks and producer/KICK traffic are
     * active. STOP leaves the mapping available until these workers drain. */
    RING_TEST_READ_THREAD Reads[4];
    HANDLE Threads[ARRAYSIZE(Reads)];
    ring_test_create(&State, &RingTestGuidWaitTeardown, 2);
    ring_test_start(&State, 4);
    InterlockedExchange(&State.HoldReads, 1);
    memset(Reads, 0, sizeof Reads);
    memset(Threads, 0, sizeof Threads);
    for (UINT32 I = 0; ARRAYSIZE(Reads) > I; I++)
    {
        Reads[I].State = &State;
        Reads[I].BlockAddress = RingTestBlockAddress + I;
        Reads[I].Finished = CreateEventW(0, TRUE, FALSE, 0);
        ASSERT(0 != Reads[I].Finished);
        Threads[I] = CreateThread(0, 0,
            ring_test_disk_read_thread, &Reads[I], 0, 0);
        ASSERT(0 != Threads[I]);
    }
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(
        State.TwoReadsStarted, RingTestWaitTimeoutMs));
    SpdStorageUnitShutdown(State.StorageUnit);
    SetEvent(State.ReleaseReads);
    ASSERT(WAIT_OBJECT_0 == WaitForMultipleObjects(
        ARRAYSIZE(Threads), Threads, TRUE, RingTestWaitTimeoutMs));
    for (UINT32 I = 0; ARRAYSIZE(Reads) > I; I++)
    {
        ASSERT(ERROR_SUCCESS != Reads[I].Error);
        CloseHandle(Threads[I]);
        CloseHandle(Reads[I].Finished);
    }
    ring_test_destroy(&State);

    ring_test_owner_process_termination_test();
}

/* Internal child entry point. Keep its registered name within tlib's 63-byte
 * limit and require the parent's readiness event before exercising owner exit. */
static void ring_owner_exit_child(void)
{
    RING_TEST_STATE State;
    HANDLE Ready = OpenEventW(EVENT_MODIFY_STATE, FALSE,
        L"Global\\WinSpdSharedRingTestOwnerExitReady");
    ASSERT(0 != Ready);

    ring_test_create(&State, &RingTestGuidOwnerDeath, 2);
    for (UINT32 I = 0; I < 32; I++)
    {
        DWORD Error = ring_test_begin_wait(&State);
        if (ERROR_SUCCESS != Error)
            break;
        if (0 != ring_test_request_available(&State))
        {
            SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
            BOOLEAN TestRead;
            Error = ring_test_finish_wait(&State, &WaitParams,
                RingTestWaitTimeoutMs);
            ASSERT(ERROR_SUCCESS == Error);
            ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(
                &State, &TestRead));
            continue;
        }

        /* Give the reverse callback time to park this retained WAIT, then
         * terminate without STOP/CLOSE to exercise owner rundown. */
        DWORD WaitResult = WaitForSingleObject(State.ManualWaitEvent, 100);
        if (WAIT_TIMEOUT == WaitResult)
        {
            ASSERT(SetEvent(Ready));
            ExitProcess(0);
        }
        ASSERT(WAIT_OBJECT_0 == WaitResult);
        SPD_IOCTL_RING_WAIT_PARAMS WaitParams;
        BOOLEAN TestRead;
        Error = ring_test_finish_wait(&State, &WaitParams, 0);
        ASSERT(ERROR_SUCCESS == Error);
        ASSERT(0 != ring_test_request_available(&State));
        ASSERT(ERROR_SUCCESS == ring_test_complete_one_manually(
            &State, &TestRead));
    }
    ExitProcess(ERROR_GEN_FAILURE);
}

static void ring_owner_io_exit_child(void)
{
    const GUID Guid = { 0x51aeb0a0, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x60 } };
    RING_TEST_STATE State;
    OVERLAPPED Io;
    UINT8 Data[RingTestDataLength];
    HANDLE Ready = OpenEventW(EVENT_MODIFY_STATE, FALSE,
        L"Global\\WinSpdSharedRingTestOwnerExitReady");
    ASSERT(0 != Ready);
    ring_test_create(&State, &Guid, 4);
    ring_test_start(&State, 2);
    State.SlowReadBlockAddress = RingTestBlockAddress;
    HANDLE Disk = ring_test_open_overlapped_disk(&State, GENERIC_READ);
    ASSERT(INVALID_HANDLE_VALUE != Disk);
    ASSERT(ERROR_SUCCESS == ring_test_begin_file_read(Disk,
        RingTestBlockAddress, Data, sizeof Data, &Io));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.SlowReadStarted, RingTestWaitTimeoutMs));
    ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Io.hEvent, 0));
    /* A real backend callback still owns the published payload. Exit without
     * STOP/CLOSE or releasing that callback, then require physical rundown. */
    ASSERT(SetEvent(Ready));
    ExitProcess(0);
}

static void ring_mapped_owner_kill_child(void)
{
    const GUID Guid = { 0x51aeb0b3, 0x2a8e, 0x4d44, { 0x91, 0xa8, 0x3f, 0x86, 0x11, 0x2f, 0x31, 0x61 } };
    RING_TEST_STATE State;
    HANDLE Ready = OpenEventW(EVENT_MODIFY_STATE, FALSE,
        L"Global\\WinSpdSharedRingTestOwnerExitReady");
    ASSERT(0 != Ready);
    ring_test_create_with_limits(&State, &Guid, 8, 8, FALSE, TRUE, 256 * 1024);
    ring_test_start(&State, 2);
    State.MappingChecks = 1;
    State.SlowReadBlockAddress = RingTestBlockAddress;
    ASSERT(SetEnvironmentVariableW(L"WINSPD_MAP_TEST_DONE_EVENT",
        L"Global\\WinSpdSharedRingTestMappedRequesterDone"));
    HANDLE Requester = ring_test_start_mapping_requester(&State, 131072, TRUE);
    ASSERT(SetEnvironmentVariableW(L"WINSPD_MAP_TEST_DONE_EVENT", 0));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(State.SlowReadStarted, RingTestWaitTimeoutMs));
    ASSERT(WAIT_TIMEOUT == WaitForSingleObject(Requester, 0));
    if (State.StorageUnit->SharedRingHeader->MapThresholdBytes)
        ASSERT(State.MappedReadCallbacks > 0);
    ASSERT(SetEvent(Ready));
    /* The parent forcibly terminates this backend, including the callback
     * holding the alias. No cooperative STOP/CLOSE or response is provided. */
    Sleep(INFINITE);
}

static void ring_test_owner_process_termination_case(UINT32 Published)
{
    HANDLE Stdout;
    HANDLE Stderr;
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 96];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    DWORD ExitCode;
    BOOL Success;
    HANDLE Ready;
    HANDLE RundownDone;
    HANDLE RequesterDone = 0;

    Ready = CreateEventW(0, TRUE, FALSE,
        L"Global\\WinSpdSharedRingTestOwnerExitReady");
    ASSERT(0 != Ready);
    ASSERT(ResetEvent(Ready));
    RundownDone = CreateEventW(0, TRUE, FALSE,
        L"Global\\WinSpdSharedRingTestOwnerRundownDone");
    ASSERT(0 != RundownDone);
    ASSERT(ResetEvent(RundownDone));
    if (2 == Published)
    {
        RequesterDone = CreateEventW(0, TRUE, FALSE,
            L"Global\\WinSpdSharedRingTestMappedRequesterDone");
        ASSERT(0 != RequesterDone && ResetEvent(RequesterDone));
    }

    CreateDirectoryW(L"artifacts", 0);
    Stdout = CreateFileW(Published ? L"artifacts\\ring-owner-io-child.stdout.log" :
        L"artifacts\\ring-owner-wait-child.stdout.log", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, CREATE_ALWAYS, 0, 0);
    ASSERT(INVALID_HANDLE_VALUE != Stdout);
    Stderr = CreateFileW(Published ? L"artifacts\\ring-owner-io-child.stderr.log" :
        L"artifacts\\ring-owner-wait-child.stderr.log", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, CREATE_ALWAYS, 0, 0);
    ASSERT(INVALID_HANDLE_VALUE != Stderr);
    ASSERT(SetHandleInformation(Stdout, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
    ASSERT(SetHandleInformation(Stderr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
    ASSERT(0 != GetModuleFileNameW(0, FileName, ARRAYSIZE(FileName)));
    _snwprintf_s(CommandLine, ARRAYSIZE(CommandLine), _TRUNCATE,
        L"\"%s\" +%s", FileName,
        2 == Published ? L"ring_mapped_owner_kill_child" :
        Published ? L"ring_owner_io_exit_child" : L"ring_owner_exit_child");
    memset(&StartupInfo, 0, sizeof StartupInfo);
    StartupInfo.cb = sizeof StartupInfo;
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    StartupInfo.hStdOutput = Stdout;
    StartupInfo.hStdError = Stderr;
    Success = CreateProcessW(FileName, CommandLine, 0, 0, TRUE,
        0, 0, 0, &StartupInfo, &ProcessInfo);
    ASSERT(Success);
    HANDLE StartHandles[] = { Ready, ProcessInfo.hProcess };
    ASSERT(WAIT_OBJECT_0 == WaitForMultipleObjects(ARRAYSIZE(StartHandles),
        StartHandles, FALSE, RingTestWaitTimeoutMs));
    if (2 == Published)
        ASSERT(TerminateProcess(ProcessInfo.hProcess, ERROR_PROCESS_ABORTED));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(
        ProcessInfo.hProcess, RingTestWaitTimeoutMs));
    ASSERT(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode));
    ASSERT((2 == Published ? ERROR_PROCESS_ABORTED : 0) == ExitCode);
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(Ready, 0));
    ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(RundownDone, RingTestWaitTimeoutMs));
    if (RequesterDone)
    {
        ASSERT(WAIT_OBJECT_0 == WaitForSingleObject(RequesterDone, RingTestWaitTimeoutMs));
        CloseHandle(RequesterDone);
    }
    CloseHandle(RundownDone);
    CloseHandle(Ready);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(Stdout);
    CloseHandle(Stderr);

    HANDLE DeviceHandle;
    UINT32 BtlBuffer[SPD_IOCTL_STORAGE_UNIT_CAPACITY];
    UINT32 BtlBytes = sizeof BtlBuffer;
    DWORD Error = SpdIoctlOpenDevice(L"" SPD_IOCTL_HARDWARE_ID,
        &DeviceHandle);
    ASSERT(ERROR_SUCCESS == Error);
    ULONGLONG Deadline = GetTickCount64() + RingTestWaitTimeoutMs;
    do
    {
        BtlBytes = sizeof BtlBuffer;
        Error = SpdIoctlGetList(DeviceHandle, BtlBuffer, &BtlBytes);
        if (ERROR_SUCCESS == Error && 0 == BtlBytes)
            break;
        Sleep(50);
    } while (GetTickCount64() < Deadline);
    ASSERT(ERROR_SUCCESS == Error);
    ASSERT(0 == BtlBytes);
    CloseHandle(DeviceHandle);
}

static void ring_test_owner_process_termination_test(void)
{
    ring_test_owner_process_termination_case(FALSE);
}

static void ioctl_ring_owner_exit_published_test(void)
{
    ring_test_owner_process_termination_case(TRUE);
}

static void ioctl_ring_mapped_owner_kill_test(void)
{
    ring_test_owner_process_termination_case(2);
}
#endif

void ring_tests_cleanup(void)
{
#if defined(WINSPD_TEST_BUILD)
    /* No state pointers are safe after a failed assertion. Release named
     * kernel test barriers so owner-process rundown can finish independently. */
    static const WCHAR *ReleaseEvents[] =
    {
        L"Global\\WinSpdSharedRingTestPostRelease",
        L"Global\\WinSpdSharedRingTestPrepareRelease",
        L"Global\\WinSpdSharedRingTestCompletionRelease",
        L"Global\\WinSpdSharedRingTestReadyRelease",
        L"Global\\WinSpdSharedRingTestPrepareCommitRelease",
        L"Global\\WinSpdSharedRingTestCompletionClaimRelease",
        L"Global\\WinSpdSharedRingTestResetRetireRelease",
        L"Global\\WinSpdSharedRingTestOpenCommitRelease",
        L"Global\\WinSpdSharedRingTestPreparationClaimRelease",
    };
    for (UINT32 I = 0; I < ARRAYSIZE(ReleaseEvents); I++)
    {
        HANDLE Event = OpenEventW(EVENT_MODIFY_STATE, FALSE, ReleaseEvents[I]);
        if (0 != Event)
        {
            SetEvent(Event);
            CloseHandle(Event);
        }
    }
#endif
}

void ring_tests(void)
{
    TEST(ioctl_ring_zero_count_flush10_test);
    TEST(ioctl_ring_zero_count_flush16_test);
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
    TEST(ioctl_ring_buffer_exhaustion_test);
    TEST(ioctl_ring_cq_saturation_test);
    TEST(ioctl_ring_stop_with_wait_blocked_test);
    TEST(ioctl_ring_stop_with_workers_active_test);
    TEST(ioctl_ring_wait_notification_test);
    TEST(ioctl_ring_flush_file_buffers_test);
#if defined(WINSPD_TEST_BUILD)
    TEST_OPT(ring_owner_exit_child);
    TEST_OPT(ring_multi_prepare_test);
    TEST_OPT(ring_multi_prepare_reset_test);
    TEST_OPT(ring_multi_prepare_stop_test);
    TEST_OPT(ring_multi_prepare_cancel_test);
    TEST_OPT(ring_multi_completion_test);
    TEST_OPT(ring_multi_completion_reset_test);
    TEST_OPT(ring_multi_completion_stop_test);
    TEST_OPT(ring_multi_completion_cancel_test);
    TEST_OPT(ring_owner_io_exit_child);
    TEST_OPT(ring_mapping_io_child);
    TEST_OPT(ring_mapped_owner_kill_child);
    TEST(ioctl_ring_cancel_during_write_prepare_test);
    TEST(ioctl_ring_reset_during_write_prepare_test);
    TEST(ioctl_ring_stop_during_write_prepare_test);
    TEST(ioctl_ring_kick_during_completion_copy_test);
    TEST(ioctl_ring_cancel_queued_completion_test);
    TEST(ioctl_ring_reset_queued_completion_test);
    TEST(ioctl_ring_stop_queued_completion_test);
    TEST(ioctl_ring_cancel_during_read_copy_test);
    TEST(ioctl_ring_reset_during_read_copy_test);
    TEST(ioctl_ring_stop_during_read_copy_test);
    TEST(ioctl_ring_batch_abort_compaction_test);
    TEST(ioctl_ring_full_sq_pending_resume_test);
    TEST(ioctl_ring_stop_retires_large_batch_test);
    TEST(ioctl_ring_abort_published_io_test);
    TEST(ioctl_ring_reset_published_io_test);
    TEST(ioctl_ring_independent_preparation_test);
    TEST(ioctl_ring_partial_prepare_commit_test);
    TEST(ioctl_ring_reset_partial_prepare_test);
    TEST(ioctl_ring_cancel_ready_slot_test);
    TEST(ioctl_ring_reset_ready_slot_test);
    TEST(ioctl_ring_stop_ready_slot_test);
    TEST(ioctl_ring_slot_generation_reuse_test);
    TEST(ioctl_ring_stale_slot_completion_test);
    TEST(ioctl_ring_slot_generation_exhaustion_test);
    TEST(ioctl_ring_invalid_slot_completion_test);
    TEST(ioctl_ring_failure_freezes_publication_test);
    TEST(ioctl_ring_flush_fence_multichunk_test);
    TEST(ioctl_ring_cancel_pending_flush_test);
    TEST(ioctl_ring_cancel_preparing_flush_test);
    TEST(ioctl_ring_cancel_ready_flush_test);
    TEST(ioctl_ring_abort_published_flush_test);
    TEST(ioctl_ring_reset_published_flush_test);
    TEST(ioctl_ring_cancel_queued_flush_test);
    TEST(ioctl_ring_stop_published_flush_test);
    TEST(ioctl_ring_two_flush_fences_test);
    TEST(ioctl_ring_concurrent_close_test);
    TEST(ioctl_ring_reset_close_rundown_test);
    TEST(ioctl_ring_open_owner_change_test);
    TEST(ioctl_ring_session_handle_test);
    TEST(ioctl_ring_unprovision_callback_test);
    TEST(ioctl_ring_owner_exit_published_test);
    TEST(ioctl_ring_noncdb_flush_helpers_test);
    TEST(ioctl_ring_mixed_copy_budgets_test);
    TEST(ioctl_legacy_production_lifecycle_test);
    TEST(ioctl_ring_multichunk_read_write_test);
    TEST(ioctl_ring_mapped_payload_test);
    TEST(ioctl_ring_mapped_stop_test);
    TEST(ioctl_ring_mapped_reset_test);
    TEST(ioctl_ring_mapped_owner_kill_test);
    TEST(ioctl_ring_mapped_requester_kill_test);
    TEST(ioctl_ring_mapped_cancel_states_test);
    TEST(ioctl_ring_publish_before_wait_test);
    TEST(ioctl_ring_post_cancel_race_test);
    TEST(ioctl_ring_producer_concurrency_test);
    TEST(ioctl_ring_wait_teardown_race_test);
#endif
}
