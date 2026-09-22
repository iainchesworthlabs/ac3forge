#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/silent_frame.hpp"  // FrameError
#include "ac3/export.hpp"
#include "ac3/verify/eac3_mirror.hpp"

// The encode-then-decode-then-compare driver over ac3/verify/eac3_mirror.hpp,
// and the E-AC-3 counterpart of ac3/verify/selfcheck.hpp.
//
// Eac3MirrorEncoder is a drop-in for eac3::AccessUnitEncoder that
// additionally decodes every access unit it emits with this repo's own
// decoder and diffs the decoder's model against the encoder's own. It is a
// debugging and test facility, not a production encode path: it costs a full
// decode per access unit on top of the encode. AccessUnitEncoder itself is
// untouched by using this - the trace pointer eac3::FrameConfig now carries
// is null unless something like this class sets it, and a null trace means
// one predictable branch per block and no allocation.
//
// The access unit, rather than the syncframe, is the unit here because that
// is what an E-AC-3 encoder produces: a 7.1.4 program is an independent
// substream plus two dependents, all coding the same 1536 samples, and a
// check that looked at one substream at a time would have nothing to say
// about the ones beside it. Each substream is decoded on its own
// (Eac3Decoder::decode_substream) rather than through decode_access_unit,
// because what is being compared is what each side PARSED, and the §E3.8.2
// render that decode_access_unit adds on top is downstream of every field
// the trace records.

namespace ac3::verify {

// One access unit, plus what the check found out about it.
struct CheckedAccessUnit {
    eac3::AccessUnit unit;
    // Empty when the two models agree, which is the only outcome a correct
    // encoder ever produces. Otherwise the first divergent substream's first
    // divergent block's findings, most useful first - see compare().
    std::vector<Eac3Mismatch> mismatches;
    // Set when the decoder refused one of the unit's substreams outright
    // rather than merely disagreeing about it - the first such refusal, in
    // transmission order. The two are independent findings and both are
    // worth having: a refusal says the stream is unusable, `mismatches` says
    // WHERE the two sides parted company, and the second is what points at
    // the bug. A desync typically produces both, with the mismatch naming an
    // earlier block than the refusal - that gap is the misdirection this
    // whole facility exists to remove.
    std::optional<DecodeError> decode_error;

    [[nodiscard]] bool ok() const { return mismatches.empty() && !decode_error.has_value(); }
};

class AC3FORGE_EXPORT Eac3MirrorEncoder {
   public:
    // `config` is taken by value and every substream's trace pointer
    // overwritten - a caller has no use for setting one here, and letting one
    // through would silently disable the check.
    explicit Eac3MirrorEncoder(eac3::AccessUnitConfig config);

    // Non-copyable and non-movable: the configs hold pointers into this
    // object's own trace members, which a copy or move would leave aimed at
    // the original.
    Eac3MirrorEncoder(const Eac3MirrorEncoder&) = delete;
    Eac3MirrorEncoder& operator=(const Eac3MirrorEncoder&) = delete;
    Eac3MirrorEncoder(Eac3MirrorEncoder&&) = delete;
    Eac3MirrorEncoder& operator=(Eac3MirrorEncoder&&) = delete;

    // Same contract as AccessUnitEncoder::encode_access_unit, with the check
    // run over the result. An encode failure propagates unchanged and
    // consumes no frame index.
    //
    // A substream using transient pre-noise processing (§3.7) makes
    // decode_substream hold its PCM back a frame; that is invisible here,
    // because the trace is filled while the frame is parsed rather than when
    // its audio is released, so a held-back frame is compared in the call
    // that decoded it exactly like any other.
    [[nodiscard]] std::expected<CheckedAccessUnit, FrameError> encode_access_unit(
        std::span<const std::span<const float>> channels);

    [[nodiscard]] const eac3::AccessUnitConfig& config() const { return encoder_.config(); }
    // Summed across substreams: the span count encode_access_unit expects.
    [[nodiscard]] int channel_count() const { return encoder_.channel_count(); }

    // Access units encoded so far, which is the index the NEXT one will
    // report.
    [[nodiscard]] std::uint64_t frames_encoded() const { return frame_index_; }

    // The two views of the most recently encoded access unit, for a caller
    // wanting more than compare() reports.
    [[nodiscard]] const Eac3AccessUnitTrace& encoder_trace() const { return encoder_trace_; }
    [[nodiscard]] const Eac3AccessUnitTrace& decoder_trace() const { return decoder_trace_; }

    // The most recent access unit's findings rendered as text, with the
    // stream names resolved from that unit's own shape. Empty when it was
    // clean.
    [[nodiscard]] std::string last_report() const;

   private:
    // Declared before the encoder and decoder below: their configs hold
    // pointers to these, so these must outlive them and be constructed first.
    Eac3AccessUnitTrace encoder_trace_;
    Eac3AccessUnitTrace decoder_trace_;
    eac3::AccessUnitEncoder encoder_;
    Eac3Decoder decoder_;
    std::vector<Eac3Mismatch> last_mismatches_;
    std::uint64_t frame_index_ = 0;
};

}  // namespace ac3::verify
