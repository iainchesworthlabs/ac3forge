/*++

The stream engine (stream.h) and the ACX stream callbacks that reach it
through the stream's context.

Packet buffers are page-aligned non-paged pool, as in the sample: the
audio engine maps them into user mode, and a packet that shared a page with
anything else would leak kernel memory to user space. POOL_FLAG_NON_PAGED
is never executable; HVCI enforces that too.

--*/

#include "stream.h"

namespace {
_Function_class_(EVT_WDF_TIMER)
_IRQL_requires_same_
_IRQL_requires_(DISPATCH_LEVEL)
VOID
NullSink_EvtTimer(
    _In_ WDFTIMER Timer
    )
{
    PNULLSINK_TIMER_CONTEXT timerContext = GetNullSinkTimerContext(Timer);
    if (timerContext->Engine != nullptr) {
        timerContext->Engine->OnTimer();
    }
}

}  // namespace

// ---- lifetime ----------------------------------------------------------------------

_Use_decl_annotations_
PAGED_CODE_SEG
CNullStream::CNullStream(
    _In_ ACXSTREAM     Stream,
    _In_ ACXDATAFORMAT StreamFormat
    )
    : m_Stream(Stream), m_Format(StreamFormat)
{
    PAGED_CODE();
    KeInitializeSpinLock(&m_Lock);
    KeQueryPerformanceCounter(&m_QpcFrequency);
}

CNullStream::~CNullStream()
{
    // The timer, if any, went with ReleaseHardware; the packets with
    // FreeRtPackets. ACX orders both before destroying the stream.
}

// ---- packets ------------------------------------------------------------------------

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
CNullStream::AllocateRtPackets(
    _In_  ULONG          PacketCount,
    _In_  ULONG          PacketSize,
    _Out_ PACX_RTPACKET* Packets
    )
{
    NTSTATUS        status = STATUS_SUCCESS;
    PACX_RTPACKET   packets = nullptr;
    size_t          packetsBytes = 0;
    ULONG           pages = 0;
    ULONG           bytesPerPacket = 0;
    ULONG           firstPacketOffset = 0;

    PAGED_CODE();

    *Packets = nullptr;
    RETURN_NTSTATUS_IF_TRUE(PacketCount == 0 || PacketCount > kMaxPackets, STATUS_INVALID_PARAMETER);
    RETURN_NTSTATUS_IF_TRUE(PacketSize == 0, STATUS_INVALID_PARAMETER);

    // Room for kMaxPackets whatever PacketCount is (it is at most that):
    // a constant size the analyser can hold against the loop below.
    packetsBytes = kMaxPackets * sizeof(ACX_RTPACKET);
    packets = static_cast<PACX_RTPACKET>(ExAllocatePool2(POOL_FLAG_NON_PAGED, packetsBytes, NULLSINK_POOLTAG));
    RETURN_NTSTATUS_IF_TRUE(packets == nullptr, STATUS_NO_MEMORY);
    // ExAllocatePool2 zeroes the block; said again here for the reader and
    // for CodeQL, which otherwise sees the struct's padding bytes leaving
    // the driver uninitialised (the array is handed to ACX).
    RtlZeroMemory(packets, packetsBytes);

    // Round each packet up to whole pages; packet 0 is placed at the end
    // of its allocation so that it ends on a page boundary and packet 1
    // begins on one.
    RETURN_NTSTATUS_IF_FAILED(RtlULongAdd(PacketSize, PAGE_SIZE - 1, &pages));
    pages /= PAGE_SIZE;
    bytesPerPacket = pages * PAGE_SIZE;
    firstPacketOffset = bytesPerPacket - PacketSize;

    for (ULONG i = 0; i < PacketCount; ++i) {
        ACX_RTPACKET_INIT(&packets[i]);

        PVOID buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, bytesPerPacket, NULLSINK_POOLTAG);
        if (buffer == nullptr) {
            status = STATUS_NO_MEMORY;
            break;
        }

        PMDL mdl = IoAllocateMdl(buffer, bytesPerPacket, FALSE, TRUE, nullptr);
        if (mdl == nullptr) {
            ExFreePoolWithTag(buffer, NULLSINK_POOLTAG);
            status = STATUS_NO_MEMORY;
            break;
        }
        MmBuildMdlForNonPagedPool(mdl);

        WDF_MEMORY_DESCRIPTOR_INIT_MDL(&packets[i].RtPacketBuffer, mdl, bytesPerPacket);
        packets[i].RtPacketSize = PacketSize;
        packets[i].RtPacketOffset = (i == 0) ? firstPacketOffset : 0;
        m_Packets[i] = buffer;
    }

    if (!NT_SUCCESS(status)) {
        FreeRtPackets(packets, PacketCount);
        return status;
    }

    m_PacketCount = PacketCount;
    m_PacketSize = PacketSize;
    m_FirstPacketOffset = firstPacketOffset;
    *Packets = packets;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
PAGED_CODE_SEG
VOID
CNullStream::FreeRtPackets(
    _In_ PACX_RTPACKET Packets,
    _In_ ULONG         PacketCount
    )
{
    PAGED_CODE();

    if (Packets == nullptr) {
        return;
    }
    for (ULONG i = 0; i < PacketCount; ++i) {
        PMDL mdl = Packets[i].RtPacketBuffer.u.MdlType.Mdl;
        if (mdl != nullptr) {
            PVOID buffer = MmGetMdlVirtualAddress(mdl);
            IoFreeMdl(mdl);
            ExFreePoolWithTag(buffer, NULLSINK_POOLTAG);
        }
        if (i < kMaxPackets) {
            m_Packets[i] = nullptr;
        }
    }
    ExFreePoolWithTag(Packets, NULLSINK_POOLTAG);
    m_PacketCount = 0;
    m_PacketSize = 0;
    m_FirstPacketOffset = 0;
}

// ---- state --------------------------------------------------------------------------

PAGED_CODE_SEG
NTSTATUS
CNullStream::PrepareHardware()
{
    WDF_TIMER_CONFIG        timerConfig;
    WDF_OBJECT_ATTRIBUTES   timerAttributes;
    PNULLSINK_TIMER_CONTEXT timerContext = nullptr;

    PAGED_CODE();

    if (m_State == AcxStreamStatePause) {
        return STATUS_SUCCESS;
    }
    RETURN_NTSTATUS_IF_TRUE(m_State != AcxStreamStateStop, STATUS_INVALID_STATE_TRANSITION);

    // Stop -> Pause: the timer exists from here to ReleaseHardware; it is
    // armed one shot at a time from Run and from its own callback.
    WDF_TIMER_CONFIG_INIT(&timerConfig, NullSink_EvtTimer);
    timerConfig.AutomaticSerialization = TRUE;
    timerConfig.UseHighResolutionTimer = WdfTrue;
    timerConfig.Period = 0;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&timerAttributes, NULLSINK_TIMER_CONTEXT);
    timerAttributes.ParentObject = m_Stream;
    RETURN_NTSTATUS_IF_FAILED(WdfTimerCreate(&timerConfig, &timerAttributes, &m_Timer));

    timerContext = GetNullSinkTimerContext(m_Timer);
    timerContext->Engine = this;

    m_Clock.configure(AcxDataFormatGetAverageBytesPerSec(m_Format), m_PacketSize);
    m_State = AcxStreamStatePause;
    return STATUS_SUCCESS;
}

NTSTATUS
CNullStream::ReleaseHardware()
{
    if (m_State == AcxStreamStateStop) {
        return STATUS_SUCCESS;
    }

    // Pause -> Stop; always succeeds on the way down.
    if (m_Timer != nullptr) {
        WdfTimerStop(m_Timer, TRUE);
        PNULLSINK_TIMER_CONTEXT timerContext = GetNullSinkTimerContext(m_Timer);
        timerContext->Engine = nullptr;
        WdfObjectDelete(m_Timer);
        m_Timer = nullptr;
    }
    KeFlushQueuedDpcs();

    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_Lock, &oldIrql);
        m_Clock.stop();
        KeReleaseSpinLock(&m_Lock, oldIrql);
    }

    m_State = AcxStreamStateStop;
    return STATUS_SUCCESS;
}

NTSTATUS
CNullStream::Run()
{
    if (m_State == AcxStreamStateRun) {
        return STATUS_SUCCESS;
    }
    RETURN_NTSTATUS_IF_TRUE(m_State != AcxStreamStatePause, STATUS_INVALID_STATE_TRANSITION);
    RETURN_NTSTATUS_IF_TRUE(m_Timer == nullptr, STATUS_INVALID_DEVICE_STATE);

    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_Lock, &oldIrql);
        const ULONGLONG now = NowHns();
        m_Clock.run(now);
        m_State = AcxStreamStateRun;
        ArmTimerLocked(now);
        KeReleaseSpinLock(&m_Lock, oldIrql);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
CNullStream::Pause()
{
    if (m_State == AcxStreamStatePause) {
        return STATUS_SUCCESS;
    }
    RETURN_NTSTATUS_IF_TRUE(m_State != AcxStreamStateRun, STATUS_INVALID_STATE_TRANSITION);

    // Run -> Pause: stop the timer first (waiting for a callback in
    // flight), then freeze the position where the clock puts it.
    WdfTimerStop(m_Timer, TRUE);
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_Lock, &oldIrql);
        m_Clock.pause(NowHns());
        m_State = AcxStreamStatePause;
        KeReleaseSpinLock(&m_Lock, oldIrql);
    }
    return STATUS_SUCCESS;
}

// ---- position -----------------------------------------------------------------------

_Use_decl_annotations_
NTSTATUS
CNullStream::GetPresentationPosition(
    _Out_ PULONGLONG PositionInBlocks,
    _Out_ PULONGLONG QpcPosition
    )
{
    const ULONG blockAlign = AcxDataFormatGetBlockAlign(m_Format);
    RETURN_NTSTATUS_IF_TRUE(blockAlign == 0, STATUS_INVALID_DEVICE_STATE);

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_Lock, &oldIrql);
    const LARGE_INTEGER qpc = KeQueryPerformanceCounter(nullptr);
    const ULONGLONG position = m_Clock.position_at(KSCONVERT_PERFORMANCE_TIME(m_QpcFrequency.QuadPart, qpc));
    KeReleaseSpinLock(&m_Lock, oldIrql);

    *PositionInBlocks = position / blockAlign;
    *QpcPosition = static_cast<ULONGLONG>(qpc.QuadPart);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
CNullStream::GetCurrentPacket(
    _Out_ PULONG CurrentPacket
    )
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_Lock, &oldIrql);
    const ULONGLONG current = m_Clock.current_packet();
    KeReleaseSpinLock(&m_Lock, oldIrql);

    *CurrentPacket = static_cast<ULONG>(current);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
CNullStream::SetRenderPacket(
    _In_ ULONG Packet,
    _In_ ULONG Flags,
    _In_ ULONG EosPacketLength
    )
{
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(EosPacketLength);

    // The engine says which packet it has just filled. Nothing is done
    // with the data; the answer says whether the packet was on time, the
    // way the sample's does.
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_Lock, &oldIrql);
    const ULONGLONG current = m_Clock.current_packet();
    KeReleaseSpinLock(&m_Lock, oldIrql);

    if (Packet < current) {
        return STATUS_DATA_LATE_ERROR;
    }
    if (Packet > current + 1) {
        return STATUS_DATA_OVERRUN;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
CNullStream::GetHwLatency(
    _Out_ ULONG* FifoSize,
    _Out_ ULONG* Delay
    )
{
    PAGED_CODE();
    // No FIFO and no delay: nothing sits between the packet and nowhere.
    *FifoSize = 0;
    *Delay = 0;
    return STATUS_SUCCESS;
}

// ---- the timer ----------------------------------------------------------------------

_Use_decl_annotations_
ULONGLONG
CNullStream::NowHns() const
{
    return KSCONVERT_PERFORMANCE_TIME(m_QpcFrequency.QuadPart, KeQueryPerformanceCounter(nullptr));
}

_Use_decl_annotations_
VOID
CNullStream::ArmTimerLocked(
    _In_ ULONGLONG NowHns
    )
{
    if (m_Timer == nullptr || m_State != AcxStreamStateRun) {
        return;
    }
    // A relative due time is negative in 100 ns units; a packet already
    // due fires as soon as the timer can.
    const ULONGLONG due = m_Clock.next_completion_hns();
    const LONGLONG delay = (due > NowHns) ? -static_cast<LONGLONG>(due - NowHns) : -1;
    (VOID)WdfTimerStart(m_Timer, delay);
}

_Use_decl_annotations_
VOID
CNullStream::OnTimer()
{
    ULONGLONG   completed[kMaxCompletionsPerPass];
    ULONGLONG   qpcAtCompletion[kMaxCompletionsPerPass];
    ULONG       count = 0;

    KeAcquireSpinLockAtDpcLevel(&m_Lock);
    if (m_State == AcxStreamStateRun) {
        const ULONGLONG now = NowHns();
        ULONGLONG due = m_Clock.packets_due(now);
        while (due > 0 && count < kMaxCompletionsPerPass) {
            completed[count] = m_Clock.mark_completed();
            qpcAtCompletion[count] = static_cast<ULONGLONG>(KeQueryPerformanceCounter(nullptr).QuadPart);
            ++count;
            --due;
        }
        ArmTimerLocked(now);
    }
    KeReleaseSpinLockFromDpcLevel(&m_Lock);

    // Outside the lock: ACX may take its own.
    for (ULONG i = 0; i < count; ++i) {
        (VOID)AcxRtStreamNotifyPacketComplete(m_Stream, completed[i], qpcAtCompletion[i]);
    }
}

// ---- ACX stream callbacks -------------------------------------------------------------

namespace {
CNullStream* EngineOf(_In_ ACXSTREAM Stream)
{
    return GetNullSinkStreamContext(Stream)->Engine;
}

}  // namespace

_Use_decl_annotations_
VOID
NullSink_EvtStreamDestroy(
    _In_ WDFOBJECT Object
    )
{
    PNULLSINK_STREAM_CONTEXT streamContext = GetNullSinkStreamContext(static_cast<ACXSTREAM>(Object));
    CNullStream* engine = streamContext->Engine;
    streamContext->Engine = nullptr;
    delete engine;
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtStreamGetHwLatency(
    _In_  ACXSTREAM Stream,
    _Out_ ULONG*    FifoSize,
    _Out_ ULONG*    Delay
    )
{
    PAGED_CODE();
    return EngineOf(Stream)->GetHwLatency(FifoSize, Delay);
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtStreamAllocateRtPackets(
    _In_  ACXSTREAM      Stream,
    _In_  ULONG          PacketCount,
    _In_  ULONG          PacketSize,
    _Out_ PACX_RTPACKET* Packets
    )
{
    PAGED_CODE();
    return EngineOf(Stream)->AllocateRtPackets(PacketCount, PacketSize, Packets);
}

_Use_decl_annotations_
PAGED_CODE_SEG
VOID
NullSink_EvtStreamFreeRtPackets(
    _In_ ACXSTREAM     Stream,
    _In_ PACX_RTPACKET Packets,
    _In_ ULONG         PacketCount
    )
{
    PAGED_CODE();
    EngineOf(Stream)->FreeRtPackets(Packets, PacketCount);
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtStreamPrepareHardware(
    _In_ ACXSTREAM Stream
    )
{
    PAGED_CODE();
    return EngineOf(Stream)->PrepareHardware();
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtStreamReleaseHardware(
    _In_ ACXSTREAM Stream
    )
{
    PAGED_CODE();
    return EngineOf(Stream)->ReleaseHardware();
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtStreamRun(
    _In_ ACXSTREAM Stream
    )
{
    PAGED_CODE();
    return EngineOf(Stream)->Run();
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtStreamPause(
    _In_ ACXSTREAM Stream
    )
{
    PAGED_CODE();
    return EngineOf(Stream)->Pause();
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtStreamSetRenderPacket(
    _In_ ACXSTREAM Stream,
    _In_ ULONG     Packet,
    _In_ ULONG     Flags,
    _In_ ULONG     EosPacketLength
    )
{
    PAGED_CODE();
    return EngineOf(Stream)->SetRenderPacket(Packet, Flags, EosPacketLength);
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtStreamGetCurrentPacket(
    _In_  ACXSTREAM Stream,
    _Out_ PULONG    CurrentPacket
    )
{
    PAGED_CODE();
    return EngineOf(Stream)->GetCurrentPacket(CurrentPacket);
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtStreamGetPresentationPosition(
    _In_  ACXSTREAM  Stream,
    _Out_ PULONGLONG PositionInBlocks,
    _Out_ PULONGLONG QpcPosition
    )
{
    PAGED_CODE();
    return EngineOf(Stream)->GetPresentationPosition(PositionInBlocks, QpcPosition);
}
