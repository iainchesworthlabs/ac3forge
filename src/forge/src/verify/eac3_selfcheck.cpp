#include "ac3/verify/eac3_selfcheck.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/verify/eac3_mirror.hpp"
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>

namespace ac3::verify {

namespace {

eac3::AccessUnitConfig with_traces(eac3::AccessUnitConfig config, Eac3AccessUnitTrace* trace) {
    // Sized once, here, and never grown again: the pointers handed out below
    // have to stay valid for as long as the encoder does, and only a resize
    // asking for MORE slots would move them.
    trace->resize(1 + config.dependents.size());
    config.independent.trace = &trace->substream(0);
    for (std::size_t i = 0; i < config.dependents.size(); ++i) {
        config.dependents[i].trace = &trace->substream(i + 1);
    }
    return config;
}

DecoderConfig decoder_config(Eac3AccessUnitTrace* trace) {
    DecoderConfig config;
    // drc_scale stays 0: this decoder exists to check what the encoder wrote,
    // and none of the compared state is affected by the §7.7 gain anyway (it
    // is applied to reconstructed coefficients, downstream of every field the
    // trace records). Left explicit rather than defaulted-by-omission so a
    // future change to DecoderConfig's default cannot quietly alter what the
    // self-check decodes.
    config.drc_scale = 0.0;
    config.eac3_trace = trace;
    return config;
}

}  // namespace

Eac3MirrorEncoder::Eac3MirrorEncoder(eac3::AccessUnitConfig config)
    : encoder_(with_traces(std::move(config), &encoder_trace_)),
      decoder_(decoder_config(&decoder_trace_)) {}

std::expected<CheckedAccessUnit, FrameError> Eac3MirrorEncoder::encode_access_unit(
    std::span<const std::span<const float>> channels) {
    auto encoded = encoder_.encode_access_unit(channels);
    if (!encoded) {
        last_mismatches_.clear();
        return std::unexpected(encoded.error());
    }

    CheckedAccessUnit result;
    result.unit = std::move(*encoded);
    // Emptied before the loop rather than relying on the independent
    // substream's own begin_substream to do it: a substream refused before it
    // gets that far (a bad CRC, an unreadable bsi) never reaches one, and the
    // previous unit's substreams must not be left behind for this one's to be
    // appended to.
    decoder_trace_.resize(0);
    for (std::size_t i = 0; i < result.unit.substream_count(); ++i) {
        // The decoder writes into decoder_trace_ as it goes, so a refusal
        // part-way through still leaves everything it managed to read - which
        // is exactly the case where naming the block matters most. Later
        // substreams are still decoded after one is refused, so the trace
        // describes the whole unit rather than stopping at the first problem.
        const auto decoded = decoder_.decode_substream(result.unit.substream(i));
        if (!decoded && !result.decode_error) {
            result.decode_error = decoded.error();
        }
        // A std::nullopt value is the §3.7 hold-back, not a failure: the
        // frame parsed, and its trace is complete - only its PCM is waiting
        // on the next frame. Nothing here reads that PCM.
    }
    result.mismatches = compare(encoder_trace_, decoder_trace_, frame_index_);
    last_mismatches_ = result.mismatches;
    ++frame_index_;
    return result;
}

std::string Eac3MirrorEncoder::last_report() const {
    return report(last_mismatches_, encoder_trace_);
}

}  // namespace ac3::verify
