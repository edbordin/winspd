/**
 * @file sys/util.c
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

NTSTATUS SpdRegistryGetValue(PUNICODE_STRING Path, PUNICODE_STRING ValueName,
    PKEY_VALUE_PARTIAL_INFORMATION ValueInformation, PULONG PValueInformationLength)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle = 0;
    NTSTATUS Result;

    InitializeObjectAttributes(&ObjectAttributes,
        Path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, 0, 0);

    Result = ZwOpenKey(&Handle, KEY_QUERY_VALUE, &ObjectAttributes);
    if (!NT_SUCCESS(Result))
        goto exit;

    Result = ZwQueryValueKey(Handle, ValueName,
        KeyValuePartialInformation, ValueInformation,
        *PValueInformationLength, PValueInformationLength);
    if (!NT_SUCCESS(Result))
        goto exit;

    Result = STATUS_SUCCESS;

exit:
    if (0 != Handle)
        ZwClose(Handle);

    return Result;
}

VOID SpdMakeDeviceIoControlRequest(
    ULONG ControlCode,
    PDEVICE_OBJECT DeviceObject,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    PIO_STATUS_BLOCK IoStatus)
{
    ASSERT(!KeAreAllApcsDisabled());

    PIRP Irp;
    KEVENT Event;
    NTSTATUS Result;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(
        ControlCode,
        DeviceObject,
        InputBuffer,
        InputBufferLength,
        OutputBuffer,
        OutputBufferLength,
        FALSE,
        &Event,
        IoStatus);
    if (0 == Irp)
    {
        IoStatus->Status = STATUS_INSUFFICIENT_RESOURCES;
        IoStatus->Information = 0;
        return;
    }

    Result = IoCallDriver(DeviceObject, Irp);
    if (STATUS_PENDING == Result)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, 0);

    if (NT_ERROR(Result))
    {
        /*
         * Arrrr! Turns out that IopCompleteRequest will not correctly set UserIosb,
         * when NT_ERROR(Irp->IoStatus.Status) and the IRP is synchronous!
         *
         * See https://tinyurl.com/y9xwuzau
         */
        IoStatus->Status = Result;
        IoStatus->Information = 0;
    }
}

NTSTATUS SpdGetScsiAddress(
    PDEVICE_OBJECT DeviceObject,
    PSCSI_ADDRESS ScsiAddress)
{
    IO_STATUS_BLOCK IoStatus;

    RtlZeroMemory(&IoStatus, sizeof IoStatus);
    RtlZeroMemory(ScsiAddress, sizeof *ScsiAddress);

    SpdMakeDeviceIoControlRequest(
        IOCTL_SCSI_GET_ADDRESS,
        DeviceObject,
        0, 0,
        ScsiAddress, sizeof *ScsiAddress,
        &IoStatus);
    if (!NT_SUCCESS(IoStatus.Status))
        return IoStatus.Status;

    if (sizeof *ScsiAddress > IoStatus.Information ||
        sizeof *ScsiAddress > ScsiAddress->Length)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    return STATUS_SUCCESS;
}

/* Emit after worker rundown. Percentiles are the upper endpoint of the bin,
 * in QPC ticks; retain the clock frequency so parsers can convert to time. */
VOID SpdHistogramLog(const SPD_HISTOGRAM *Histogram, PCSTR Name)
{
    UINT64 Count = 0, Cumulative = 0, P50 = 0, P99 = 0;
    LARGE_INTEGER Frequency;
    for (ULONG I = 0; I < SPD_HISTOGRAM_BUCKETS; I++)
        Count += Histogram->Bins[I];
    UINT64 Rank50 = Count / 2 + (Count % 2 != 0);
    UINT64 Rank99 = Count - Count / 100;
    for (ULONG I = 0; I < SPD_HISTOGRAM_BUCKETS && 0 != Count; I++)
    {
        Cumulative += Histogram->Bins[I];
        if (0 == P50 && Cumulative >= Rank50)
            P50 = 1ULL << I;
        if (Cumulative >= Rank99)
        {
            P99 = 1ULL << I;
            break;
        }
    }
    (void)KeQueryPerformanceCounter(&Frequency);
    DbgPrint(DRIVER_NAME ": histogram name=%s count=%I64u p50_ticks=%I64u p99_ticks=%I64u qpc_hz=%I64d\n",
        Name, Count, P50, P99, Frequency.QuadPart);
}

/* Called only after rundown, like the existing histogram dump. */
VOID SpdLatencyLog(const SPD_LATENCY_METRIC *Metric, PCSTR Name, BOOLEAN IsDepth)
{
    LARGE_INTEGER Frequency;
    (void)KeQueryPerformanceCounter(&Frequency);
    DbgPrint(DRIVER_NAME ": latency_total name=%s count=%I64u total=%I64u maximum=%I64u qpc_hz=%I64d depth=%u\n",
        Name, Metric->Count, Metric->Total, Metric->Maximum, Frequency.QuadPart, (unsigned)IsDepth);
    SpdHistogramLog(&Metric->Histogram, Name);
}
