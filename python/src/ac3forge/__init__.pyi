from collections.abc import Sequence
from enum import Enum

import numpy as np
import numpy.typing as npt

SAMPLES_PER_FRAME: int
BLOCKS_PER_FRAME: int
MAX_AC3_CHANNELS: int
__version__: str

FloatArray = npt.NDArray[np.float32]
# Every encode_frame/encode_access_unit `channels`/`objects` parameter (roadmap AP6): either a
# single 2-D (n_channels, n_samples) array, or a sequence of 1-D per-channel arrays - both
# zero-copy when already contiguous float32 (see python-api.md's "Zero-copy numpy" section).
ChannelsIn = FloatArray | Sequence[FloatArray]

class Ac3Error(RuntimeError): ...

class Ac3EncodeError(Ac3Error):
    error: FrameError

class Ac3DecodeError(Ac3Error):
    error: DecodeError

class Acmod(Enum):
    kDualMono = ...
    k1_0 = ...
    k2_0 = ...
    k3_0 = ...
    k2_1 = ...
    k3_1 = ...
    k2_2 = ...
    k3_2 = ...

class SampleRate(Enum):
    k48000 = ...
    k44100 = ...
    k32000 = ...
    k24000 = ...
    k22050 = ...
    k16000 = ...

class DecodeError(Enum):
    kTruncated = ...
    kBadSyncWord = ...
    kBadCrc = ...
    kReservedValue = ...
    kUnsupported = ...
    kInvalidStream = ...

class FrameError(Enum):
    kInvalidBitrate = ...
    kInvalidDialnorm = ...
    kInvalidSubstream = ...
    kInvalidChannelMap = ...
    kTooManyChannels = ...
    kInvalidMixLevel = ...
    kInvalidBsi = ...
    kInvalidObjectAudio = ...

class StreamType(Enum):
    kIndependent = ...
    kDependent = ...
    kConvertible = ...

class ProfileId(Enum):
    kFilmStandard = ...
    kFilmLight = ...
    kMusicStandard = ...
    kMusicLight = ...
    kSpeech = ...

class CentreMixLevel(Enum):
    kMinus3dB = ...
    kMinus4_5dB = ...
    kMinus6dB = ...

class SurroundMixLevel(Enum):
    kMinus3dB = ...
    kMinus6dB = ...
    kSilent = ...

def fullbw_channel_count(acmod: Acmod) -> int: ...
def sample_rate_hz(sample_rate: SampleRate) -> int: ...
def describe(error: DecodeError) -> str: ...
def profile_for(id: ProfileId) -> Profile: ...
def profile_name(id: ProfileId) -> str: ...
def split_frames(stream: bytes) -> list[bytes]: ...
def split_access_units(stream: bytes) -> list[bytes]: ...
def stream_bsid(frame: bytes) -> int: ...

class LatencyBudget:
    frame_samples: int
    transform_samples: int
    lookahead_samples: int
    holdback_samples: int
    @property
    def total_samples(self) -> int: ...
    def milliseconds(self, sample_rate: SampleRate) -> float: ...

class Profile:
    null_low_db: float
    null_high_db: float
    boost_ratio: float
    max_boost_db: float
    early_cut_ratio: float
    early_cut_end_db: float
    cut_ratio: float
    attack_ms: float
    release_ms: float
    def __init__(self, **kwargs: float) -> None: ...

class HeavyConfig:
    dialogue_target_dbfs: float
    peak_ceiling_dbfs: float
    release_db_per_second: float
    def __init__(self, **kwargs: float) -> None: ...

class Position:
    x: float
    y: float
    z: float
    def __init__(self, **kwargs: float) -> None: ...

class ObjectPlacement:
    position: Position
    gain: float
    lfe_send: float
    def __init__(self, **kwargs: object) -> None: ...

class DynamicObject:
    position: Position
    gain_db: float

class Program:
    dynamic_only: bool
    lfe: bool
    bed: int
    dynamic_objects: int

class DecodedProgram:
    program: Program
    objects: list[DynamicObject]

class EncoderConfig:
    sample_rate: SampleRate
    bitrate_kbps: int
    dialnorm: int
    dialnorm2: int | None
    chbwcod: int
    acmod: Acmod
    lfe: bool
    coupling: bool
    cplbegf: int
    cplendf: int
    fast_mdct: bool
    drc: Profile | None
    heavy: HeavyConfig | None
    drc2: Profile | None
    heavy2: HeavyConfig | None
    cmixlev: CentreMixLevel
    surmixlev: SurroundMixLevel
    def __init__(self, **kwargs: object) -> None: ...

class FrameEncoder:
    def __init__(self, config: EncoderConfig) -> None: ...
    def encode_frame(self, channels: ChannelsIn) -> bytes: ...
    @property
    def config(self) -> EncoderConfig: ...
    @property
    def channel_count(self) -> int: ...
    @property
    def latency(self) -> LatencyBudget: ...
    @property
    def latency_samples(self) -> int: ...

class DecoderConfig:
    drc_scale: float
    heavy_compression: bool
    def __init__(self, **kwargs: object) -> None: ...

class DecodedFrame:
    sample_rate: SampleRate
    bitrate_kbps: int
    acmod: Acmod
    lfe: bool
    dialnorm: int
    compr: int | None
    dialnorm2: int | None
    compr2: int | None
    dynrng: list[int]
    dynrng2: list[int]
    blksw: list[list[bool]]
    channels: list[FloatArray]
    channel_labels: list[str]

class DecodedSubstream:
    strmtyp: StreamType
    substreamid: int
    sample_rate: SampleRate
    acmod: Acmod
    lfe: bool
    dialnorm: int
    compr: int | None
    dialnorm2: int | None
    compr2: int | None
    dynrng: list[int]
    dynrng2: list[int]
    numblkscod: int
    chanmap: int | None
    last_dependent: bool
    blksw: list[list[bool]]
    channels: list[FloatArray]
    object_metadata: DecodedProgram | None
    object_audio: list[FloatArray]
    channel_labels: list[str]

class DecodedAccessUnit:
    sample_rate: SampleRate
    acmod: Acmod
    dialnorm: int
    compr: int | None
    dynrng: list[int]
    numblkscod: int
    object_metadata: DecodedProgram | None
    object_audio: list[FloatArray]
    substream_count: int
    channels: list[FloatArray]
    channel_labels: list[str]

class FrameDecoder:
    def __init__(self, config: DecoderConfig = ...) -> None: ...
    def decode_frame(self, frame: bytes) -> DecodedFrame: ...
    def decode_frame_into(self, frame: bytes, out: ChannelsIn) -> DecodedFrame: ...
    @property
    def latency_samples(self) -> int: ...

class Eac3Decoder:
    def __init__(self, config: DecoderConfig = ...) -> None: ...
    def decode_substream(self, frame: bytes) -> DecodedSubstream | None: ...
    def decode_access_unit(self, unit: bytes) -> DecodedAccessUnit | None: ...
    def decode_access_unit_into(self, unit: bytes, out: ChannelsIn) -> DecodedAccessUnit | None: ...
    def flush(self) -> list[DecodedSubstream]: ...
    @property
    def latency_samples(self) -> int: ...

class AtmosConfig:
    sample_rate: SampleRate
    bitrate_kbps: int
    dialnorm: int
    num_bands_idx: int
    fine_quant: bool
    emit_object_metadata: bool
    fast_mdct: bool
    def __init__(self, **kwargs: object) -> None: ...

class AtmosEncoder:
    def __init__(self, config: AtmosConfig, objects: int) -> None: ...
    def encode_frame(self, objects: ChannelsIn, placement: Sequence[ObjectPlacement]) -> bytes: ...
    @property
    def dynamic_object_count(self) -> int: ...
    @property
    def program(self) -> Program: ...
    @property
    def latency(self) -> LatencyBudget: ...
    @property
    def latency_samples(self) -> int: ...
    @property
    def bed_latency(self) -> LatencyBudget: ...

# ac3::eac3::FrameEncoder/AccessUnitEncoder - roadmap AP6. A real submodule (m.def_submodule) at
# runtime, stubbed as a nested-class namespace here rather than a separate eac3.pyi, the way
# pybind11-stubgen represents one too - `ac3forge.eac3.FrameConfig` resolves through this class the
# same way it resolves through the runtime module.
class eac3:
    MAX_RENDER_CHANNELS: int

    class LayoutId(Enum):
        kMono = ...
        kStereo = ...
        kDualMono = ...
        k51 = ...
        k71 = ...
        k512 = ...
        k514 = ...
        k714 = ...

    class FrameConfig:
        sample_rate: SampleRate
        bitrate_kbps: int
        numblkscod: int
        dialnorm: int
        dialnorm2: int | None
        chbwcod: int
        acmod: Acmod
        lfe: bool
        strmtyp: StreamType
        substreamid: int
        chanmap: int | None
        last_dependent: bool
        drc: Profile | None
        heavy: HeavyConfig | None
        drc2: Profile | None
        heavy2: HeavyConfig | None
        auto_tools: bool
        coupling: bool
        cplbegf: int
        enhanced: bool
        spx: bool
        spxbegf: int
        spx_atten: bool
        spxattencod: int
        aht: bool
        gaqmod: int
        transient_prenoise: bool
        fast_mdct: bool
        dither: bool
        oba_complexity_index: int | None
        def __init__(self, **kwargs: object) -> None: ...

    class FrameMetadata:
        dynrng: list[int]
        compr: int | None
        dynrng2: list[int]
        compr2: int | None
        def __init__(self, **kwargs: object) -> None: ...

    class AccessUnitConfig:
        independent: eac3.FrameConfig
        dependents: list[eac3.FrameConfig]
        def __init__(self, **kwargs: object) -> None: ...

    class AccessUnit:
        bytes: bytes
        substream_bytes: list[int]
        @property
        def substream_count(self) -> int: ...

    class FrameEncoder:
        def __init__(self, config: eac3.FrameConfig) -> None: ...
        def encode_frame(
            self,
            channels: ChannelsIn,
            metadata: eac3.FrameMetadata | None = ...,
            aux: bytes = ...,
        ) -> bytes: ...
        @property
        def config(self) -> eac3.FrameConfig: ...
        @property
        def channel_count(self) -> int: ...
        @property
        def samples_per_frame(self) -> int: ...
        @property
        def latency(self) -> LatencyBudget: ...
        @property
        def latency_samples(self) -> int: ...

    class AccessUnitEncoder:
        def __init__(self, config: eac3.AccessUnitConfig) -> None: ...
        def encode_access_unit(self, channels: ChannelsIn, aux: bytes = ...) -> eac3.AccessUnit: ...
        @property
        def config(self) -> eac3.AccessUnitConfig: ...
        @property
        def channel_count(self) -> int: ...
        @property
        def latency(self) -> LatencyBudget: ...
        @property
        def latency_samples(self) -> int: ...

    @staticmethod
    def access_unit_config_for_layout(
        layout: eac3.LayoutId,
        bitrate_kbps: int,
        dependent_bitrate_kbps: int | None = ...,
        sample_rate: SampleRate = ...,
    ) -> eac3.AccessUnitConfig: ...
