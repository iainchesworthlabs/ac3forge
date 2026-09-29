//! AC-4 encode and decode - `ac4::Decoder`/`ac4::Encoder` via
//! `ac3forge_ac4_decoder_t`/`ac3forge_ac4_encoder_t` (ETSI TS 103 190-1/-2).
//!
//! Mirrors the same "core surface" the C API itself mirrors (see
//! `ac3forge_ac4_encoder_config_t`'s own comment in `ac3forge.h`): channel-based
//! and channel-based-immersive content, one substream, one presentation. The
//! loudness/DRC/downmix/dialogue-enhancement metadata groups, multi-substream/
//! multi-presentation configurations and A-JOC/direct-coded objects on the
//! encoder side are not exposed here either - a caller who needs them links
//! `ac4enc`/`ac4dec` directly instead of through this crate.
//!
//! Present only when the linked `ac3forge_c` was built with `AC3FORGE_BUILD_AC4`
//! on (the default) - `ac3forge-sys`'s bindgen output simply has no
//! `ac3forge_ac4_*` items otherwise, so this whole module fails to compile
//! rather than link. There is no Cargo feature for it (unlike the C library's
//! own build option): `-sys`'s `build.rs` leaves `AC3FORGE_BUILD_AC4` at its
//! CMake default, so this module is unconditionally available in practice
//! the same way `atmos` is (also compiled unconditionally, having no matching
//! CMake option of its own).

use ac3forge_sys as sys;
use std::ffi::{CStr, CString};
use std::ptr;

use crate::bytes::Bytes;
use crate::error::Error;

// --- shared enums -----------------------------------------------------------

/// Mirrors `ac3forge_ac4_speaker_t` (Part 1 clause D.1, Part 2 clause A.3).
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
    fn from_raw(raw: sys::ac3forge_ac4_speaker_t) -> Self {
        #[allow(non_upper_case_globals)]
        match raw {
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_LEFT => Speaker::Left,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_RIGHT => Speaker::Right,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_CENTRE => Speaker::Centre,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_LFE => Speaker::Lfe,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_LEFT_SURROUND => Speaker::LeftSurround,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_RIGHT_SURROUND => Speaker::RightSurround,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_LEFT_BACK => Speaker::LeftBack,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_RIGHT_BACK => Speaker::RightBack,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_LEFT_WIDE => Speaker::LeftWide,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_RIGHT_WIDE => Speaker::RightWide,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_TOP_FRONT_LEFT => Speaker::TopFrontLeft,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_TOP_FRONT_RIGHT => {
                Speaker::TopFrontRight
            }
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_TOP_BACK_LEFT => Speaker::TopBackLeft,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_TOP_BACK_RIGHT => Speaker::TopBackRight,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_TOP_SIDE_LEFT => Speaker::TopSideLeft,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_TOP_SIDE_RIGHT => Speaker::TopSideRight,
            sys::ac3forge_ac4_speaker_AC3FORGE_AC4_SPEAKER_LFE2 => Speaker::Lfe2,
            // An unrecognized ordinal cannot happen from this crate's own calls (every
            // accessor's C side clamps out-of-range indices to AC3FORGE_AC4_SPEAKER_LEFT
            // rather than an unmapped value) - Left is the same fallback ac3forge.h's own
            // null-safety convention uses.
            _ => Speaker::Left,
        }
    }
}

/// Mirrors `ac3forge_ac4_object_kind_t` (`ac4::ObjectKind`): a bed object, a dynamic
/// object, or an intermediate spatial format object (rendered into channels, not
/// listed - see [`DecodedFrame::objects`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ObjectKind {
    Bed,
    Dyn,
    Isf,
}

impl ObjectKind {
    fn from_raw(raw: sys::ac3forge_ac4_object_kind_t) -> Self {
        #[allow(non_upper_case_globals)]
        match raw {
            sys::ac3forge_ac4_object_kind_AC3FORGE_AC4_OBJECT_BED => ObjectKind::Bed,
            sys::ac3forge_ac4_object_kind_AC3FORGE_AC4_OBJECT_ISF => ObjectKind::Isf,
            _ => ObjectKind::Dyn,
        }
    }
}

/// Mirrors `ac3forge_ac4_downmix_target_t` (`ac4::DownmixTarget`): the layout
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
    fn to_raw(self) -> sys::ac3forge_ac4_downmix_target_t {
        match self {
            DownmixTarget::AsCoded => {
                sys::ac3forge_ac4_downmix_target_AC3FORGE_AC4_DOWNMIX_AS_CODED
            }
            DownmixTarget::FiveX => sys::ac3forge_ac4_downmix_target_AC3FORGE_AC4_DOWNMIX_5X,
            DownmixTarget::Stereo => sys::ac3forge_ac4_downmix_target_AC3FORGE_AC4_DOWNMIX_STEREO,
            DownmixTarget::LoRo => sys::ac3forge_ac4_downmix_target_AC3FORGE_AC4_DOWNMIX_LORO,
            DownmixTarget::LtRt => sys::ac3forge_ac4_downmix_target_AC3FORGE_AC4_DOWNMIX_LTRT,
            DownmixTarget::Mono => sys::ac3forge_ac4_downmix_target_AC3FORGE_AC4_DOWNMIX_MONO,
            DownmixTarget::SevenX4 => sys::ac3forge_ac4_downmix_target_AC3FORGE_AC4_DOWNMIX_7X4,
            DownmixTarget::SevenX2 => sys::ac3forge_ac4_downmix_target_AC3FORGE_AC4_DOWNMIX_7X2,
            DownmixTarget::SevenX0 => sys::ac3forge_ac4_downmix_target_AC3FORGE_AC4_DOWNMIX_7X0,
            DownmixTarget::FiveX4 => sys::ac3forge_ac4_downmix_target_AC3FORGE_AC4_DOWNMIX_5X4,
            DownmixTarget::FiveX2 => sys::ac3forge_ac4_downmix_target_AC3FORGE_AC4_DOWNMIX_5X2,
        }
    }
}

/// Mirrors `ac3forge_ac4_drc_mode_t` (`ac4::DrcMode`, Part 1 Table 161).
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
    fn to_raw(self) -> sys::ac3forge_ac4_drc_mode_t {
        match self {
            DrcMode::Off => sys::ac3forge_ac4_drc_mode_AC3FORGE_AC4_DRC_OFF,
            DrcMode::Default => sys::ac3forge_ac4_drc_mode_AC3FORGE_AC4_DRC_DEFAULT,
            DrcMode::HomeTheatre => sys::ac3forge_ac4_drc_mode_AC3FORGE_AC4_DRC_HOME_THEATRE,
            DrcMode::FlatPanelTv => sys::ac3forge_ac4_drc_mode_AC3FORGE_AC4_DRC_FLAT_PANEL_TV,
            DrcMode::PortableSpeakers => {
                sys::ac3forge_ac4_drc_mode_AC3FORGE_AC4_DRC_PORTABLE_SPEAKERS
            }
            DrcMode::PortableHeadphones => {
                sys::ac3forge_ac4_drc_mode_AC3FORGE_AC4_DRC_PORTABLE_HEADPHONES
            }
        }
    }
}

/// Mirrors `ac3forge_ac4_decoding_mode_t` (`ac4::DecodingMode`, Part 2 clause 4.7).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum DecodingMode {
    #[default]
    Full,
    Core,
}

impl DecodingMode {
    fn to_raw(self) -> sys::ac3forge_ac4_decoding_mode_t {
        match self {
            DecodingMode::Full => sys::ac3forge_ac4_decoding_mode_AC3FORGE_AC4_DECODING_FULL,
            DecodingMode::Core => sys::ac3forge_ac4_decoding_mode_AC3FORGE_AC4_DECODING_CORE,
        }
    }
}

/// Mirrors `ac3forge_ac4_concealment_policy_t` (`ac4::ConcealmentPolicy`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum ConcealmentPolicy {
    #[default]
    None,
    RepeatFade,
    Mute,
}

impl ConcealmentPolicy {
    fn to_raw(self) -> sys::ac3forge_ac4_concealment_policy_t {
        match self {
            ConcealmentPolicy::None => {
                sys::ac3forge_ac4_concealment_policy_AC3FORGE_AC4_CONCEALMENT_NONE
            }
            ConcealmentPolicy::RepeatFade => {
                sys::ac3forge_ac4_concealment_policy_AC3FORGE_AC4_CONCEALMENT_REPEAT_FADE
            }
            ConcealmentPolicy::Mute => {
                sys::ac3forge_ac4_concealment_policy_AC3FORGE_AC4_CONCEALMENT_MUTE
            }
        }
    }
}

/// Mirrors `ac3forge_ac4_concealment_action_t` (`ac4::ConcealmentAction`): what a
/// concealed frame actually got.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ConcealmentAction {
    RepeatFade,
    Mute,
}

impl ConcealmentAction {
    fn from_raw(raw: sys::ac3forge_ac4_concealment_action_t) -> Self {
        #[allow(non_upper_case_globals)]
        match raw {
            sys::ac3forge_ac4_concealment_action_AC3FORGE_AC4_CONCEALMENT_ACTION_REPEAT_FADE => {
                ConcealmentAction::RepeatFade
            }
            _ => ConcealmentAction::Mute,
        }
    }
}

/// Mirrors `ac3forge_ac4_associated_type_t` (`ac4::AssociatedType`, Part 1 Table 92).
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
    fn to_raw(self) -> sys::ac3forge_ac4_associated_type_t {
        match self {
            AssociatedType::Any => sys::ac3forge_ac4_associated_type_AC3FORGE_AC4_ASSOCIATED_ANY,
            AssociatedType::AudioDescription => {
                sys::ac3forge_ac4_associated_type_AC3FORGE_AC4_ASSOCIATED_AUDIO_DESCRIPTION
            }
            AssociatedType::AudioDescriptionSubtitles => sys::ac3forge_ac4_associated_type_AC3FORGE_AC4_ASSOCIATED_AUDIO_DESCRIPTION_SUBTITLES,
            AssociatedType::SpokenSubtitles => {
                sys::ac3forge_ac4_associated_type_AC3FORGE_AC4_ASSOCIATED_SPOKEN_SUBTITLES
            }
            AssociatedType::EmergencyInformation => {
                sys::ac3forge_ac4_associated_type_AC3FORGE_AC4_ASSOCIATED_EMERGENCY_INFORMATION
            }
        }
    }
}

/// Mirrors `ac3forge_ac4_codec_mode_t` (`ac4::CodecMode`, Part 1 clause 4.3.6.1).
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
    fn to_raw(self) -> sys::ac3forge_ac4_codec_mode_t {
        match self {
            CodecMode::Auto => sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_AUTO,
            CodecMode::Simple => sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_SIMPLE,
            CodecMode::Aspx => sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_ASPX,
            CodecMode::AspxAcpl1 => sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_ASPX_ACPL1,
            CodecMode::AspxAcpl2 => sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_ASPX_ACPL2,
            CodecMode::AspxAcpl3 => sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_ASPX_ACPL3,
            CodecMode::Scpl => sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_SCPL,
            CodecMode::AspxScpl => sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_ASPX_SCPL,
            CodecMode::AspxAjcc => sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_ASPX_AJCC,
        }
    }

    fn from_raw(raw: sys::ac3forge_ac4_codec_mode_t) -> Self {
        #[allow(non_upper_case_globals)]
        match raw {
            sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_SIMPLE => CodecMode::Simple,
            sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_ASPX => CodecMode::Aspx,
            sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_ASPX_ACPL1 => CodecMode::AspxAcpl1,
            sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_ASPX_ACPL2 => CodecMode::AspxAcpl2,
            sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_ASPX_ACPL3 => CodecMode::AspxAcpl3,
            sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_SCPL => CodecMode::Scpl,
            sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_ASPX_SCPL => CodecMode::AspxScpl,
            sys::ac3forge_ac4_codec_mode_AC3FORGE_AC4_CODEC_ASPX_AJCC => CodecMode::AspxAjcc,
            _ => CodecMode::Auto,
        }
    }
}

/// Mirrors `ac3forge_ac4_rate_mode_t` (`ac4::RateMode`, Part 1 Table 81's `wait_frames`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum RateMode {
    #[default]
    Constant,
    Average,
    Variable,
}

impl RateMode {
    fn to_raw(self) -> sys::ac3forge_ac4_rate_mode_t {
        match self {
            RateMode::Constant => sys::ac3forge_ac4_rate_mode_AC3FORGE_AC4_RATE_CONSTANT,
            RateMode::Average => sys::ac3forge_ac4_rate_mode_AC3FORGE_AC4_RATE_AVERAGE,
            RateMode::Variable => sys::ac3forge_ac4_rate_mode_AC3FORGE_AC4_RATE_VARIABLE,
        }
    }
}

// --- decoder configuration ---------------------------------------------------

/// Mirrors `ac3forge_ac4_output_config_t` (`ac4::OutputConfig`). Construct with
/// [`OutputConfig::default`] (which calls the raw `ac3forge_ac4_output_config_init()` -
/// same "call the real _init(), never derive it" reasoning as `ac3::EncoderConfig`) and
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
    fn to_raw(&self) -> sys::ac3forge_ac4_output_config_t {
        sys::ac3forge_ac4_output_config_t {
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

    fn from_raw(raw: &sys::ac3forge_ac4_output_config_t) -> Self {
        OutputConfig {
            output_level_dbfs: (raw.has_output_level_dbfs != 0).then_some(raw.output_level_dbfs),
            drc: match raw.drc {
                #[allow(non_upper_case_globals)]
                sys::ac3forge_ac4_drc_mode_AC3FORGE_AC4_DRC_OFF => DrcMode::Off,
                _ => DrcMode::Default,
            },
            headphones: raw.headphones != 0,
            dialogue_enhancement_db: raw.dialogue_enhancement_db,
            downmix: DownmixTarget::AsCoded, // the only value ac3forge_ac4_output_config_init() sets
            mix_lfe: raw.mix_lfe != 0,
            dialogue_gain_db: raw.dialogue_gain_db,
            associated_gain_db: raw.associated_gain_db,
        }
    }
}

impl Default for OutputConfig {
    fn default() -> Self {
        let mut raw = unsafe { std::mem::zeroed() };
        // SAFETY: ac3forge_ac4_output_config_init() unconditionally overwrites every field of
        // `raw` - the one sanctioned way to obtain the real OutputConfig{} defaults (mix_lfe
        // true, etc.) rather than guessing at them Rust-side.
        unsafe { sys::ac3forge_ac4_output_config_init(&mut raw) };
        OutputConfig::from_raw(&raw)
    }
}

/// Mirrors `ac3forge_ac4_presentation_choice_t` (`ac4::PresentationChoice`). Every field's
/// `Default` (`None`/empty/`Any`/`false`) already matches
/// `ac3forge_ac4_presentation_choice_init()`'s own defaults, so unlike [`OutputConfig`] this
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
    fn with_raw<R>(&self, f: impl FnOnce(&sys::ac3forge_ac4_presentation_choice_t) -> R) -> R {
        // NUL bytes in a BCP 47 tag are not meaningful; drop them rather than fail outright -
        // this field cannot fail the call it feeds (ac3forge_ac4_presentation_choice_init()
        // treats NULL as "no language" too).
        let language = CString::new(self.language.replace('\0', "")).unwrap_or_default();
        let raw = sys::ac3forge_ac4_presentation_choice_t {
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

/// Mirrors `ac3forge_ac4_decoder_config_t` (`ac4::DecoderConfig`, less its syntax
/// trace - an internal diagnostic hook with no C surface, same omission as
/// `ac3forge_ac4_decoder_config_t` itself). Construct with [`DecoderConfig::default`] (which
/// calls the raw `ac3forge_ac4_decoder_config_init()`, same "never derive a default with a
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
        // SAFETY: ac3forge_ac4_decoder_config_init() unconditionally overwrites every field of
        // `raw`, output/presentation included (each via the matching _init() function - see
        // ac3forge.h's own comment on why every _config_init() must run first).
        unsafe { sys::ac3forge_ac4_decoder_config_init(&mut raw) };
        DecoderConfig {
            output: OutputConfig::from_raw(&raw.output),
            concealment: match raw.concealment {
                #[allow(non_upper_case_globals)]
                sys::ac3forge_ac4_concealment_policy_AC3FORGE_AC4_CONCEALMENT_REPEAT_FADE => {
                    ConcealmentPolicy::RepeatFade
                }
                #[allow(non_upper_case_globals)]
                sys::ac3forge_ac4_concealment_policy_AC3FORGE_AC4_CONCEALMENT_MUTE => {
                    ConcealmentPolicy::Mute
                }
                _ => ConcealmentPolicy::None,
            },
            // ac3forge_ac4_presentation_choice_init() gives every field the same value
            // PresentationChoice::default() already does (see its own doc comment) - no
            // pointer in raw.presentation to read back, so this skips converting it.
            presentation: PresentationChoice::default(),
            level: raw.level,
            decoding: match raw.decoding {
                #[allow(non_upper_case_globals)]
                sys::ac3forge_ac4_decoding_mode_AC3FORGE_AC4_DECODING_CORE => DecodingMode::Core,
                _ => DecodingMode::Full,
            },
        }
    }
}

impl DecoderConfig {
    fn with_raw<R>(&self, f: impl FnOnce(&sys::ac3forge_ac4_decoder_config_t) -> R) -> R {
        self.presentation.with_raw(|presentation| {
            let raw = sys::ac3forge_ac4_decoder_config_t {
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

/// An AC-4 decoder - `ac4::Decoder` via `ac3forge_ac4_decoder_t`.
pub struct Decoder {
    raw: ptr::NonNull<sys::ac3forge_ac4_decoder_t>,
}

unsafe impl Send for Decoder {}

impl Decoder {
    pub fn new(config: &DecoderConfig) -> Result<Self, Error> {
        config.with_raw(|raw_config| {
            let mut out: *mut sys::ac3forge_ac4_decoder_t = ptr::null_mut();
            // SAFETY: `raw_config` is fully initialized for the duration of this call; `out`
            // is a valid out-parameter.
            let status = unsafe { sys::ac3forge_ac4_decoder_create(raw_config, &mut out) };
            Error::check(status)?;
            let raw = ptr::NonNull::new(out)
                .expect("ac3forge_ac4_decoder_create returned OK with a null decoder");
            Ok(Decoder { raw })
        })
    }

    /// The output processing, from the next frame.
    pub fn set_output(&mut self, output: &OutputConfig) {
        let raw = output.to_raw();
        // SAFETY: `self.raw` is valid; `raw` lives for the duration of this call.
        unsafe { sys::ac3forge_ac4_decoder_set_output(self.raw.as_ptr(), &raw) };
    }

    /// The presentation choice, from the next frame.
    pub fn set_presentation(&mut self, choice: &PresentationChoice) {
        choice.with_raw(|raw| unsafe {
            sys::ac3forge_ac4_decoder_set_presentation(self.raw.as_ptr(), raw)
        });
    }

    /// Forgets everything carried between frames.
    pub fn reset(&mut self) {
        unsafe { sys::ac3forge_ac4_decoder_reset(self.raw.as_ptr()) };
    }

    /// The decoder's own added delay at the output rate, for the stream as last decoded; 0
    /// before a frame has decoded.
    pub fn latency_samples(&self) -> i32 {
        unsafe { sys::ac3forge_ac4_decoder_latency_samples(self.raw.as_ptr()) }
    }

    /// Why the last [`Decoder::decode`] call failed, returned `None`, or returned a
    /// concealed frame; empty after one that decoded normally.
    pub fn refusal_reason(&self) -> String {
        // SAFETY: ac3forge_ac4_decoder_refusal_reason() returns library-owned storage valid
        // for the process lifetime, always a valid NUL-terminated C string (never NULL - see
        // its own doc comment on normalizing the empty case).
        unsafe {
            CStr::from_ptr(sys::ac3forge_ac4_decoder_refusal_reason(self.raw.as_ptr()))
                .to_string_lossy()
                .into_owned()
        }
    }

    /// Reads one `raw_ac4_frame`. `None` means this frame has no output yet (its substreams
    /// need configuration no I-frame has sent) - not an error.
    pub fn decode(&mut self, frame: &[u8]) -> Result<Option<DecodedFrame>, Error> {
        let mut out: *mut sys::ac3forge_ac4_decoded_frame_t = ptr::null_mut();
        // SAFETY: `frame` is a valid slice for the duration of this call; `out` is a valid
        // out-parameter.
        let status = unsafe {
            sys::ac3forge_ac4_decoder_decode(
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
        let count = unsafe { sys::ac3forge_ac4_decoder_presentation_count(self.raw.as_ptr()) };
        (0..count)
            .map(|index| {
                // SAFETY: `index` is in `[0, count)`, so every accessor below reads a real
                // presentation rather than taking its null-safety fallback.
                unsafe {
                    let speaker_count = sys::ac3forge_ac4_decoder_presentation_speaker_count(
                        self.raw.as_ptr(),
                        index,
                    );
                    let speakers = (0..speaker_count)
                        .map(|s| {
                            Speaker::from_raw(sys::ac3forge_ac4_decoder_presentation_speaker(
                                self.raw.as_ptr(),
                                index,
                                s,
                            ))
                        })
                        .collect();
                    PresentationInfo {
                        toc_index: sys::ac3forge_ac4_decoder_presentation_toc_index(
                            self.raw.as_ptr(),
                            index,
                        ),
                        presentation_id: (sys::ac3forge_ac4_decoder_presentation_has_id(
                            self.raw.as_ptr(),
                            index,
                        ) != 0)
                            .then(|| {
                                sys::ac3forge_ac4_decoder_presentation_id(self.raw.as_ptr(), index)
                            }),
                        md_compat: (sys::ac3forge_ac4_decoder_presentation_has_md_compat(
                            self.raw.as_ptr(),
                            index,
                        ) != 0)
                            .then(|| {
                                sys::ac3forge_ac4_decoder_presentation_md_compat(
                                    self.raw.as_ptr(),
                                    index,
                                )
                            }),
                        enabled: sys::ac3forge_ac4_decoder_presentation_enabled(
                            self.raw.as_ptr(),
                            index,
                        ) != 0,
                        alternative: sys::ac3forge_ac4_decoder_presentation_alternative(
                            self.raw.as_ptr(),
                            index,
                        ) != 0,
                        pre_virtualized: sys::ac3forge_ac4_decoder_presentation_pre_virtualized(
                            self.raw.as_ptr(),
                            index,
                        ) != 0,
                        name: CStr::from_ptr(sys::ac3forge_ac4_decoder_presentation_name(
                            self.raw.as_ptr(),
                            index,
                        ))
                        .to_string_lossy()
                        .into_owned(),
                        language: CStr::from_ptr(sys::ac3forge_ac4_decoder_presentation_language(
                            self.raw.as_ptr(),
                            index,
                        ))
                        .to_string_lossy()
                        .into_owned(),
                        decodable: sys::ac3forge_ac4_decoder_presentation_decodable(
                            self.raw.as_ptr(),
                            index,
                        ) != 0,
                        selectable: sys::ac3forge_ac4_decoder_presentation_selectable(
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
    /// selected, as the frames read so far have sent it (`ac4::LoudnessInfo`'s "big four" -
    /// see `ac3forge_ac4_loudness_info_t`'s own comment on the DRC/dialogue-enhancement/
    /// downmix detail this omits).
    pub fn metadata_loudness(&self) -> LoudnessInfo {
        // SAFETY: `self.raw` is valid.
        let raw = unsafe { sys::ac3forge_ac4_decoder_metadata_loudness(self.raw.as_ptr()) };
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
        unsafe { sys::ac3forge_ac4_decoder_destroy(self.raw.as_ptr()) };
    }
}

/// `ac4::PresentationInfo`'s core surface (see [`Decoder::presentations`]'s own comment on
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

/// Mirrors `ac3forge_ac4_loudness_info_t` (`ac4::LoudnessInfo`'s "big four").
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct LoudnessInfo {
    pub dialnorm_dbfs: Option<f64>,
    pub integrated_lkfs: Option<f64>,
    pub true_peak_dbtp: Option<f64>,
    pub loudness_range_lu: Option<f64>,
}

/// Mirrors `ac3forge_ac4_object_properties_t` (`ac4::ObjectProperties`' scalar fields, Part 2
/// Annex F.2 to F.10) - what is in force at the frame's first sample. The within-frame
/// `ObjectUpdate` ramps are not exposed (same "reasonable cost" cut as the C API).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ObjectProperties {
    pub active: bool,
    pub gain_db: f64,
    pub priority: f64,
    pub position: [f64; 3],
    pub zone_mask: i32,
    pub enable_elevation: bool,
    pub snap: bool,
    pub width: [f64; 3],
    pub screen_factor: f64,
    pub depth_exponent: f64,
    pub distance: Option<f64>,
    pub divergence: f64,
    pub trim_disabled: bool,
    pub headphone_render_mode: Option<i32>,
    pub head_track_disabled: bool,
}

impl ObjectProperties {
    fn from_raw(raw: sys::ac3forge_ac4_object_properties_t) -> Self {
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

/// One decoded object of a [`DecodedFrame`] - `ac4::DecodedObject` (Part 2 clause 4.8.3.4).
#[derive(Debug, Clone, PartialEq)]
pub struct DecodedObject {
    pub kind: ObjectKind,
    pub lfe: bool,
    pub speaker: Option<Speaker>,
    /// `samples_per_channel()` long, at full scale 1.0.
    pub samples: Vec<f32>,
    /// What is in force at the frame's first sample.
    pub properties: ObjectProperties,
}

/// What a concealed frame's decode did - `ac4::Concealment`.
#[derive(Debug, Clone, PartialEq)]
pub struct Concealment {
    pub action: ConcealmentAction,
    /// Why the frame did not decode.
    pub error: Error,
}

/// One decoded AC-4 frame - `ac4::DecodedFrame` via `ac3forge_ac4_decoded_frame_t`. Owns its
/// PCM and object audio; every accessor borrows from `&self`.
pub struct DecodedFrame {
    raw: ptr::NonNull<sys::ac3forge_ac4_decoded_frame_t>,
}

unsafe impl Send for DecodedFrame {}

impl DecodedFrame {
    pub fn sample_rate_hz(&self) -> i32 {
        unsafe { sys::ac3forge_ac4_decoded_frame_sample_rate_hz(self.raw.as_ptr()) }
    }

    pub fn sequence_counter(&self) -> i32 {
        unsafe { sys::ac3forge_ac4_decoded_frame_sequence_counter(self.raw.as_ptr()) }
    }

    /// The presentation decoded: its index in the frame's table of contents.
    pub fn presentation_index(&self) -> usize {
        unsafe { sys::ac3forge_ac4_decoded_frame_presentation_index(self.raw.as_ptr()) }
    }

    pub fn presentation_id(&self) -> Option<i32> {
        unsafe {
            (sys::ac3forge_ac4_decoded_frame_has_presentation_id(self.raw.as_ptr()) != 0)
                .then(|| sys::ac3forge_ac4_decoded_frame_presentation_id(self.raw.as_ptr()))
        }
    }

    pub fn channel_count(&self) -> usize {
        unsafe { sys::ac3forge_ac4_decoded_frame_channel_count(self.raw.as_ptr()) }
    }

    /// AC-4's frame length varies by frame rate - unlike AC-3/E-AC-3 there is no fixed
    /// constant, so this is a real per-frame accessor.
    pub fn samples_per_channel(&self) -> usize {
        unsafe { sys::ac3forge_ac4_decoded_frame_samples_per_channel(self.raw.as_ptr()) }
    }

    /// `channel_index` in `[0, channel_count())`. Panics if out of range.
    pub fn channel_samples(&self, channel_index: usize) -> &[f32] {
        assert!(
            channel_index < self.channel_count(),
            "channel index out of range"
        );
        // SAFETY: the pointer is valid until `self` is destroyed (ac3forge.h's own
        // convention); samples_per_channel() gives the real length.
        unsafe {
            let ptr =
                sys::ac3forge_ac4_decoded_frame_channel_samples(self.raw.as_ptr(), channel_index);
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
            sys::ac3forge_ac4_decoded_frame_speaker(self.raw.as_ptr(), channel_index)
        })
    }

    /// Set only on a frame the decoder's [`ConcealmentPolicy`] made in place of one that
    /// did not decode.
    pub fn concealed(&self) -> Option<Concealment> {
        unsafe {
            (sys::ac3forge_ac4_decoded_frame_has_concealed(self.raw.as_ptr()) != 0).then(|| {
                Concealment {
                    action: ConcealmentAction::from_raw(
                        sys::ac3forge_ac4_decoded_frame_concealment_action(self.raw.as_ptr()),
                    ),
                    error: Error::from_status(sys::ac3forge_ac4_decoded_frame_concealment_error(
                        self.raw.as_ptr(),
                    ))
                    .unwrap_or(Error::Internal),
                }
            })
        }
    }

    /// A presentation with object audio: its objects, each substream's in turn. Empty for
    /// channel-based/channel-based-immersive content, which this crate's [`Encoder`] writes
    /// exclusively as of this version (A-JOC and direct-coded objects are a later phase).
    pub fn objects(&self) -> Vec<DecodedObject> {
        // SAFETY: `self.raw` is valid.
        let count = unsafe { sys::ac3forge_ac4_decoded_frame_object_count(self.raw.as_ptr()) };
        let samples_per_channel = self.samples_per_channel();
        (0..count)
            .map(|index| unsafe {
                let speaker =
                    (sys::ac3forge_ac4_decoded_frame_object_has_speaker(self.raw.as_ptr(), index)
                        != 0)
                        .then(|| {
                            Speaker::from_raw(sys::ac3forge_ac4_decoded_frame_object_speaker(
                                self.raw.as_ptr(),
                                index,
                            ))
                        });
                let ptr = sys::ac3forge_ac4_decoded_frame_object_samples(self.raw.as_ptr(), index);
                let samples = if ptr.is_null() {
                    Vec::new()
                } else {
                    std::slice::from_raw_parts(ptr, samples_per_channel).to_vec()
                };
                DecodedObject {
                    kind: ObjectKind::from_raw(sys::ac3forge_ac4_decoded_frame_object_kind(
                        self.raw.as_ptr(),
                        index,
                    )),
                    lfe: sys::ac3forge_ac4_decoded_frame_object_lfe(self.raw.as_ptr(), index) != 0,
                    speaker,
                    samples,
                    properties: ObjectProperties::from_raw(
                        sys::ac3forge_ac4_decoded_frame_object_properties(self.raw.as_ptr(), index),
                    ),
                }
            })
            .collect()
    }
}

impl Drop for DecodedFrame {
    fn drop(&mut self) {
        unsafe { sys::ac3forge_ac4_decoded_frame_destroy(self.raw.as_ptr()) };
    }
}

// --- encoder -------------------------------------------------------------

/// Mirrors `ac3forge_ac4_encoder_config_t` (`ac4::EncoderConfig`'s core surface - see this
/// module's own doc comment on what is deliberately left out).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct EncoderConfig {
    /// 1, 2, 5, 6, 9 or 10 - see `ac4::EncoderConfig::channels`'s own comment.
    pub channels: i32,
    /// 48000, or 44100 (`frame_rate_index` 13 only).
    pub sample_rate_hz: i32,
    /// Part 1 Table 83/84; default 13, the 2048-sample frame.
    pub frame_rate_index: i32,
    pub bitrate_kbps: i32,
    pub rate_mode: RateMode,
    pub codec_mode: CodecMode,
    pub iframe_interval: i32,
    pub dialnorm_db: f64,
}

impl EncoderConfig {
    fn to_raw(self) -> sys::ac3forge_ac4_encoder_config_t {
        sys::ac3forge_ac4_encoder_config_t {
            channels: self.channels,
            sample_rate_hz: self.sample_rate_hz,
            frame_rate_index: self.frame_rate_index,
            bitrate_kbps: self.bitrate_kbps,
            rate_mode: self.rate_mode.to_raw(),
            codec_mode: self.codec_mode.to_raw(),
            iframe_interval: self.iframe_interval,
            dialnorm_db: self.dialnorm_db,
        }
    }
}

impl Default for EncoderConfig {
    fn default() -> Self {
        let mut raw = unsafe { std::mem::zeroed() };
        // SAFETY: ac3forge_ac4_encoder_config_init() unconditionally overwrites every field of
        // `raw` - the one sanctioned way to obtain the real EncoderConfig{} defaults, per
        // ac3.rs's identical reasoning for its own EncoderConfig.
        unsafe { sys::ac3forge_ac4_encoder_config_init(&mut raw) };
        EncoderConfig {
            channels: raw.channels,
            sample_rate_hz: raw.sample_rate_hz,
            frame_rate_index: raw.frame_rate_index,
            bitrate_kbps: raw.bitrate_kbps,
            rate_mode: match raw.rate_mode {
                #[allow(non_upper_case_globals)]
                sys::ac3forge_ac4_rate_mode_AC3FORGE_AC4_RATE_AVERAGE => RateMode::Average,
                #[allow(non_upper_case_globals)]
                sys::ac3forge_ac4_rate_mode_AC3FORGE_AC4_RATE_VARIABLE => RateMode::Variable,
                _ => RateMode::Constant,
            },
            codec_mode: CodecMode::from_raw(raw.codec_mode),
            iframe_interval: raw.iframe_interval,
            dialnorm_db: raw.dialnorm_db,
        }
    }
}

/// One encoded AC-4 frame - `ac4::EncodedFrame` via `ac3forge_ac4_encoded_frame_t`. What an
/// MP4 sample holds as it is; [`sync_frame`] wraps it for a raw `.ac4` file or MPEG-2 TS.
pub struct EncodedFrame {
    raw: ptr::NonNull<sys::ac3forge_ac4_encoded_frame_t>,
}

unsafe impl Send for EncodedFrame {}
unsafe impl Sync for EncodedFrame {}

impl EncodedFrame {
    pub fn data(&self) -> &[u8] {
        unsafe {
            let ptr = sys::ac3forge_ac4_encoded_frame_data(self.raw.as_ptr());
            let size = sys::ac3forge_ac4_encoded_frame_size(self.raw.as_ptr());
            if ptr.is_null() || size == 0 {
                &[]
            } else {
                std::slice::from_raw_parts(ptr, size)
            }
        }
    }

    /// PCM samples per channel this frame decodes to, at the input's rate.
    pub fn samples(&self) -> i32 {
        unsafe { sys::ac3forge_ac4_encoded_frame_samples(self.raw.as_ptr()) }
    }

    pub fn iframe(&self) -> bool {
        unsafe { sys::ac3forge_ac4_encoded_frame_iframe(self.raw.as_ptr()) != 0 }
    }
}

impl Drop for EncodedFrame {
    fn drop(&mut self) {
        unsafe { sys::ac3forge_ac4_encoded_frame_destroy(self.raw.as_ptr()) };
    }
}

/// TS 103 190-2 Table E.1: the media time scale an ISOBMFF track of the stream counts in,
/// and each sample's duration in it - `ac4::MediaTiming`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MediaTiming {
    pub timescale: u32,
    pub sample_delta: u32,
}

/// An owned copy of an encoder's table of contents - `ac4::Toc` via `ac3forge_ac4_toc_t`,
/// for the dac4 box a container muxer needs.
pub struct Toc {
    raw: ptr::NonNull<sys::ac3forge_ac4_toc_t>,
}

unsafe impl Send for Toc {}

impl Toc {
    /// The 'dac4' box payload (`ac4_dsi_v1`, Annex E.6, box header excluded) - `ac4::build_dac4`.
    /// Empty where [`Toc::dac4_refusal`] names what this cannot describe whole.
    pub fn build_dac4(&self) -> Result<Bytes, Error> {
        let mut out: *mut sys::ac3forge_bytes_t = ptr::null_mut();
        let status = unsafe { sys::ac3forge_ac4_build_dac4(self.raw.as_ptr(), &mut out) };
        Error::check(status)?;
        Ok(unsafe { Bytes::from_raw(out) })
    }

    /// Why [`Toc::build_dac4`] wrote nothing; empty where it describes every presentation
    /// whole.
    pub fn dac4_refusal(&self) -> String {
        // SAFETY: always a valid NUL-terminated C string (never NULL - see
        // ac3forge_ac4_dac4_refusal()'s own doc comment on normalizing the empty case).
        unsafe {
            CStr::from_ptr(sys::ac3forge_ac4_dac4_refusal(self.raw.as_ptr()))
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
            sys::ac3forge_ac4_media_timing(self.raw.as_ptr(), &mut timescale, &mut sample_delta)
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
            unsafe { sys::ac3forge_ac4_samples_per_frame(self.raw.as_ptr(), &mut samples) };
        (has_value != 0).then_some(samples)
    }
}

impl Drop for Toc {
    fn drop(&mut self) {
        unsafe { sys::ac3forge_ac4_toc_destroy(self.raw.as_ptr()) };
    }
}

/// An AC-4 encoder - `ac4::Encoder` via `ac3forge_ac4_encoder_t`.
pub struct Encoder {
    raw: ptr::NonNull<sys::ac3forge_ac4_encoder_t>,
}

unsafe impl Send for Encoder {}

impl Encoder {
    /// Fails with [`Error::Ac4EncodeInvalidConfig`] for a configuration outside what the
    /// encoder writes, or whose rate cannot hold its least frame.
    pub fn new(config: &EncoderConfig) -> Result<Self, Error> {
        let raw_config = config.to_raw();
        let mut out: *mut sys::ac3forge_ac4_encoder_t = ptr::null_mut();
        let status = unsafe { sys::ac3forge_ac4_encoder_create(&raw_config, &mut out) };
        Error::check(status)?;
        let raw = ptr::NonNull::new(out)
            .expect("ac3forge_ac4_encoder_create returned OK with a null encoder");
        Ok(Encoder { raw })
    }

    /// The codec mode the stream is actually coded in - never [`CodecMode::Auto`].
    pub fn codec_mode(&self) -> CodecMode {
        CodecMode::from_raw(unsafe { sys::ac3forge_ac4_encoder_codec_mode(self.raw.as_ptr()) })
    }

    /// Samples of silence the encoder puts before the input, at the input's rate.
    pub fn delay_samples(&self) -> i32 {
        unsafe { sys::ac3forge_ac4_encoder_delay_samples(self.raw.as_ptr()) }
    }

    /// The delay a matching [`Decoder`] adds on top, at the input's rate.
    pub fn decoder_delay_samples(&self) -> i32 {
        unsafe { sys::ac3forge_ac4_encoder_decoder_delay_samples(self.raw.as_ptr()) }
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

    /// Ends the stream: pads the input with silence to the end of its last frame and returns
    /// the frames the delay still held. The encoder takes no input after it.
    pub fn flush(&mut self) -> Result<Vec<EncodedFrame>, Error> {
        let mut out: *mut *mut sys::ac3forge_ac4_encoded_frame_t = ptr::null_mut();
        let mut count: usize = 0;
        // SAFETY: `out`/`count` are valid out-parameters.
        let status =
            unsafe { sys::ac3forge_ac4_encoder_flush(self.raw.as_ptr(), &mut out, &mut count) };
        Error::check(status)?;
        Ok(Self::collect_frames(out, count))
    }

    /// The table of contents every frame carries, as it stands after the frames encoded so
    /// far.
    pub fn toc(&self) -> Result<Toc, Error> {
        let mut out: *mut sys::ac3forge_ac4_toc_t = ptr::null_mut();
        // SAFETY: `self.raw` is valid; `out` is a valid out-parameter.
        let status = unsafe { sys::ac3forge_ac4_encoder_toc(self.raw.as_ptr(), &mut out) };
        Error::check(status)?;
        let raw =
            ptr::NonNull::new(out).expect("ac3forge_ac4_encoder_toc returned OK with a null toc");
        Ok(Toc { raw })
    }

    fn encode_raw(
        &mut self,
        pointers: &[*const f32],
        samples_per_channel: usize,
    ) -> Result<Vec<EncodedFrame>, Error> {
        let mut out: *mut *mut sys::ac3forge_ac4_encoded_frame_t = ptr::null_mut();
        let mut count: usize = 0;
        // SAFETY: `pointers` holds one valid pointer per channel, each to
        // `samples_per_channel` live f32s for the duration of this call; `out`/`count` are
        // valid out-parameters.
        let status = unsafe {
            sys::ac3forge_ac4_encoder_encode(
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
        out: *mut *mut sys::ac3forge_ac4_encoded_frame_t,
        count: usize,
    ) -> Vec<EncodedFrame> {
        if out.is_null() || count == 0 {
            return Vec::new();
        }
        // SAFETY: AC3FORGE_OK with a non-NULL array guarantees `count` valid, exclusively-
        // owned handles - ac3forge_ac4_encoder_encode()/_flush()'s own out-parameter contract.
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
            // ac3forge_ac4_encoded_frame_array_destroy() does, since that would double-free
            // the elements `frames` now owns. ac3forge.h documents this split for exactly
            // this reason (see ac3forge_decoded_substream_array_destroy()'s own comment on
            // taking every handle first and passing count 0).
            sys::ac3forge_ac4_encoded_frame_array_destroy(out, 0);
            frames
        };
        frames
    }
}

impl Drop for Encoder {
    fn drop(&mut self) {
        unsafe { sys::ac3forge_ac4_encoder_destroy(self.raw.as_ptr()) };
    }
}

/// Part 2 Annex G.3.1's `ac4_syncframe()`: the sync word 0xAC40, or 0xAC41 and a trailing
/// `crc_word` when `crc` is set, then `frame_size` and `raw_frame`.
pub fn sync_frame(raw_frame: &[u8], crc: bool) -> Result<Bytes, Error> {
    let mut out: *mut sys::ac3forge_bytes_t = ptr::null_mut();
    // SAFETY: `raw_frame` is a valid slice for the duration of this call; `out` is a valid
    // out-parameter.
    let status = unsafe {
        sys::ac3forge_ac4_sync_frame(raw_frame.as_ptr(), raw_frame.len(), crc as i32, &mut out)
    };
    Error::check(status)?;
    Ok(unsafe { Bytes::from_raw(out) })
}
