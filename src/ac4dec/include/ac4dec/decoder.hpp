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
// reports what a frame carries. It decodes to PCM a mono or stereo substream
// in the SIMPLE codec mode at frame_rate_index 13 (2 048 samples a frame at
// 48 or 44.1 kHz, which needs no sample rate converter): the audio spectral
// frontend, stereo processing, the inverse transform with block switching,
// and frame alignment (Part 1 clauses 5.1, 5.3, 5.5 and 5.6). The table of
// contents and the substream framing come from ac4::parse_raw_frame (the
// inspector, src/ac4); this library starts where the inspector stops.
//
// What it refuses, with DecodeError::kUnsupported and a reason: the speech
// spectral frontend (Part 1 clause 5.2), immersive and 22.2 channel elements,
// object substreams, and a 96/192 kHz substream whose HSF extension
// substream could not be resolved and read alongside it. Refusing is per
// substream and per frame; the next frame is attempted afresh. decode()
// refuses, the same way, everything above that it does not turn into PCM
// yet: the A-SPX and A-CPL codec modes, the 3.0, 5.X and 7.X elements, other
// frame rates and 96/192 kHz.
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

struct DecoderConfig {
    // Null by default, at the cost of one branch per syntax element read.
    SyntaxSink syntax{};
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
// Where a decoded channel is meant to be heard, by Part 1 clause D.1's names.
// Later versions add the rest of the layouts.
enum class Speaker : std::uint8_t {
    kLeft,
    kRight,
    kCentre,
};

[[nodiscard]] AC4DEC_EXPORT std::string_view describe(Speaker speaker);

// One frame of output.
struct DecodedFrame {
    int sample_rate_hz = 0;
    int sequence_counter = 0;             // of the frame this came from
    std::vector<Speaker> speakers;        // one per channel, in the order of `channels`
    // Planar PCM, one vector per channel, all the same length, at full scale
    // 1.0. The decoder's delay is applied: Part 1's frame alignment (clause
    // 5.6), the QMF banks and the QMF domain's history (5.7.1), 1 313 samples
    // at frame_rate_index 13 in every codec mode.
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
