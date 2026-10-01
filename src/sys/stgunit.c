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

ERESOURCE SpdGlobalDeviceResource;
SPD_DEVICE_EXTENSION *SpdGlobalDeviceExtension;
ULONG SpdStorageUnitCapacity = SPD_IOCTL_STORAGE_UNIT_CAPACITY;

static VOID SpdDeviceExtensionNotifyRoutine(HANDLE ParentId, HANDLE ProcessId0, BOOLEAN Create);

static BOOLEAN SpdRingSizeAdd(SIZE_T *PSize, SIZE_T Add)
{
    if (*PSize > (SIZE_T)-1 - Add)
        return FALSE;
    *PSize += Add;
    return TRUE;
}

static VOID SpdStorageUnitRingReset(SPD_STORAGE_UNIT *StorageUnit,
    BOOLEAN UserProcessExiting)
{
    if (0 != StorageUnit->RingMdl)
    {
        MmUnlockPages(StorageUnit->RingMdl);
        IoFreeMdl(StorageUnit->RingMdl);
        StorageUnit->RingMdl = 0;
    }

    if (0 != StorageUnit->RingUserAddress && !UserProcessExiting)
    {
        ZwUnmapViewOfSection(ZwCurrentProcess(),
            StorageUnit->RingUserAddress);
        StorageUnit->RingUserAddress = 0;
    }

    if (0 != StorageUnit->RingSystemAddress)
    {
        MmUnmapViewInSystemSpace(StorageUnit->RingSystemAddress);
        StorageUnit->RingSystemAddress = 0;
    }

    if (0 != StorageUnit->RingSectionHandle)
    {
        ZwClose(StorageUnit->RingSectionHandle);
        StorageUnit->RingSectionHandle = 0;
    }

    StorageUnit->RingSectionSize = 0;
    StorageUnit->RingProcessId = 0;
    StorageUnit->RingSubmissionOffset = 0;
    StorageUnit->RingSubmissionCount = 0;
    StorageUnit->RingCompletionOffset = 0;
    StorageUnit->RingCompletionCount = 0;
    StorageUnit->RingBufferOffset = 0;
    StorageUnit->RingBufferCount = 0;
    StorageUnit->RingBufferSize = 0;
    if (0 != StorageUnit->RingPending)
    {
        SpdFree(StorageUnit->RingPending, SpdTagStorageUnit);
        StorageUnit->RingPending = 0;
    }
    StorageUnit->RingPendingCount = 0;
    StorageUnit->RingActiveCalls = 0;
    StorageUnit->RingWaitActive = FALSE;
    StorageUnit->RingClosing = FALSE;
    StorageUnit->RingFailed = FALSE;
}

NTSTATUS SpdStorageUnitRingOpen(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId,
    SPD_IOCTL_RING_OPEN_PARAMS *Params)
{
    NTSTATUS Result;
    HANDLE SectionHandle = 0;
    PVOID SystemAddress = 0;
    PVOID UserAddress = 0;
    PVOID SectionObject = 0;
    PMDL RingMdl = 0;
    SIZE_T SectionSize;
    SIZE_T ViewSize;
    SIZE_T Offset;
    LARGE_INTEGER MaximumSize;
    OBJECT_ATTRIBUTES ObjectAttributes;
    SPD_RING_HEADER *Header;
    SPD_RING_PENDING *Pending = 0;

    if (ProcessId != StorageUnit->TransactProcessId)
        return STATUS_ACCESS_DENIED;
    if (0 != StorageUnit->RingSectionHandle)
        return STATUS_DEVICE_BUSY;
    if (SPD_RING_VERSION_1 != Params->Version)
        return STATUS_NOT_SUPPORTED;
    if (0 == Params->SubmissionCount || 4096 < Params->SubmissionCount ||
        0 == Params->CompletionCount || 4096 < Params->CompletionCount ||
        0 == Params->BufferCount || 4096 < Params->BufferCount ||
        Params->BufferCount > Params->CompletionCount ||
        4096 > Params->BufferSize ||
        (1024 * 1024) < Params->BufferSize ||
        Params->BufferSize < StorageUnit->StorageUnitParams.MaxTransferLength ||
        0 != (Params->BufferSize & 4095))
        return STATUS_INVALID_PARAMETER;

    Offset = SPD_IOCTL_ALIGN_UP(sizeof(SPD_RING_HEADER), 64);
    if (!SpdRingSizeAdd(&Offset,
        (SIZE_T)Params->SubmissionCount * sizeof(SPD_RING_REQUEST)))
        return STATUS_INVALID_PARAMETER;
    Offset = SPD_IOCTL_ALIGN_UP(Offset, 64);
    if (!SpdRingSizeAdd(&Offset,
        (SIZE_T)Params->CompletionCount * sizeof(SPD_RING_COMPLETION)))
        return STATUS_INVALID_PARAMETER;
    Offset = SPD_IOCTL_ALIGN_UP(Offset, 4096);
    if (!SpdRingSizeAdd(&Offset,
        (SIZE_T)Params->BufferCount * Params->BufferSize))
        return STATUS_INVALID_PARAMETER;
    SectionSize = SPD_IOCTL_ALIGN_UP(Offset, 4096);
    if (SPD_RING_MAX_SECTION_BYTES < SectionSize)
        return STATUS_INVALID_PARAMETER;

    Pending = SpdAllocNonPaged(
        (SIZE_T)Params->BufferCount * sizeof *Pending,
        SpdTagStorageUnit);
    if (0 == Pending)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Pending,
        (SIZE_T)Params->BufferCount * sizeof *Pending);

    MaximumSize.QuadPart = (LONGLONG)SectionSize;
    InitializeObjectAttributes(&ObjectAttributes, 0,
        OBJ_KERNEL_HANDLE, 0, 0);
    Result = ZwCreateSection(&SectionHandle,
        SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
        &ObjectAttributes, &MaximumSize, PAGE_READWRITE,
        SEC_COMMIT, 0);
    if (!NT_SUCCESS(Result))
        goto exit;

    /* MmMapViewInSystemSpace takes the section object, not its handle. */
    Result = ObReferenceObjectByHandle(SectionHandle,
        SECTION_MAP_READ | SECTION_MAP_WRITE, 0, KernelMode,
        &SectionObject, 0);
    if (!NT_SUCCESS(Result))
        goto exit;

    ViewSize = SectionSize;
    Result = MmMapViewInSystemSpace(SectionObject, &SystemAddress,
        &ViewSize);
    ObDereferenceObject(SectionObject);
    SectionObject = 0;
    if (!NT_SUCCESS(Result))
        goto exit;

    ViewSize = SectionSize;
    Result = ZwMapViewOfSection(SectionHandle, ZwCurrentProcess(),
        &UserAddress, 0, 0, 0, &ViewSize, ViewUnmap, 0, PAGE_READWRITE);
    if (!NT_SUCCESS(Result))
        goto exit;

    /* The Storport prepare/complete callbacks run at DISPATCH_LEVEL. A
     * pagefile-backed section view is not sufficient for driver-side copies
     * at that IRQL, so pin the section pages for the ring lifetime. Probe the
     * user view while the open IOCTL is executing in the owning process; the
     * kernel view refers to the same locked physical pages. */
    RingMdl = IoAllocateMdl(UserAddress, (ULONG)SectionSize,
        FALSE, FALSE, 0);
    if (0 == RingMdl)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    __try
    {
        MmProbeAndLockPages(RingMdl, UserMode, IoModifyAccess);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Result = GetExceptionCode();
        IoFreeMdl(RingMdl);
        RingMdl = 0;
        goto exit;
    }

    RtlZeroMemory(SystemAddress, SectionSize);
    Header = (SPD_RING_HEADER *)SystemAddress;
    Header->Version = SPD_RING_VERSION_1;
    Header->HeaderSize = sizeof *Header;
    Header->SubmissionOffset = (UINT32)SPD_IOCTL_ALIGN_UP(
        sizeof *Header, 64);
    Header->SubmissionCount = Params->SubmissionCount;
    Header->CompletionOffset = (UINT32)SPD_IOCTL_ALIGN_UP(
        Header->SubmissionOffset +
        Params->SubmissionCount * sizeof(SPD_RING_REQUEST), 64);
    Header->CompletionCount = Params->CompletionCount;
    Header->BufferOffset = (UINT32)SPD_IOCTL_ALIGN_UP(
        Header->CompletionOffset +
        Params->CompletionCount * sizeof(SPD_RING_COMPLETION), 4096);
    Header->BufferCount = Params->BufferCount;
    Header->BufferSize = Params->BufferSize;

    StorageUnit->RingSectionHandle = SectionHandle;
    StorageUnit->RingSystemAddress = SystemAddress;
    StorageUnit->RingMdl = RingMdl;
    StorageUnit->RingSectionSize = SectionSize;
    StorageUnit->RingUserAddress = UserAddress;
    StorageUnit->RingProcessId = ProcessId;
    StorageUnit->RingSubmissionOffset = Header->SubmissionOffset;
    StorageUnit->RingSubmissionCount = Header->SubmissionCount;
    StorageUnit->RingCompletionOffset = Header->CompletionOffset;
    StorageUnit->RingCompletionCount = Header->CompletionCount;
    StorageUnit->RingBufferOffset = Header->BufferOffset;
    StorageUnit->RingBufferCount = Header->BufferCount;
    StorageUnit->RingBufferSize = Header->BufferSize;
    KeInitializeSpinLock(&StorageUnit->RingLock);
    StorageUnit->RingWaitActive = FALSE;
    StorageUnit->RingFailed = FALSE;
    StorageUnit->RingGeneration++;
    if (0 == StorageUnit->RingGeneration)
        StorageUnit->RingGeneration = 1;
    StorageUnit->RingPendingCount = Params->BufferCount;
    StorageUnit->RingPending = Pending;
    KeInitializeEvent(&StorageUnit->RingIdleEvent,
        NotificationEvent, TRUE);
    StorageUnit->RingActiveCalls = 0;
    StorageUnit->RingClosing = FALSE;

    Params->UserAddress = (UINT64)(UINT_PTR)UserAddress;
    Params->SectionSize = SectionSize;
    Params->Features = 0;
    SectionHandle = 0;
    SystemAddress = 0;
    RingMdl = 0;
    UserAddress = 0;
    Pending = 0;
    Result = STATUS_SUCCESS;

exit:
    if (0 != SectionObject)
        ObDereferenceObject(SectionObject);
    if (0 != RingMdl)
    {
        MmUnlockPages(RingMdl);
        IoFreeMdl(RingMdl);
    }
    if (0 != UserAddress)
        ZwUnmapViewOfSection(ZwCurrentProcess(), UserAddress);
    if (0 != SystemAddress)
        MmUnmapViewInSystemSpace(SystemAddress);
    if (0 != SectionHandle)
        ZwClose(SectionHandle);
    if (0 != Pending)
        SpdFree(Pending, SpdTagStorageUnit);
    return Result;
}

VOID SpdStorageUnitRingClose(
    SPD_STORAGE_UNIT *StorageUnit,
    BOOLEAN UserProcessExiting)
{
    KIRQL Irql;

    if (0 == StorageUnit->RingSectionHandle &&
        0 == StorageUnit->RingPending)
        return;

    KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
    StorageUnit->RingClosing = TRUE;
    KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
    KeWaitForSingleObject(&StorageUnit->RingIdleEvent,
        Executive, KernelMode, FALSE, NULL);
    SpdStorageUnitRingReset(StorageUnit, UserProcessExiting);
}

NTSTATUS SpdStorageUnitRingStop(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId)
{
    KIRQL Irql;

    if (ProcessId != StorageUnit->TransactProcessId)
        return STATUS_ACCESS_DENIED;

    KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
    if (0 == StorageUnit->RingSectionHandle ||
        0 == StorageUnit->RingPending)
    {
        KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
        return STATUS_INVALID_DEVICE_STATE;
    }
    KeReleaseSpinLock(&StorageUnit->RingLock, Irql);

    /* This wakes a userspace RingWait blocked waiting for the next SRB. Keep
     * the section and mappings intact; userspace joins its threads before
     * issuing the final RingClose/unprovision sequence. */
    SpdIoqReset(StorageUnit->Ioq, TRUE);
    return STATUS_SUCCESS;
}

static BOOLEAN SpdStorageUnitRingEnter(SPD_STORAGE_UNIT *StorageUnit)
{
    KIRQL Irql;
    BOOLEAN Entered = FALSE;

    KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
    if (0 != StorageUnit->RingSectionHandle &&
        0 != StorageUnit->RingPending &&
        !StorageUnit->RingClosing && !StorageUnit->RingFailed)
    {
        if (0 == StorageUnit->RingActiveCalls)
            KeResetEvent(&StorageUnit->RingIdleEvent);
        StorageUnit->RingActiveCalls++;
        Entered = TRUE;
    }
    KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
    return Entered;
}

static VOID SpdStorageUnitRingLeave(SPD_STORAGE_UNIT *StorageUnit)
{
    KIRQL Irql;

    KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
    ASSERT(0 != StorageUnit->RingActiveCalls);
    if (0 != --StorageUnit->RingActiveCalls)
        KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
    else
    {
        KeSetEvent(&StorageUnit->RingIdleEvent, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
    }
}

static PVOID SpdStorageUnitRingBuffer(
    SPD_STORAGE_UNIT *StorageUnit, UINT32 Slot, UINT32 Offset,
    UINT32 Length)
{
    SPD_RING_HEADER *Header = StorageUnit->RingSystemAddress;
    SIZE_T BufferOffset;

    if (0 == Header || Slot >= StorageUnit->RingBufferCount ||
        Offset > StorageUnit->RingBufferSize ||
        Length > StorageUnit->RingBufferSize - Offset)
        return 0;

    BufferOffset = (SIZE_T)StorageUnit->RingBufferOffset +
        (SIZE_T)Slot * StorageUnit->RingBufferSize + Offset;
    if (BufferOffset > StorageUnit->RingSectionSize ||
        Length > StorageUnit->RingSectionSize - BufferOffset)
        return 0;
    return (PUINT8)StorageUnit->RingSystemAddress + BufferOffset;
}

static BOOLEAN SpdStorageUnitRingAllocateBuffer(
    SPD_STORAGE_UNIT *StorageUnit, PUINT32 PSlot)
{
    for (UINT32 I = 0; StorageUnit->RingPendingCount > I; I++)
        if (!StorageUnit->RingPending[I].InUse)
        {
            StorageUnit->RingPending[I].InUse = TRUE;
            *PSlot = I;
            return TRUE;
        }
    return FALSE;
}

static VOID SpdStorageUnitRingFreeBuffer(
    SPD_STORAGE_UNIT *StorageUnit, UINT32 Slot)
{
    if (Slot < StorageUnit->RingPendingCount)
        RtlZeroMemory(&StorageUnit->RingPending[Slot],
            sizeof StorageUnit->RingPending[Slot]);
}

static VOID SpdStorageUnitRingAbortPreparedSrb(
    SPD_STORAGE_UNIT *StorageUnit, PVOID SrbExtension)
{
    if (0 != SrbExtension)
    {
        SPD_SRB_EXTENSION *Extension = SrbExtension;
        if (0 != Extension->Srb)
            SpdIoqCancelSrb(StorageUnit->Ioq, Extension->Srb);
    }
}

NTSTATUS SpdStorageUnitRingWait(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId,
    SPD_IOCTL_RING_WAIT_PARAMS *Params,
    PIRP Irp)
{
    SPD_RING_HEADER *Header;
    BOOLEAN First = TRUE;
    BOOLEAN ActiveSet = FALSE;
    ULONG Produced = 0;
    NTSTATUS Result = STATUS_SUCCESS;

    if (ProcessId != StorageUnit->TransactProcessId)
        return STATUS_ACCESS_DENIED;
    if (0 == Params->MaxRequests)
        return STATUS_INVALID_PARAMETER;
    if (!SpdStorageUnitRingEnter(StorageUnit))
        return STATUS_INVALID_DEVICE_STATE;
    if (Params->MaxRequests > StorageUnit->RingBufferCount)
    {
        SpdStorageUnitRingLeave(StorageUnit);
        return STATUS_INVALID_PARAMETER;
    }

    Header = StorageUnit->RingSystemAddress;
    {
        KIRQL Irql;
        KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
        if (StorageUnit->RingWaitActive)
            Result = STATUS_DEVICE_BUSY;
        else
        {
            UINT64 Producer = SpdRingLoadCounter(
                &Header->SubmissionProducer);
            UINT64 Consumer = SpdRingLoadCounter(
                &Header->SubmissionConsumer);
            if (Consumer > Producer ||
                Producer - Consumer > StorageUnit->RingSubmissionCount)
            {
                StorageUnit->RingFailed = TRUE;
                Result = STATUS_INVALID_PARAMETER;
            }
            else
            {
                StorageUnit->RingWaitActive = TRUE;
                ActiveSet = TRUE;
            }
        }
        KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
    }
    if (!NT_SUCCESS(Result))
    {
        SpdStorageUnitRingLeave(StorageUnit);
        return Result;
    }

    while (Produced < Params->MaxRequests)
    {
        SPD_RING_REQUEST *RingRequest;
        PVOID DataBuffer;
        UINT32 Slot;
        UINT64 Producer;
        UINT64 Consumer;
        NTSTATUS StartResult;
        PVOID SrbExtension;

        {
            KIRQL Irql;
            KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
            Producer = SpdRingLoadCounter(&Header->SubmissionProducer);
            Consumer = SpdRingLoadCounter(&Header->SubmissionConsumer);
            if (Consumer > Producer ||
                Producer - Consumer >= StorageUnit->RingSubmissionCount ||
                !SpdStorageUnitRingAllocateBuffer(StorageUnit, &Slot))
            {
                KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
                break;
            }
            RingRequest = (SPD_RING_REQUEST *)
                ((PUINT8)StorageUnit->RingSystemAddress +
                StorageUnit->RingSubmissionOffset +
                (Producer % StorageUnit->RingSubmissionCount) *
                sizeof *RingRequest);
            RtlZeroMemory(RingRequest, sizeof *RingRequest);
            DataBuffer = SpdStorageUnitRingBuffer(StorageUnit, Slot,
                0, StorageUnit->RingBufferSize);
            KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
        }

        if (0 == DataBuffer)
        {
            KIRQL Irql;
            KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
            SpdStorageUnitRingFreeBuffer(StorageUnit, Slot);
            StorageUnit->RingFailed = TRUE;
            KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
            Result = STATUS_INVALID_PARAMETER;
            break;
        }

        LARGE_INTEGER Timeout;
        Timeout.QuadPart = 0;
        StartResult = SpdIoqStartProcessingSrb(StorageUnit->Ioq,
            First ? 0 : &Timeout, Irp,
            SpdSrbExecuteScsiPrepare, &RingRequest->Request,
            DataBuffer);
        if (STATUS_TIMEOUT == StartResult)
        {
            KIRQL Irql;
            KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
            SpdStorageUnitRingFreeBuffer(StorageUnit, Slot);
            KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
            break;
        }
        if (!NT_SUCCESS(StartResult))
        {
            KIRQL Irql;
            KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
            SpdStorageUnitRingFreeBuffer(StorageUnit, Slot);
            KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
            if (First)
            {
                Result = StartResult;
            }
            break;
        }

        SrbExtension = (PVOID)(UINT_PTR)RingRequest->Request.Hint;
        if (0 == SrbExtension)
        {
            /* Prepare did not return a correlation token, so abort the
             * complete I/O queue rather than leave this processed SRB
             * unaddressable. */
            SpdIoqReset(StorageUnit->Ioq, TRUE);
            KIRQL Irql;
            KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
            SpdStorageUnitRingFreeBuffer(StorageUnit, Slot);
            StorageUnit->RingFailed = TRUE;
            KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
            Result = STATUS_INVALID_PARAMETER;
            break;
        }

        {
            UINT64 DataLength64 = 0;
            if (SpdIoctlTransactReadKind == RingRequest->Request.Kind)
                DataLength64 = (UINT64)RingRequest->Request.Op.Read.BlockCount *
                    StorageUnit->StorageUnitParams.BlockLength;
            else if (SpdIoctlTransactWriteKind == RingRequest->Request.Kind)
                DataLength64 = (UINT64)RingRequest->Request.Op.Write.BlockCount *
                    StorageUnit->StorageUnitParams.BlockLength;
            else if (SpdIoctlTransactUnmapKind == RingRequest->Request.Kind)
                DataLength64 = (UINT64)RingRequest->Request.Op.Unmap.Count *
                    sizeof(SPD_IOCTL_UNMAP_DESCRIPTOR);

            if (DataLength64 > StorageUnit->RingBufferSize ||
                DataLength64 > MAXULONG)
            {
                SpdStorageUnitRingAbortPreparedSrb(StorageUnit,
                    SrbExtension);
                KIRQL Irql;
                KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
                SpdStorageUnitRingFreeBuffer(StorageUnit, Slot);
                StorageUnit->RingFailed = TRUE;
                KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
                Result = STATUS_INVALID_BUFFER_SIZE;
                break;
            }

            RingRequest->Data.Slot =
                SpdIoctlTransactFlushKind == RingRequest->Request.Kind ?
                SPD_RING_NO_BUFFER : Slot;
            RingRequest->Data.Offset = 0;
            RingRequest->Data.Length = (UINT32)DataLength64;
            RingRequest->Data.Flags = 0;

            {
                KIRQL Irql;
                KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
                StorageUnit->RingPending[Slot].SrbExtension = SrbExtension;
                StorageUnit->RingPending[Slot].Token =
                    ((UINT64)StorageUnit->RingGeneration << 32) | Slot;
                StorageUnit->RingPending[Slot].BufferSlot = Slot;
                StorageUnit->RingPending[Slot].DataLength = (UINT32)DataLength64;
                StorageUnit->RingPending[Slot].Kind = RingRequest->Request.Kind;
                RingRequest->Request.Hint = StorageUnit->RingPending[Slot].Token;
                MemoryBarrier();
                SpdRingStoreCounter(&Header->SubmissionProducer, Producer + 1);
                KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
            }
        }

        First = FALSE;
        Produced++;
    }

    if (ActiveSet)
    {
        KIRQL Irql;
        KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
        StorageUnit->RingWaitActive = FALSE;
        KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
    }
    Params->Produced = Produced;
    SpdStorageUnitRingLeave(StorageUnit);
    return Result;
}

NTSTATUS SpdStorageUnitRingKick(
    SPD_STORAGE_UNIT *StorageUnit,
    ULONG ProcessId,
    SPD_IOCTL_RING_KICK_PARAMS *Params)
{
    SPD_RING_HEADER *Header;
    UINT64 Consumer;
    UINT64 Producer;
    ULONG Consumed = 0;

    if (ProcessId != StorageUnit->TransactProcessId)
        return STATUS_ACCESS_DENIED;
    if (!SpdStorageUnitRingEnter(StorageUnit))
        return STATUS_INVALID_DEVICE_STATE;

    Header = StorageUnit->RingSystemAddress;
    Consumer = SpdRingLoadCounter(&Header->CompletionConsumer);
    Producer = SpdRingLoadCounter(&Header->CompletionProducer);
    if (Consumer > Producer ||
        Producer - Consumer > StorageUnit->RingCompletionCount)
    {
        StorageUnit->RingFailed = TRUE;
        SpdStorageUnitRingLeave(StorageUnit);
        return STATUS_INVALID_PARAMETER;
    }

    while (Consumer < Producer)
    {
        SPD_RING_COMPLETION *Completion = (SPD_RING_COMPLETION *)
            ((PUINT8)StorageUnit->RingSystemAddress +
            StorageUnit->RingCompletionOffset +
            (Consumer % StorageUnit->RingCompletionCount) * sizeof *Completion);
        UINT64 Token = Completion->Response.Hint;
        UINT32 Slot = (UINT32)Token;
        SPD_RING_PENDING Pending;
        PVOID DataBuffer;

        if ((UINT32)(Token >> 32) != StorageUnit->RingGeneration ||
            Slot >= StorageUnit->RingPendingCount)
        {
            StorageUnit->RingFailed = TRUE;
            SpdStorageUnitRingLeave(StorageUnit);
            return STATUS_INVALID_PARAMETER;
        }

        {
            KIRQL Irql;
            KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
            Pending = StorageUnit->RingPending[Slot];
            KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
        }
        if (!Pending.InUse || Pending.Token != Token ||
            Pending.Kind != Completion->Response.Kind)
        {
            StorageUnit->RingFailed = TRUE;
            SpdStorageUnitRingLeave(StorageUnit);
            return STATUS_INVALID_PARAMETER;
        }

        DataBuffer = 0 != Pending.DataLength ?
            SpdStorageUnitRingBuffer(StorageUnit,
                Pending.BufferSlot, 0, Pending.DataLength) : 0;
        SpdIoqEndProcessingSrbByExtension(StorageUnit->Ioq,
            Pending.SrbExtension, SpdSrbExecuteScsiComplete,
            &Completion->Response, DataBuffer);

        {
            KIRQL Irql;
            KeAcquireSpinLock(&StorageUnit->RingLock, &Irql);
            SpdStorageUnitRingFreeBuffer(StorageUnit, Slot);
            KeReleaseSpinLock(&StorageUnit->RingLock, Irql);
        }
        Consumer++;
        Consumed++;
    }

    SpdRingStoreCounter(&Header->CompletionConsumer, Consumer);
    Params->Consumed = Consumed;
    SpdStorageUnitRingLeave(StorageUnit);
    return STATUS_SUCCESS;
}

NTSTATUS SpdDeviceExtensionInit(SPD_DEVICE_EXTENSION *DeviceExtension, PVOID BusInformation)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(0 != DeviceExtension);

    NTSTATUS Result;

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
    DeviceExtension->DeviceObject = BusInformation;
    DeviceExtension->StorageUnitCapacity = SpdStorageUnitCapacity;
    SpdGlobalDeviceExtension = DeviceExtension;

    Result = STATUS_SUCCESS;

exit:
    ExReleaseResourceLite(&SpdGlobalDeviceResource);
    KeLeaveCriticalRegion();

    return Result;
}

VOID SpdDeviceExtensionFini(SPD_DEVICE_EXTENSION *DeviceExtension)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(0 != DeviceExtension);

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&SpdGlobalDeviceResource, TRUE);

    if (DeviceExtension == SpdGlobalDeviceExtension)
    {
        PsSetCreateProcessNotifyRoutine(SpdDeviceExtensionNotifyRoutine, TRUE);
        SpdGlobalDeviceExtension = 0;
    }

    ExReleaseResourceLite(&SpdGlobalDeviceResource);
    KeLeaveCriticalRegion();
}

static VOID SpdDeviceExtensionNotifyRoutine(HANDLE ParentId, HANDLE ProcessId0, BOOLEAN Create)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());

    if (Create)
        return;

    ULONG ProcessId = (ULONG)(UINT_PTR)ProcessId0;
    UINT8 Bitmap[32];
    ULONG Count;

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&SpdGlobalDeviceResource, TRUE);

    ASSERT(0 != SpdGlobalDeviceExtension);

    Count = SpdStorageUnitGetUseBitmap(SpdGlobalDeviceExtension, &ProcessId, Bitmap);

    for (ULONG I = 0; 0 < Count && sizeof Bitmap * 8 > I; I++)
        if (FlagOn(Bitmap[I >> 3], 1 << (I & 7)))
        {
            SpdStorageUnitUnprovision(SpdGlobalDeviceExtension, 0, I, ProcessId);
            Count--;
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
    StorageUnit->RefCount = 1;
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

    Result = SpdIoqCreate(DeviceExtension, &StorageUnit->Ioq);
    if (!NT_SUCCESS(Result))
        goto exit;

    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    DuplicateUnit = 0;
    Btl = (UINT32)-1;
    for (ULONG I = 0; DeviceExtension->StorageUnitCapacity > I; I++)
    {
        SPD_STORAGE_UNIT *Unit = DeviceExtension->StorageUnits[I];
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
    if (!NT_SUCCESS(Result))
    {
        if (0 != StorageUnit->Ioq)
            SpdIoqDelete(StorageUnit->Ioq);

        if (0 != StorageUnit)
            SpdFree(StorageUnit, SpdTagStorageUnit);
    }

    return Result;
}

NTSTATUS SpdStorageUnitUnprovision(
    SPD_DEVICE_EXTENSION *DeviceExtension,
    PGUID Guid, ULONG Index,
    ULONG ProcessId)
{
    ASSERT(PASSIVE_LEVEL == KeGetCurrentIrql());
    ASSERT(0 != DeviceExtension);

    NTSTATUS Result;
    SPD_STORAGE_UNIT *StorageUnit;
    KIRQL Irql;

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
    if (0 != StorageUnit && ProcessId == StorageUnit->OwnerProcessId)
    {
        DeviceExtension->StorageUnitCount--;
        DeviceExtension->StorageUnits[Index] = 0;
    }
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);

    if (0 == StorageUnit)
    {
        Result = STATUS_OBJECT_NAME_NOT_FOUND;
        goto exit;
    }
    if (ProcessId != StorageUnit->OwnerProcessId)
    {
        Result = STATUS_ACCESS_DENIED;
        goto exit;
    }

    /* stop the ioq before reclaiming ring request state and mappings */
    SpdIoqReset(StorageUnit->Ioq, TRUE);
    SpdStorageUnitRingClose(StorageUnit,
        0 != StorageUnit->RingSectionHandle &&
        StorageUnit->RingProcessId !=
            (ULONG)(ULONG_PTR)PsGetCurrentProcessId());
    SpdStorageUnitDereference(DeviceExtension, StorageUnit);

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
