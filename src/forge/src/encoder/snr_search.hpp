#pragma once

#include <algorithm>

// The SNR-offset search's shared core, warm-startable. Both encoders (AC-3's
// encoder.cpp and E-AC-3's eac3_frame.cpp) rate-control the same way: find
// the largest composite SNR offset ((csnroffst << 4) | fsnroffst, 0..1023)
// whose mantissa cost still fits the frame's budget, binary-searched as if
// that predicate were monotone. That search is the whole of CBR's rate
// control and runs its bit-allocation evaluation several times a frame; the
// phase-5 Tracy zones (PR #115/#120) put that cluster at the top of both
// paths' profiles, and on an ESP32-S3 it is a quarter of an E-AC-3 frame.
//
// "As if": the cost is NOT quite monotone. A higher offset never lowers any
// bin's bap, but mantissa_bits_per_block packs bap-1, bap-2 and bap-4
// mantissas three or two to a codeword across the block, so a bin that moves
// from bap 1 to bap 2 can empty a partly filled 5-bit group and land in a
// partly filled 7-bit one, and the block costs five bits less than it did.
// The fitting predicate therefore has more than one boundary in rare frames,
// and which one a search lands on depends on the probes it makes - so a
// different probe sequence (a different hint, below) can return an offset a
// unit or two away from what another sequence returns. Every such answer is
// a fitting offset with a non-fitting neighbour above it; none is wrong, and
// the difference is below anything the gold-reference gate can measure. But
// it does mean the streams an encoder produces depend on how it warm-starts,
// which is why changing that re-pinned the golden hashes (2026-09-10).
//
// Internal to src/forge/src/encoder/ on purpose - this is plumbing between the
// two encoder translation units, not library surface.

namespace ac3::internal {

// Largest x in [0, limit] with fits(x), where fits is monotone
// non-increasing in x (true up to some boundary, false beyond it). Returns 0
// when nothing fits, exactly as the plain binary search's lo never moves off
// 0 in that case.
//
// `hint` is the previous answer to the same question (or negative when there
// is none - then this IS the plain binary search over [0, limit]). For a
// monotone predicate the hint changes how fast the answer is found and never
// what it is: the bracket grown around it contains the boundary - every
// failed probe bounds the answer from above, every fitting probe from below
// - and the search inside the bracket is the same binary loop the cold path
// runs. For the predicate the encoders actually pass (see the note at the
// top) it also decides which of the rare frames' boundaries is found. Either
// way the number of probes is what the hint is for: the answer to the same
// question moves by a handful of units from one frame to the next, so a hint
// that IS the previous answer to that question costs 2-4 evaluations instead
// of log2(limit + 1) + 1 - and a hint that is the answer to a different
// question, thirty units away, costs ten (the delta race's two passes, when
// they shared one hint).
template <typename Fits>
int search_max_fitting(int limit, int hint, Fits&& fits) {
    int lo = 0;
    int hi = limit;
    if (hint >= 0 && hint <= limit) {
        if (fits(hint)) {
            // The answer is at or above the hint: march the lower bound up
            // in doubling steps until a probe fails (new upper bound) or the
            // range ends.
            lo = hint;
            int step = 1;
            while (lo + step <= limit && fits(lo + step)) {
                lo += step;
                step *= 2;
            }
            hi = std::min(limit, lo + step - 1);
        } else {
            // The answer is strictly below the hint: march the upper bound
            // down in doubling steps until a probe fits (new lower bound) or
            // the bottom of the range is passed.
            int step = 1;
            hi = hint - 1;
            while (hi >= 0) {
                const int probe = std::max(0, hi - step + 1);
                if (fits(probe)) {
                    lo = probe;
                    break;
                }
                hi = probe - 1;
                step *= 2;
            }
            if (hi < 0) {
                return 0;
            }
        }
    }
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (fits(mid)) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

}  // namespace ac3::internal
