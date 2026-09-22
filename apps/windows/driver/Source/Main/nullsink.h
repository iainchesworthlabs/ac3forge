/*++

Ac3ForgeNullSink: the Desktop Atmos Demo's silent output device, on ACX.

Derived from Microsoft's ACX AudioCodec sample (audio/Acx/Samples in
microsoft/Windows-driver-samples, MS-PL; see ../../LICENSE and ../../README.md
for what was kept and what was cut). One root-enumerated device, one render
circuit, one format (7.1 at 48 kHz, 16-bit), and a stream engine that
discards what it is given while its position advances at the nominal rate.

This header is everything the translation units share: the kit headers in
the order the sample establishes, the status macros the sample gets from
WPP (not used here), the WDF context types, and the callback declarations.

--*/

#pragma once

#include <wdm.h>
#include <windef.h>
#include <mmsystem.h>
#include <ks.h>
#include <ksmedia.h>

extern "C" {
#include <initguid.h>
#include <ntddk.h>
#include <ntstrsafe.h>
#include <ntintsafe.h>
#include <wdf.h>
#include <acx.h>
}

#define PAGED_CODE_SEG __declspec(code_seg("PAGE"))
#define INIT_CODE_SEG __declspec(code_seg("INIT"))

// The sample's RETURN_NTSTATUS_IF_* come from its WPP tracing configuration;
// this driver has no tracing, so they are plain.
#define RETURN_NTSTATUS_IF_FAILED(expression)          \
    do {                                                \
        const NTSTATUS status_ = (expression);          \
        if (!NT_SUCCESS(status_)) {                     \
            return status_;                             \
        }                                               \
    } while (0)

#define RETURN_NTSTATUS_IF_TRUE(condition, status)      \
    do {                                                \
        if (condition) {                                \
            return (status);                            \
        }                                               \
    } while (0)

// The same, for the steps that build the device: a failure is written to
// the service's Parameters key (LastFailedStep, LastFailedStatus) before it
// is returned, so an AddDevice that fails on a machine with no kernel
// debugger still says which call refused what. The PortCls driver's start
// failure took a bisect to find; this driver names it.
#define NULLSINK_WIDEN2(x) L##x
#define NULLSINK_WIDEN(x) NULLSINK_WIDEN2(x)
#define NOTE_AND_RETURN_IF_FAILED(expression)                           \
    do {                                                                \
        const NTSTATUS status_ = (expression);                          \
        if (!NT_SUCCESS(status_)) {                                     \
            NullSink_NoteFailure(NULLSINK_WIDEN(#expression), status_); \
            return status_;                                             \
        }                                                               \
    } while (0)

#ifndef SIZEOF_ARRAY
#define SIZEOF_ARRAY(array) (sizeof(array) / sizeof((array)[0]))
#endif

// One pool tag for everything the driver allocates: "ASnk" in a pool dump.
constexpr ULONG NULLSINK_POOLTAG = 'knSA';

// The endpoint's one format: 8 channels, so every per-channel element has
// eight states.
constexpr ULONG NULLSINK_CHANNELS = 8;
constexpr ULONG NULLSINK_ALL_CHANNELS = 0xFFFFFFFFu;

// Volume element range and step, in the KS 16.16 fixed-point decibels the
// audio engine expects: -96 dB to 0 dB in half-decibel steps. The values are
// held and reported back, never applied - nothing is heard from this device.
constexpr LONG NULLSINK_VOLUME_MINIMUM = -96 * 0x10000;
constexpr LONG NULLSINK_VOLUME_MAXIMUM = 0;
constexpr LONG NULLSINK_VOLUME_STEP = 0x8000;

// Idle power: the device may go to D3 after this long with no stream open,
// the same policy the sample applies; a stream brings it back to D0.
constexpr ULONG NULLSINK_IDLE_TIMEOUT_MS = 5000;

// The render circuit's component id: unique to this driver (vendor-specific
// in ACX's terms), generated for it.
DEFINE_GUID(NULLSINK_RENDER_COMPONENT_GUID,
            0x7c2f1c6e, 0x5a8d, 0x4d3b, 0x9e, 0x0a, 0x2f, 0x6b, 0x1c, 0x9d, 0x4a, 0x17);

// The circuit name is the KS filter's reference string and must match
// KSNAME_Speaker in Ac3ForgeNullSink.inx.
DECLARE_CONST_UNICODE_STRING(NULLSINK_RENDER_CIRCUIT_NAME, L"Speaker0");

// ---- contexts ---------------------------------------------------------------

typedef struct _NULLSINK_DEVICE_CONTEXT {
    ACXCIRCUIT      Render = nullptr;
    WDF_TRI_STATE   ExcludeD3Cold = WdfFalse;
} NULLSINK_DEVICE_CONTEXT, *PNULLSINK_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NULLSINK_DEVICE_CONTEXT, GetNullSinkDeviceContext)

typedef struct _NULLSINK_CIRCUIT_CONTEXT {
    ACXVOLUME       Volume = nullptr;
    ACXMUTE         Mute = nullptr;
} NULLSINK_CIRCUIT_CONTEXT, *PNULLSINK_CIRCUIT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NULLSINK_CIRCUIT_CONTEXT, GetNullSinkCircuitContext)

typedef struct _NULLSINK_PIN_CONTEXT {
    BOOLEAN         IsHostPin = FALSE;
} NULLSINK_PIN_CONTEXT, *PNULLSINK_PIN_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NULLSINK_PIN_CONTEXT, GetNullSinkPinContext)

typedef struct _NULLSINK_VOLUME_CONTEXT {
    LONG            Level[NULLSINK_CHANNELS] = {};
} NULLSINK_VOLUME_CONTEXT, *PNULLSINK_VOLUME_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NULLSINK_VOLUME_CONTEXT, GetNullSinkVolumeContext)

typedef struct _NULLSINK_MUTE_CONTEXT {
    BOOLEAN         Muted[NULLSINK_CHANNELS] = {};
} NULLSINK_MUTE_CONTEXT, *PNULLSINK_MUTE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NULLSINK_MUTE_CONTEXT, GetNullSinkMuteContext)

typedef struct _NULLSINK_JACK_CONTEXT {
    ULONG           Unused = 0;
} NULLSINK_JACK_CONTEXT, *PNULLSINK_JACK_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NULLSINK_JACK_CONTEXT, GetNullSinkJackContext)

typedef struct _NULLSINK_FORMAT_CONTEXT {
    ULONG           Unused = 0;
} NULLSINK_FORMAT_CONTEXT, *PNULLSINK_FORMAT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NULLSINK_FORMAT_CONTEXT, GetNullSinkFormatContext)

class CNullStream;

typedef struct _NULLSINK_STREAM_CONTEXT {
    CNullStream*    Engine = nullptr;
} NULLSINK_STREAM_CONTEXT, *PNULLSINK_STREAM_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NULLSINK_STREAM_CONTEXT, GetNullSinkStreamContext)

typedef struct _NULLSINK_TIMER_CONTEXT {
    CNullStream*    Engine = nullptr;
} NULLSINK_TIMER_CONTEXT, *PNULLSINK_TIMER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NULLSINK_TIMER_CONTEXT, GetNullSinkTimerContext)

// ---- pool -------------------------------------------------------------------

PVOID operator new(_In_ size_t size, _In_ POOL_FLAGS poolFlags, _In_ ULONG tag);
void __cdecl operator delete(_Pre_maybenull_ __drv_freesMem(Mem) PVOID buffer);
void __cdecl operator delete(_Pre_maybenull_ __drv_freesMem(Mem) PVOID buffer, _In_ size_t size);

// ---- callbacks ------------------------------------------------------------------
//
// C linkage for every function a callback table points at. MSVC folds a
// function's code segment into its C++ mangled name, so a callback declared
// here without PAGED_CODE_SEG and defined with it would be two symbols and
// the link would fail; with C linkage the name is just the name. The ACX
// sample does the same through its extern "C" public.h.

extern "C" {

// ---- driver.cpp ---------------------------------------------------------------

DRIVER_INITIALIZE                   DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD           NullSink_EvtDeviceAdd;
EVT_WDF_DRIVER_UNLOAD               NullSink_EvtDriverUnload;

// ---- device.cpp -----------------------------------------------------------------

EVT_WDF_DEVICE_PREPARE_HARDWARE     NullSink_EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     NullSink_EvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY             NullSink_EvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT              NullSink_EvtDeviceD0Exit;

PAGED_CODE_SEG
NTSTATUS
NullSink_ApplyPowerPolicy(
    _In_ WDFDEVICE Device
    );

// Writes the failing step and status under the service's Parameters key;
// best effort, never fails the caller.
PAGED_CODE_SEG
VOID
NullSink_NoteFailure(
    _In_ PCWSTR   Step,
    _In_ NTSTATUS Status
    );

// ---- circuit.cpp --------------------------------------------------------------

PAGED_CODE_SEG
NTSTATUS
NullSink_CreateRenderCircuit(
    _In_  WDFDEVICE   Device,
    _Out_ ACXCIRCUIT* Circuit
    );

EVT_ACX_CIRCUIT_CREATE_STREAM       NullSink_EvtCircuitCreateStream;
EVT_ACX_CIRCUIT_POWER_UP            NullSink_EvtCircuitPowerUp;
EVT_ACX_CIRCUIT_POWER_DOWN          NullSink_EvtCircuitPowerDown;
EVT_ACX_PIN_SET_DATAFORMAT          NullSink_EvtPinSetDataFormat;
EVT_ACX_MUTE_ASSIGN_STATE           NullSink_EvtMuteAssignState;
EVT_ACX_MUTE_RETRIEVE_STATE         NullSink_EvtMuteRetrieveState;
EVT_ACX_RAMPED_VOLUME_ASSIGN_LEVEL  NullSink_EvtVolumeAssignLevel;
EVT_ACX_VOLUME_RETRIEVE_LEVEL       NullSink_EvtVolumeRetrieveLevel;

// ---- stream.cpp ---------------------------------------------------------------

EVT_WDF_OBJECT_CONTEXT_DESTROY      NullSink_EvtStreamDestroy;
EVT_ACX_STREAM_PREPARE_HARDWARE     NullSink_EvtStreamPrepareHardware;
EVT_ACX_STREAM_RELEASE_HARDWARE     NullSink_EvtStreamReleaseHardware;
EVT_ACX_STREAM_RUN                  NullSink_EvtStreamRun;
EVT_ACX_STREAM_PAUSE                NullSink_EvtStreamPause;
EVT_ACX_STREAM_GET_HW_LATENCY       NullSink_EvtStreamGetHwLatency;
EVT_ACX_STREAM_ALLOCATE_RTPACKETS   NullSink_EvtStreamAllocateRtPackets;
EVT_ACX_STREAM_FREE_RTPACKETS       NullSink_EvtStreamFreeRtPackets;
EVT_ACX_STREAM_SET_RENDER_PACKET    NullSink_EvtStreamSetRenderPacket;
EVT_ACX_STREAM_GET_CURRENT_PACKET   NullSink_EvtStreamGetCurrentPacket;
EVT_ACX_STREAM_GET_PRESENTATION_POSITION NullSink_EvtStreamGetPresentationPosition;

}  // extern "C"
