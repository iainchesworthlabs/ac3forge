/*++

The render circuit: a host pin the audio engine streams into, a volume
element and a mute element that hold state without applying it (the engine
expects an endpoint to answer both), a bridge pin categorised as a speaker
with one always-present jack, and the one format. Streams created on the
host pin get a CNullStream (stream.cpp).

    Host pin ---> [ Volume ] --- [ Mute ] ---> Bridge pin (speaker, jack)

--*/

#include "nullsink.h"
#include "stream.h"

namespace {

// The endpoint's one format: 8 channels, 48 kHz, 16-bit, 7.1 surround, so
// an application that can render surround does, and its tap reaches the
// demo's bed as eight channels. The same format the PortCls driver offered.
KSDATAFORMAT_WAVEFORMATEXTENSIBLE Pcm48000c8 = {
    {
        sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    {
        {
            WAVE_FORMAT_EXTENSIBLE,
            NULLSINK_CHANNELS,
            48000,
            48000 * NULLSINK_CHANNELS * 2,  // average bytes per second
            NULLSINK_CHANNELS * 2,          // block align
            16,
            sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
        },
        16,
        KSAUDIO_SPEAKER_7POINT1_SURROUND,
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
    }
};

enum : ULONG {
    kHostPin = 0,
    kBridgePin = 1,
    kPinCount = 2,
};

enum : ULONG {
    kVolumeElement = 0,
    kMuteElement = 1,
    kElementCount = 2,
};

PAGED_CODE_SEG
NTSTATUS
AllocateFormat(
    _In_  WDFDEVICE                         Device,
    _In_  ACXCIRCUIT                        Circuit,
    _In_  KSDATAFORMAT_WAVEFORMATEXTENSIBLE& WaveFormat,
    _Out_ ACXDATAFORMAT*                    Format
    )
{
    WDF_OBJECT_ATTRIBUTES   attributes;
    ACX_DATAFORMAT_CONFIG   formatConfig;

    PAGED_CODE();

    ACX_DATAFORMAT_CONFIG_INIT_KS(&formatConfig, &WaveFormat);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NULLSINK_FORMAT_CONTEXT);
    attributes.ParentObject = Circuit;

    return AcxDataFormatCreate(Device, &attributes, &formatConfig, Format);
}

}  // namespace

// ---- elements: state that is held, not applied ---------------------------------

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtMuteAssignState(
    _In_ ACXMUTE Mute,
    _In_ ULONG   Channel,
    _In_ ULONG   State
    )
{
    PNULLSINK_MUTE_CONTEXT muteContext = nullptr;

    PAGED_CODE();

    muteContext = GetNullSinkMuteContext(Mute);
    if (Channel == NULLSINK_ALL_CHANNELS) {
        for (ULONG i = 0; i < NULLSINK_CHANNELS; ++i) {
            muteContext->Muted[i] = State != 0 ? TRUE : FALSE;
        }
    } else {
        RETURN_NTSTATUS_IF_TRUE(Channel >= NULLSINK_CHANNELS, STATUS_INVALID_PARAMETER);
        muteContext->Muted[Channel] = State != 0 ? TRUE : FALSE;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtMuteRetrieveState(
    _In_  ACXMUTE Mute,
    _In_  ULONG   Channel,
    _Out_ ULONG*  State
    )
{
    PNULLSINK_MUTE_CONTEXT muteContext = nullptr;

    PAGED_CODE();

    muteContext = GetNullSinkMuteContext(Mute);
    const ULONG index = (Channel == NULLSINK_ALL_CHANNELS) ? 0 : Channel;
    RETURN_NTSTATUS_IF_TRUE(index >= NULLSINK_CHANNELS, STATUS_INVALID_PARAMETER);
    *State = muteContext->Muted[index] ? 1 : 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtVolumeAssignLevel(
    _In_ ACXVOLUME             Volume,
    _In_ ULONG                 Channel,
    _In_ LONG                  VolumeLevel,
    _In_ ACX_VOLUME_CURVE_TYPE CurveType,
    _In_ ULONGLONG             CurveDuration
    )
{
    PNULLSINK_VOLUME_CONTEXT volumeContext = nullptr;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(CurveType);
    UNREFERENCED_PARAMETER(CurveDuration);

    volumeContext = GetNullSinkVolumeContext(Volume);
    if (Channel == NULLSINK_ALL_CHANNELS) {
        for (ULONG i = 0; i < NULLSINK_CHANNELS; ++i) {
            volumeContext->Level[i] = VolumeLevel;
        }
    } else {
        RETURN_NTSTATUS_IF_TRUE(Channel >= NULLSINK_CHANNELS, STATUS_INVALID_PARAMETER);
        volumeContext->Level[Channel] = VolumeLevel;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtVolumeRetrieveLevel(
    _In_  ACXVOLUME Volume,
    _In_  ULONG     Channel,
    _Out_ LONG*     VolumeLevel
    )
{
    PNULLSINK_VOLUME_CONTEXT volumeContext = nullptr;

    PAGED_CODE();

    volumeContext = GetNullSinkVolumeContext(Volume);
    const ULONG index = (Channel == NULLSINK_ALL_CHANNELS) ? 0 : Channel;
    RETURN_NTSTATUS_IF_TRUE(index >= NULLSINK_CHANNELS, STATUS_INVALID_PARAMETER);
    *VolumeLevel = volumeContext->Level[index];
    return STATUS_SUCCESS;
}

// ---- pins and jack ------------------------------------------------------------------

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtPinSetDataFormat(
    _In_ ACXPIN        Pin,
    _In_ ACXDATAFORMAT DataFormat
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(DataFormat);
    // The device format is fixed; the engine keeps the one it was given.
    return STATUS_NOT_SUPPORTED;
}

// ---- circuit power -------------------------------------------------------------------

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtCircuitPowerUp(
    _In_ WDFDEVICE              Device,
    _In_ ACXCIRCUIT             Circuit,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Circuit);
    UNREFERENCED_PARAMETER(PreviousState);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtCircuitPowerDown(
    _In_ WDFDEVICE              Device,
    _In_ ACXCIRCUIT             Circuit,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Circuit);
    UNREFERENCED_PARAMETER(TargetState);
    return STATUS_SUCCESS;
}

// ---- the circuit ---------------------------------------------------------------------

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_CreateRenderCircuit(
    _In_  WDFDEVICE   Device,
    _Out_ ACXCIRCUIT* Circuit
    )
{
    NTSTATUS                        status = STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES           attributes;
    ACXCIRCUIT                      circuit = nullptr;
    PNULLSINK_CIRCUIT_CONTEXT       circuitContext = nullptr;
    ACXPIN                          pins[kPinCount] = {};

    PAGED_CODE();

    *Circuit = nullptr;

    // The circuit itself.
    {
        PACXCIRCUIT_INIT                circuitInit = nullptr;
        ACX_CIRCUIT_PNPPOWER_CALLBACKS  powerCallbacks;

        circuitInit = AcxCircuitInitAllocate(Device);
        RETURN_NTSTATUS_IF_TRUE(circuitInit == nullptr, STATUS_INSUFFICIENT_RESOURCES);

        AcxCircuitInitSetComponentId(circuitInit, &NULLSINK_RENDER_COMPONENT_GUID);
        (VOID)AcxCircuitInitAssignName(circuitInit, &NULLSINK_RENDER_CIRCUIT_NAME);
        AcxCircuitInitSetCircuitType(circuitInit, AcxCircuitTypeRender);

        ACX_CIRCUIT_PNPPOWER_CALLBACKS_INIT(&powerCallbacks);
        powerCallbacks.EvtAcxCircuitPowerUp = NullSink_EvtCircuitPowerUp;
        powerCallbacks.EvtAcxCircuitPowerDown = NullSink_EvtCircuitPowerDown;
        AcxCircuitInitSetAcxCircuitPnpPowerCallbacks(circuitInit, &powerCallbacks);

        status = AcxCircuitInitAssignAcxCreateStreamCallback(circuitInit, NullSink_EvtCircuitCreateStream);
        if (!NT_SUCCESS(status)) {
            NullSink_NoteFailure(L"AcxCircuitInitAssignAcxCreateStreamCallback", status);
            AcxCircuitInitFree(circuitInit);
            return status;
        }

        // On success ACX owns and frees the init; on failure it is ours.
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NULLSINK_CIRCUIT_CONTEXT);
        status = AcxCircuitCreate(Device, &attributes, &circuitInit, &circuit);
        if (!NT_SUCCESS(status)) {
            NullSink_NoteFailure(L"AcxCircuitCreate", status);
            if (circuitInit != nullptr) {
                AcxCircuitInitFree(circuitInit);
            }
            return status;
        }

        circuitContext = GetNullSinkCircuitContext(circuit);
    }

    // Volume and mute: per-channel state the engine can set and read back.
    {
        ACXELEMENT              elements[kElementCount] = {};
        ACX_VOLUME_CALLBACKS    volumeCallbacks;
        ACX_VOLUME_CONFIG       volumeConfig;
        ACX_MUTE_CALLBACKS      muteCallbacks;
        ACX_MUTE_CONFIG         muteConfig;

        ACX_VOLUME_CALLBACKS_INIT(&volumeCallbacks);
        volumeCallbacks.EvtAcxRampedVolumeAssignLevel = NullSink_EvtVolumeAssignLevel;
        volumeCallbacks.EvtAcxVolumeRetrieveLevel = NullSink_EvtVolumeRetrieveLevel;

        ACX_VOLUME_CONFIG_INIT(&volumeConfig);
        volumeConfig.ChannelsCount = NULLSINK_CHANNELS;
        volumeConfig.Minimum = NULLSINK_VOLUME_MINIMUM;
        volumeConfig.Maximum = NULLSINK_VOLUME_MAXIMUM;
        volumeConfig.SteppingDelta = NULLSINK_VOLUME_STEP;
        volumeConfig.Name = &KSAUDFNAME_VOLUME_CONTROL;
        volumeConfig.Callbacks = &volumeCallbacks;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NULLSINK_VOLUME_CONTEXT);
        attributes.ParentObject = circuit;
        NOTE_AND_RETURN_IF_FAILED(AcxVolumeCreate(circuit, &attributes, &volumeConfig,
                                                  reinterpret_cast<ACXVOLUME*>(&elements[kVolumeElement])));

        ACX_MUTE_CALLBACKS_INIT(&muteCallbacks);
        muteCallbacks.EvtAcxMuteAssignState = NullSink_EvtMuteAssignState;
        muteCallbacks.EvtAcxMuteRetrieveState = NullSink_EvtMuteRetrieveState;

        ACX_MUTE_CONFIG_INIT(&muteConfig);
        muteConfig.ChannelsCount = NULLSINK_CHANNELS;
        muteConfig.Name = &KSAUDFNAME_WAVE_MUTE;
        muteConfig.Callbacks = &muteCallbacks;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NULLSINK_MUTE_CONTEXT);
        attributes.ParentObject = circuit;
        NOTE_AND_RETURN_IF_FAILED(AcxMuteCreate(circuit, &attributes, &muteConfig,
                                                reinterpret_cast<ACXMUTE*>(&elements[kMuteElement])));

        circuitContext->Volume = reinterpret_cast<ACXVOLUME>(elements[kVolumeElement]);
        circuitContext->Mute = reinterpret_cast<ACXMUTE>(elements[kMuteElement]);

        NOTE_AND_RETURN_IF_FAILED(AcxCircuitAddElements(circuit, elements, SIZEOF_ARRAY(elements)));
    }

    // The pins: the host pin the engine streams into, and the bridge pin
    // that names what the device is (a speaker).
    {
        ACX_PIN_CONFIG          pinConfig;
        ACX_PIN_CALLBACKS       pinCallbacks;
        PNULLSINK_PIN_CONTEXT   pinContext = nullptr;

        ACX_PIN_CALLBACKS_INIT(&pinCallbacks);
        pinCallbacks.EvtAcxPinSetDataFormat = NullSink_EvtPinSetDataFormat;

        ACX_PIN_CONFIG_INIT(&pinConfig);
        pinConfig.Type = AcxPinTypeSink;
        pinConfig.Communication = AcxPinCommunicationSink;
        pinConfig.Category = &KSCATEGORY_AUDIO;
        pinConfig.PinCallbacks = &pinCallbacks;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NULLSINK_PIN_CONTEXT);
        attributes.ParentObject = circuit;
        NOTE_AND_RETURN_IF_FAILED(AcxPinCreate(circuit, &attributes, &pinConfig, &pins[kHostPin]));
        RETURN_NTSTATUS_IF_TRUE(pins[kHostPin] == nullptr, STATUS_INSUFFICIENT_RESOURCES);
        pinContext = GetNullSinkPinContext(pins[kHostPin]);
        pinContext->IsHostPin = TRUE;

        ACX_PIN_CONFIG_INIT(&pinConfig);
        pinConfig.Type = AcxPinTypeSource;
        pinConfig.Communication = AcxPinCommunicationNone;
        pinConfig.Category = &KSNODETYPE_SPEAKER;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NULLSINK_PIN_CONTEXT);
        attributes.ParentObject = circuit;
        NOTE_AND_RETURN_IF_FAILED(AcxPinCreate(circuit, &attributes, &pinConfig, &pins[kBridgePin]));
        RETURN_NTSTATUS_IF_TRUE(pins[kBridgePin] == nullptr, STATUS_INSUFFICIENT_RESOURCES);
        pinContext = GetNullSinkPinContext(pins[kBridgePin]);
        pinContext->IsHostPin = FALSE;
    }

    // One jack on the bridge pin, always present: the audio stack reads
    // presence to decide whether an endpoint is plugged in, and a device
    // with no socket is never unplugged. No jack-detection flag and no
    // presence callback: ACX then reports the jack as always connected, and
    // it refuses (STATUS_INVALID_PARAMETER, at AddDevice) a presence
    // callback on a jack that has not declared detection.
    {
        ACX_JACK_CONFIG     jackConfig;
        ACXJACK             jack = nullptr;

        ACX_JACK_CONFIG_INIT(&jackConfig);
        jackConfig.Description.ChannelMapping = KSAUDIO_SPEAKER_7POINT1_SURROUND;
        jackConfig.Description.Color = 0;
        jackConfig.Description.ConnectionType = AcxConnTypeAtapiInternal;
        jackConfig.Description.GeoLocation = AcxGeoLocFront;
        jackConfig.Description.GenLocation = AcxGenLocPrimaryBox;
        jackConfig.Description.PortConnection = AcxPortConnIntegratedDevice;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NULLSINK_JACK_CONTEXT);
        attributes.ParentObject = pins[kBridgePin];
        NOTE_AND_RETURN_IF_FAILED(AcxJackCreate(pins[kBridgePin], &attributes, &jackConfig, &jack));
        NOTE_AND_RETURN_IF_FAILED(AcxPinAddJacks(pins[kBridgePin], &jack, 1));
    }

    // The one format, on the host pin's raw-mode list.
    {
        ACXDATAFORMAT       format = nullptr;
        ACXDATAFORMATLIST   formatList = nullptr;

        NOTE_AND_RETURN_IF_FAILED(AllocateFormat(Device, circuit, Pcm48000c8, &format));

        formatList = AcxPinGetRawDataFormatList(pins[kHostPin]);
        RETURN_NTSTATUS_IF_TRUE(formatList == nullptr, STATUS_INSUFFICIENT_RESOURCES);
        NOTE_AND_RETURN_IF_FAILED(AcxDataFormatListAddDataFormat(formatList, format));
    }

    NOTE_AND_RETURN_IF_FAILED(AcxCircuitAddPins(circuit, pins, kPinCount));

    *Circuit = circuit;
    return STATUS_SUCCESS;
}

// ---- streams ----------------------------------------------------------------------------

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtCircuitCreateStream(
    _In_ WDFDEVICE       Device,
    _In_ ACXCIRCUIT      Circuit,
    _In_ ACXPIN          Pin,
    _In_ PACXSTREAM_INIT StreamInit,
    _In_ ACXDATAFORMAT   StreamFormat,
    _In_ const GUID*     SignalProcessingMode,
    _In_ ACXOBJECTBAG    VarArguments
    )
{
    WDF_OBJECT_ATTRIBUTES       attributes;
    ACX_STREAM_CALLBACKS        streamCallbacks;
    ACX_RT_STREAM_CALLBACKS     rtCallbacks;
    ACXSTREAM                   stream = nullptr;
    PNULLSINK_STREAM_CONTEXT    streamContext = nullptr;
    CNullStream*                engine = nullptr;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(SignalProcessingMode);
    UNREFERENCED_PARAMETER(VarArguments);

    ACX_STREAM_CALLBACKS_INIT(&streamCallbacks);
    streamCallbacks.EvtAcxStreamPrepareHardware = NullSink_EvtStreamPrepareHardware;
    streamCallbacks.EvtAcxStreamReleaseHardware = NullSink_EvtStreamReleaseHardware;
    streamCallbacks.EvtAcxStreamRun = NullSink_EvtStreamRun;
    streamCallbacks.EvtAcxStreamPause = NullSink_EvtStreamPause;
    NOTE_AND_RETURN_IF_FAILED(AcxStreamInitAssignAcxStreamCallbacks(StreamInit, &streamCallbacks));

    ACX_RT_STREAM_CALLBACKS_INIT(&rtCallbacks);
    rtCallbacks.EvtAcxStreamGetHwLatency = NullSink_EvtStreamGetHwLatency;
    rtCallbacks.EvtAcxStreamAllocateRtPackets = NullSink_EvtStreamAllocateRtPackets;
    rtCallbacks.EvtAcxStreamFreeRtPackets = NullSink_EvtStreamFreeRtPackets;
    rtCallbacks.EvtAcxStreamSetRenderPacket = NullSink_EvtStreamSetRenderPacket;
    rtCallbacks.EvtAcxStreamGetCurrentPacket = NullSink_EvtStreamGetCurrentPacket;
    rtCallbacks.EvtAcxStreamGetPresentationPosition = NullSink_EvtStreamGetPresentationPosition;
    NOTE_AND_RETURN_IF_FAILED(AcxStreamInitAssignAcxRtStreamCallbacks(StreamInit, &rtCallbacks));

    // The engine is told when each packet completes (event-driven mode).
    AcxStreamInitSetAcxRtStreamSupportsNotifications(StreamInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NULLSINK_STREAM_CONTEXT);
    attributes.EvtDestroyCallback = NullSink_EvtStreamDestroy;
    NOTE_AND_RETURN_IF_FAILED(AcxRtStreamCreate(Device, Circuit, &attributes, &StreamInit, &stream));

    engine = new (POOL_FLAG_NON_PAGED, NULLSINK_POOLTAG) CNullStream(stream, StreamFormat);
    RETURN_NTSTATUS_IF_TRUE(engine == nullptr, STATUS_INSUFFICIENT_RESOURCES);

    streamContext = GetNullSinkStreamContext(stream);
    streamContext->Engine = engine;

    return STATUS_SUCCESS;
}
