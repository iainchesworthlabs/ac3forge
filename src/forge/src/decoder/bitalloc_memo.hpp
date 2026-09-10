#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/tables.hpp"

// §7.2.2's inputs for one stream's last bit allocation, and whether that
// allocation is still what the stream's `bap` holds. Shared by both decoders
// (decoder.cpp's AC-3 and eac3_decoder.cpp's Annex E), which make the same
// call at the same point.
//
// The allocation is a pure function of these inputs, and the standard's own
// remark that a decoder "need only" recompute it when they change is the
// whole justification for keeping them: an E-AC-3 stream commonly sends a
// channel's exponents once a frame and reuses them, so five of its six
// blocks would derive the same allocation again. Measured on an ESP32-S3
// before this existed, bit allocation was 1.6 to 2.1 ms of a 14 ms 5.1
// frame (docs/platforms/esp32.md).
//
// Only the untraced call consults it: a syntax trace wants the mask, which
// the memo does not keep, so the traced form always recomputes and marks the
// memo invalid.

namespace ac3::internal {

struct BitAllocMemo {
    std::vector<std::uint8_t> exps;
    BitAllocCodes codes{};
    BitAllocRegion region{};
    SampleRate sample_rate{};
    int csnroffst = 0;
    int fsnroffst = 0;
    bool valid = false;

    [[nodiscard]] bool matches(std::span<const std::uint8_t> exps_now, SampleRate rate,
                               const BitAllocCodes& codes_now, int csnr, int fsnr,
                               const BitAllocRegion& region_now) const {
        return valid && sample_rate == rate && csnroffst == csnr && fsnroffst == fsnr &&
               codes == codes_now && region == region_now && std::ranges::equal(exps, exps_now);
    }

    void remember(std::span<const std::uint8_t> exps_now, SampleRate rate,
                  const BitAllocCodes& codes_now, int csnr, int fsnr,
                  const BitAllocRegion& region_now) {
        exps.assign(exps_now.begin(), exps_now.end());
        sample_rate = rate;
        csnroffst = csnr;
        fsnroffst = fsnr;
        codes = codes_now;
        region = region_now;
        valid = true;
    }
};

}  // namespace ac3::internal
