#include "ac3/core/bitalloc.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/aht_tables.hpp"
#include "ac3/core/bitalloc_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/internal/arch/simd.hpp"
#include "ac3/internal/profiling.hpp"

namespace ac3 {

namespace {

using namespace tables;

// A/52 §7.2.2.2: the widest allocation region the standard admits - 253
// mantissas in one channel (the fbw bandwidth ceiling), which is exactly the
// length of the psd array §7.2.2.2 builds. Named rather than repeated as a
// literal because compute_bit_allocation now enforces it at run time and not
// only in a debug assert; see its own comment for why.
inline constexpr std::size_t kMaxMantissas = 253;

// §7.2.2.3: log-addition of two banded PSD values.
int logadd(int a, int b) {
    const int c = a - b;
    const int address = std::min(std::abs(c) >> 1, 255);
    return (c >= 0 ? a : b) + kLogAdd[static_cast<std::size_t>(address)];
}

// §7.2.2.4 calc_lowcomp. The spec pseudocode carries a known erratum (a
// stray semicolon after `if ((b0 + 256) == b1)` in the bin < 7 branch); the
// universally implemented intent mirrors the bin < 20 branch's structure.
int calc_lowcomp(int a, int b0, int b1, int bin) {
    if (bin < 7) {
        if (b0 + 256 == b1) {
            a = 384;
        } else if (b0 > b1) {
            a = std::max(0, a - 64);
        }
    } else if (bin < 20) {
        if (b0 + 256 == b1) {
            a = 320;
        } else if (b0 > b1) {
            a = std::max(0, a - 64);
        }
    } else {
        a = std::max(0, a - 128);
    }
    return a;
}

// §7.2.2.2: exponents -> 13-bit signed log PSD, four bins at a time through
// the arch seam (ROADMAP PF5).
//
// The only loop in the allocator that vectorises at all, which is worth
// saying explicitly so nobody goes looking for the other two: §7.2.2.4's
// excitation function is a serial recurrence (fastleak, slowleak and lowcomp
// each carry from one band into the next, and one of them can break the loop
// early), and §7.2.2.5's masking curve is a per-band conditional over at
// most 50 elements. Neither is a shape a vector unit helps.
//
// Exactness is not an argument here the way it is for the transforms: these
// are the same widening, shift and subtract on the same integers, and
// integer arithmetic does not round.
void exponents_to_psd(std::span<const std::uint8_t> exps, int start, int end,
                      std::span<std::int32_t> psd) {
    const auto stop = static_cast<std::size_t>(end);
    const auto base = internal::arch::i32x4::broadcast(3072);
    std::size_t bin = static_cast<std::size_t>(start);
    for (; bin + 4 <= stop; bin += 4) {
        const auto raw = internal::arch::i32x4::load_u8_widen(exps.data() + bin);
        (base - internal::arch::shift_left<7>(raw)).store(psd.data() + bin);
    }
    // end is a mantissa count (37, 61, ... 253), never a multiple of four.
    for (; bin < stop; ++bin) {
        psd[bin] = 3072 - (exps[bin] << 7);
    }
}

// A run of consecutive absolute bands sharing one Table 5.17 deltba code.
struct DeltaRun {
    int band = 0;
    int length = 0;
    int code = 0;
};

// The inverse of §7.2.2.6's `delta = (code>=4 ? code-3 : code-4) << 7`: the
// nearest code correcting `diff` units of mask[], or nullopt when |diff| is
// under half a step - not worth spending a segment on.
std::optional<int> delta_code_for(int diff) {
    // Require a full Table 5.17 step (128 units, 6 dB) before spending a
    // segment, rather than rounding to the nearest one: a "nearest step"
    // threshold fires on roughly half of all bins even for ordinary
    // quantization residue, which measurably narrowed coupling's usual cost
    // advantage over independent per-channel coding without a matching
    // quality win to show for it.
    if (diff >= 128) {
        return std::clamp(diff / 128 + 3, 4, 7);
    }
    if (diff <= -128) {
        return std::clamp(4 - (-diff) / 128, 0, 3);
    }
    return std::nullopt;
}

}  // namespace

int fast_gain(int fgaincod) {
    return kFastGain[static_cast<std::size_t>(std::clamp(fgaincod, 0, 7))];
}

int slow_gain(int sgaincod) {
    return kSlowGain[static_cast<std::size_t>(std::clamp(sgaincod, 0, 3))];
}

int rate_adaptive_fgaincod(int bitrate_kbps, int nfchans) {
    // The line through (38, 7) and (128, 0), rounded rather than truncated.
    constexpr int kTopKbps = 128;
    constexpr int kSpanKbps = 90;
    const int per_channel_kbps = bitrate_kbps / std::max(nfchans, 1);
    const int numerator = (kTopKbps - per_channel_kbps) * 7 + kSpanKbps / 2;
    return std::clamp(numerator / kSpanKbps, 0, 7);
}

int bin_to_band(int bin) {
    return kMaskTab[static_cast<std::size_t>(bin)];
}

// Factored out - and now exported - so every curve this file derives from a
// spectrum is banded with identical arithmetic: the exponent-derived one
// compute_bit_allocation uses, the encoder-only real-coefficient one
// choose_delta_segments builds, and the coded-bandwidth decision in
// ac3/encoder/bandwidth.hpp. Comparable units are the whole point.
std::array<int, 50> band_psd(std::span<const int> psd, int start, int end) {
    std::array<int, 50> bndpsd{};
    int j = start;
    int k = kMaskTab[static_cast<std::size_t>(start)];
    int lastbin = 0;
    do {
        // std::min<int>, not deduced: kBandStart/kBandSize hold std::int32_t,
        // which on arm-none-eabi is `long int` rather than `int` - so the two
        // arguments are different types there and deduction fails. Spelled out
        // at every such site in this file.
        lastbin = std::min<int>(
            kBandStart[static_cast<std::size_t>(k)] + kBandSize[static_cast<std::size_t>(k)], end);
        bndpsd[static_cast<std::size_t>(k)] = psd[static_cast<std::size_t>(j)];
        ++j;
        for (int i = j; i < lastbin; ++i) {
            bndpsd[static_cast<std::size_t>(k)] =
                logadd(bndpsd[static_cast<std::size_t>(k)], psd[static_cast<std::size_t>(j)]);
            ++j;
        }
        ++k;
    } while (end > lastbin);
    return bndpsd;
}

void compute_bit_allocation(std::span<const std::uint8_t> exps, SampleRate sample_rate,
                            const BitAllocCodes& codes, int csnroffst, int fsnroffst,
                            std::span<std::uint8_t> bap, const BitAllocRegion& region) {
    AC3_ZONE_SCOPED_N("compute_bit_allocation");
    assert(exps.size() == bap.size());
    // A region outside 1..kMaxMantissas allocates nothing, and says so here
    // rather than walking off the end of the arrays below. Both ends are
    // real, and fuzz_signing_verify (roadmap VX3) reported both:
    //
    //  - Empty. §7.2.2.4's band walk runs from kMaskTab[start] to
    //    kMaskTab[end - 1], and `end - 1` on end == 0 is -1, indexing that
    //    256-entry table at SIZE_MAX.
    //  - Longer than kMaxMantissas. §7.2.2.2's psd array is exactly that
    //    long (A/52 admits no more mantissas than that in one channel), so
    //    the first bin past it is a stack write one element off the end -
    //    which is what ASan actually reported, a 4-byte WRITE at offset
    //    1076 of a 1012-byte frame object.
    //
    // No encode path produces either, and the assert below still says so for
    // a caller's benefit. But `exps` reaches here, through both the decoder
    // and ac3::signing's own frame walk, sized by a field value a hostile
    // stream picks - and this project has been here before (8386c8f: a
    // decoder shifting by an unvalidated exponent). A contract that only a
    // debug assert enforces is not enforced in the builds that ship.
    //
    // bap is filled rather than left alone: it is the caller's output, and
    // "no allocation" is what an unreadable region gets, the same answer
    // §7.2.2.1.1's all-zero-SNR case gives just below.
    //
    // region.start joins the same guard rather than waiting to be reported
    // separately: it comes from the same stream (cplstrtmant, spx_startmant),
    // it indexes kMaskTab directly two statements after the size check, and
    // the assert immediately below already states the range - so leaving it
    // to that assert alone would repeat the exact mistake the two findings
    // above were.
    if (exps.empty() || exps.size() > kMaxMantissas || region.start < 0 ||
        static_cast<std::size_t>(region.start) >= exps.size()) {
        std::ranges::fill(bap, std::uint8_t{0});
        return;
    }
    const int end = static_cast<int>(exps.size());
    assert(end >= 1 && end <= static_cast<int>(kMaxMantissas));
    assert(region.start >= 0 && region.start < end);

    // §7.2.2.1.1 special case: when EVERY SNR offset in the block is zero,
    // the whole bap array is zero and no allocation runs. The condition spans
    // all channels, so the caller supplies it.
    if (region.snr_all_zero) {
        std::ranges::fill(bap, std::uint8_t{0});
        return;
    }

    const int sdecay = kSlowDec[static_cast<std::size_t>(codes.sdcycod)];
    const int fdecay = kFastDec[static_cast<std::size_t>(codes.fdcycod)];
    const int sgain = kSlowGain[static_cast<std::size_t>(codes.sgaincod)];
    const int dbknee = kDbPerBit[static_cast<std::size_t>(codes.dbpbcod)];
    int floor = kFloor[static_cast<std::size_t>(codes.floorcod)];
    if (floor >= 0x8000) {
        floor -= 0x10000;  // 0xf800 is a negative 16-bit value (-2048)
    }
    const int fgain = kFastGain[static_cast<std::size_t>(codes.fgaincod)];
    const int snroffset = snr_offset(csnroffst, fsnroffst);
    const int kStart = region.start;

    // §7.2.2.2: exponents -> 13-bit signed log PSD. exponents_to_psd's own
    // SIMD store writes std::int32_t, which on a 32-bit target
    // (arm-none-eabi, where the minimum-footprint decoder profile runs) is
    // `long` rather than `int` - two different types, so a separate buffer
    // and an explicit copy into the plain-`int` one band_psd (exported,
    // ac3/core/bitalloc.hpp) and the rest of this function already use, is
    // needed rather than widening that public signature for one caller.
    std::array<std::int32_t, kMaxMantissas> psd_wide{};
    exponents_to_psd(exps, kStart, end, psd_wide);
    std::array<int, kMaxMantissas> psd{};
    std::ranges::copy(psd_wide, psd.begin());

    // §7.2.2.3: banded integration via log-addition.
    const std::array<int, 50> bndpsd = band_psd(psd, kStart, end);

    // §7.2.2.4: excitation function. Two shapes: fbw/LFE channels start at
    // band 0 and run the lowcomp low-frequency compensation, the coupling
    // channel starts higher and seeds its leaks instead.
    const int bndstrt = kMaskTab[static_cast<std::size_t>(kStart)];
    const int bndend = kMaskTab[static_cast<std::size_t>(end - 1)] + 1;
    std::array<int, 50> excite{};
    int lowcomp = 0;
    int fastleak = 0;
    int slowleak = 0;
    int begin_band = bndstrt;
    if (region.coupling) {
        // §7.2.2.1 / §7.2.2.4: the coupling channel starts above the
        // low-frequency region entirely, so it skips the lowcomp machinery
        // and instead seeds the leak state from the transmitted cplfleak /
        // cplsleak, continuing the decay from wherever the fbw channels left
        // off below the coupling frequency.
        fastleak = (region.cplfleak << 8) + 768;
        slowleak = (region.cplsleak << 8) + 768;
    } else {
        assert(bndstrt == 0);
        // §7.2.2.4: for the LFE channel (bndend == 7), calc_lowcomp and the
        // monotone-rise break check are skipped for the last band (bin 6) —
        // bndpsd[7] does not exist there.
        const auto not_lfe_last = [bndend](int bin) { return bndend != 7 || bin != 6; };
        lowcomp = calc_lowcomp(lowcomp, bndpsd[0], bndpsd[1], 0);
        excite[0] = bndpsd[0] - fgain - lowcomp;
        lowcomp = calc_lowcomp(lowcomp, bndpsd[1], bndpsd[2], 1);
        excite[1] = bndpsd[1] - fgain - lowcomp;
        int begin = 7;
        for (int bin = 2; bin < 7; ++bin) {
            if (not_lfe_last(bin)) {
                lowcomp = calc_lowcomp(lowcomp, bndpsd[static_cast<std::size_t>(bin)],
                                       bndpsd[static_cast<std::size_t>(bin) + 1], bin);
            }
            fastleak = bndpsd[static_cast<std::size_t>(bin)] - fgain;
            slowleak = bndpsd[static_cast<std::size_t>(bin)] - sgain;
            excite[static_cast<std::size_t>(bin)] = fastleak - lowcomp;
            if (not_lfe_last(bin) &&
                bndpsd[static_cast<std::size_t>(bin)] <= bndpsd[static_cast<std::size_t>(bin) + 1]) {
                begin = bin + 1;
                break;
            }
        }
        for (int bin = begin; bin < std::min(bndend, 22); ++bin) {
            if (not_lfe_last(bin)) {
                lowcomp = calc_lowcomp(lowcomp, bndpsd[static_cast<std::size_t>(bin)],
                                       bndpsd[static_cast<std::size_t>(bin) + 1], bin);
            }
            fastleak -= fdecay;
            fastleak = std::max(fastleak, bndpsd[static_cast<std::size_t>(bin)] - fgain);
            slowleak -= sdecay;
            slowleak = std::max(slowleak, bndpsd[static_cast<std::size_t>(bin)] - sgain);
            excite[static_cast<std::size_t>(bin)] = std::max(fastleak - lowcomp, slowleak);
        }
        begin_band = 22;
    }

    // The common upper region: no lowcomp, plain dual-leak decay. For fbw
    // channels this picks up at band 22; for the coupling channel it is the
    // whole range.
    for (int bin = begin_band; bin < bndend; ++bin) {
        fastleak -= fdecay;
        fastleak = std::max(fastleak, bndpsd[static_cast<std::size_t>(bin)] - fgain);
        slowleak -= sdecay;
        slowleak = std::max(slowleak, bndpsd[static_cast<std::size_t>(bin)] - sgain);
        excite[static_cast<std::size_t>(bin)] = std::max(fastleak, slowleak);
    }

    // §7.2.2.5: masking curve (excitation, dB knee boost, hearing threshold).
    std::array<int, 50> mask{};
    const auto& hth = *kHearingThreshold[static_cast<std::size_t>(fscod_family(sample_rate))];
    for (int bin = bndstrt; bin < bndend; ++bin) {
        if (bndpsd[static_cast<std::size_t>(bin)] < dbknee) {
            excite[static_cast<std::size_t>(bin)] +=
                (dbknee - bndpsd[static_cast<std::size_t>(bin)]) >> 2;
        }
        mask[static_cast<std::size_t>(bin)] =
            std::max<int>(excite[static_cast<std::size_t>(bin)],
                          hth[static_cast<std::size_t>(bin)]);
    }

    // §7.2.2.6: delta bit allocation. mask[]/psd[] units are 128 per exponent
    // step, which is exactly one Table 5.17 6 dB step, so `delta` below is
    // added directly with no unit conversion. `region.delta.deltnseg == 0`
    // (the default) makes this a no-op, matching the spec's own recommended
    // reset state.
    //
    // The spec pseudocode initializes `band = 0` literally, but mask[] here
    // is this routine's own global-indexed array (Table 7.13's bin-to-band
    // map, the same one bndstrt/bndend above come from) - for the coupling
    // channel, whose bndstrt is not 0, a literal reading would need every
    // deltoffst to encode an absolute band number, which two independent
    // real-world decoders disagree with: both FFmpeg's ff_ac3_bit_alloc_calc_mask()
    // and Dolby's own reference decoder (dlbac3dec, verified directly via
    // gst-launch) reject a coupling-channel delta stream built on that
    // reading and accept one where band starts at bndstrt instead - the
    // same kind of pseudocode erratum as calc_lowcomp's stray semicolon
    // above. choose_delta_segments() below matches this.
    {
        int band = bndstrt;
        for (int seg = 0; seg < region.delta.deltnseg; ++seg) {
            band += region.delta.deltoffst[static_cast<std::size_t>(seg)];
            const int code = region.delta.deltba[static_cast<std::size_t>(seg)];
            const int delta = (code >= 4 ? code - 3 : code - 4) << 7;
            const int len = region.delta.deltlen[static_cast<std::size_t>(seg)];
            // Bitstream-level bounds are enforced by the decoder's own
            // parser before this ever runs (deltoffst/deltlen are
            // attacker-controlled, mask[] is exactly 50 bands wide) - this
            // is a defense-in-depth backstop that must hold even in the
            // CI static-analysis build, which defines NDEBUG and would
            // silently compile an assert here away.
            if (band < 0 || band + len > 50) {
                break;
            }
            for (int k = 0; k < len; ++k) {
                mask[static_cast<std::size_t>(band)] += delta;
                ++band;
            }
        }
    }

    // §7.2.2.7: bap computation. The snroffset/floor/truncation order is
    // normative: subtract snroffset, subtract floor, clamp at zero, truncate
    // with & 0x1fe0, re-add floor.
    {
        int i = kStart;
        int j = kMaskTab[static_cast<std::size_t>(kStart)];
        int lastbin = 0;
        do {
            lastbin = std::min<int>(kBandStart[static_cast<std::size_t>(j)] +
                                        kBandSize[static_cast<std::size_t>(j)],
                                    end);
            int m = mask[static_cast<std::size_t>(j)];
            m -= snroffset;
            m -= floor;
            if (m < 0) {
                m = 0;
            }
            m &= 0x1fe0;
            m += floor;
            for (int k = i; k < lastbin; ++k) {
                int address = (psd[static_cast<std::size_t>(i)] - m) >> 5;
                address = std::min(63, std::max(0, address));
                bap[static_cast<std::size_t>(i)] =
                    region.high_efficiency
                        ? kHeBapTab[static_cast<std::size_t>(address)]
                        : kBapTab[static_cast<std::size_t>(address)];
                ++i;
            }
            ++j;
        } while (end > lastbin);
    }
}

DeltaSegments choose_delta_segments(std::span<const double> coefficients,
                                    std::span<const std::uint8_t> exps, int start) {
    AC3_ZONE_SCOPED_N("choose_delta_segments");
    assert(coefficients.size() == exps.size());
    const int end = static_cast<int>(exps.size());
    assert(end >= 1 && end <= static_cast<int>(kMaxMantissas));
    assert(start >= 0 && start < end);

    // Two psd curves in identical units: the flat one compute_bit_allocation
    // itself would build from exps alone, and one built from the real
    // pre-quantization coefficient magnitude. Table 5.17's 128-units-per-6dB
    // step is exactly one exponent step (§7.2.2.2's psd = 3072 - exp<<7), so
    // exponent = -1 - log2(|c|) gives real_psd = 3200 + 128*log2(|c|).
    // See compute_bit_allocation's own sibling call above for why
    // exponents_to_psd's result needs a separate wide buffer and an explicit
    // copy rather than landing straight in a plain `int` one on a 32-bit
    // target.
    std::array<std::int32_t, kMaxMantissas> psd_wide{};
    exponents_to_psd(exps, start, end, psd_wide);
    std::array<int, kMaxMantissas> psd{};
    std::ranges::copy(psd_wide, psd.begin());
    std::array<int, kMaxMantissas> real_psd{};
    for (int bin = start; bin < end; ++bin) {
        const auto i = static_cast<std::size_t>(bin);
        const double magnitude = std::abs(static_cast<double>(coefficients[i]));
        real_psd[i] = magnitude > 0.0
                          ? static_cast<int>(std::lround(3200.0 + 128.0 * std::log2(magnitude)))
                          : psd[i];  // silence: nothing to correct
    }

    const std::array<int, 50> bndpsd = band_psd(psd, start, end);
    const std::array<int, 50> real_bndpsd = band_psd(real_psd, start, end);
    const int bndstrt = kMaskTab[static_cast<std::size_t>(start)];
    const int bndend = kMaskTab[static_cast<std::size_t>(end - 1)] + 1;

    // Merge bands whose correction rounds to the same code into runs, so
    // adjacent agreement costs one segment instead of one per band.
    std::vector<DeltaRun> runs;
    for (int band = bndstrt; band < bndend; ++band) {
        const auto b = static_cast<std::size_t>(band);
        const auto code = delta_code_for(real_bndpsd[b] - bndpsd[b]);
        if (!code) {
            continue;
        }
        if (!runs.empty() && runs.back().band + runs.back().length == band &&
            runs.back().code == *code) {
            ++runs.back().length;
        } else {
            runs.push_back({.band = band, .length = 1, .code = *code});
        }
    }
    if (runs.empty()) {
        return {};
    }
    // §5.4.3.54/E2.3.2.9: at most 8 segments (3-bit deltnseg field + 1). Keep
    // the largest-magnitude corrections when more runs qualify.
    if (runs.size() > 8) {
        std::ranges::nth_element(runs, runs.begin() + 8, std::ranges::greater{},
                                 [](const DeltaRun& r) { return std::abs(2 * r.code - 7); });
        runs.resize(8);
        std::ranges::sort(runs, {}, &DeltaRun::band);
    }

    // Encode each run as (offset, length, code). deltoffst is 5 bits (max
    // 31): a wider gap is bridged with inert (deltlen == 0) filler segments
    // that only advance the cursor. deltlen is 4 bits (max 15): a longer run
    // splits into consecutive zero-offset chunks of the same code. Either can
    // push the total past the 8-segment cap in rare cases (a correction
    // stranded far past band 31, or one spanning most of the spectrum) - the
    // segment-count truncation above already prioritised by magnitude, so
    // this second, band-order-driven cutoff only ever bites the tail of an
    // already-large run and is documented rather than silently mis-encoded.
    //
    // cursor starts at bndstrt, not 0: see compute_bit_allocation()'s
    // matching note on its own delta band cursor for why. For fbw/LFE
    // (bndstrt == 0) this is unchanged.
    DeltaSegments out;
    int cursor = bndstrt;
    for (const auto& run : runs) {
        int band = run.band;
        int remaining = run.length;
        while (remaining > 0) {
            while (band - cursor > 31) {
                if (out.deltnseg >= 8) {
                    return out;
                }
                out.deltoffst[static_cast<std::size_t>(out.deltnseg)] = 31;
                out.deltlen[static_cast<std::size_t>(out.deltnseg)] = 0;
                out.deltba[static_cast<std::size_t>(out.deltnseg)] = 4;  // inert: deltlen == 0
                ++out.deltnseg;
                cursor += 31;
            }
            if (out.deltnseg >= 8) {
                return out;
            }
            const int len = std::min(remaining, 15);
            out.deltoffst[static_cast<std::size_t>(out.deltnseg)] =
                static_cast<std::uint8_t>(band - cursor);
            out.deltlen[static_cast<std::size_t>(out.deltnseg)] = static_cast<std::uint8_t>(len);
            out.deltba[static_cast<std::size_t>(out.deltnseg)] =
                static_cast<std::uint8_t>(run.code);
            ++out.deltnseg;
            cursor = band + len;
            band += len;
            remaining -= len;
        }
    }
    return out;
}

}  // namespace ac3
