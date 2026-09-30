#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ac3/export.hpp"
#include "ac3/verify/eac3_mirror.hpp"
#include "ac3/verify/mirror.hpp"

// How many bins each coded stream gave zero bits to, counted over a whole
// decode.
//
// This exists to make a masked comparison honest, and it has to land BEFORE
// one. tools/checks/compare_wav.py measures agreement between two decoders
// over every bin, which means the bins the encoder spent nothing on - where
// §7.3.4 lets a decoder substitute "any reasonably random sequence" and two
// correct decoders are therefore REQUIRED to differ - are inside the
// measurement, and on a 5.1 fixture they dominate the surround channels. The
// obvious fix is to exclude them and score only what was actually coded.
//
// The trap in that fix is that the mask would come from the decoder under
// test. If a bit-allocation misread made this decoder believe a bin was
// bap == 0 when the stream allocated it bits, the unmasked comparison catches
// it - we would dither a bin that should carry audio, and the SNR falls - but
// a masked comparison would EXCLUDE that bin precisely because the wrong bap
// said to, and report an improvement. That is the class of defect the
// external-baseline fixtures exist to catch (verify_gold_reference.sh lists
// five real ones; firstcplcos[ch] is per channel by nature), so a mask that
// can swallow it is worse than no mask.
//
// So the population of bap == 0 bins is itself pinned. A decoder bug that
// shifts the allocation shows up here as "this stream now gives zero bits to
// a different share of its bins than every previous run did" - a direct,
// binary statement about the allocation rather than an inference from a
// changed SNR, and a much sharper signal than the SNR was ever going to be.
//
// Counts, not fractions, are what accumulate; fractions are derived on the way
// out. A count is exact and comparable, and dividing early would lose the bin
// total that says how much evidence each figure rests on.

namespace ac3::verify {

// One coded stream's census. Streams are numbered exactly as StreamTrace and
// Eac3StreamTrace already number them: the full-bandwidth channels first, then
// the LFE, then the coupling channel where one is in use.
struct StreamBapCensus {
    // Every (block, bin) this stream was observed over. Blocks the decoder
    // never reached an allocation for contribute nothing - see BapCensus::
    // observe - so this is evidence actually gathered, not blocks attempted.
    std::uint64_t bins = 0;
    // Of those, the ones the allocator gave no bits to. §7.3.4's dither (or a
    // true zero when dithflag is clear) is what a decoder puts in each of
    // these, and it is the only place two correct decoders may differ.
    std::uint64_t zero_bit_bins = 0;

    [[nodiscard]] double zero_bit_fraction() const {
        return bins == 0 ? 0.0 : static_cast<double>(zero_bit_bins) / static_cast<double>(bins);
    }
};

class AC3FORGE_EXPORT BapCensus {
   public:
    // Accumulates one frame / one access unit. Safe to call for every frame of
    // a file; streams are added as they are first seen, so a stream that only
    // appears part-way through (a channel joining coupling, say) is counted
    // over the blocks it actually appeared in rather than assumed absent.
    //
    // A block whose `allocated` flag is clear is skipped entirely. Its `bap`
    // vector was never filled, and counting it would report a burst of
    // zero-bit bins for what is really a refused or truncated frame - exactly
    // the misreading a census is supposed to prevent.
    void observe(const FrameTrace& trace);
    void observe(const Eac3AccessUnitTrace& trace);

    [[nodiscard]] std::span<const StreamBapCensus> streams() const { return streams_; }
    [[nodiscard]] std::uint64_t frames() const { return frames_; }

    // A versioned JSON document, the schema tools/checks/check_bap_census.py
    // gates on. Stable field names: this is a contract, not a dump.
    [[nodiscard]] std::string to_json() const;

   private:
    StreamBapCensus& slot(std::size_t stream);

    std::vector<StreamBapCensus> streams_;
    std::uint64_t frames_ = 0;
};

}  // namespace ac3::verify
