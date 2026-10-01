//! AC-4 encode and decode - `iclforge::ac4::Decoder`/`iclforge::ac4::Encoder` via
//! `iclforge_ac4_decoder_t`/`iclforge_ac4_encoder_t` (ETSI TS 103 190-1/-2).
//!
//! Mirrors the surface the C API itself mirrors (see
//! `iclforge_ac4_encoder_config_t`'s own comment in `iclforge.h`): channel-based
//! and channel-based-immersive content, or one object substream (A-JOC or
//! direct-coded, [`ObjectsConfig`]), in one substream and one presentation. The
//! loudness/DRC/downmix/dialogue-enhancement metadata groups, multi-substream/
//! multi-presentation configurations, EMDF payloads and the `drc_gains` and
//! `three_zero` experimental flags are not exposed here either - a caller who
//! needs them links `ac4enc`/`ac4dec` directly instead of through this crate.
//!
//! Present only when the linked `iclforge_c` was built with `ICLFORGE_BUILD_AC4`
//! on (the default) - `iclforge-sys`'s bindgen output simply has no
//! `iclforge_ac4_*` items otherwise, so this whole module fails to compile
//! rather than link. There is no Cargo feature for it (unlike the C library's
//! own build option): `-sys`'s `build.rs` leaves `ICLFORGE_BUILD_AC4` at its
//! CMake default, so this module is unconditionally available in practice
//! the same way `atmos` is (also compiled unconditionally, having no matching
//! CMake option of its own).

use iclforge_sys as sys;
use std::ffi::{CStr, CString};
use std::ptr;

use crate::bytes::Bytes;
use crate::error::Error;

// --- shared enums -----------------------------------------------------------

/// Mirrors `iclforge_ac4_speaker_t` (Part 1 clause D.1, Part 2 clause A.3).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Speaker {
    Left,
    Right,
    Centre,
    Lfe,
    LeftSurround,
    RightSurround,
    LeftBack,
    RightBack,
    LeftWide,
    RightWide,
    TopFrontLeft,
    TopFrontRight,
    TopBackLeft,
    TopBackRight,
    TopSideLeft,
    TopSideRight,
    Lfe2,
}

impl Speaker {
    fn from_raw(raw: sys::iclforge_ac4_speaker_t) -> Self {
        #[allow(non_upper_case_globals)]
        match raw {
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_LEFT => Speaker::Left,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_RIGHT => Speaker::Right,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_CENTRE => Speaker::Centre,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_LFE => Speaker::Lfe,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_LEFT_SURROUND => Speaker::LeftSurround,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_RIGHT_SURROUND => Speaker::RightSurround,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_LEFT_BACK => Speaker::LeftBack,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_RIGHT_BACK => Speaker::RightBack,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_LEFT_WIDE => Speaker::LeftWide,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_RIGHT_WIDE => Speaker::RightWide,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_TOP_FRONT_LEFT => Speaker::TopFrontLeft,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_TOP_FRONT_RIGHT => {
                Speaker::TopFrontRight
            }
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_TOP_BACK_LEFT => Speaker::TopBackLeft,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_TOP_BACK_RIGHT => Speaker::TopBackRight,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_TOP_SIDE_LEFT => Speaker::TopSideLeft,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_TOP_SIDE_RIGHT => Speaker::TopSideRight,
            sys::iclforge_ac4_speaker_ICLFORGE_AC4_SPEAKER_LFE2 => Speaker::Lfe2,
            // An unrecognized ordinal cannot happen from this crate's own calls (every
            // accessor's C side clamps out-of-range indices to ICLFORGE_AC4_SPEAKER_LEFT
            // rather than an unmapped value) - Left is the same fallback iclforge.h's own
            // null-safety convention uses.
            _ => Speaker::Left,
        }
    }
}

/// Mirrors `iclforge_ac4_object_kind_t` (`iclforge::ac4::ObjectKind`): a bed object, a dynamic
/// object, or an intermediate spatial format object (rendered into channels, not
/// listed - see [`DecodedFrame::objects`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ObjectKind {
    Bed,
    Dyn,
    Isf,
}

impl ObjectKind {
    fn from_raw(raw: sys::iclforge_ac4_object_kind_t) -> Self {
        #[allow(non_upper_case_globals)]
        match raw {
            sys::iclforge_ac4_object_kind_ICLFORGE_AC4_OBJECT_BED => ObjectKind::Bed,
            sys::iclforge_ac4_object_kind_ICLFORGE_AC4_OBJECT_ISF => ObjectKind::Isf,
            _ => ObjectKind::Dyn,
        }
    }
}

/// Mirrors `iclforge_ac4_downmix_target_t` (`iclforge::ac4::DownmixTarget`): the layout
/// [`Decoder::decode`] renders to.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum DownmixTarget {
    #[default]
    AsCoded,
    FiveX,
    Stereo,
    LoRo,
    LtRt,
    Mono,
    SevenX4,
    SevenX2,
    SevenX0,
    FiveX4,
    FiveX2,
}

impl DownmixTarget {
    fn to_raw(self) -> sys::iclforge_ac4_downmix_target_t {
        match self {
            DownmixTarget::AsCoded => {
                sys::iclforge_ac4_downmix_target_ICLFORGE_AC4_DOWNMIX_AS_CODED
            }
            DownmixTarget::FiveX => sys::iclforge_ac4_downmix_target_ICLFORGE_AC4_DOWNMIX_5X,
            DownmixTarget::Stereo => sys::iclforge_ac4_downmix_target_ICLFORGE_AC4_DOWNMIX_STEREO,
            DownmixTarget::LoRo => sys::iclforge_ac4_downmix_target_ICLFORGE_AC4_DOWNMIX_LORO,
            DownmixTarget::LtRt => sys::iclforge_ac4_downmix_target_ICLFORGE_AC4_DOWNMIX_LTRT,
            DownmixTarget::Mono => sys::iclforge_ac4_downmix_target_ICLFORGE_AC4_DOWNMIX_MONO,
            DownmixTarget::SevenX4 => sys::iclforge_ac4_downmix_target_ICLFORGE_AC4_DOWNMIX_7X4,
            DownmixTarget::SevenX2 => sys::iclforge_ac4_downmix_target_ICLFORGE_AC4_DOWNMIX_7X2,
            DownmixTarget::SevenX0 => sys::iclforge_ac4_downmix_target_ICLFORGE_AC4_DOWNMIX_7X0,
            DownmixTarget::FiveX4 => sys::iclforge_ac4_downmix_target_ICLFORGE_AC4_DOWNMIX_5X4,
            DownmixTarget::FiveX2 => sys::iclforge_ac4_downmix_target_ICLFORGE_AC4_DOWNMIX_5X2,
        }
    }
}

/// Mirrors `iclforge_ac4_drc_mode_t` (`iclforge::ac4::DrcMode`, Part 1 Table 161).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum DrcMode {
    Off,
    #[default]
    Default,
    HomeTheatre,
    FlatPanelTv,
    PortableSpeakers,
    PortableHeadphones,
}

impl DrcMode {
    fn to_raw(self) -> sys::iclforge_ac4_drc_mode_t {
        match self {
            DrcMode::Off => sys::iclforge_ac4_drc_mode_ICLFORGE_AC4_DRC_OFF,
            DrcMode::Default => sys::iclforge_ac4_drc_mode_ICLFORGE_AC4_DRC_DEFAULT,
            DrcMode::HomeTheatre => sys::iclforge_ac4_drc_mode_ICLFORGE_AC4_DRC_HOME_THEATRE,
            DrcMode::FlatPanelTv => sys::iclforge_ac4_drc_mode_ICLFORGE_AC4_DRC_FLAT_PANEL_TV,
            DrcMode::PortableSpeakers => {
                sys::iclforge_ac4_drc_mode_ICLFORGE_AC4_DRC_PORTABLE_SPEAKERS
            }
            DrcMode::PortableHeadphones => {
                sys::iclforge_ac4_drc_mode_ICLFORGE_AC4_DRC_PORTABLE_HEADPHONES
            }
        }
    }
}

/// Mirrors `iclforge_ac4_decoding_mode_t` (`iclforge::ac4::DecodingMode`, Part 2 clause 4.7).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum DecodingMode {
    #[default]
    Full,
    Core,
}

impl DecodingMode {
    fn to_raw(self) -> sys::iclforge_ac4_decoding_mode_t {
        match self {
            DecodingMode::Full => sys::iclforge_ac4_decoding_mode_ICLFORGE_AC4_DECODING_FULL,
            DecodingMode::Core => sys::iclforge_ac4_decoding_mode_ICLFORGE_AC4_DECODING_CORE,
        }
    }
}

/// Mirrors `iclforge_ac4_concealment_policy_t` (`iclforge::ac4::ConcealmentPolicy`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum ConcealmentPolicy {
    #[default]
    None,
    RepeatFade,
    Mute,
}

impl ConcealmentPolicy {
    fn to_raw(self) -> sys::iclforge_ac4_concealment_policy_t {
        match self {
            ConcealmentPolicy::None => {
                sys::iclforge_ac4_concealment_policy_ICLFORGE_AC4_CONCEALMENT_NONE
            }
            ConcealmentPolicy::RepeatFade => {
                sys::iclforge_ac4_concealment_policy_ICLFORGE_AC4_CONCEALMENT_REPEAT_FADE
            }
            ConcealmentPolicy::Mute => {
                sys::iclforge_ac4_concealment_policy_ICLFORGE_AC4_CONCEALMENT_MUTE
            }
        }
    }
}

/// Mirrors `iclforge_ac4_concealment_action_t` (`iclforge::ac4::ConcealmentAction`): what a
/// concealed frame actually got.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ConcealmentAction {
    RepeatFade,
    Mute,
}

impl ConcealmentAction {
    fn from_raw(raw: sys::iclforge_ac4_concealment_action_t) -> Self {
        #[allow(non_upper_case_globals)]
        match raw {
            sys::iclforge_ac4_concealment_action_ICLFORGE_AC4_CONCEALMENT_ACTION_REPEAT_FADE => {
                ConcealmentAction::RepeatFade
            }
            _ => ConcealmentAction::Mute,
        }
    }
}

/// Mirrors `iclforge_ac4_associated_type_t` (`iclforge::ac4::AssociatedType`, Part 1 Table 92).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum AssociatedType {
    #[default]
    Any,
    AudioDescription,
    AudioDescriptionSubtitles,
    SpokenSubtitles,
    EmergencyInformation,
}

impl AssociatedType {
    fn to_raw(self) -> sys::iclforge_ac4_associated_type_t {
        match self {
            AssociatedType::Any => sys::iclforge_ac4_associated_type_ICLFORGE_AC4_ASSOCIATED_ANY,
            AssociatedType::AudioDescription => {
                sys::iclforge_ac4_associated_type_ICLFORGE_AC4_ASSOCIATED_AUDIO_DESCRIPTION
            }
            AssociatedType::AudioDescriptionSubtitles => sys::iclforge_ac4_associated_type_ICLFORGE_AC4_ASSOCIATED_AUDIO_DESCRIPTION_SUBTITLES,
            AssociatedType::SpokenSubtitles => {
                sys::iclforge_ac4_associated_type_ICLFORGE_AC4_ASSOCIATED_SPOKEN_SUBTITLES
            }
            AssociatedType::EmergencyInformation => {
                sys::iclforge_ac4_associated_type_ICLFORGE_AC4_ASSOCIATED_EMERGENCY_INFORMATION
            }
        }
    }
}

/// Mirrors `iclforge_ac4_codec_mode_t` (`iclforge::ac4::CodecMode`, Part 1 clause 4.3.6.1).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum CodecMode {
    #[default]
    Auto,
    Simple,
    Aspx,
    AspxAcpl1,
    AspxAcpl2,
    AspxAcpl3,
    Scpl,
    AspxScpl,
    AspxAjcc,
}

impl CodecMode {
    fn to_raw(self) -> sys::iclforge_ac4_codec_mode_t {
        match self {
            CodecMode::Auto => sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_AUTO,
            CodecMode::Simple => sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_SIMPLE,
            CodecMode::Aspx => sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_ASPX,
            CodecMode::AspxAcpl1 => sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_ASPX_ACPL1,
            CodecMode::AspxAcpl2 => sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_ASPX_ACPL2,
            CodecMode::AspxAcpl3 => sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_ASPX_ACPL3,
            CodecMode::Scpl => sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_SCPL,
            CodecMode::AspxScpl => sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_ASPX_SCPL,
            CodecMode::AspxAjcc => sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_ASPX_AJCC,
        }
    }

    fn from_raw(raw: sys::iclforge_ac4_codec_mode_t) -> Self {
        #[allow(non_upper_case_globals)]
        match raw {
            sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_SIMPLE => CodecMode::Simple,
            sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_ASPX => CodecMode::Aspx,
            sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_ASPX_ACPL1 => CodecMode::AspxAcpl1,
            sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_ASPX_ACPL2 => CodecMode::AspxAcpl2,
            sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_ASPX_ACPL3 => CodecMode::AspxAcpl3,
            sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_SCPL => CodecMode::Scpl,
            sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_ASPX_SCPL => CodecMode::AspxScpl,
            sys::iclforge_ac4_codec_mode_ICLFORGE_AC4_CODEC_ASPX_AJCC => CodecMode::AspxAjcc,
            _ => CodecMode::Auto,
        }
    }
}

/// Mirrors `iclforge_ac4_rate_mode_t` (`iclforge::ac4::RateMode`, Part 1 Table 81's `wait_frames`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum RateMode {
    #[default]
    Constant,
    Average,
    Variable,
}

impl RateMode {
    fn to_raw(self) -> sys::iclforge_ac4_rate_mode_t {
        match self {
            RateMode::Constant => sys::iclforge_ac4_rate_mode_ICLFORGE_AC4_RATE_CONSTANT,
            RateMode::Average => sys::iclforge_ac4_rate_mode_ICLFORGE_AC4_RATE_AVERAGE,
            RateMode::Variable => sys::iclforge_ac4_rate_mode_ICLFORGE_AC4_RATE_VARIABLE,
        }
    }
}

/// Mirrors `iclforge_ac4_bed_channel_t` (`iclforge::ac4::BedChannel`): the loudspeaker a bed object plays
/// from (Part 2 Table 66's `nonstd_bed_channel_assignment`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum BedChannel {
    Left,
    Right,
    Centre,
    LeftSurround,
    RightSurround,
    LeftBack,
    RightBack,
    TopFrontLeft,
    TopFrontRight,
    TopSideLeft,
    TopSideRight,
    TopBackLeft,
    TopBackRight,
    LeftWide,
    RightWide,
}

impl BedChannel {
    fn to_raw(self) -> sys::iclforge_ac4_bed_channel_t {
        match self {
            BedChannel::Left => sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_LEFT,
            BedChannel::Right => sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_RIGHT,
            BedChannel::Centre => sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_CENTRE,
            BedChannel::LeftSurround => {
                sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_LEFT_SURROUND
            }
            BedChannel::RightSurround => {
                sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_RIGHT_SURROUND
            }
            BedChannel::LeftBack => sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_LEFT_BACK,
            BedChannel::RightBack => sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_RIGHT_BACK,
            BedChannel::TopFrontLeft => {
                sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_TOP_FRONT_LEFT
            }
            BedChannel::TopFrontRight => {
                sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_TOP_FRONT_RIGHT
            }
            BedChannel::TopSideLeft => sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_TOP_SIDE_LEFT,
            BedChannel::TopSideRight => {
                sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_TOP_SIDE_RIGHT
            }
            BedChannel::TopBackLeft => sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_TOP_BACK_LEFT,
            BedChannel::TopBackRight => {
                sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_TOP_BACK_RIGHT
            }
            BedChannel::LeftWide => sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_LEFT_WIDE,
            BedChannel::RightWide => sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_RIGHT_WIDE,
        }
    }
}

/// Mirrors `iclforge_ac4_object_coding_t` (`iclforge::ac4::ObjectCoding`): how an object substream's
/// objects are coded.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum ObjectCoding {
    /// An A-JOC substream (Part 2 clause 5.7): a downmix coded in a `var_channel_element()` or a
    /// static 5.X bed, and the matrices that rebuild the objects from it.
    #[default]
    Ajoc,
    /// Direct-coded object substreams (clause 6.2.1.11): the objects coded as channels of Part
    /// 1's elements, with the group's OAMD substream. Dynamic objects and the LFE only.
    Direct,
}

impl ObjectCoding {
    fn to_raw(self) -> sys::iclforge_ac4_object_coding_t {
        match self {
            ObjectCoding::Ajoc => sys::iclforge_ac4_object_coding_ICLFORGE_AC4_OBJECT_CODING_AJOC,
            ObjectCoding::Direct => {
                sys::iclforge_ac4_object_coding_ICLFORGE_AC4_OBJECT_CODING_DIRECT
            }
        }
    }
}

/// Mirrors `iclforge_ac4_ajoc_downmix_t` (`iclforge::ac4::AjocDownmix`): A-JOC's downmix, which Part 2
/// leaves to the encoder.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum AjocDownmix {
    /// Downmix signals the encoder computes: the objects in groups by where they start, each
    /// signal the sum of its group's objects.
    #[default]
    Computed,
    /// A static bed (`b_static_dmx`): the objects panned onto L, R, C, Ls and Rs by X and Y.
    Static50,
    /// As `Static50`, and the LFE object onto the LFE.
    Static51,
}

impl AjocDownmix {
    fn to_raw(self) -> sys::iclforge_ac4_ajoc_downmix_t {
        match self {
            AjocDownmix::Computed => {
                sys::iclforge_ac4_ajoc_downmix_ICLFORGE_AC4_AJOC_DOWNMIX_COMPUTED
            }
            AjocDownmix::Static50 => {
                sys::iclforge_ac4_ajoc_downmix_ICLFORGE_AC4_AJOC_DOWNMIX_STATIC_50
            }
            AjocDownmix::Static51 => {
                sys::iclforge_ac4_ajoc_downmix_ICLFORGE_AC4_AJOC_DOWNMIX_STATIC_51
            }
        }
    }
}

/// Mirrors `iclforge_ac4_additional_pair_t` (`iclforge::ac4::AdditionalPair`, Part 1 Table 88): the 7.X
/// element's pair beyond L, R, C, Ls and Rs.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum AdditionalPair {
    #[default]
    None,
    /// 3/4/0: Lb and Rb.
    Back,
    /// 5/2/0: Lw and Rw.
    Wide,
    /// 3/2/2: Tfl and Tfr.
    TopFront,
}

impl AdditionalPair {
    fn to_raw(self) -> sys::iclforge_ac4_additional_pair_t {
        match self {
            AdditionalPair::None => sys::iclforge_ac4_additional_pair_ICLFORGE_AC4_PAIR_NONE,
            AdditionalPair::Back => sys::iclforge_ac4_additional_pair_ICLFORGE_AC4_PAIR_BACK,
            AdditionalPair::Wide => sys::iclforge_ac4_additional_pair_ICLFORGE_AC4_PAIR_WIDE,
            AdditionalPair::TopFront => {
                sys::iclforge_ac4_additional_pair_ICLFORGE_AC4_PAIR_TOP_FRONT
            }
        }
    }
}

// --- decoder configuration ---------------------------------------------------

/// Mirrors `iclforge_ac4_output_config_t` (`iclforge::ac4::OutputConfig`). Construct with
/// [`OutputConfig::default`] (which calls the raw `iclforge_ac4_output_config_init()` -
/// same "call the real _init(), never derive it" reasoning as `iclforge::EncoderConfig`) and
/// override only the fields you need.
#[derive(Debug, Clone, PartialEq)]
pub struct OutputConfig {
    pub output_level_dbfs: Option<f64>,
    pub drc: DrcMode,
    pub headphones: bool,
    pub dialogue_enhancement_db: f64,
    pub downmix: DownmixTarget,
    /// Default `true`.
    pub mix_lfe: bool,
    pub dialogue_gain_db: f64,
    pub associated_gain_db: f64,
}

impl OutputConfig {
    fn to_raw(&self) -> sys::iclforge_ac4_output_config_t {
        sys::iclforge_ac4_output_config_t {
            has_output_level_dbfs: self.output_level_dbfs.is_some() as i32,
            output_level_dbfs: self.output_level_dbfs.unwrap_or_default(),
            drc: self.drc.to_raw(),
            headphones: self.headphones as i32,
            dialogue_enhancement_db: self.dialogue_enhancement_db,
            downmix: self.downmix.to_raw(),
            mix_lfe: self.mix_lfe as i32,
            dialogue_gain_db: self.dialogue_gain_db,
            associated_gain_db: self.associated_gain_db,
        }
    }

    fn from_raw(raw: &sys::iclforge_ac4_output_config_t) -> Self {
        OutputConfig {
            output_level_dbfs: (raw.has_output_level_dbfs != 0).then_some(raw.output_level_dbfs),
            drc: match raw.drc {
                #[allow(non_upper_case_globals)]
                sys::iclforge_ac4_drc_mode_ICLFORGE_AC4_DRC_OFF => DrcMode::Off,
                _ => DrcMode::Default,
            },
            headphones: raw.headphones != 0,
            dialogue_enhancement_db: raw.dialogue_enhancement_db,
            downmix: DownmixTarget::AsCoded, // the only value iclforge_ac4_output_config_init() sets
            mix_lfe: raw.mix_lfe != 0,
            dialogue_gain_db: raw.dialogue_gain_db,
            associated_gain_db: raw.associated_gain_db,
        }
    }
}

impl Default for OutputConfig {
    fn default() -> Self {
        let mut raw = unsafe { std::mem::zeroed() };
        // SAFETY: iclforge_ac4_output_config_init() unconditionally overwrites every field of
        // `raw` - the one sanctioned way to obtain the real OutputConfig{} defaults (mix_lfe
        // true, etc.) rather than guessing at them Rust-side.
        unsafe { sys::iclforge_ac4_output_config_init(&mut raw) };
        OutputConfig::from_raw(&raw)
    }
}

/// Mirrors `iclforge_ac4_presentation_choice_t` (`iclforge::ac4::PresentationChoice`). Every field's
/// `Default` (`None`/empty/`Any`/`false`) already matches
/// `iclforge_ac4_presentation_choice_init()`'s own defaults, so unlike [`OutputConfig`] this
/// derives it rather than calling that function - there is no `unsafe` value it would need to
/// discover that the derive gets wrong.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct PresentationChoice {
    pub presentation_id: Option<i32>,
    pub index: Option<usize>,
    /// An IETF BCP 47 tag; empty for no language preference.
    pub language: String,
    /// Part 1 Table 91 `content_classifier`.
    pub associated: Option<i32>,
    pub associated_type: AssociatedType,
    pub headphones: bool,
}

impl PresentationChoice {
    /// Builds the raw struct and hands it to `f` for the duration of the call - the raw
    /// struct's `language` field borrows from a `CString` this function keeps alive on its own
    /// stack frame, which is why this isn't a plain `to_raw(&self) -> RawT` the way every
    /// pointer-free config in this crate is: a `const char*` field cannot outlive the value
    /// that owns its bytes, and this crate's config structs are otherwise always returned by
    /// value (see `EncoderConfig::to_raw` in `ac3.rs`).
    fn with_raw<R>(&self, f: impl FnOnce(&sys::iclforge_ac4_presentation_choice_t) -> R) -> R {
        // NUL bytes in a BCP 47 tag are not meaningful; drop them rather than fail outright -
        // this field cannot fail the call it feeds (iclforge_ac4_presentation_choice_init()
        // treats NULL as "no language" too).
        let language = CString::new(self.language.replace('\0', "")).unwrap_or_default();
        let raw = sys::iclforge_ac4_presentation_choice_t {
            has_presentation_id: self.presentation_id.is_some() as i32,
            presentation_id: self.presentation_id.unwrap_or_default(),
            has_index: self.index.is_some() as i32,
            index: self.index.unwrap_or_default(),
            language: if self.language.is_empty() {
                ptr::null()
            } else {
                language.as_ptr()
            },
            has_associated: self.associated.is_some() as i32,
            associated: self.associated.unwrap_or_default(),
            associated_type: self.associated_type.to_raw(),
            headphones: self.headphones as i32,
        };
        f(&raw)
    }
}

/// Mirrors `iclforge_ac4_decoder_config_t` (`iclforge::ac4::DecoderConfig`, less its syntax
/// trace - an internal diagnostic hook with no C surface, same omission as
/// `iclforge_ac4_decoder_config_t` itself). Construct with [`DecoderConfig::default`] (which
/// calls the raw `iclforge_ac4_decoder_config_init()`, same "never derive a default with a
/// non-zero/non-empty field" reasoning as [`OutputConfig`] - `level`'s real default is 3, which
/// a struct-level `#[derive(Default)]` would silently give as 0).
#[derive(Debug, Clone, PartialEq)]
pub struct DecoderConfig {
    pub output: OutputConfig,
    pub concealment: ConcealmentPolicy,
    pub presentation: PresentationChoice,
    /// The `md_compat` ceiling (Part 2 Table 55); default 3.
    pub level: i32,
    pub decoding: DecodingMode,
}

impl Default for DecoderConfig {
    fn default() -> Self {
        let mut raw = unsafe { std::mem::zeroed() };
        // SAFETY: iclforge_ac4_decoder_config_init() unconditionally overwrites every field of
        // `raw`, output/presentation included (each via the matching _init() function - see
        // iclforge.h's own comment on why every _config_init() must run first).
        unsafe { sys::iclforge_ac4_decoder_config_init(&mut raw) };
        DecoderConfig {
            output: OutputConfig::from_raw(&raw.output),
            concealment: match raw.concealment {
                #[allow(non_upper_case_globals)]
                sys::iclforge_ac4_concealment_policy_ICLFORGE_AC4_CONCEALMENT_REPEAT_FADE => {
                    ConcealmentPolicy::RepeatFade
                }
                #[allow(non_upper_case_globals)]
                sys::iclforge_ac4_concealment_policy_ICLFORGE_AC4_CONCEALMENT_MUTE => {
                    ConcealmentPolicy::Mute
                }
                _ => ConcealmentPolicy::None,
            },
            // iclforge_ac4_presentation_choice_init() gives every field the same value
            // PresentationChoice::default() already does (see its own doc comment) - no
            // pointer in raw.presentation to read back, so this skips converting it.
            presentation: PresentationChoice::default(),
            level: raw.level,
            decoding: match raw.decoding {
                #[allow(non_upper_case_globals)]
                sys::iclforge_ac4_decoding_mode_ICLFORGE_AC4_DECODING_CORE => DecodingMode::Core,
                _ => DecodingMode::Full,
            },
        }
    }
}

impl DecoderConfig {
    fn with_raw<R>(&self, f: impl FnOnce(&sys::iclforge_ac4_decoder_config_t) -> R) -> R {
        self.presentation.with_raw(|presentation| {
            let raw = sys::iclforge_ac4_decoder_config_t {
                output: self.output.to_raw(),
                concealment: self.concealment.to_raw(),
                presentation: *presentation,
                level: self.level,
                decoding: self.decoding.to_raw(),
            };
            f(&raw)
        })
    }
}

// --- decoder -------------------------------------------------------------

/// An AC-4 decoder - `iclforge::ac4::Decoder` via `iclforge_ac4_decoder_t`.
pub struct Decoder {
    raw: ptr::NonNull<sys::iclforge_ac4_decoder_t>,
}

unsafe impl Send for Decoder {}

impl Decoder {
    pub fn new(config: &DecoderConfig) -> Result<Self, Error> {
        config.with_raw(|raw_config| {
            let mut out: *mut sys::iclforge_ac4_decoder_t = ptr::null_mut();
            // SAFETY: `raw_config` is fully initialized for the duration of this call; `out`
            // is a valid out-parameter.
            let status = unsafe { sys::iclforge_ac4_decoder_create(raw_config, &mut out) };
            Error::check(status)?;
            let raw = ptr::NonNull::new(out)
                .expect("iclforge_ac4_decoder_create returned OK with a null decoder");
            Ok(Decoder { raw })
        })
    }

    /// The output processing, from the next frame.
    pub fn set_output(&mut self, output: &OutputConfig) {
        let raw = output.to_raw();
        // SAFETY: `self.raw` is valid; `raw` lives for the duration of this call.
        unsafe { sys::iclforge_ac4_decoder_set_output(self.raw.as_ptr(), &raw) };
    }

    /// The presentation choice, from the next frame.
    pub fn set_presentation(&mut self, choice: &PresentationChoice) {
        choice.with_raw(|raw| unsafe {
            sys::iclforge_ac4_decoder_set_presentation(self.raw.as_ptr(), raw)
        });
    }

    /// Forgets everything carried between frames.
    pub fn reset(&mut self) {
        unsafe { sys::iclforge_ac4_decoder_reset(self.raw.as_ptr()) };
    }

    /// The decoder's own added delay at the output rate, for the stream as last decoded; 0
    /// before a frame has decoded.
    pub fn latency_samples(&self) -> i32 {
        unsafe { sys::iclforge_ac4_decoder_latency_samples(self.raw.as_ptr()) }
    }

    /// Why the last [`Decoder::decode`] call failed, returned `None`, or returned a
    /// concealed frame; empty after one that decoded normally.
    pub fn refusal_reason(&self) -> String {
        // SAFETY: iclforge_ac4_decoder_refusal_reason() returns library-owned storage valid
        // for the process lifetime, always a valid NUL-terminated C string (never NULL - see
        // its own doc comment on normalizing the empty case).
        unsafe {
            CStr::from_ptr(sys::iclforge_ac4_decoder_refusal_reason(self.raw.as_ptr()))
                .to_string_lossy()
                .into_owned()
        }
    }

    /// Reads one `raw_ac4_frame`. `None` means this frame has no output yet (its substreams
    /// need configuration no I-frame has sent) - not an error.
    pub fn decode(&mut self, frame: &[u8]) -> Result<Option<DecodedFrame>, Error> {
        let mut out: *mut sys::iclforge_ac4_decoded_frame_t = ptr::null_mut();
        // SAFETY: `frame` is a valid slice for the duration of this call; `out` is a valid
        // out-parameter.
        let status = unsafe {
            sys::iclforge_ac4_decoder_decode(
                self.raw.as_ptr(),
                frame.as_ptr(),
                frame.len(),
                &mut out,
            )
        };
        Error::check(status)?;
        Ok(ptr::NonNull::new(out).map(|raw| DecodedFrame { raw }))
    }

    /// The presentations of the last frame read, in the table of contents' own order; empty
    /// before one.
    pub fn presentations(&self) -> Vec<PresentationInfo> {
        // SAFETY: `self.raw` is valid.
        let count = unsafe { sys::iclforge_ac4_decoder_presentation_count(self.raw.as_ptr()) };
        (0..count)
            .map(|index| {
                // SAFETY: `index` is in `[0, count)`, so every accessor below reads a real
                // presentation rather than taking its null-safety fallback.
                unsafe {
                    let speaker_count = sys::iclforge_ac4_decoder_presentation_speaker_count(
                        self.raw.as_ptr(),
                        index,
                    );
                    let speakers = (0..speaker_count)
                        .map(|s| {
                            Speaker::from_raw(sys::iclforge_ac4_decoder_presentation_speaker(
                                self.raw.as_ptr(),
                                index,
                                s,
                            ))
                        })
                        .collect();
                    PresentationInfo {
                        toc_index: sys::iclforge_ac4_decoder_presentation_toc_index(
                            self.raw.as_ptr(),
                            index,
                        ),
                        presentation_id: (sys::iclforge_ac4_decoder_presentation_has_id(
                            self.raw.as_ptr(),
                            index,
                        ) != 0)
                            .then(|| {
                                sys::iclforge_ac4_decoder_presentation_id(self.raw.as_ptr(), index)
                            }),
                        md_compat: (sys::iclforge_ac4_decoder_presentation_has_md_compat(
                            self.raw.as_ptr(),
                            index,
                        ) != 0)
                            .then(|| {
                                sys::iclforge_ac4_decoder_presentation_md_compat(
                                    self.raw.as_ptr(),
                                    index,
                                )
                            }),
                        enabled: sys::iclforge_ac4_decoder_presentation_enabled(
                            self.raw.as_ptr(),
                            index,
                        ) != 0,
                        alternative: sys::iclforge_ac4_decoder_presentation_alternative(
                            self.raw.as_ptr(),
                            index,
                        ) != 0,
                        pre_virtualized: sys::iclforge_ac4_decoder_presentation_pre_virtualized(
                            self.raw.as_ptr(),
                            index,
                        ) != 0,
                        name: CStr::from_ptr(sys::iclforge_ac4_decoder_presentation_name(
                            self.raw.as_ptr(),
                            index,
                        ))
                        .to_string_lossy()
                        .into_owned(),
                        language: CStr::from_ptr(sys::iclforge_ac4_decoder_presentation_language(
                            self.raw.as_ptr(),
                            index,
                        ))
                        .to_string_lossy()
                        .into_owned(),
                        decodable: sys::iclforge_ac4_decoder_presentation_decodable(
                            self.raw.as_ptr(),
                            index,
                        ) != 0,
                        selectable: sys::iclforge_ac4_decoder_presentation_selectable(
                            self.raw.as_ptr(),
                            index,
                        ) != 0,
                        speakers,
                    }
                }
            })
            .collect()
    }

    /// The loudness metadata of the presentation the last [`Decoder::decode`] call
    /// selected, as the frames read so far have sent it (`iclforge::ac4::LoudnessInfo`'s "big four" -
    /// see `iclforge_ac4_loudness_info_t`'s own comment on the DRC/dialogue-enhancement/
    /// downmix detail this omits).
    pub fn metadata_loudness(&self) -> LoudnessInfo {
        // SAFETY: `self.raw` is valid.
        let raw = unsafe { sys::iclforge_ac4_decoder_metadata_loudness(self.raw.as_ptr()) };
        LoudnessInfo {
            dialnorm_dbfs: (raw.has_dialnorm_dbfs != 0).then_some(raw.dialnorm_dbfs),
            integrated_lkfs: (raw.has_integrated_lkfs != 0).then_some(raw.integrated_lkfs),
            true_peak_dbtp: (raw.has_true_peak_dbtp != 0).then_some(raw.true_peak_dbtp),
            loudness_range_lu: (raw.has_loudness_range_lu != 0).then_some(raw.loudness_range_lu),
        }
    }
}

impl Drop for Decoder {
    fn drop(&mut self) {
        unsafe { sys::iclforge_ac4_decoder_destroy(self.raw.as_ptr()) };
    }
}

/// `iclforge::ac4::PresentationInfo`'s core surface (see [`Decoder::presentations`]'s own comment on
/// what is omitted: `AlternativeTarget`s, `PresentationMember`s and `substream_groups`).
#[derive(Debug, Clone, PartialEq)]
pub struct PresentationInfo {
    pub toc_index: usize,
    pub presentation_id: Option<i32>,
    pub md_compat: Option<i32>,
    pub enabled: bool,
    pub alternative: bool,
    pub pre_virtualized: bool,
    pub name: String,
    pub language: String,
    pub decodable: bool,
    pub selectable: bool,
    pub speakers: Vec<Speaker>,
}

/// Mirrors `iclforge_ac4_loudness_info_t` (`iclforge::ac4::LoudnessInfo`'s "big four").
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct LoudnessInfo {
    pub dialnorm_dbfs: Option<f64>,
    pub integrated_lkfs: Option<f64>,
    pub true_peak_dbtp: Option<f64>,
    pub loudness_range_lu: Option<f64>,
}

/// Mirrors `iclforge_ac4_object_properties_t` (`iclforge::ac4::ObjectProperties`, Part 2 Annex F.2 to
/// F.10 and `add_per_object_md()`'s data): what one block update of an object's metadata sets.
/// The decoder reports it and the encoder takes it in these terms; `iclforge.h` gives each
/// field's range and the steps its code has (an encoder rounds to the nearest and refuses a
/// value off its range). Construct with [`ObjectProperties::default`], which calls the raw
/// `iclforge_ac4_object_properties_init()`: a zeroed struct is a depth exponent no code holds.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ObjectProperties {
    pub active: bool,
    /// +15 to -49 dB in steps of 1, or `f64::NEG_INFINITY` for silence.
    pub gain_db: f64,
    /// 0 to 1 in steps of 1/31.
    pub priority: f64,
    /// X from the left wall (0) to the right (1) and Y from the front wall (0) to the back (1)
    /// in steps of 1/62, Z from the floor (-1) to the ceiling (1) in steps of 1/15; a dynamic
    /// object's, ignored for a bed object and the LFE.
    pub position: [f64; 3],
    /// Part 2 Table 104, 0 to 7.
    pub zone_mask: i32,
    pub enable_elevation: bool,
    pub snap: bool,
    /// The object's width in X, Y and Z, each 0 to 1 in steps of 1/31.
    pub width: [f64; 3],
    /// 0, or 1/8 to 1 in steps of 1/8.
    pub screen_factor: f64,
    /// Exactly 0.25, 0.5, 1 or 2.
    pub depth_exponent: f64,
    /// 1 or more, or `f64::INFINITY` for an object at infinity.
    pub distance: Option<f64>,
    pub divergence: f64,
    pub trim_disabled: bool,
    /// 0 to 3.
    pub headphone_render_mode: Option<i32>,
    pub head_track_disabled: bool,
}

impl Default for ObjectProperties {
    fn default() -> Self {
        let mut raw = unsafe { std::mem::zeroed() };
        // SAFETY: iclforge_ac4_object_properties_init() unconditionally overwrites every field
        // of `raw` with iclforge::ac4::ObjectProperties{}'s defaults (room centre, unity gain, priority 1,
        // depth exponent 1) - a struct-level derive would give priority 0 and depth exponent 0,
        // which the encoder refuses.
        unsafe { sys::iclforge_ac4_object_properties_init(&mut raw) };
        ObjectProperties::from_raw(raw)
    }
}

impl ObjectProperties {
    fn to_raw(self) -> sys::iclforge_ac4_object_properties_t {
        sys::iclforge_ac4_object_properties_t {
            active: self.active as i32,
            gain_db: self.gain_db,
            priority: self.priority,
            x: self.position[0],
            y: self.position[1],
            z: self.position[2],
            zone_mask: self.zone_mask,
            enable_elevation: self.enable_elevation as i32,
            snap: self.snap as i32,
            width_x: self.width[0],
            width_y: self.width[1],
            width_z: self.width[2],
            screen_factor: self.screen_factor,
            depth_exponent: self.depth_exponent,
            has_distance: self.distance.is_some() as i32,
            distance: self.distance.unwrap_or_default(),
            divergence: self.divergence,
            trim_disabled: self.trim_disabled as i32,
            has_headphone_render_mode: self.headphone_render_mode.is_some() as i32,
            headphone_render_mode: self.headphone_render_mode.unwrap_or_default(),
            head_track_disabled: self.head_track_disabled as i32,
        }
    }

    fn from_raw(raw: sys::iclforge_ac4_object_properties_t) -> Self {
        ObjectProperties {
            active: raw.active != 0,
            gain_db: raw.gain_db,
            priority: raw.priority,
            position: [raw.x, raw.y, raw.z],
            zone_mask: raw.zone_mask,
            enable_elevation: raw.enable_elevation != 0,
            snap: raw.snap != 0,
            width: [raw.width_x, raw.width_y, raw.width_z],
            screen_factor: raw.screen_factor,
            depth_exponent: raw.depth_exponent,
            distance: (raw.has_distance != 0).then_some(raw.distance),
            divergence: raw.divergence,
            trim_disabled: raw.trim_disabled != 0,
            headphone_render_mode: (raw.has_headphone_render_mode != 0)
                .then_some(raw.headphone_render_mode),
            head_track_disabled: raw.head_track_disabled != 0,
        }
    }
}

/// One block update of an object's metadata within a frame - `iclforge::ac4::ObjectUpdate` (Part 2 Annex
/// F.11) via `iclforge_ac4_object_update_t`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ObjectUpdate {
    /// The output sample of the frame the update takes effect at, counted with the decoder's
    /// delay as the frame's channels are.
    pub sample: usize,
    /// The samples a renderer takes to move to `properties` from what was in force.
    pub ramp_samples: i32,
    pub properties: ObjectProperties,
}

/// One decoded object of a [`DecodedFrame`] - `iclforge::ac4::DecodedObject` (Part 2 clause 4.8.3.4).
#[derive(Debug, Clone, PartialEq)]
pub struct DecodedObject {
    pub kind: ObjectKind,
    pub lfe: bool,
    pub speaker: Option<Speaker>,
    /// `samples_per_channel()` long, at full scale 1.0.
    pub samples: Vec<f32>,
    /// What is in force at the frame's first sample.
    pub properties: ObjectProperties,
    /// The updates within the frame, in the order they take effect.
    pub updates: Vec<ObjectUpdate>,
}

/// What a concealed frame's decode did - `iclforge::ac4::Concealment`.
#[derive(Debug, Clone, PartialEq)]
pub struct Concealment {
    pub action: ConcealmentAction,
    /// Why the frame did not decode.
    pub error: Error,
}

/// One decoded AC-4 frame - `iclforge::ac4::DecodedFrame` via `iclforge_ac4_decoded_frame_t`. Owns its
/// PCM and object audio; every accessor borrows from `&self`.
pub struct DecodedFrame {
    raw: ptr::NonNull<sys::iclforge_ac4_decoded_frame_t>,
}

unsafe impl Send for DecodedFrame {}

impl DecodedFrame {
    pub fn sample_rate_hz(&self) -> i32 {
        unsafe { sys::iclforge_ac4_decoded_frame_sample_rate_hz(self.raw.as_ptr()) }
    }

    pub fn sequence_counter(&self) -> i32 {
        unsafe { sys::iclforge_ac4_decoded_frame_sequence_counter(self.raw.as_ptr()) }
    }

    /// The presentation decoded: its index in the frame's table of contents.
    pub fn presentation_index(&self) -> usize {
        unsafe { sys::iclforge_ac4_decoded_frame_presentation_index(self.raw.as_ptr()) }
    }

    pub fn presentation_id(&self) -> Option<i32> {
        unsafe {
            (sys::iclforge_ac4_decoded_frame_has_presentation_id(self.raw.as_ptr()) != 0)
                .then(|| sys::iclforge_ac4_decoded_frame_presentation_id(self.raw.as_ptr()))
        }
    }

    pub fn channel_count(&self) -> usize {
        unsafe { sys::iclforge_ac4_decoded_frame_channel_count(self.raw.as_ptr()) }
    }

    /// AC-4's frame length varies by frame rate - unlike AC-3/E-AC-3 there is no fixed
    /// constant, so this is a real per-frame accessor.
    pub fn samples_per_channel(&self) -> usize {
        unsafe { sys::iclforge_ac4_decoded_frame_samples_per_channel(self.raw.as_ptr()) }
    }

    /// `channel_index` in `[0, channel_count())`. Panics if out of range.
    pub fn channel_samples(&self, channel_index: usize) -> &[f32] {
        assert!(
            channel_index < self.channel_count(),
            "channel index out of range"
        );
        // SAFETY: the pointer is valid until `self` is destroyed (iclforge.h's own
        // convention); samples_per_channel() gives the real length.
        unsafe {
            let ptr =
                sys::iclforge_ac4_decoded_frame_channel_samples(self.raw.as_ptr(), channel_index);
            std::slice::from_raw_parts(ptr, self.samples_per_channel())
        }
    }

    /// `channel_index` in `[0, channel_count())`. Panics if out of range.
    pub fn speaker(&self, channel_index: usize) -> Speaker {
        assert!(
            channel_index < self.channel_count(),
            "channel index out of range"
        );
        Speaker::from_raw(unsafe {
            sys::iclforge_ac4_decoded_frame_speaker(self.raw.as_ptr(), channel_index)
        })
    }

    /// Set only on a frame the decoder's [`ConcealmentPolicy`] made in place of one that
    /// did not decode.
    pub fn concealed(&self) -> Option<Concealment> {
        unsafe {
            (sys::iclforge_ac4_decoded_frame_has_concealed(self.raw.as_ptr()) != 0).then(|| {
                Concealment {
                    action: ConcealmentAction::from_raw(
                        sys::iclforge_ac4_decoded_frame_concealment_action(self.raw.as_ptr()),
                    ),
                    error: Error::from_status(sys::iclforge_ac4_decoded_frame_concealment_error(
                        self.raw.as_ptr(),
                    ))
                    .unwrap_or(Error::Internal),
                }
            })
        }
    }

    /// A presentation with object audio: its objects, in the decoder's order - the LFE object
    /// first, then the bed objects, then the dynamic objects, each group in the order the
    /// encoder's [`ObjectsConfig`] lists it. Empty for channel-based and channel-based-immersive
    /// content.
    pub fn objects(&self) -> Vec<DecodedObject> {
        // SAFETY: `self.raw` is valid.
        let count = unsafe { sys::iclforge_ac4_decoded_frame_object_count(self.raw.as_ptr()) };
        let samples_per_channel = self.samples_per_channel();
        (0..count)
            .map(|index| unsafe {
                let speaker =
                    (sys::iclforge_ac4_decoded_frame_object_has_speaker(self.raw.as_ptr(), index)
                        != 0)
                        .then(|| {
                            Speaker::from_raw(sys::iclforge_ac4_decoded_frame_object_speaker(
                                self.raw.as_ptr(),
                                index,
                            ))
                        });
                let ptr = sys::iclforge_ac4_decoded_frame_object_samples(self.raw.as_ptr(), index);
                let samples = if ptr.is_null() {
                    Vec::new()
                } else {
                    std::slice::from_raw_parts(ptr, samples_per_channel).to_vec()
                };
                DecodedObject {
                    kind: ObjectKind::from_raw(sys::iclforge_ac4_decoded_frame_object_kind(
                        self.raw.as_ptr(),
                        index,
                    )),
                    lfe: sys::iclforge_ac4_decoded_frame_object_lfe(self.raw.as_ptr(), index) != 0,
                    speaker,
                    samples,
                    properties: ObjectProperties::from_raw(
                        sys::iclforge_ac4_decoded_frame_object_properties(self.raw.as_ptr(), index),
                    ),
                    updates: (0..sys::iclforge_ac4_decoded_frame_object_update_count(
                        self.raw.as_ptr(),
                        index,
                    ))
                        .map(|update_index| {
                            let update = sys::iclforge_ac4_decoded_frame_object_update(
                                self.raw.as_ptr(),
                                index,
                                update_index,
                            );
                            ObjectUpdate {
                                sample: update.sample,
                                ramp_samples: update.ramp_samples,
                                properties: ObjectProperties::from_raw(update.properties),
                            }
                        })
                        .collect(),
                }
            })
            .collect()
    }
}

impl Drop for DecodedFrame {
    fn drop(&mut self) {
        unsafe { sys::iclforge_ac4_decoded_frame_destroy(self.raw.as_ptr()) };
    }
}

// --- encoder -------------------------------------------------------------

/// Mirrors `iclforge_ac4_object_config_t` (`iclforge::ac4::ObjectConfig`): one object of an
/// [`ObjectsConfig`], the input channel at its index. Construct with
/// [`ObjectConfig::default`], a dynamic object at the room's centre.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ObjectConfig {
    /// A bed object, from this loudspeaker; `None` for a dynamic object.
    pub bed: Option<BedChannel>,
    /// The LFE, at most one object's: its bed channel and position are ignored.
    pub lfe: bool,
    /// What is in force from the first sample.
    pub properties: ObjectProperties,
}

impl ObjectConfig {
    fn to_raw(&self) -> sys::iclforge_ac4_object_config_t {
        sys::iclforge_ac4_object_config_t {
            has_bed: self.bed.is_some() as i32,
            bed: self.bed.map_or(
                sys::iclforge_ac4_bed_channel_ICLFORGE_AC4_BED_LEFT,
                BedChannel::to_raw,
            ),
            lfe: self.lfe as i32,
            properties: self.properties.to_raw(),
        }
    }
}

/// Mirrors `iclforge_ac4_objects_config_t` (`iclforge::ac4::ObjectsConfig`): the objects of the one
/// object substream a stream can have, and how they are coded. The limits (1 to
/// `ICLFORGE_AC4_MAX_OBJECTS` objects, at most one the LFE, a computed downmix of at most
/// `ICLFORGE_AC4_MAX_DOWNMIX_SIGNALS` signals, `frame_rate_index` 13 only, and the rest) are the
/// encoder's: [`Encoder::refusal_reason`] names the rule a configuration breaks.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ObjectsConfig {
    pub objects: Vec<ObjectConfig>,
    pub coding: ObjectCoding,
    pub downmix: AjocDownmix,
    /// A computed downmix's signals; `None` takes one a 32 kbps of the substream's rate, up to
    /// 10.
    pub downmix_signals: Option<i32>,
    /// A-JOC's decorrelators (Part 2 clause 5.7.3.5).
    pub decorrelation: bool,
    /// The parameter bands A-JOC's matrices take (Table 78: 23, 15, 12, 9, 7, 5, 3 or 1) and
    /// whether they are quantised coarsely; `None` takes 23 fine from 64 kbps a downmix signal,
    /// 15 fine from 32 and 12 coarse below.
    pub parameter_bands: Option<i32>,
    pub coarse: Option<bool>,
    /// `oamd_common_data()`'s `master_screen_size_ratio_code` (0 to 31; `None` for
    /// `b_default_screen_size_ratio`) and `b_bed_object_chan_distribute`.
    pub screen_size_ratio_code: Option<i32>,
    pub bed_object_chan_distribute: bool,
}

/// Mirrors `iclforge_ac4_experimental_t` (`iclforge::ac4::EncoderConfig::Experimental`): syntax only this
/// project's readers have read from this encoder, off unless asked for. Not mirrored:
/// `drc_gains` and `three_zero`, which need the DRC modes and the substream list this crate does
/// not carry.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Experimental {
    /// The ASPX mode's pairs as sum and balance where that is fewer bits.
    pub aspx_balance: bool,
    /// The ASPX mode's VARVAR framing.
    pub aspx_varvar: bool,
    /// Frequency interleaved waveform coding above the crossover.
    pub aspx_interleave: bool,
    /// The 5.X and 7.X elements' `coding_config` 1 to 3 and `2ch_mode` 1.
    pub coding_configs: bool,
    /// Seven or eight input channels, with this pair beyond L R C Ls Rs.
    pub seven_x: AdditionalPair,
    /// The A-CPL modes DEE's streams do not use (ASPX_ACPL_1, stereo A-CPL).
    pub acpl: bool,
    /// 7.0.4 and 7.1.4 with the back pair: eleven or twelve input channels.
    pub back_pair: bool,
    /// The immersive element's ASPX_AJCC.
    pub ajcc: bool,
    /// Object audio; required by an [`EncoderConfig::objects`].
    pub objects: bool,
}

impl Experimental {
    fn to_raw(self) -> sys::iclforge_ac4_experimental_t {
        sys::iclforge_ac4_experimental_t {
            aspx_balance: self.aspx_balance as i32,
            aspx_varvar: self.aspx_varvar as i32,
            aspx_interleave: self.aspx_interleave as i32,
            coding_configs: self.coding_configs as i32,
            seven_x: self.seven_x.to_raw(),
            acpl: self.acpl as i32,
            back_pair: self.back_pair as i32,
            ajcc: self.ajcc as i32,
            objects: self.objects as i32,
        }
    }
}

/// Mirrors `iclforge_ac4_encoder_config_t` (`iclforge::ac4::EncoderConfig`, less what this module's own doc
/// comment leaves out). Construct with [`EncoderConfig::default`].
#[derive(Debug, Clone, PartialEq)]
pub struct EncoderConfig {
    /// 1, 2, 5, 6, 9 or 10 - see `iclforge::ac4::EncoderConfig::channels`'s own comment; ignored with
    /// `objects`.
    pub channels: i32,
    /// 48000, or 44100 (`frame_rate_index` 13 only).
    pub sample_rate_hz: i32,
    /// Part 1 Table 83/84; default 13, the 2048-sample frame.
    pub frame_rate_index: i32,
    pub bitrate_kbps: i32,
    pub rate_mode: RateMode,
    /// With `objects`, the object substream's.
    pub codec_mode: CodecMode,
    pub iframe_interval: i32,
    pub dialnorm_db: f64,
    /// Frames, counted from 0, that must be I-frames besides those `iframe_interval` makes.
    pub iframes: Vec<i64>,
    /// Where the caller's fragments start, in samples of the decoded output from its first:
    /// the frame whose output starts there, or the first to start after it, is an I-frame.
    pub fragment_starts: Vec<i64>,
    pub experimental: Experimental,
    /// `None` for channel-based content; otherwise the stream is one object substream of these
    /// objects, and needs `experimental.objects`.
    pub objects: Option<ObjectsConfig>,
}

impl EncoderConfig {
    /// Builds the raw struct - and the arrays it points to, which this function keeps alive on
    /// its own stack frame - and hands it to `f` for the duration of the call.
    fn with_raw<R>(&self, f: impl FnOnce(&sys::iclforge_ac4_encoder_config_t) -> R) -> R {
        let object_configs: Vec<sys::iclforge_ac4_object_config_t> = self
            .objects
            .as_ref()
            .map(|objects| objects.objects.iter().map(ObjectConfig::to_raw).collect())
            .unwrap_or_default();
        let objects_raw = self
            .objects
            .as_ref()
            .map(|objects| sys::iclforge_ac4_objects_config_t {
                objects: if object_configs.is_empty() {
                    ptr::null()
                } else {
                    object_configs.as_ptr()
                },
                object_count: object_configs.len(),
                coding: objects.coding.to_raw(),
                downmix: objects.downmix.to_raw(),
                has_downmix_signals: objects.downmix_signals.is_some() as i32,
                downmix_signals: objects.downmix_signals.unwrap_or_default(),
                decorrelation: objects.decorrelation as i32,
                has_parameter_bands: objects.parameter_bands.is_some() as i32,
                parameter_bands: objects.parameter_bands.unwrap_or_default(),
                has_coarse: objects.coarse.is_some() as i32,
                coarse: objects.coarse.unwrap_or_default() as i32,
                has_screen_size_ratio_code: objects.screen_size_ratio_code.is_some() as i32,
                screen_size_ratio_code: objects.screen_size_ratio_code.unwrap_or_default(),
                bed_object_chan_distribute: objects.bed_object_chan_distribute as i32,
            });
        let raw = sys::iclforge_ac4_encoder_config_t {
            channels: self.channels,
            sample_rate_hz: self.sample_rate_hz,
            frame_rate_index: self.frame_rate_index,
            bitrate_kbps: self.bitrate_kbps,
            rate_mode: self.rate_mode.to_raw(),
            codec_mode: self.codec_mode.to_raw(),
            iframe_interval: self.iframe_interval,
            dialnorm_db: self.dialnorm_db,
            iframes: if self.iframes.is_empty() {
                ptr::null()
            } else {
                self.iframes.as_ptr()
            },
            iframe_count: self.iframes.len(),
            fragment_starts: if self.fragment_starts.is_empty() {
                ptr::null()
            } else {
                self.fragment_starts.as_ptr()
            },
            fragment_start_count: self.fragment_starts.len(),
            experimental: self.experimental.to_raw(),
            objects: objects_raw
                .as_ref()
                .map_or(ptr::null(), |objects| objects as *const _),
        };
        f(&raw)
    }
}

impl Default for EncoderConfig {
    fn default() -> Self {
        let mut raw = unsafe { std::mem::zeroed() };
        // SAFETY: iclforge_ac4_encoder_config_init() unconditionally overwrites every field of
        // `raw` - the one sanctioned way to obtain the real EncoderConfig{} defaults, per
        // ac3.rs's identical reasoning for its own EncoderConfig.
        unsafe { sys::iclforge_ac4_encoder_config_init(&mut raw) };
        EncoderConfig {
            channels: raw.channels,
            sample_rate_hz: raw.sample_rate_hz,
            frame_rate_index: raw.frame_rate_index,
            bitrate_kbps: raw.bitrate_kbps,
            rate_mode: match raw.rate_mode {
                #[allow(non_upper_case_globals)]
                sys::iclforge_ac4_rate_mode_ICLFORGE_AC4_RATE_AVERAGE => RateMode::Average,
                #[allow(non_upper_case_globals)]
                sys::iclforge_ac4_rate_mode_ICLFORGE_AC4_RATE_VARIABLE => RateMode::Variable,
                _ => RateMode::Constant,
            },
            codec_mode: CodecMode::from_raw(raw.codec_mode),
            iframe_interval: raw.iframe_interval,
            dialnorm_db: raw.dialnorm_db,
            // Every flag of iclforge::ac4::EncoderConfig::Experimental is off by default, as is the
            // additional pair (iclforge_ac4_encoder_config_init()), which Experimental::default()
            // spells - no value in `raw` a derive would get wrong.
            iframes: Vec::new(),
            fragment_starts: Vec::new(),
            experimental: Experimental::default(),
            objects: None,
        }
    }
}

/// Mirrors `iclforge_ac4_object_metadata_update_t` (`iclforge::ac4::ObjectMetadataUpdate`): a change to an
/// object's metadata, given with the input it belongs to (see [`Encoder::encode_objects`]).
/// Construct with [`ObjectMetadataUpdate::default`].
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct ObjectMetadataUpdate {
    /// An index into [`ObjectsConfig::objects`].
    pub object: usize,
    /// From input sample `sample` of the call's channels (0 its first, and any later one) the
    /// object moves to `properties`.
    pub sample: i64,
    /// Over `ramp_samples` (0 to 2047, or 2048).
    pub ramp_samples: i32,
    pub properties: ObjectProperties,
}

impl ObjectMetadataUpdate {
    fn to_raw(self) -> sys::iclforge_ac4_object_metadata_update_t {
        sys::iclforge_ac4_object_metadata_update_t {
            object: self.object,
            sample: self.sample,
            ramp_samples: self.ramp_samples,
            properties: self.properties.to_raw(),
        }
    }
}

/// One encoded AC-4 frame - `iclforge::ac4::EncodedFrame` via `iclforge_ac4_encoded_frame_t`. What an
/// MP4 sample holds as it is; [`sync_frame`] wraps it for a raw `.ac4` file or MPEG-2 TS.
pub struct EncodedFrame {
    raw: ptr::NonNull<sys::iclforge_ac4_encoded_frame_t>,
}

unsafe impl Send for EncodedFrame {}
unsafe impl Sync for EncodedFrame {}

impl EncodedFrame {
    pub fn data(&self) -> &[u8] {
        unsafe {
            let ptr = sys::iclforge_ac4_encoded_frame_data(self.raw.as_ptr());
            let size = sys::iclforge_ac4_encoded_frame_size(self.raw.as_ptr());
            if ptr.is_null() || size == 0 {
                &[]
            } else {
                std::slice::from_raw_parts(ptr, size)
            }
        }
    }

    /// PCM samples per channel this frame decodes to, at the input's rate.
    pub fn samples(&self) -> i32 {
        unsafe { sys::iclforge_ac4_encoded_frame_samples(self.raw.as_ptr()) }
    }

    pub fn iframe(&self) -> bool {
        unsafe { sys::iclforge_ac4_encoded_frame_iframe(self.raw.as_ptr()) != 0 }
    }
}

impl Drop for EncodedFrame {
    fn drop(&mut self) {
        unsafe { sys::iclforge_ac4_encoded_frame_destroy(self.raw.as_ptr()) };
    }
}

/// TS 103 190-2 Table E.1: the media time scale an ISOBMFF track of the stream counts in,
/// and each sample's duration in it - `iclforge::ac4::MediaTiming`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MediaTiming {
    pub timescale: u32,
    pub sample_delta: u32,
}

/// An owned copy of an encoder's table of contents - `iclforge::ac4::Toc` via `iclforge_ac4_toc_t`,
/// for the dac4 box a container muxer needs.
pub struct Toc {
    raw: ptr::NonNull<sys::iclforge_ac4_toc_t>,
}

unsafe impl Send for Toc {}

impl Toc {
    /// The 'dac4' box payload (`ac4_dsi_v1`, Annex E.6, box header excluded) - `iclforge::ac4::build_dac4`.
    /// Empty where [`Toc::dac4_refusal`] names what this cannot describe whole.
    pub fn build_dac4(&self) -> Result<Bytes, Error> {
        let mut out: *mut sys::iclforge_bytes_t = ptr::null_mut();
        let status = unsafe { sys::iclforge_ac4_build_dac4(self.raw.as_ptr(), &mut out) };
        Error::check(status)?;
        Ok(unsafe { Bytes::from_raw(out) })
    }

    /// Why [`Toc::build_dac4`] wrote nothing; empty where it describes every presentation
    /// whole.
    pub fn dac4_refusal(&self) -> String {
        // SAFETY: always a valid NUL-terminated C string (never NULL - see
        // iclforge_ac4_dac4_refusal()'s own doc comment on normalizing the empty case).
        unsafe {
            CStr::from_ptr(sys::iclforge_ac4_dac4_refusal(self.raw.as_ptr()))
                .to_string_lossy()
                .into_owned()
        }
    }

    /// `None` for a frame rate Table 83/84 does not define a single time scale for (the
    /// 1000/1001-family rates, whose frame length alternates).
    pub fn media_timing(&self) -> Option<MediaTiming> {
        let mut timescale = 0u32;
        let mut sample_delta = 0u32;
        let has_value = unsafe {
            sys::iclforge_ac4_media_timing(self.raw.as_ptr(), &mut timescale, &mut sample_delta)
        };
        (has_value != 0).then_some(MediaTiming {
            timescale,
            sample_delta,
        })
    }

    /// Samples per AC-4 frame at the stream's own sample rate; `None` for a frame rate whose
    /// length alternates (see [`Toc::media_timing`]).
    pub fn samples_per_frame(&self) -> Option<u32> {
        let mut samples = 0u32;
        let has_value =
            unsafe { sys::iclforge_ac4_samples_per_frame(self.raw.as_ptr(), &mut samples) };
        (has_value != 0).then_some(samples)
    }
}

impl Drop for Toc {
    fn drop(&mut self) {
        unsafe { sys::iclforge_ac4_toc_destroy(self.raw.as_ptr()) };
    }
}

/// An AC-4 encoder - `iclforge::ac4::Encoder` via `iclforge_ac4_encoder_t`.
pub struct Encoder {
    raw: ptr::NonNull<sys::iclforge_ac4_encoder_t>,
}

unsafe impl Send for Encoder {}

impl Encoder {
    /// Fails with [`Error::Ac4EncodeInvalidConfig`] for a configuration outside what the
    /// encoder writes, or whose rate cannot hold its least frame; [`Encoder::refusal_reason`]
    /// says which rule it breaks.
    pub fn new(config: &EncoderConfig) -> Result<Self, Error> {
        config.with_raw(|raw_config| {
            let mut out: *mut sys::iclforge_ac4_encoder_t = ptr::null_mut();
            // SAFETY: `raw_config` and the arrays it points to are valid for the duration of
            // this call; `out` is a valid out-parameter.
            let status = unsafe { sys::iclforge_ac4_encoder_create(raw_config, &mut out) };
            Error::check(status)?;
            let raw = ptr::NonNull::new(out)
                .expect("iclforge_ac4_encoder_create returned OK with a null encoder");
            Ok(Encoder { raw })
        })
    }

    /// Why [`Encoder::new`] refuses `config`: the first rule it breaks, such as "objects at a
    /// frame_rate_index other than 13"; empty where it makes an encoder of it
    /// (`iclforge::ac4::Encoder::refusal_reason`). It does `new()`'s work to find out.
    pub fn refusal_reason(config: &EncoderConfig) -> String {
        config.with_raw(|raw_config| {
            // SAFETY: `raw_config` is valid for the duration of this call; the result is
            // library-owned storage, always a valid NUL-terminated C string (never NULL).
            unsafe {
                CStr::from_ptr(sys::iclforge_ac4_encoder_refusal_reason(raw_config))
                    .to_string_lossy()
                    .into_owned()
            }
        })
    }

    /// The codec mode the stream is actually coded in - never [`CodecMode::Auto`].
    pub fn codec_mode(&self) -> CodecMode {
        CodecMode::from_raw(unsafe { sys::iclforge_ac4_encoder_codec_mode(self.raw.as_ptr()) })
    }

    /// Samples of silence the encoder puts before the input, at the input's rate.
    pub fn delay_samples(&self) -> i32 {
        unsafe { sys::iclforge_ac4_encoder_delay_samples(self.raw.as_ptr()) }
    }

    /// The delay a matching [`Decoder`] adds on top, at the input's rate.
    pub fn decoder_delay_samples(&self) -> i32 {
        unsafe { sys::iclforge_ac4_encoder_decoder_delay_samples(self.raw.as_ptr()) }
    }

    /// Planar samples at full scale 1.0, one span per input channel, all the same length, any
    /// length - the encoder buffers input to its own frame length internally. Returns the
    /// frames this input completed, in order; the encoder's delay holds back the frames the
    /// last input still needs.
    pub fn encode(&mut self, channels: &[&[f32]]) -> Result<Vec<EncodedFrame>, Error> {
        if channels.is_empty() {
            return Err(Error::InvalidArgument);
        }
        let samples_per_channel = channels[0].len();
        if channels.iter().any(|c| c.len() != samples_per_channel) {
            return Err(Error::InvalidArgument);
        }
        let pointers: Vec<*const f32> = channels.iter().map(|c| c.as_ptr()).collect();
        self.encode_raw(&pointers, samples_per_channel)
    }

    /// [`Encoder::encode`] for an encoder with an [`ObjectsConfig`], and the changes to the
    /// objects' metadata within this input or after it, in any order: `objects` is one slice of
    /// PCM per object, all the same length. An update for an object the configuration lacks,
    /// before this input's first sample, or with a property off its range is
    /// [`Error::Ac4EncodeInvalidInput`], and so is any update to an encoder without an object
    /// substream (`iclforge::ac4::Encoder::encode`'s overload with updates).
    pub fn encode_objects(
        &mut self,
        objects: &[&[f32]],
        updates: &[ObjectMetadataUpdate],
    ) -> Result<Vec<EncodedFrame>, Error> {
        if objects.is_empty() {
            return Err(Error::InvalidArgument);
        }
        let samples_per_object = objects[0].len();
        if objects.iter().any(|c| c.len() != samples_per_object) {
            return Err(Error::InvalidArgument);
        }
        let pointers: Vec<*const f32> = objects.iter().map(|c| c.as_ptr()).collect();
        let raw_updates: Vec<sys::iclforge_ac4_object_metadata_update_t> =
            updates.iter().map(|update| update.to_raw()).collect();
        let mut out: *mut *mut sys::iclforge_ac4_encoded_frame_t = ptr::null_mut();
        let mut count: usize = 0;
        // SAFETY: `pointers` holds one valid pointer per object, each to `samples_per_object`
        // live f32s, and `raw_updates` holds `raw_updates.len()` initialized updates, for the
        // duration of this call; `out`/`count` are valid out-parameters.
        let status = unsafe {
            sys::iclforge_ac4_encoder_encode_objects(
                self.raw.as_ptr(),
                pointers.as_ptr(),
                pointers.len(),
                samples_per_object,
                if raw_updates.is_empty() {
                    ptr::null()
                } else {
                    raw_updates.as_ptr()
                },
                raw_updates.len(),
                &mut out,
                &mut count,
            )
        };
        Error::check(status)?;
        Ok(Self::collect_frames(out, count))
    }

    /// Ends the stream: pads the input with silence to the end of its last frame and returns
    /// the frames the delay still held. The encoder takes no input after it.
    pub fn flush(&mut self) -> Result<Vec<EncodedFrame>, Error> {
        let mut out: *mut *mut sys::iclforge_ac4_encoded_frame_t = ptr::null_mut();
        let mut count: usize = 0;
        // SAFETY: `out`/`count` are valid out-parameters.
        let status =
            unsafe { sys::iclforge_ac4_encoder_flush(self.raw.as_ptr(), &mut out, &mut count) };
        Error::check(status)?;
        Ok(Self::collect_frames(out, count))
    }

    /// The table of contents every frame carries, as it stands after the frames encoded so
    /// far.
    pub fn toc(&self) -> Result<Toc, Error> {
        let mut out: *mut sys::iclforge_ac4_toc_t = ptr::null_mut();
        // SAFETY: `self.raw` is valid; `out` is a valid out-parameter.
        let status = unsafe { sys::iclforge_ac4_encoder_toc(self.raw.as_ptr(), &mut out) };
        Error::check(status)?;
        let raw =
            ptr::NonNull::new(out).expect("iclforge_ac4_encoder_toc returned OK with a null toc");
        Ok(Toc { raw })
    }

    fn encode_raw(
        &mut self,
        pointers: &[*const f32],
        samples_per_channel: usize,
    ) -> Result<Vec<EncodedFrame>, Error> {
        let mut out: *mut *mut sys::iclforge_ac4_encoded_frame_t = ptr::null_mut();
        let mut count: usize = 0;
        // SAFETY: `pointers` holds one valid pointer per channel, each to
        // `samples_per_channel` live f32s for the duration of this call; `out`/`count` are
        // valid out-parameters.
        let status = unsafe {
            sys::iclforge_ac4_encoder_encode(
                self.raw.as_ptr(),
                pointers.as_ptr(),
                pointers.len(),
                samples_per_channel,
                &mut out,
                &mut count,
            )
        };
        Error::check(status)?;
        Ok(Self::collect_frames(out, count))
    }

    fn collect_frames(
        out: *mut *mut sys::iclforge_ac4_encoded_frame_t,
        count: usize,
    ) -> Vec<EncodedFrame> {
        if out.is_null() || count == 0 {
            return Vec::new();
        }
        // SAFETY: ICLFORGE_OK with a non-NULL array guarantees `count` valid, exclusively-
        // owned handles - iclforge_ac4_encoder_encode()/_flush()'s own out-parameter contract.
        let frames = unsafe {
            let slice = std::slice::from_raw_parts(out, count);
            let frames: Vec<EncodedFrame> = slice
                .iter()
                .map(|&raw| EncodedFrame {
                    raw: ptr::NonNull::new(raw).expect("encoded frame array held a null entry"),
                })
                .collect();
            // The array itself (not its elements, which `frames` now owns) still needs
            // freeing - a plain array free, not the combined array+elements
            // iclforge_ac4_encoded_frame_array_destroy() does, since that would double-free
            // the elements `frames` now owns. iclforge.h documents this split for exactly
            // this reason (see iclforge_decoded_substream_array_destroy()'s own comment on
            // taking every handle first and passing count 0).
            sys::iclforge_ac4_encoded_frame_array_destroy(out, 0);
            frames
        };
        frames
    }
}

impl Drop for Encoder {
    fn drop(&mut self) {
        unsafe { sys::iclforge_ac4_encoder_destroy(self.raw.as_ptr()) };
    }
}

/// Part 2 Annex G.3.1's `ac4_syncframe()`: the sync word 0xAC40, or 0xAC41 and a trailing
/// `crc_word` when `crc` is set, then `frame_size` and `raw_frame`.
pub fn sync_frame(raw_frame: &[u8], crc: bool) -> Result<Bytes, Error> {
    let mut out: *mut sys::iclforge_bytes_t = ptr::null_mut();
    // SAFETY: `raw_frame` is a valid slice for the duration of this call; `out` is a valid
    // out-parameter.
    let status = unsafe {
        sys::iclforge_ac4_sync_frame(raw_frame.as_ptr(), raw_frame.len(), crc as i32, &mut out)
    };
    Error::check(status)?;
    Ok(unsafe { Bytes::from_raw(out) })
}
