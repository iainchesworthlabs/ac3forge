#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
// sequence_counter (Part 2 clause 5.11). It decodes the immersive element of
// the 7.X.4 channel modes (Part 2 clause 6.2.4) in every codec mode, in full or
// core decoding (DecodingMode), with Part 2's stereo and multichannel
// processing, S-CPL, A-SPX, A-CPL and A-JCC (clauses 5.2 to 5.6), and renders
// it by Part 2's channel renderer (clause 5.10.2, DownmixTarget). It decodes
// the presentation a system chooses (Part 2 clause 4.8.2) with all its
// substreams: music and effects with dialogue, main audio with associated
// audio, both, and a main substream with the dialogue enhancement substream
// the hybrid dialogue enhancement methods take (Part 1 clauses 5.7.8.9 and
// 6.2.16, Part 2 clauses 4.8.3.17 to 4.8.4). The table of contents and the
// substream framing come from ac4::parse_raw_frame (the inspector, src/ac4);
// this library starts where the inspector stops.
//
// What it refuses, with DecodeError::kUnsupported and a reason: the speech
// spectral frontend (Part 1 clause 5.2), the 9.X.4 channel modes (Part 2's
// immersive element with b_5fronts) and the 22.2 channel element, object
// substreams, and a 96/192 kHz substream whose HSF extension substream could
// not be resolved and read alongside it. Refusing is per substream and per
// frame; the next frame is attempted afresh. decode() refuses, the same way,
// everything above that it does not turn into PCM yet: 96/192 kHz, which it
// reads.
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
// diagnosing a stream: ac4::SyntaxRecord and ac4::SyntaxSink, in
// ac4/syntax.hpp, whose comment states what a record holds. The encoder writes
// records of the same shape, and so does tools/references/ac4_syntax.py.

// --- Output processing -------------------------------------------------------
//
// What decode() does to the decoded channels as a system configures it:
// dialogue enhancement (Part 1 clause 5.7.8), then the output level and
// dynamic range control (5.7.9), then the downmix (6.2.17), or for the
// immersive element Part 2's channel renderer (Part 2 clause 5.10.2).

// The layout decode() renders the decoded channels to (Part 1 clause 6.2.17;
// Part 2 clause 5.10.2 for the immersive element). The .X is the stream's LFE,
// where it has one.
enum class DownmixTarget : std::uint8_t {
    // The channels as coded: for the immersive element, the layout its source
    // had (b_4_back_channels_present and top_channels_present), and in core
    // decoding its 5.X.2 core, 5.X.0 where the source has no top channels.
    kAsCoded,
    k5X,  // a 7.X element's channels folded to 5.X (Table 219); 5.X.0 for the immersive element
    // Two channels, Lo/Ro or Lt/Rt as the stream's preferred_dmx_method says,
    // Lo/Ro where it says neither.
    kStereo,
    kLoRo,
    kLtRt,  // in its Pro Logic II form where the stream prefers that
    kMono,  // L + R of the stereo downmix
    // The immersive element's other layouts (Part 2 Tables 38 to 42; core
    // decoding has 5.X.2 and 5.X.0 alone, Table 44, and takes the one of those
    // with the target's top channels or without). The other elements come out
    // as coded.
    k7X4,
    k7X2,
    k7X0,
    k5X4,
    k5X2,
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

struct OutputConfig {
    // Lout of Part 1 clause 5.7.9.3.3, in dBFS: the level the stream's
    // dialnorm is taken to, by 2^((Lout - dialnorm) / 6), which cuts or
    // boosts. Part 1 gives no default, the system supplies it; unset leaves the
    // stream at its coded level and compresses nothing.
    std::optional<double> output_level_dbfs;
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
    std::optional<int> presentation_id;
    // Else the presentation at this position of the table of contents, which
    // the text warns can change over time.
    std::optional<std::size_t> index;
    // Else the preferences. The language of the main or dialogue audio: an
    // IETF BCP 47 tag, a presentation's tag matching it whole before one whose
    // primary subtag matches; empty for none.
    std::string language;
    // The associated audio: Part 1 Table 91's content_classifier of the
    // service a presentation should carry (0b010 visually impaired, 0b011
    // hearing impaired, 0b101 commentary, and so on), with Table 92's
    // refinement of it; unset for a presentation without associated audio.
    std::optional<int> associated;
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

// --- Decoding modes ----------------------------------------------------------
//
// Part 2 clause 4.7: full decoding, in which A-CPL and A-JCC reconstruct every
// channel an immersive element codes, or core decoding, which gives the
// element's core, 5.X.2, with those tools replaced or reduced, for
// low-complexity platforms, and renders it to 5.X.2 or 5.X.0 alone (Part 2
// Table 44). The Part 1 channel elements have no core (Part 2
// Table 71) and decode alike in both (src/ac4dec/ERRATA.md, "Core decoding of
// the Part 1 elements").
enum class DecodingMode : std::uint8_t {
    kFull,
    kCore,
};

[[nodiscard]] AC4DEC_EXPORT std::string_view describe(DecodingMode mode);

struct DecoderConfig {
    // Null by default, at the cost of one branch per syntax element read.
    SyntaxSink syntax{};
    OutputConfig output{};
    ConcealmentPolicy concealment = ConcealmentPolicy::kNone;
    DecodingMode decoding = DecodingMode::kFull;
    // Which presentation decode() decodes (select_presentation()).
    PresentationChoice presentation{};
    // The md_compat level the decoder claims: presentations above it are not
    // selected (Part 2 clause 6.3.2.2.3).
    int level = 3;
};

// What one substream of a frame turned out to be.
struct SubstreamReport {
    // kAudio covers channel-coded, A-JOC coded and direct-coded object
    // substreams alike (ac4_substream(), Part 2 Table 50); kOamd is an
    // oamd_substream() (Part 2 clause 6.2.2.4).
    enum class Kind : std::uint8_t { kAudio, kPresentation, kEmdfPayloads, kHsfExt, kOther, kOamd };
    int index = 0;
    Kind kind = Kind::kOther;
    std::size_t size_bits = 0;           // the substream's size in substream_index_table(), in bits
    std::size_t bits_read = 0;           // bits the syntax consumed, alignment included
    std::optional<DecodeError> refused;  // set when this substream was not read to its end
    std::string_view refused_reason;
};

struct FrameReport {
    int sequence_counter = 0;
    bool b_iframe_global = false;
    std::vector<SubstreamReport> substreams;
};

// --- Decoding to PCM ---------------------------------------------------------
//
// Where a decoded channel is meant to be heard, by Part 1 clause D.1's names
// and Part 2 clause A.3's: those of the channel modes of Part 1 Table 88, and
// the immersive layouts' (Part 2 Table A.27).
enum class Speaker : std::uint8_t {
    kLeft,
    kRight,
    kCentre,
    kLfe,            // Low-Frequency Effects
    kLeftSurround,   // Left Side/Surround, Ls: a side speaker in the 7.X modes
    kRightSurround,  // Right Side/Surround, Rs
    kLeftBack,       // Lb, in 7.X 3/4/0 and 7.X.4
    kRightBack,      // Rb
    kLeftWide,       // Lw, in 7.X 5/2/0
    kRightWide,      // Rw
    kTopFrontLeft,   // Tfl, in 7.X 3/2/2 and the X.4 layouts
    kTopFrontRight,  // Tfr
    kTopBackLeft,    // Tbl, in the X.4 layouts
    kTopBackRight,   // Tbr
    kTopSideLeft,    // Tsl, the top pair of the X.2 layouts: 5.X.2, the core layout
    kTopSideRight,   // Tsr
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
    // then a 7.X mode's last pair, or an immersive layout's Lb and Rb and then
    // Tfl, Tfr, Tbl and Tbr, or Tsl and Tsr, each where the layout has it.
    std::vector<Speaker> speakers;
    // Planar PCM, one vector per channel, all the same length, at full scale
    // 1.0: a frame's worth, which at 29.97, 59.94 and 119.88 fps alternates
    // by a sample in the sequence Part 2 Table 47 locks to sequence_counter
    // (1 601 or 1 602 at 29.97). The decoder's delay is applied: Part 1's
    // frame alignment (clause 5.6), the QMF banks and the QMF domain's history
    // (5.7.1), 1 313 samples at frame_rate_index 13 in every codec mode, and
    // at the other indices the sample rate converter's too.
    std::vector<std::vector<float>> channels;
    // Set only on a frame DecoderConfig::concealment made in place of one that
    // did not decode.
    std::optional<Concealment> concealed;
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
    // when its table of contents does.
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

    // Why the last decode() failed, returned nothing or returned a concealed
    // frame, a string literal; empty after a decode() that decoded its frame.
    [[nodiscard]] std::string_view refusal_reason() const noexcept;

    // Forgets everything carried between frames.
    void reset();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac4
