#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "ac4/ac4.hpp"
#include "ac4/syntax.hpp"
#include "ac4dec/export.hpp"

// An AC-4 decoder: ETSI TS 103 190-1 V1.4.1 (2025-07), "Part 1: Channel based
// coding", and ETSI TS 103 190-2 V1.3.1 (2025-07), "Part 2: Immersive and
// personalized audio", written from the published texts. Clause numbers
// below name the part that defines the element; Part 2 clause 6 amends Part 1
// clause 4 for bitstream_version 2, which is what every stream this project
// has seen uses.
//
// What this version does: it reads every syntax element of a raw AC-4
// frame's substreams - the presentation substream, channel-coded audio
// substreams in the Part 1 channel elements (their HSF extension substreams,
// ac4_hsf_ext_substream(), included), and EMDF payload substreams - and
// reports what a frame carries. It decodes to PCM the mono, stereo, 3.0, 5.X
// and 7.X channel elements in every codec mode Part 1 gives them (SIMPLE,
// ASPX and the A-CPL modes), at every frame rate of Part 1 Tables 83 and 84:
// the audio spectral frontend, stereo and multichannel processing, the
// inverse transform with block switching, frame alignment, the QMF domain's
// companding, A-SPX and A-CPL (Part 1 clauses 5.1, 5.3, 5.5, 5.6 and 5.7),
// and at every frame_rate_index but 13 the sample rate converter from the
// internal rate to 48 kHz (clause 6.2.15), its phase locked to
// sequence_counter (Part 2 clause 5.11). It decodes the presentation a
// system chooses (Part 2 clause 4.8.2) with all its substreams: music and
// effects with dialogue, main audio with associated audio, both, and a main
// substream with the dialogue enhancement substream the hybrid dialogue
// enhancement methods take (Part 1 clauses 5.7.8.9 and 6.2.16, Part 2
// clauses 4.8.3.17 to 4.8.4). The table of contents and the substream framing
// come from ac4::parse_raw_frame (the inspector, src/ac4); this library starts
// where the inspector stops.
//
// What it refuses, with DecodeError::kUnsupported and a reason: the speech
// spectral frontend (Part 1 clause 5.2), immersive and 22.2 channel elements,
// object substreams, a 96/192 kHz substream whose HSF extension substream
// could not be resolved and read alongside it, and a substream no element of
// the table of contents this decoder reads names (an HSF extension substream
// no ac4_hsf_ext_substream_info() names among them). Refusing is per
// substream and per frame; the next frame is attempted afresh. decode()
// refuses, the same way, everything above that it does not turn into PCM
// yet: 96/192 kHz.
//
// ERRATA.md beside this library records where the two standards are
// ambiguous or defective and the reading taken for each.

namespace ac4 {

enum class DecodeError : std::uint8_t {
    kTruncated,        // a syntax element ran past the end of its substream
    kInvalidToc,       // ac4::parse_raw_frame refused the table of contents
    kInvalidStream,    // a value the syntax cannot follow (a reserved code, an impossible count)
    kUnsupported,      // legal AC-4 this decoder does not read yet - see the header comment
    kMissingIFrame,    // a non-I-frame that needs configuration no I-frame has supplied
};

[[nodiscard]] AC4DEC_EXPORT std::string_view describe(DecodeError error);

// --- The syntax trace -------------------------------------------------------
//
// One record per syntax element read, in bitstream order, for tests and for
// diagnosing a stream: ac4::SyntaxRecord and ac4::SyntaxTrace, in
// ac4/syntax.hpp, whose comment states what a record holds. The encoder writes
// records of the same shape, and so does tools/references/ac4_syntax.py.

// --- Output processing -------------------------------------------------------
//
// What decode() does to the decoded channels as a system configures it:
// dialogue enhancement (Part 1 clause 5.7.8), then the output level and
// dynamic range control (5.7.9), then the downmix (6.2.17).

// The layout decode() renders the decoded channels to (Part 1 clause 6.2.17).
enum class DownmixTarget : std::uint8_t {
    kAsCoded,  // the channels as coded
    k5X,       // a 7.X element's channels folded to 5.X (Table 219)
    // Two channels, Lo/Ro or Lt/Rt as the stream's preferred_dmx_method says,
    // Lo/Ro where it says neither.
    kStereo,
    kLoRo,
    kLtRt,  // in its Pro Logic II form where the stream prefers that
    kMono,  // L + R of the stereo downmix
};

[[nodiscard]] AC4DEC_EXPORT std::string_view describe(DownmixTarget target);

// Part 1 Table 161's DRC decoder modes, and how decode() chooses one.
enum class DrcMode : std::uint8_t {
    kOff,                 // no compression: the output level gain alone
    kDefault,             // the mode clause 5.7.9.2 selects for the output level
    kHomeTheatre,         // decoder mode 0
    kFlatPanelTv,         // 1
    kPortableSpeakers,    // 2
    kPortableHeadphones,  // 3
};

[[nodiscard]] AC4DEC_EXPORT std::string_view describe(DrcMode mode);

// The controls of planning/ac4.md's "One control for both formats" that act on
// the decoded channels. Decoder::set_output() changes them from the next frame.
// A later version adds the immersive output layouts (Part 2 clause 5.10.2) as
// fields after these. Every field has a default, so a designated initializer
// names only the fields it sets; the same holds for PresentationChoice and
// DecoderConfig.
struct OutputConfig {
    // Lout of Part 1 clause 5.7.9.3.3, in dBFS: the level the stream's
    // dialnorm is taken to, by 2^((Lout - dialnorm) / 6), which cuts or
    // boosts. Part 1 gives no default, the system supplies it; unset leaves the
    // stream at its coded level and compresses nothing.
    std::optional<double> output_level_dbfs{};
    // With an output level: the mode that compresses. A mode the stream does
    // not configure compresses nothing.
    DrcMode drc = DrcMode::kDefault;
    // Where kDefault's output level falls in the portable modes' range (-16 to
    // 0 dBFS), whether it takes portable headphones or portable speakers.
    bool headphones = false;
    // G_DE of Part 1 clause 5.7.8, in dB: how far the dialogue is raised where
    // the stream sends dialogue enhancement parameters, up to the stream's cap
    // of 3, 6, 9 or 12 dB. 0 leaves the output as the tool bypassed would.
    double dialogue_enhancement_db = 0.0;
    // The layout the channels come out in; a stream narrower than the target
    // comes out as coded, except mono, which a two-channel target takes to
    // both channels.
    DownmixTarget downmix = DownmixTarget::kAsCoded;
    // Whether a two-channel or mono downmix takes the LFE, at the stream's
    // lfe_mixgain, as Part 1 does; off drops it, outside the text.
    bool mix_lfe = true;
    // g_dialog of Part 1 clause 6.2.16.1, in dB: the level of a presentation's
    // dialogue substreams against its music and effects, up to the
    // g_dialog_max the stream allows (0 dB where it sends none). Below -120
    // dB the dialogue is silent.
    double dialogue_gain_db = 0.0;
    // g_assoc of Part 1 clause 6.2.16.2, in dB, 0 or less: the level of a
    // presentation's associated audio. Below -120 dB it is silent.
    double associated_gain_db = 0.0;
};

// --- Presentations -----------------------------------------------------------
//
// Which presentation decode() decodes, when a stream carries several (Part 2
// clause 4.8.2): of those it can decode, of a presentation_version it decodes,
// carrying audio, whose md_compat is within the decoder's level and which the
// stream has not disabled, the one a system asks for by presentation_id or by
// position, or else the one that best meets its preferences, in the order the
// clause lists them, the first in the table of contents among equals. Where
// the table of contents changes from one frame to the next, the choice is made
// again. src/ac4dec/ERRATA.md ("Which presentations can be selected" and "The
// order of the preferences") records the readings.

// Part 1 Table 92's refinements of associated audio, which an associated
// substream's language_tag_bytes carry in place of a language.
enum class AssociatedType : std::uint8_t {
    kAny,                        // whatever the content_classifier says
    kAudioDescription,           // qad, or qax premixed
    kAudioDescriptionSubtitles,  // audio description with spoken subtitles: qas, or qtx premixed
    kSpokenSubtitles,            // qss, or qsx premixed
    kEmergencyInformation,       // qei, or qex premixed
};

struct PresentationChoice {
    // The presentation carrying this presentation_id (Part 2 clause
    // 6.3.2.2.4a); where no presentation that can be selected carries it, the
    // rest decides.
    std::optional<int> presentation_id{};
    // Else the presentation at this position of the table of contents, which
    // the text warns can change over time.
    std::optional<std::size_t> index{};
    // Else the preferences. The language of the main or dialogue audio: an
    // IETF BCP 47 tag, a presentation's tag matching it whole before one whose
    // primary subtag matches; empty for none.
    std::string language{};
    // The associated audio: Part 1 Table 91's content_classifier of the
    // service a presentation should carry (0b010 visually impaired, 0b011
    // hearing impaired, 0b101 commentary, and so on), with Table 92's
    // refinement of it; unset for a presentation without associated audio.
    std::optional<int> associated{};
    AssociatedType associated_type = AssociatedType::kAny;
    // The kind of audio: a presentation rendered for headphones before it was
    // encoded (b_pre_virtualized, Part 1 clause 4.3.3.3.5) before one that was
    // not, or the other way round.
    bool headphones = false;
};

// The presentation decode() selects from `toc` for `choice` at compatibility
// level `level` (md_compat, Part 1 Table 86 and Part 2 Table 55): its index in
// Toc::presentations_v1, or in presentations_v0 below bitstream_version 2;
// nothing when no presentation can be selected.
[[nodiscard]] AC4DEC_EXPORT std::optional<std::size_t> select_presentation(const Toc& toc,
                                                                           const PresentationChoice& choice,
                                                                           int level);

// --- Concealment ---------------------------------------------------------------
//
// What decode() does with a frame that will not decode, the policies forge's
// AC-3 and E-AC-3 decoders offer. kNone, the default, returns the error; the
// others return a frame's worth of audio instead, made by the decoder's own
// inverse transform and output stages, so the overlap with the frames either
// side stays continuous. The QMF-domain tools (A-SPX, A-CPL) pass a concealed
// frame through, and the frame after it resumes them. A frame that fails before
// any frame has decoded still returns its error: there is nothing to conceal
// from.
//
// After a change of source, the frames that wait for the new source's first
// I-frame are concealed the same way, from the old source's last frame, where
// without a policy they return nothing.
enum class ConcealmentPolicy : std::uint8_t {
    kNone,
    // The last good frame again, fading at the rate forge's decoders fade a
    // repeat, 20 dB for each 32 ms lost in a row: each concealed frame at the
    // level the fade reaches at its end.
    kRepeatFade,
    // Silence, the last good frame's overlap playing out through it.
    kMute,
};

// What a concealed frame's decode() did, on the frame.
enum class ConcealmentAction : std::uint8_t {
    kRepeatFade,
    kMute,
};

struct Concealment {
    DecodeError error = DecodeError::kInvalidStream;  // why the frame did not decode
    ConcealmentAction action = ConcealmentAction::kMute;
};

// A decoder's configuration. Decoder::set_output() and set_presentation()
// change the two halves a system changes while a stream plays; the rest is
// fixed for the decoder. A later version adds full or core decoding (Part 2
// clause 4.7) as a field after these.
struct DecoderConfig {
    // One record per syntax element read. The configuration owns a copy of the
    // callable, and the decoder one of its own (ac4/syntax.hpp); empty, the
    // default, costs one branch per syntax element.
    SyntaxTrace syntax{};
    OutputConfig output{};
    ConcealmentPolicy concealment = ConcealmentPolicy::kNone;
    // Which presentation decode() decodes (select_presentation()).
    PresentationChoice presentation{};
    // The md_compat level the decoder claims: presentations above it are not
    // selected (Part 2 clause 6.3.2.2.3).
    int level = 3;
};

// What one substream of a frame turned out to be.
struct SubstreamReport {
    enum class Kind : std::uint8_t { kAudio, kPresentation, kEmdfPayloads, kHsfExt, kOther };
    int index = 0;
    Kind kind = Kind::kOther;
    std::size_t size_bits = 0;           // the substream's size in substream_index_table(), in bits
    std::size_t bits_read = 0;           // bits the syntax consumed, alignment included
    std::optional<DecodeError> refused;  // set when this substream was not read to its end
    std::string_view refused_reason;
};

// Every substream of the frame's substream_index_table(), in index order.
struct FrameReport {
    int sequence_counter = 0;
    bool b_iframe_global = false;
    std::vector<SubstreamReport> substreams;
};

// --- Decoding to PCM ---------------------------------------------------------
//
// Where a decoded channel is meant to be heard, by Part 1 clause D.1's names:
// those of the channel modes of Part 1 Table 88. Later versions add the
// immersive layouts'.
enum class Speaker : std::uint8_t {
    kLeft,
    kRight,
    kCentre,
    kLfe,            // Low-Frequency Effects
    kLeftSurround,   // Left Side/Surround, Ls: a side speaker in the 7.X modes
    kRightSurround,  // Right Side/Surround, Rs
    kLeftBack,       // Lb, in 7.X 3/4/0
    kRightBack,      // Rb
    kLeftWide,       // Lw, in 7.X 5/2/0
    kRightWide,      // Rw
    kTopFrontLeft,   // Tfl, in 7.X 3/2/2
    kTopFrontRight,  // Tfr
};

[[nodiscard]] AC4DEC_EXPORT std::string_view describe(Speaker speaker);

// One frame of output.
struct DecodedFrame {
    int sample_rate_hz = 0;
    // Of the frame this came from; for a concealed frame whose table of
    // contents did not read, the counter the stream expected.
    int sequence_counter = 0;
    // The presentation decoded: its index in the frame's table of contents
    // (select_presentation()) and presentation_id where it carries one; for a
    // concealed frame, the last one decoded.
    std::size_t presentation = 0;
    std::optional<int> presentation_id;
    // One per channel, in the order of `channels`: L, R, C, the LFE, Ls, Rs,
    // then a 7.X mode's last pair, each where the channel mode has it.
    std::vector<Speaker> speakers;
    // Planar PCM, one vector per channel, all `samples` long, at full scale
    // 1.0: a frame's worth, which at 29.97, 59.94 and 119.88 fps alternates by
    // a sample in the sequence Part 2 Table 47 locks to sequence_counter (1 601
    // or 1 602 at 29.97). The decoder's delay is applied: Part 1's frame
    // alignment (clause 5.6), the QMF banks and the QMF domain's history
    // (5.7.1), 1 313 samples at frame_rate_index 13 in every codec mode, and at
    // the other indices the sample rate converter's too (latency_samples()).
    std::vector<std::vector<float>> channels;
    std::size_t samples = 0;
    // Set only on a frame DecoderConfig::concealment made in place of one that
    // did not decode.
    std::optional<Concealment> concealed;
};

// --- Decoding by block ---------------------------------------------------------
//
// decode_by_block() hands the output over in blocks of kBlockSamples samples,
// the size ac3::forge's decoders hand over, whatever a frame's length: a frame
// of 2 002 samples at 23.976 fps gives seven blocks and holds 210 samples back
// for the next. flush() hands over what is held back.

inline constexpr std::size_t kBlockSamples = 256;

struct PcmBlock {
    // One span per channel, in the order of `speakers`, each `samples` long:
    // kBlockSamples, and fewer only in the block flush() hands over. Valid for
    // the duration of the sink's call.
    std::span<const std::span<const float>> channels;
    std::span<const Speaker> speakers;
    std::size_t samples = kBlockSamples;
    int sample_rate_hz = 0;
    // The position of the block's first sample in the decoder's output, since
    // it was built or reset.
    std::uint64_t position = 0;
    // Whether any of the block's samples came from a concealed frame.
    bool concealed = false;
};

// A non-owning reference to any callable taking a const PcmBlock&, in the shape
// of ac3::BlockSink: no allocation, and the callable must outlive the call it
// is handed to, which a lambda written in the call's arguments does.
class BlockSink {
   public:
    template <typename F>
        requires std::invocable<F&, const PcmBlock&> &&
                     (!std::same_as<std::remove_cvref_t<F>, BlockSink>)
    // NOLINTNEXTLINE(google-explicit-constructor): the call site is the point
    BlockSink(F&& f) noexcept
        : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          call_([](void* object, const PcmBlock& block) {
              (*static_cast<std::remove_reference_t<F>*>(object))(block);
          }) {}

    void operator()(const PcmBlock& block) const { call_(object_, block); }

   private:
    void* object_;
    void (*call_)(void*, const PcmBlock&);
};

// What decode_by_block() decoded: a DecodedFrame's description without its
// samples, which went to the sink.
struct FrameInfo {
    int sample_rate_hz = 0;
    int sequence_counter = 0;
    std::size_t presentation = 0;
    std::optional<int> presentation_id;
    // The frame's channels, valid until the next call on the decoder.
    std::span<const Speaker> speakers;
    std::size_t samples = 0;  // the frame's, as DecodedFrame::samples
    std::size_t blocks = 0;   // the blocks this call handed over
    std::optional<Concealment> concealed;
};

// --- What the decoder reports of a stream ---------------------------------------
//
// planning/ac4.md's "Media information": each presentation of the table of
// contents, and the metadata of the one decode() selects, as the frames read
// so far have sent it. Values a stream sends only in I-frames are kept until a
// change of source.

// What one substream is to a presentation (Part 2 clause 4.8.3.2, and the
// dialogue enhancement substream of Part 2 Table 53 and Part 1 Table 85).
enum class SubstreamRole : std::uint8_t {
    kMain,
    kMusicAndEffects,
    kDialogue,
    kDialogueEnhancement,
    kAssociated,
};

[[nodiscard]] AC4DEC_EXPORT std::string_view describe(SubstreamRole role);

struct PresentationMember {
    int substream = 0;  // substream_index: the first of a frame-rate-multiplied series
    SubstreamRole role = SubstreamRole::kMain;
    int group = -1;  // its substream group (version 1), -1 for a version 0 presentation
    std::optional<int> content_classifier;  // Part 1 Table 91
    // language_tag_bytes: a BCP 47 tag, or for associated audio a Part 1
    // Table 92 code; empty without one.
    std::string language;
    // Its channel mode's channels; empty for a substream this decoder does
    // not turn into PCM.
    std::vector<Speaker> speakers;
};

struct PresentationInfo {
    // In Toc::presentations_v1, or presentations_v0 below bitstream_version 2.
    std::size_t index = 0;
    std::optional<int> presentation_id;
    int presentation_version = 0;
    // Part 2 Table 53 (Part 1 Table 85); unset for a single substream group.
    std::optional<int> presentation_config;
    // The level it needs: Part 2 Table 55, Part 1 Table 86.
    std::optional<int> md_compat;
    bool enabled = true;           // enable_presentation, version 1
    bool alternative = false;      // b_alternative, version 1
    bool pre_virtualized = false;  // b_pre_virtualized
    // presentation_name, for an alternative presentation, once the decoder has
    // it whole (Part 2 clause 6.3.3.1.4, a name in chunks over several frames
    // included); empty until then, and without one.
    std::string name;
    // Its dialogue substream's language, else its main or music and effects
    // substream's (Part 1 clause 4.3.3.8.8), as selection compares it.
    std::string language;
    // The channels decode() puts out as coded: its main or music and effects
    // substream's, which the others are mixed into.
    std::vector<Speaker> speakers;
    std::vector<int> substream_groups;  // ac4_sgi_specifier()'s group_index values, version 1
    std::vector<PresentationMember> members;
    // Whether this decoder turns every substream of it into PCM, and whether
    // select_presentation() may choose it at the decoder's level.
    bool decodable = false;
    bool selectable = false;
};

// Part 1 clause 4.3.12's loudness values as a stream sends them, in dB (LKFS,
// LUFS, dBTP) and LU. Each is set where the stream sends it.
struct LoudnessInfo {
    // dialnorm (Part 1 clause 4.3.12.2.1): the dialogue level the output
    // level is taken from, 0 to -31.75 dBFS.
    std::optional<double> dialnorm_dbfs;
    // further_loudness_info() (Part 1 clause 4.3.12.3, Part 2 clause 6.3.8.2):
    // loud_prac_type (Table 156), the dialogue gating a correction used
    // (dialgate_prac_type, Table 157) and whether it ran in real time
    // (b_loudcorr_type).
    std::optional<int> practice;
    std::optional<int> correction_gating;
    bool corrected_in_real_time = false;
    std::optional<double> integrated_lkfs;       // loudrelgat
    std::optional<double> speech_gated_lkfs;     // loudspchgat
    std::optional<int> speech_gating;            // its dialgate_prac_type
    std::optional<double> short_term_lufs;       // loudstrm3s
    std::optional<double> max_short_term_lufs;   // max_loudstrm3s
    std::optional<double> true_peak_dbtp;        // truepk
    std::optional<double> max_true_peak_dbtp;    // max_truepk
    std::optional<double> loudness_range_lu;     // lra
    std::optional<int> loudness_range_practice;  // lra_prac_type
    std::optional<double> momentary_lufs;        // loudmntry
    std::optional<double> max_momentary_lufs;    // max_loudmntry
};

// One DRC decoder mode a stream carries (Part 1 clause 4.3.13.3).
struct DrcModeInfo {
    // Table 161's drc_decoder_mode_id: 0 home theatre, 1 flat panel TV, 2
    // portable speakers, 3 portable headphones; 4 to 7 the output levels from
    // `output_level_from_db` down to `output_level_to_db`.
    int id = 0;
    std::optional<int> output_level_from_db;
    std::optional<int> output_level_to_db;
    enum class Compression : std::uint8_t {
        kDefaultProfile,  // drc_default_profile_flag: the stream's drc_eac3_profile
        kCurve,           // a compression curve of its own (Table 166)
        kGains,           // gains the stream transmits (drc_compression_curve_flag 0)
    };
    Compression compression = Compression::kDefaultProfile;
    std::optional<int> repeat_of;     // drc_repeat_profile_flag's drc_repeat_id
    std::optional<int> gains_config;  // Table 163, with transmitted gains
};

struct DrcInfo {
    int eac3_profile = 0;            // drc_eac3_profile, Table 160
    std::vector<DrcModeInfo> modes;  // in the order the stream sends them
    // The mode decode() compresses with at the output level and DrcMode set
    // (clause 5.7.9.2); nothing where it compresses nothing.
    std::optional<int> applied_mode;
};

// Dialogue enhancement's configuration (Part 1 clause 4.3.14).
struct DialogueEnhancementInfo {
    // de_method, Table 170: 0 channel independent, 1 cross-channel, 2 and 3 the
    // hybrid methods, whose waveform a dialogue enhancement substream carries.
    int method = 0;
    // Which of L, R and C the parameters are for (de_channel_config, Table 171).
    bool left = false;
    bool right = false;
    bool centre = false;
    double max_gain_db = 0.0;  // de_max_gain: the cap on G_DE, 3, 6, 9 or 12 dB
};

// The stereo downmix's values (Part 1 clauses 4.3.12.2.8 to 4.3.12.2.19, Part 2
// clauses 6.2.9.1 and 6.2.9.2), gains in dB; -infinity for a gain of 0.
struct DownmixInfo {
    double loro_centre_db = -3.0;
    double loro_surround_db = -3.0;
    // Lt/Rt's, which are the Lo/Ro values where b_ltrt_mixinfo is 0.
    double ltrt_centre_db = -3.0;
    double ltrt_surround_db = -3.0;
    std::optional<double> lfe_db;  // lfe_mg, 5.5 - lfe_mixgain dB
    // preferred_dmx_method, Table 150.
    enum class Preferred : std::uint8_t { kNotIndicated, kLoRo, kLtRt, kLtRtProLogicII };
    Preferred preferred = Preferred::kNotIndicated;
    // The loudness corrections, in dB2 (6 dB2 a factor of 2).
    std::optional<double> loro_correction_db2;
    std::optional<double> ltrt_correction_db2;
};

// The metadata of the presentation decode() selected, for display: what the
// frames read so far have sent, each part unset until one has.
struct PresentationMetadata {
    // The presentation this describes; unset before one is selected.
    std::optional<std::size_t> presentation;
    LoudnessInfo loudness;
    std::optional<DrcInfo> drc;
    std::optional<DialogueEnhancementInfo> dialogue_enhancement;
    std::optional<DownmixInfo> downmix;
};

// One decoder per stream: configuration sent only in I-frames (A-SPX, A-CPL,
// DRC, dialogue enhancement) persists from one frame to the next, until a
// sequence_counter that does not continue the stream marks a change of
// source (Part 1 clause 4.3.3.2.2), which forgets it, so that frames wait for
// the new source's first I-frame. decode()'s signal carries on across the
// change: the old source's audio still in the decoder comes out to its end,
// overlapping the new source's first frame, which makes a splice or a switch
// of streams at an I-frame seamless (Part 1 clause 6.2.19). A frame that
// returns nothing while it waits drops that signal, so that the first frame
// decoded after the wait starts from silence. A frame whose table of contents
// does not read is taken to be the frame the stream expected, so one damaged
// frame is not a change of source.
//
// A system changes the output processing and the presentation while a stream
// plays with set_output() and set_presentation(), which keep everything the
// decoder has read: a new decoder waits for an I-frame.
class AC4DEC_EXPORT Decoder {
   public:
    Decoder();
    explicit Decoder(const DecoderConfig& config);
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Reads one raw_ac4_frame - an ac4::SyncFrame's raw_ac4_frame, or an MP4
    // sample. An error in one substream is recorded in that substream's
    // report and the others are still read; the frame itself fails only
    // when its table of contents does. Updates presentations() and
    // metadata() as decode() does.
    [[nodiscard]] std::expected<FrameReport, DecodeError> parse(
        std::span<const std::byte> raw_ac4_frame);

    // Reads one raw_ac4_frame as parse() does and decodes the presentation
    // select_presentation() gives for DecoderConfig::presentation: each of its
    // substreams, mixed into the channels of its main or music and effects
    // substream. Nothing for a frame that has no output: one whose substreams
    // need configuration no I-frame has sent yet. The error, when there is
    // one, is a substream's (or the table of contents'), and refusal_reason()
    // says why. Under a concealment policy, a concealed frame in place of
    // either, once a frame has decoded.
    [[nodiscard]] std::expected<std::optional<DecodedFrame>, DecodeError> decode(
        std::span<const std::byte> raw_ac4_frame);

    // decode(), with the output handed to `sink` in blocks of kBlockSamples as
    // it completes them, the samples left over held for the next frame. The
    // decoder keeps the frame's storage, so a stream decoded this way
    // allocates nothing per frame once its layout is set. A change of layout
    // or rate first hands over what is held, as a shorter block.
    [[nodiscard]] std::expected<std::optional<FrameInfo>, DecodeError> decode_by_block(
        std::span<const std::byte> raw_ac4_frame, BlockSink sink);

    // The samples decode_by_block() holds back, handed to `sink` as one
    // shorter block, at the end of a stream; returns how many there were.
    std::size_t flush(BlockSink sink);

    // Why the last decode() failed, returned nothing or returned a concealed
    // frame, a string literal; empty after a decode() that decoded its frame.
    [[nodiscard]] std::string_view refusal_reason() const noexcept;

    // The output processing, from the next frame. The stages take the new
    // values as they take the stream's own from one frame to the next; a new
    // layout starts its channels' synthesis from silence.
    void set_output(const OutputConfig& output);
    [[nodiscard]] const OutputConfig& output() const noexcept;

    // The presentation choice, from the next frame. A presentation's
    // substreams are read in every frame whichever is decoded, so a newly
    // chosen one needs no I-frame; its signal starts from silence.
    void set_presentation(const PresentationChoice& choice);

    // The presentations of the last frame read, in its table of contents'
    // order; empty before one.
    [[nodiscard]] std::span<const PresentationInfo> presentations() const;

    // The metadata of the presentation the last frame selected.
    [[nodiscard]] const PresentationMetadata& metadata() const;

    // The decoder's delay at the output rate for the stream as last decoded:
    // 1 313 samples at frame_rate_index 13, and at the other indices the same
    // at the internal rate and the converter's delay, to the nearest sample; 0
    // before a frame has decoded. decode_by_block() holds back up to
    // kBlockSamples - 1 samples more.
    [[nodiscard]] int latency_samples() const noexcept;

    // Forgets everything carried between frames, the samples decode_by_block()
    // holds back included.
    void reset();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac4
