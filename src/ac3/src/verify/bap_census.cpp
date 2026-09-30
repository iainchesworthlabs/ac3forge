#include "ac3/verify/bap_census.hpp"

#include <fmt/format.h>

#include <cstddef>
#include <string>

#include "ac3/core/exponents.hpp"
#include "ac3/verify/eac3_mirror.hpp"
#include "ac3/verify/mirror.hpp"

namespace ac3::verify {

namespace {

// Counts one stream's bins for one block, excluding the ones that are not this
// stream's to carry.
//
// The exclusion is not cosmetic. A coupling stream's bap vector is indexed
// from bin 0 like every other stream's, but everything below cplstrtmant is
// kMaxExponent FILLER - those bins belong to the coupled channels' own
// basebands, not to the coupling channel (see StreamTrace::exponents). Counted
// naively, that filler is a run of bap == 0 bins, and on a real 448 kbps 5.1
// fixture it is about 72% of the coupling stream's vector: the census reported
// 88.9% zero-bit for a stream whose genuine figure is far lower, which would
// have made a mask built on it exclude most of the coupling channel.
//
// The filler is a contiguous PREFIX, so skipping the leading run of
// kMaxExponent bins removes exactly it. A regular channel is unaffected: bin 0
// of a real channel reaching kMaxExponent means digital silence at DC, and a
// silent stream contributes nothing either way.
void observe_stream(StreamBapCensus& census, const StreamTrace& stream) {
    std::size_t first = 0;
    while (first < stream.bap.size() && first < stream.exponents.size() &&
           stream.exponents[first] == kMaxExponent) {
        ++first;
    }
    for (std::size_t i = first; i < stream.bap.size(); ++i) {
        ++census.bins;
        if (stream.bap[i] == 0) {
            ++census.zero_bit_bins;
        }
    }
}

// The Annex E trace carries the same convention in its own stream type.
void observe_stream(StreamBapCensus& census, const Eac3StreamTrace& stream) {
    std::size_t first = 0;
    while (first < stream.bap.size() && first < stream.exponents.size() &&
           stream.exponents[first] == kMaxExponent) {
        ++first;
    }
    for (std::size_t i = first; i < stream.bap.size(); ++i) {
        ++census.bins;
        if (stream.bap[i] == 0) {
            ++census.zero_bit_bins;
        }
    }
}

}  // namespace

StreamBapCensus& BapCensus::slot(std::size_t stream) {
    if (stream >= streams_.size()) {
        streams_.resize(stream + 1);
    }
    return streams_[stream];
}

void BapCensus::observe(const FrameTrace& trace) {
    ++frames_;
    for (const auto& block : trace.blocks) {
        // See the header: a block with no allocation has an empty bap vector,
        // and counting it would read as a burst of zero-bit bins rather than
        // as the refused frame it is.
        if (!block.allocated) {
            continue;
        }
        for (std::size_t s = 0; s < block.streams.size(); ++s) {
            observe_stream(slot(s), block.streams[s]);
        }
    }
}

void BapCensus::observe(const Eac3AccessUnitTrace& trace) {
    ++frames_;
    // Substreams are folded together by stream index rather than kept apart.
    // The census answers "does this decoder allocate the way it used to", and
    // a dependent substream's channel N is the same coded stream slot as an
    // independent one's for that purpose. Keeping them separate would make the
    // baseline depend on substream count, which changes with the fixture
    // rather than with the decoder.
    for (const auto& substream : trace.substreams()) {
        for (const auto& block : substream.blocks) {
            if (!block.allocated) {
                continue;
            }
            for (std::size_t s = 0; s < block.streams.size(); ++s) {
                observe_stream(slot(s), block.streams[s]);
            }
        }
    }
}

std::string BapCensus::to_json() const {
    std::string out;
    out += "{\n";
    out += fmt::format("  \"schema\": 1,\n  \"frames\": {},\n  \"streams\": [\n", frames_);
    for (std::size_t i = 0; i < streams_.size(); ++i) {
        const auto& s = streams_[i];
        out += fmt::format(
            "    {{\"stream\": {}, \"bins\": {}, \"zero_bit_bins\": {}, "
            "\"zero_bit_fraction\": {:.6f}}}{}\n",
            i, s.bins, s.zero_bit_bins, s.zero_bit_fraction(),
            i + 1 == streams_.size() ? "" : ",");
    }
    out += "  ]\n}\n";
    return out;
}

}  // namespace ac3::verify
