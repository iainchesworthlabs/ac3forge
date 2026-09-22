/*++

CNullStream: one render stream's engine. It owns the RT packet buffers the
audio engine writes into (and never reads them), runs the timer that tells
ACX when each packet is complete, and answers position queries - all from
the PositionClock in position.h, which is the only logic here.

--*/

#pragma once

#include "nullsink.h"
#include "position.h"

class CNullStream {
public:
    // Two packets is what the sample allows and what the engine asks for.
    static constexpr ULONG kMaxPackets = 2;

    // Completions reported in one timer pass before the timer is re-armed,
    // so a stream that fell far behind (a debugger break, a suspended VM)
    // catches up in bounded steps rather than one long DPC.
    static constexpr ULONG kMaxCompletionsPerPass = 16;

    PAGED_CODE_SEG
    CNullStream(_In_ ACXSTREAM Stream, _In_ ACXDATAFORMAT StreamFormat);

    // Not paged: WDF's destroy callback, which deletes the engine, may run
    // at DISPATCH_LEVEL. Nor are the methods that take the spin lock
    // (Driver Verifier trims pageable code the moment IRQL rises, and a
    // paged function holding a spin lock then faults on its own next
    // instruction: bugcheck 0xD1, execute access, which is how this was
    // found).
    ~CNullStream();

    PAGED_CODE_SEG
    NTSTATUS AllocateRtPackets(_In_ ULONG PacketCount, _In_ ULONG PacketSize, _Out_ PACX_RTPACKET* Packets);

    PAGED_CODE_SEG
    VOID FreeRtPackets(_In_ PACX_RTPACKET Packets, _In_ ULONG PacketCount);

    PAGED_CODE_SEG
    NTSTATUS PrepareHardware();

    NTSTATUS ReleaseHardware();

    NTSTATUS Run();

    NTSTATUS Pause();

    NTSTATUS GetPresentationPosition(_Out_ PULONGLONG PositionInBlocks, _Out_ PULONGLONG QpcPosition);

    NTSTATUS GetCurrentPacket(_Out_ PULONG CurrentPacket);

    NTSTATUS SetRenderPacket(_In_ ULONG Packet, _In_ ULONG Flags, _In_ ULONG EosPacketLength);

    PAGED_CODE_SEG
    NTSTATUS GetHwLatency(_Out_ ULONG* FifoSize, _Out_ ULONG* Delay);

    // The timer's DPC: report the packets the clock says are complete and
    // arm the timer for the next one.
    _IRQL_requires_(DISPATCH_LEVEL)
    VOID OnTimer();

private:
    _IRQL_requires_max_(DISPATCH_LEVEL)
    ULONGLONG NowHns() const;

    // Arms the timer for the current packet's completion; the lock is held.
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID ArmTimerLocked(_In_ ULONGLONG NowHns);

    ACXSTREAM                   m_Stream = nullptr;
    ACXDATAFORMAT               m_Format = nullptr;
    WDFTIMER                    m_Timer = nullptr;
    ACX_STREAM_STATE            m_State = AcxStreamStateStop;
    KSPIN_LOCK                  m_Lock = 0;
    LARGE_INTEGER               m_QpcFrequency = {};
    ac3nullsink::PositionClock  m_Clock = {};

    PVOID                       m_Packets[kMaxPackets] = {};
    ULONG                       m_PacketCount = 0;
    ULONG                       m_PacketSize = 0;
    ULONG                       m_FirstPacketOffset = 0;
};
