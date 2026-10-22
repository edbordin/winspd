/**
 * @file sys/adapter.c
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

/* Query support rather than assuming physical-HBA performance options apply
 * to a virtual adapter. Mode 0 preserves Storport's defaults; modes 1/2 opt
 * into completion DPC redirection, optionally toward the completing CPU. */
#ifndef SPD_RING_COMPLETION_DPC_MODE
#define SPD_RING_COMPLETION_DPC_MODE 0
#endif
#if SPD_RING_COMPLETION_DPC_MODE < 0 || SPD_RING_COMPLETION_DPC_MODE > 2
#error Completion DPC mode must be 0 (default), 1 (origin CPU), or 2 (current CPU).
#endif
static VOID SpdInitializeCompletionPerf(SPD_DEVICE_EXTENSION *DeviceExtension)
{
    PERF_CONFIGURATION_DATA Perf;
    ULONG QueryStatus = STOR_STATUS_NOT_IMPLEMENTED;
    DeviceExtension->CompletionPerfMode = SPD_RING_COMPLETION_DPC_MODE;
    DeviceExtension->CompletionPerfApplyStatus = STOR_STATUS_NOT_IMPLEMENTED;
    /* An older OS may understand fewer PERF_CONFIGURATION_DATA versions. */
    for (ULONG Version = STOR_PERF_VERSION; Version >= STOR_PERF_VERSION_2; Version--)
    {
        RtlZeroMemory(&Perf, sizeof Perf);
        Perf.Version = Version;
        Perf.Size = sizeof Perf;
        QueryStatus = StorPortInitializePerfOpts(DeviceExtension, TRUE, &Perf);
        if (STOR_STATUS_SUCCESS == QueryStatus)
            break;
    }
    DeviceExtension->CompletionPerfQueryStatus = QueryStatus;
    if (STOR_STATUS_SUCCESS != QueryStatus)
        return; /* Optional tuning must never prevent adapter startup. */
    DeviceExtension->CompletionPerfVersion = Perf.Version;
    DeviceExtension->CompletionPerfSupported = Perf.Flags;
#if SPD_RING_COMPLETION_DPC_MODE != 0
    ULONG Desired = STOR_PERF_DPC_REDIRECTION;
#if SPD_RING_COMPLETION_DPC_MODE == 2
    Desired |= STOR_PERF_DPC_REDIRECTION_CURRENT_CPU;
#endif
    DeviceExtension->CompletionPerfRequested = Desired;
    if ((Perf.Flags & Desired) != Desired)
        return;
    /* Do not enable concurrent StartIo, interrupt routing, or any unrelated
     * optimization simply because the support query advertises it. */
    Perf.Flags = Desired;
    DeviceExtension->CompletionPerfApplyStatus =
        StorPortInitializePerfOpts(DeviceExtension, FALSE, &Perf);
    if (STOR_STATUS_SUCCESS == DeviceExtension->CompletionPerfApplyStatus)
        DeviceExtension->CompletionPerfApplied = Desired;
#endif
}

ULONG SpdHwFindAdapter(
    PVOID DeviceExtension0,
    PVOID HwContext,
    PVOID BusInformation,
    PVOID LowerDevice,
    PCHAR ArgumentString,
    PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    PBOOLEAN Again)
{
    ULONG Result = SP_RETURN_NOT_FOUND;
    SPD_ENTER(adapter,
        ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql()));

    SPD_DEVICE_EXTENSION *DeviceExtension = DeviceExtension0;
    if (NT_SUCCESS(SpdDeviceExtensionInit(DeviceExtension, BusInformation)))
    {
        ConfigInfo->MaximumTransferLength = SP_UNINITIALIZED_VALUE;
        ConfigInfo->NumberOfPhysicalBreaks = SP_UNINITIALIZED_VALUE;
        ConfigInfo->AlignmentMask = FILE_BYTE_ALIGNMENT;
        ConfigInfo->NumberOfBuses = 1;
        ConfigInfo->ScatterGather = TRUE;
        ConfigInfo->Master = TRUE;
        /* The virtual adapter accepts Storport's full 64-bit address range. */
        ConfigInfo->Dma64BitAddresses = TRUE;
        ConfigInfo->CachesData = TRUE;
        ConfigInfo->MaximumNumberOfTargets = (UCHAR)DeviceExtension->StorageUnitCapacity;
        ConfigInfo->MaximumNumberOfLogicalUnits = 1;
        ConfigInfo->MaxIOsPerLun = SPD_RING_DEFAULT_LUN_QUEUE_DEPTH;
        ConfigInfo->InitialLunQueueDepth =
            SPD_RING_DEFAULT_LUN_QUEUE_DEPTH;
        ConfigInfo->MaxNumberOfIO = DeviceExtension->StorageUnitCapacity *
            SPD_RING_DEFAULT_LUN_QUEUE_DEPTH;
        ConfigInfo->WmiDataProvider = FALSE;
        ConfigInfo->SynchronizationModel = StorSynchronizeFullDuplex;
        ConfigInfo->VirtualDevice = TRUE;

        Result = SP_RETURN_FOUND;
    }

    SPD_LEAVE(adapter,
        "%p, HwContext=%p, BusInformation=%p, LowerDevice=%p, ArgumentString=\"%s\"",
        " = %lu",
        DeviceExtension0, HwContext, BusInformation, LowerDevice, ArgumentString, Result);
    return Result;
}

BOOLEAN SpdHwInitialize(PVOID DeviceExtension)
{
    BOOLEAN Result = TRUE;
    SPD_ENTER(adapter);

    SpdInitializeCompletionPerf(DeviceExtension);

    SPD_LEAVE(adapter,
        "%p", " = %d",
        DeviceExtension, Result);
    return Result;
}

VOID SpdHwFreeAdapterResources(PVOID DeviceExtension)
{
    SPD_ENTER(adapter,
        ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql()));

    SpdDeviceExtensionFini(DeviceExtension);

    SPD_LEAVE(adapter,
        "%p", "",
        DeviceExtension);
}

BOOLEAN SpdHwResetBus(PVOID DeviceExtension0, ULONG PathId)
{
    BOOLEAN Result = FALSE;
    SPD_ENTER(adapter);

    if (0 == PathId)
    {
        SPD_DEVICE_EXTENSION *DeviceExtension = DeviceExtension0;
        SPD_STORAGE_UNIT *StorageUnit;

        for (ULONG I = 0; DeviceExtension->StorageUnitCapacity > I; I++)
        {
            StorageUnit = SpdStorageUnitReferenceByBtl(DeviceExtension, SPD_IOCTL_BTL(0, I, 0));
            if (0 == StorageUnit)
                continue;

            SpdIoqReset(StorageUnit->Ioq, FALSE,
                SpdIoqResetReasonBusReset);

            SpdStorageUnitDereference(DeviceExtension, StorageUnit);
        }

        Result = TRUE;
    }

    SPD_LEAVE(adapter,
        "%p, PathId=%lu", " = %d",
        DeviceExtension0, PathId, Result);
    return Result;
}

SCSI_ADAPTER_CONTROL_STATUS SpdHwAdapterControl(
    PVOID DeviceExtension,
    SCSI_ADAPTER_CONTROL_TYPE ControlType,
    PVOID Parameters)
{
    SCSI_ADAPTER_CONTROL_STATUS Result = ScsiAdapterControlUnsuccessful;
    SPD_ENTER(adapter);

    switch (ControlType)
    {
    case ScsiQuerySupportedControlTypes:
#define SetSupportedControlType(i)      \
    if (((PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters)->MaxControlType > (i))\
        ((PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters)->SupportedTypeList[(i)] = TRUE
        SetSupportedControlType(ScsiQuerySupportedControlTypes);
        SetSupportedControlType(ScsiStopAdapter);
        SetSupportedControlType(ScsiRestartAdapter);
        Result = ScsiAdapterControlSuccess;
        break;
#undef SetSupportedControlType

    case ScsiStopAdapter:
        Result = ScsiAdapterControlSuccess;
        break;

    case ScsiRestartAdapter:
        Result = ScsiAdapterControlSuccess;
        break;

    default:
        Result = ScsiAdapterControlUnsuccessful;
        break;
    }

    SPD_LEAVE(adapter,
        "%p, ControlType=%s", " = %d",
        DeviceExtension, AdapterControlSym(ControlType), (int)Result);
    return Result;
}
