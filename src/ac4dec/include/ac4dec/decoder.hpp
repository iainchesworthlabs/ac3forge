#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

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
// sequence_counter (Part 2 clause 5.11). The table of contents and the
// substream framing come from ac4::parse_raw_frame (the inspector, src/ac4);
// this library starts where the inspector stops.
//
// What it refuses, with DecodeError::kUnsupported and a reason: the speech
// spectral frontend (Part 1 clause 5.2), immersive and 22.2 channel elements,
// object substreams, and a 96/192 kHz substream whose HSF extension
// substream could not be resolved and read alongside it. Refusing is per
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
// diagnosing a stream: ac4::SyntaxRecord and ac4::SyntaxSink, in
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
};

struct DecoderConfig {
    // Null by default, at the cost of one branch per syntax element read.
    SyntaxSink syntax{};
    OutputConfig output{};
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
    int sequence_counter = 0;             // of the frame this came from
    // One per channel, in the order of `channels`: L, R, C, the LFE, Ls, Rs,
    // then a 7.X mode's last pair, each where the channel mode has it.
    std::vector<Speaker> speakers;
    // Planar PCM, one vector per channel, all the same length, at full scale
    // 1.0: a frame's worth, which at 29.97, 59.94 and 119.88 fps alternates
    // by a sample in the sequence Part 2 Table 47 locks to sequence_counter
    // (1 601 or 1 602 at 29.97). The decoder's delay is applied: Part 1's
    // frame alignment (clause 5.6), the QMF banks and the QMF domain's history
    // (5.7.1), 1 313 samples at frame_rate_index 13 in every codec mode, and
    // at the other indices the sample rate converter's too.
    std::vector<std::vector<float>> channels;
};

// One decoder per stream: configuration sent only in I-frames (A-SPX, A-CPL,
// DRC, dialogue enhancement) persists from one frame to the next, until a
// sequence_counter that does not continue the stream marks a change of
// source (Part 1 clause 4.3.3.2.2), which forgets it as reset() does. That
// includes decode()'s overlap buffers and delay lines, which start again from
// silence.
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

    // Reads one raw_ac4_frame as parse() does and decodes the audio of the
    // first channel-coded substream of the first presentation that has one.
    // Nothing for a frame that has no output: one whose substream needs
    // configuration no I-frame has sent yet. The error, when there is one, is
    // that substream's (or the table of contents'), and refusal_reason() says
    // why.
    [[nodiscard]] std::expected<std::optional<DecodedFrame>, DecodeError> decode(
        std::span<const std::byte> raw_ac4_frame);

    // Why the last decode() failed or returned nothing, a string literal;
    // empty after a decode() that returned a frame.
    [[nodiscard]] std::string_view refusal_reason() const noexcept;

    // Forgets everything carried between frames.
    void reset();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac4
