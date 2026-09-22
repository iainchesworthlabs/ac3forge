#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

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
// reports what a frame carries. It produces no audio yet. The table of
// contents and the substream framing come from ac4::parse_raw_frame (the
// inspector, src/ac4); this library starts where the inspector stops.
//
// What it refuses, with DecodeError::kUnsupported and a reason: the speech
// spectral frontend (Part 1 clause 5.2), immersive and 22.2 channel elements,
// object substreams, and a 96/192 kHz substream whose HSF extension
// substream could not be resolved and read alongside it. Refusing is per
// substream and per frame; the next frame is attempted afresh.
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
// diagnosing a stream. A syntax element is one entry with a bit count in a
// syntax table of either part; its record carries the bit offset where it
// starts within its substream, how many bits it took, and its value:
//
//   - a fixed-width field: its width and its value as an unsigned integer;
//   - variable_bits(n) (Part 1 clause 4.2.2): one record for the whole
//     element, with the total bits read and the decoded value;
//   - a Huffman codeword: its length and the index of the codeword in its
//     codebook, before any cb_off is subtracted;
//   - quad_sign_bits and pair_sign_bits: one record for the group, with as
//     many bits as there were nonzero lines and the bits as an integer;
//   - ext_code: one record for the escape, with its total length and the
//     decoded magnitude;
//   - a field whose width the stream sets and whose bits the syntax does not
//     interpret (add_data, extensions_bits, drc2_bits): one record of its
//     width, valued at its last 64 bits, split into 65535-bit records when
//     longer.
//
// byte_align, fill_bits and fill_area are not recorded. The records are the
// same shape tools/references/ac4_syntax.py writes, which is what lets two
// transcriptions of the syntax be compared record by record.
struct SyntaxRecord {
    int substream = 0;               // index into the frame's substream_index_table
    std::uint32_t bit_offset = 0;    // from the first bit of that substream
    std::uint16_t bits = 0;
    std::uint64_t value = 0;
    std::string_view name;           // the element's name in the syntax table
};

// A non-owning reference to any callable taking a const SyntaxRecord&, in the
// shape of ac3::BlockSink: no allocation, and the callable must outlive the
// call it is handed to.
class SyntaxSink {
   public:
    SyntaxSink() noexcept = default;

    template <typename F>
        requires std::invocable<F&, const SyntaxRecord&> &&
                 (!std::same_as<std::remove_cvref_t<F>, SyntaxSink>)
    // NOLINTNEXTLINE(google-explicit-constructor): the call site is the point
    SyntaxSink(F&& f) noexcept
        : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          call_([](void* object, const SyntaxRecord& record) {
              (*static_cast<std::remove_reference_t<F>*>(object))(record);
          }) {}

    explicit operator bool() const noexcept { return call_ != nullptr; }
    void operator()(const SyntaxRecord& record) const { call_(object_, record); }

   private:
    void* object_ = nullptr;
    void (*call_)(void*, const SyntaxRecord&) = nullptr;
};

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

// One decoder per stream: configuration sent only in I-frames (A-SPX, A-CPL,
// DRC, dialogue enhancement) persists from one frame to the next, until a
// sequence_counter that does not continue the stream marks a change of
// source (Part 1 clause 4.3.3.2.2), which forgets it as reset() does.
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

    // Forgets everything carried between frames.
    void reset();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac4
