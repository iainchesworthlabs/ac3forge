#include "ac3/oba/joc.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/dsp/qmf.hpp"
#include "ac3/oba/joc_tables.hpp"

namespace ac3::joc {

namespace {

// §6.6.4: joc_mix_mtx_dq = (q - nquant/2) * 820 / (4096 * (1 + quant_idx)).
[[nodiscard]] constexpr int quant_steps(bool fine) { return fine ? 192 : 96; }
[[nodiscard]] constexpr double quant_scale(bool fine) {
    return 820.0 / (4096.0 * (fine ? 2.0 : 1.0));
}

void put_code(BitWriter& w, const HuffCode& code) {
    w.put(code.code, code.bits);
}

// §6.6.3 Pseudocode 4, decoding by longest match rather than walking a tree:
// a prefix code is uniquely determined by its (code, length) pairs, so this
// is equivalent to the normative tree and was how tests/oba/test_oba.cpp
// originally validated the generated encode tables (kMtxCoarse/kMtxFine were
// inverted FROM those trees, so agreeing with an independent forward walk of
// them, not with build_payload's own logic, is what that test proved). This
// promotes the same algorithm to production decode.
[[nodiscard]] int huff_decode(std::span<const HuffCode> table, BitReader& r) {
    std::uint32_t accumulated = 0;
    for (int bits = 1; bits <= 32; ++bits) {
        accumulated = (accumulated << 1) | r.read_bit();
        if (r.overflowed()) {
            return -1;
        }
        for (std::size_t value = 0; value < table.size(); ++value) {
            if (table[value].bits == bits && table[value].code == accumulated) {
                return static_cast<int>(value);
            }
        }
    }
    return -1;
}

}  // namespace

int quantize(double coefficient, bool fine_quant) {
    const int steps = quant_steps(fine_quant);
    const long code = std::lround(coefficient / quant_scale(fine_quant)) + steps / 2;
    return static_cast<int>(std::clamp(code, 0L, static_cast<long>(steps) - 1));
}

double dequantize(int code, bool fine_quant) {
    // Both step counts are even, so the origin is exact and the subtraction
    // stays in integers until the scale is applied.
    const int origin = quant_steps(fine_quant) / 2;
    return static_cast<double>(code - origin) * quant_scale(fine_quant);
}

std::vector<std::byte> build_payload(const FrameParameters& params) {
    assert(params.objects >= 1 && params.objects <= kMaxObjects);
    assert(params.channels == kNumChannels5X);
    assert(params.matrix.size() == params.coefficient_count());

    const int bands = params.bands();
    const int steps = quant_steps(params.fine_quant);
    // The two tables differ in length, so they only meet as a span.
    const std::span<const HuffCode> table =
        params.fine_quant ? std::span<const HuffCode>{kMtxFine}
                          : std::span<const HuffCode>{kMtxCoarse};

    BitWriter w;

    // --- joc_header (§6.2.2) ---
    w.put(kDmxConfig5X, 3);
    w.put(static_cast<std::uint32_t>(params.objects - 1), 6);  // joc_num_objects_bits
    w.put(0, 3);  // joc_ext_config_idx: no extensional configuration data

    // --- joc_info (§6.2.3) ---
    // §6.3.3.2's equation renders ambiguously in the published PDF - the
    // fraction, the power of two and the bracketing all collide - but every
    // reading of it agrees that zero for both fields is joc_clipgain = 1. This
    // encoder applies no clip protection, so unity is the honest value and the
    // ambiguity does not bite.
    w.put(0, 3);  // joc_clipgain_x_bits
    w.put(0, 5);  // joc_clipgain_y_bits
    w.put(static_cast<std::uint32_t>(params.seq_count), 10);

    for (int object = 0; object < params.objects; ++object) {
        w.put(1, 1);  // b_joc_obj_present: every object is coded every frame
        w.put(static_cast<std::uint32_t>(params.num_bands_idx), 3);
        // Sparse mode codes one channel per band and gives every other channel
        // a fixed value - and that value is joc_num_quant/2 + 2 (§6.6.2), not
        // the quantizer's zero, so the channels it does not name still leak
        // about 0,4 into the object. The whole-matrix mode says what it means
        // for every channel, which for a downmix this encoder built itself is
        // both cheap enough and exactly right.
        w.put(0, 1);  // b_joc_sparse
        w.put(params.fine_quant ? 1u : 0u, 1);  // joc_num_quant_idx

        // --- joc_data_point_info (§6.2.4) ---
        // Smooth interpolation with a single data point: §6.6.5 then ramps the
        // matrix linearly from the previous frame's values across all the
        // frame's QMF timeslots. A steep slope would step at the frame edge,
        // which is audible on a moving object; two data points would let the
        // ramp bend mid-frame, which one OAMD update per frame cannot use.
        w.put(0, 1);  // joc_slope_idx: smooth
        w.put(0, 1);  // joc_num_dpoints_bits => one data point
    }

    // --- joc_data (§6.2.5) ---
    // §6.6.2 Pseudocode 3 runs the differential the other way: the decoder
    // accumulates modulo nquant along the bands, starting from nquant/2 - the
    // quantizer's zero - so the first band's codeword is the coefficient
    // itself and every later one is a step. Working modulo nquant means the
    // difference always fits the alphabet, however far apart two bands are.
    for (int object = 0; object < params.objects; ++object) {
        // One offset walk per object, not one per coefficient encoded - see
        // FrameParameters::ObjectMatrixView.
        const auto view = params.object_view(object);
        for (int channel = 0; channel < params.channels; ++channel) {
            int previous = steps / 2;
            for (int band = 0; band < bands; ++band) {
                const int code = quantize(view.at(0, channel, band), params.fine_quant);
                const int difference = ((code - previous) % steps + steps) % steps;
                put_code(w, table[static_cast<std::size_t>(difference)]);
                previous = code;
            }
        }
    }

    return w.take();  // padding_bits (§6.2.1) to the byte boundary
}

std::optional<FrameParameters> parse_payload(std::span<const std::byte> payload) {
    BitReader r{payload};

    // --- joc_header (§6.2.2) ---
    const int dmx_config_idx = static_cast<int>(r.read(3));
    const int channels = dmx_channel_count(dmx_config_idx);
    if (channels == 0) {
        // Table 48 gives indices 5..7 no channel count, so joc_data has no
        // loop bound and nothing after this point can be located.
        return std::nullopt;
    }
    const int objects = static_cast<int>(r.read(6)) + 1;  // joc_num_objects_bits
    if (objects > kMaxObjects) {
        return std::nullopt;
    }
    if (r.read(3) != 0) {
        // joc_ext_config_idx. Table 49 reserves every nonzero value and
        // §6.2.1 gives joc_ext_data() no syntax and no length, so there is
        // nothing to read and nothing to skip.
        return std::nullopt;
    }

    // --- joc_info (§6.2.3) ---
    // §6.3.3.2 renders ambiguously in the published PDF - the fraction, the
    // power of two and the bracketing all collide - and the only reading the
    // fragments support, (1 + y/32) * 2^x, does not agree with the clause's
    // own stated range of [1; 8,75]: a real DEE stream sends x = 4, y = 0,
    // which that reading makes 16. Nothing in TS 103 420 says where in the
    // decode chain the gain is applied either. So it is computed, reported
    // and left alone rather than folded into audio on a formula this
    // codebase cannot verify.
    const auto clipgain_x = r.read(3);
    const auto clipgain_y = r.read(5);
    const double clip_gain =
        (1.0 + static_cast<double>(clipgain_y) / 32.0) * std::exp2(static_cast<double>(clipgain_x));
    const int seq_count = static_cast<int>(r.read(10));

    FrameParameters params;
    params.objects = objects;
    params.channels = channels;
    params.dmx_config_idx = dmx_config_idx;
    params.clip_gain = clip_gain;
    params.seq_count = seq_count;
    params.shapes.assign(static_cast<std::size_t>(objects), ObjectShape{});

    // Object 0's own fields, captured as scalars rather than read back from
    // `shapes` afterward - `objects` is always >= 1 (joc_num_objects_bits + 1)
    // so params.shapes is never empty, but a read through it later is exactly
    // the shape GCC's -Wnull-dereference cannot see through at -O3.
    int first_num_bands_idx = params.num_bands_idx;
    bool first_fine_quant = params.fine_quant;

    for (int object = 0; object < objects; ++object) {
        auto& shape = params.shapes[static_cast<std::size_t>(object)];
        shape.present = r.read(1) != 0;  // b_joc_obj_present
        if (!shape.present) {
            shape.data_points = 0;
            continue;
        }
        shape.num_bands_idx = static_cast<int>(r.read(3));
        shape.sparse = r.read(1) != 0;             // b_joc_sparse
        shape.fine_quant = r.read(1) != 0;         // joc_num_quant_idx
        // --- joc_data_point_info (§6.2.4) ---
        shape.steep = r.read(1) != 0;              // joc_slope_idx, Table 52
        shape.data_points = static_cast<int>(r.read(1)) + 1;
        if (shape.steep) {
            for (int dp = 0; dp < shape.data_points; ++dp) {
                // §6.3.4.4: joc_offset_ts = joc_offset_ts_bits + 1.
                shape.offset_ts[static_cast<std::size_t>(dp)] = static_cast<int>(r.read(5)) + 1;
            }
        }
        if (object == 0) {
            first_num_bands_idx = shape.num_bands_idx;
            first_fine_quant = shape.fine_quant;
        }
    }

    // The frame-wide fields stay meaningful for the uniform case every
    // in-repo caller works with; `shapes` is authoritative regardless.
    params.num_bands_idx = first_num_bands_idx;
    params.fine_quant = first_fine_quant;
    params.matrix.assign(params.coefficient_count(), 0.0);

    // --- joc_data (§6.2.5) ---
    for (int object = 0; object < objects; ++object) {
        const auto& shape = params.shapes[static_cast<std::size_t>(object)];
        if (!shape.present) {
            continue;
        }
        const int steps = quant_steps(shape.fine_quant);
        const int bands = shape.bands();
        // One offset walk per object rather than one per coefficient written
        // - parse fills channels * bands * data_points of them, and at()
        // re-walks every earlier object's sizes on each call. See
        // FrameParameters::ObjectMatrixView.
        const auto wview = params.object_view_mut(object);
        for (int dp = 0; dp < shape.data_points; ++dp) {
            if (shape.sparse) {
                // §6.2.5: one raw 3-bit channel index for band 0, then a
                // Huffman codeword per remaining band, then one coefficient
                // codeword per band. §6.6.2 Pseudocode 2 turns the pair into
                // a single named channel per band, every other channel
                // holding the sparse offset.
                const std::span<const HuffCode> idx_table =
                    channels == kNumChannels5X ? std::span<const HuffCode>{kIdx5ch}
                                               : std::span<const HuffCode>{kIdx7ch};
                std::array<int, 23> channel_idx{};  // kNumBands caps at 23
                channel_idx[0] = static_cast<int>(r.read(3));
                if (channel_idx[0] >= channels) {
                    return std::nullopt;
                }
                for (int band = 1; band < bands; ++band) {
                    const int value = huff_decode(idx_table, r);
                    if (value < 0) {
                        return std::nullopt;
                    }
                    channel_idx[static_cast<std::size_t>(band)] = value;
                }
                // §6.6.2 offset: 50 coarse / 100 fine, which is NOT the
                // quantizer's zero - both dequantize to about +0,4, so the
                // channels a band does not name still leak into the object.
                const int sparse_offset = shape.fine_quant ? 100 : 50;
                const std::span<const HuffCode> vec_table =
                    shape.fine_quant ? std::span<const HuffCode>{kVecFine}
                                     : std::span<const HuffCode>{kVecCoarse};
                for (int channel = 0; channel < channels; ++channel) {
                    for (int band = 0; band < bands; ++band) {
                        wview.at(dp, channel, band) =
                            dequantize(sparse_offset, shape.fine_quant);
                    }
                }
                // Pseudocode 2's coefficient differential is against
                // joc_mix_mtx_q[ch][pb-1] for the channel THIS band names -
                // which is the previous band's decoded value only if that
                // band named the same channel, and the sparse offset
                // otherwise, since that is what every unnamed channel holds.
                // A single running "previous" would carry a value across a
                // channel change that the clause never puts there.
                std::array<int, kMaxChannels> previous{};
                previous.fill(sparse_offset);
                for (int band = 0; band < bands; ++band) {
                    const int value = huff_decode(vec_table, r);
                    if (value < 0) {
                        return std::nullopt;
                    }
                    // Pseudocode 2 names the RAW transmitted index of the
                    // previous band here, not the reconstructed one - and
                    // joc_channel_idx_mod is a scalar recomputed each band,
                    // with no array of reconstructed values to accumulate
                    // from, so that is deliberate rather than a typo.
                    const int raw = channel_idx[static_cast<std::size_t>(band)];
                    const int named =
                        band == 0 ? raw
                                  : (channel_idx[static_cast<std::size_t>(band - 1)] + raw) %
                                        channels;
                    const int code =
                        (previous[static_cast<std::size_t>(named)] + value) % steps;
                    previous.fill(sparse_offset);
                    previous[static_cast<std::size_t>(named)] = code;
                    wview.at(dp, named, band) = dequantize(code, shape.fine_quant);
                }
            } else {
                // §6.6.2 Pseudocode 3 runs the differential the other way:
                // the decoder accumulates modulo nquant along the bands,
                // starting from nquant/2 - the quantizer's zero - so the
                // first band's codeword is the coefficient itself and every
                // later one is a step.
                const std::span<const HuffCode> table =
                    shape.fine_quant ? std::span<const HuffCode>{kMtxFine}
                                     : std::span<const HuffCode>{kMtxCoarse};
                for (int channel = 0; channel < channels; ++channel) {
                    int previous = steps / 2;
                    for (int band = 0; band < bands; ++band) {
                        const int difference = huff_decode(table, r);
                        if (difference < 0) {
                            return std::nullopt;
                        }
                        const int code = (previous + difference) % steps;
                        previous = code;
                        wview.at(dp, channel, band) = dequantize(code, shape.fine_quant);
                    }
                }
            }
        }
    }

    // At most a byte of padding_bits should remain (§6.2.1), same bound
    // tests/oba/test_oba.cpp's own encode-side test holds build_payload to - a
    // corrupt object/band count that made this decode stop short leaves
    // more than that unaccounted for.
    if (r.overflowed() || payload.size() * 8 - r.bit_position() >= 8) {
        return std::nullopt;
    }
    return params;
}

namespace {

// §6.6.5 Pseudocode 6, for one (object, channel, subband) at one timeslot,
// within a smooth-interpolation segment whose two endpoints are `previous`
// (joc_mix_mtx_prev) and this object's own `dq` (its one or two transmitted
// data points). `ts` is this object's own position in ITS frame's 24-
// timeslot window - see each caller for how it maps its own loop variable
// onto that window.
[[nodiscard]] double interpolate(const ObjectShape& shape, double previous,
                                 std::span<const double, kMaxDataPoints> dq, int ts) {
    if (!shape.steep) {
        if (shape.data_points == 1) {
            return previous + static_cast<double>(ts + 1) * (dq[0] - previous) /
                                  static_cast<double>(kQmfTimeslots);
        }
        constexpr int kHalf = kQmfTimeslots / 2;
        if (ts < kHalf) {
            return previous +
                   static_cast<double>(ts + 1) * (dq[0] - previous) / static_cast<double>(kHalf);
        }
        return dq[0] + static_cast<double>(ts - kHalf + 1) * (dq[1] - dq[0]) /
                           static_cast<double>(kQmfTimeslots - kHalf);
    }
    if (ts < shape.offset_ts[0]) {
        return previous;
    }
    if (shape.data_points == 1 || ts < shape.offset_ts[1]) {
        return dq[0];
    }
    return dq[1];
}

// Domain::kMdctBand. Per-object band count, quantizer, sparse mode,
// interpolation slope and data-point count - everything parse_payload can
// now produce - applied inside the same block-granular MDCT reconstruction
// this domain has always used. `previous_matrix` is kept per QMF SUBBAND
// rather than per parameter band (objects * channels * kQmfSubbands), which
// is what lets an object change its band count from one frame to the next
// and still have something meaningful to ramp from.
[[nodiscard]] std::vector<std::vector<float>> reconstruct_mdct_band(
    std::span<const std::span<const float>> bed, const FrameParameters& params,
    ReconstructionState& state, bool fast_mdct, bool fast_imdct) {
    const int objects = params.objects;
    const int channels = params.channels;

    // §6.3.3.3: no ramp on the first frame or right after a splice, and
    // equally none if the previous frame's matrix does not even have the
    // same shape to ramp from (an object or channel count change this
    // project's own AtmosEncoder never makes mid-stream, but a general JOC
    // stream could in principle) - both collapse to "this frame's matrix
    // applies to the whole frame outright", the same as ReconstructionState's
    // own default-constructed (never-reconstructed-before) state.
    const std::size_t previous_size = static_cast<std::size_t>(objects) *
                                      static_cast<std::size_t>(channels) *
                                      static_cast<std::size_t>(kQmfSubbands);
    const bool has_ramp = params.seq_count != 0 && state.previous_objects == objects &&
                          state.previous_channels == channels &&
                          state.previous_matrix.size() == previous_size;
    if (state.previous_matrix.size() != previous_size) {
        state.previous_matrix.assign(previous_size, 0.0);
    }

    if (static_cast<int>(state.object_history.size()) != objects) {
        // A changed object count invalidates any old per-object history
        // anyway (index i no longer names the same object), so this also
        // covers the very first call, where object_history starts empty.
        state.object_history.assign(static_cast<std::size_t>(objects), {});
    }

    std::vector<std::vector<float>> out(
        static_cast<std::size_t>(objects),
        std::vector<float>(static_cast<std::size_t>(kSamplesPerFrame)));

    auto& bed_mdct = state.bed_mdct_scratch;
    auto& time = state.time_scratch;
    auto& windowed = state.windowed_scratch;
    auto& object_mdct = state.object_mdct_scratch;
    auto& x = state.synth_scratch;

    for (int block = 0; block < kBlocksPerFrame; ++block) {
        // --- analyze this block of the downmix, one MDCT per bed channel ---
        // Only block 0 ever reads negative indices (into the previous
        // frame's tail); every later block's window sits entirely inside
        // THIS frame's own already-decoded samples.
        // Gather-and-window one channel into lane `lane` of the windowed
        // scratch. Split out so the batched and one-at-a-time paths below
        // share it verbatim rather than restating the §7.9.4 window walk.
        const auto gather_and_window = [&](int ch, std::size_t lane) {
            for (int n = 0; n < 512; ++n) {
                const int index = block * kSamplesPerBlock + n - 256;
                time[static_cast<std::size_t>(n)] =
                    index >= 0
                        ? static_cast<double>(
                              bed[static_cast<std::size_t>(ch)][static_cast<std::size_t>(index)])
                        : state.bed_history[static_cast<std::size_t>(ch)]
                                           [static_cast<std::size_t>(256 + index)];
            }
            apply_analysis_window(time, windowed[lane]);
        };
        // Four channels' forward transforms at a time (ROADMAP PF5 phase
        // 4c), the forward twin of the object loop's batching below:
        // mdct512_forward_batch4 checks has_avx2() internally and falls
        // back to four ordinary calls, so this is bit-identical either
        // way. channels is kNumChannels5X = 5, so this is one batch of
        // four plus one ordinary call; mode=reference (fast_mdct false)
        // never batches, exactly as the object loop does not.
        int bed_ch = 0;
        while (bed_ch < channels) {
            if (fast_mdct && bed_ch + 4 <= channels) {
                for (std::size_t lane = 0; lane < 4; ++lane) {
                    gather_and_window(bed_ch + static_cast<int>(lane), lane);
                }
                mdct512_forward_batch4(windowed[0], windowed[1], windowed[2], windowed[3],
                                       bed_mdct[static_cast<std::size_t>(bed_ch)],
                                       bed_mdct[static_cast<std::size_t>(bed_ch + 1)],
                                       bed_mdct[static_cast<std::size_t>(bed_ch + 2)],
                                       bed_mdct[static_cast<std::size_t>(bed_ch + 3)]);
                bed_ch += 4;
                continue;
            }
            gather_and_window(bed_ch, 0);
            mdct512_forward(windowed[0], bed_mdct[static_cast<std::size_t>(bed_ch)], fast_mdct);
            ++bed_ch;
        }

        // §6.6.5 counts in QMF timeslots, four to a 256-sample block. Taking
        // each block's LAST timeslot keeps the smooth single-data-point case
        // exactly the (block + 1) / kBlocksPerFrame ramp this used to
        // compute, and atmos.cpp's own bed ramp still agrees with it.
        const int ts = (block + 1) * kQmfTimeslots / kBlocksPerFrame - 1;

        // §6.6.6 spectrum accumulation, unchanged, but into
        // object_mdct[object] rather than a single shared scratch: every
        // present object's spectrum now coexists once this pass finishes,
        // which is what lets the synthesis pass below batch four at a time
        // instead of one at a time (ROADMAP PF5's batch-axis follow-on).
        // Absent objects drain their overlap tail immediately here, same as
        // before, and never enter `present` below - synthesis only ever
        // runs on objects that actually have a spectrum to transform.
        std::array<int, kMaxObjects> present{};
        int n_present = 0;
        for (int object = 0; object < objects; ++object) {
            const auto shape = params.shape(object);
            if (!shape.present) {
                // Nothing was coded for this object this frame. Its overlap
                // tail still has to drain, or the next frame it reappears in
                // would start from a stale one.
                auto& pcm = out[static_cast<std::size_t>(object)];
                auto& history = state.object_history[static_cast<std::size_t>(object)];
                for (int n = 0; n < kSamplesPerBlock; ++n) {
                    pcm[static_cast<std::size_t>(block * kSamplesPerBlock + n)] =
                        static_cast<float>(2.0 * history[static_cast<std::size_t>(n)]);
                    history[static_cast<std::size_t>(n)] = 0.0;
                }
                continue;
            }
            present[static_cast<std::size_t>(n_present++)] = object;
            // One offset walk per object per block instead of one per
            // coefficient read - see FrameParameters::ObjectMatrixView.
            const auto view = params.object_view(object);
            const auto& mapping = kSubbandToBand[static_cast<std::size_t>(shape.num_bands_idx)];

            // --- §6.6.6: this object's spectrum is a per-band linear
            // combination of the downmix's ---
            //
            // band (and therefore each channel's mixing coefficient m) is
            // constant across the 4 MDCT bins one QMF subband covers - the
            // interpolation/ramp state a bin's own coefficient depends on is
            // keyed by (object, channel, subband), never by bin. The loop
            // below computes each channel's m once per subband and reuses
            // it across those 4 bins instead of recomputing an identical
            // value 4 times; the per-bin summation itself (order, operands)
            // is untouched, so this is the same arithmetic, done less often.
            std::array<double, kMaxChannels> m{};
            for (int subband = 0; subband < kQmfSubbands; ++subband) {
                const int band = mapping[static_cast<std::size_t>(subband)];
                for (int ch = 0; ch < channels; ++ch) {
                    const std::array<double, kMaxDataPoints> dq = {
                        view.at(0, ch, band),
                        shape.data_points > 1 ? view.at(1, ch, band) : view.at(0, ch, band)};
                    const std::size_t previous_index =
                        (static_cast<std::size_t>(object) * static_cast<std::size_t>(channels) +
                         static_cast<std::size_t>(ch)) *
                            static_cast<std::size_t>(kQmfSubbands) +
                        static_cast<std::size_t>(subband);
                    const double previous =
                        has_ramp ? state.previous_matrix[previous_index] : dq[0];
                    m[static_cast<std::size_t>(ch)] =
                        has_ramp ? interpolate(shape, previous, dq, ts)
                                 : dq[static_cast<std::size_t>(shape.data_points - 1)];
                }
                for (int bin = subband * 4; bin < subband * 4 + 4; ++bin) {
                    double sum = 0.0;
                    for (int ch = 0; ch < channels; ++ch) {
                        sum += m[static_cast<std::size_t>(ch)] *
                               bed_mdct[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bin)];
                    }
                    object_mdct[static_cast<std::size_t>(object)][static_cast<std::size_t>(bin)] =
                        sum;
                }
            }
        }

        // --- synthesize, same overlap-add eac3_decoder.cpp's own channel
        // reconstruction uses --- four present objects at a time via
        // imdct512_windowed_batch4 (ROADMAP PF5's batch-axis follow-on: it
        // internally checks has_avx2() and falls back to four ordinary
        // calls when there is no AVX2 tier, so this is bit-identical to the
        // scalar loop either way, just potentially slower without AVX2),
        // batching only ever applies to the fast fold - fast_imdct==false
        // (mode=reference) always takes the one-at-a-time branch below, at
        // every present object, exactly as it always has.
        int idx = 0;
        while (idx < n_present) {
            if (fast_imdct && idx + 4 <= n_present) {
                const int o0 = present[static_cast<std::size_t>(idx)];
                const int o1 = present[static_cast<std::size_t>(idx + 1)];
                const int o2 = present[static_cast<std::size_t>(idx + 2)];
                const int o3 = present[static_cast<std::size_t>(idx + 3)];
                imdct512_windowed_batch4(object_mdct[static_cast<std::size_t>(o0)],
                                         object_mdct[static_cast<std::size_t>(o1)],
                                         object_mdct[static_cast<std::size_t>(o2)],
                                         object_mdct[static_cast<std::size_t>(o3)],
                                         x[static_cast<std::size_t>(o0)],
                                         x[static_cast<std::size_t>(o1)],
                                         x[static_cast<std::size_t>(o2)],
                                         x[static_cast<std::size_t>(o3)]);
                idx += 4;
                continue;
            }
            const int o = present[static_cast<std::size_t>(idx)];
            imdct512_windowed(object_mdct[static_cast<std::size_t>(o)],
                              x[static_cast<std::size_t>(o)], fast_imdct);
            ++idx;
        }

        for (int i = 0; i < n_present; ++i) {
            const int object = present[static_cast<std::size_t>(i)];
            auto& pcm = out[static_cast<std::size_t>(object)];
            auto& history = state.object_history[static_cast<std::size_t>(object)];
            const auto& xo = x[static_cast<std::size_t>(object)];
            for (int n = 0; n < kSamplesPerBlock; ++n) {
                pcm[static_cast<std::size_t>(block * kSamplesPerBlock + n)] = static_cast<float>(
                    2.0 * (xo[static_cast<std::size_t>(n)] + history[static_cast<std::size_t>(n)]));
                history[static_cast<std::size_t>(n)] = xo[static_cast<std::size_t>(256 + n)];
            }
        }
    }

    for (int ch = 0; ch < channels; ++ch) {
        for (int n = 0; n < 256; ++n) {
            state.bed_history[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] =
                static_cast<double>(
                    bed[static_cast<std::size_t>(ch)]
                       [static_cast<std::size_t>(kSamplesPerFrame - 256 + n)]);
        }
    }
    // §6.6.5's own tail: joc_mix_mtx_prev takes the LAST data point, per
    // subband rather than per parameter band, so a band-count change next
    // frame still has something to ramp from.
    for (int object = 0; object < objects; ++object) {
        const auto shape = params.shape(object);
        const auto& mapping = kSubbandToBand[static_cast<std::size_t>(shape.num_bands_idx)];
        // One offset walk per object, not one per (channel, subband) -
        // see FrameParameters::ObjectMatrixView.
        const auto wb_view = params.object_view(object);
        for (int ch = 0; ch < channels; ++ch) {
            for (int subband = 0; subband < kQmfSubbands; ++subband) {
                const std::size_t index =
                    (static_cast<std::size_t>(object) * static_cast<std::size_t>(channels) +
                     static_cast<std::size_t>(ch)) *
                        static_cast<std::size_t>(kQmfSubbands) +
                    static_cast<std::size_t>(subband);
                state.previous_matrix[index] =
                    shape.present ? wb_view.at(shape.data_points - 1, ch,
                                               mapping[static_cast<std::size_t>(subband)])
                                  : 0.0;
            }
        }
    }
    state.previous_objects = objects;
    state.previous_channels = channels;

    return out;
}

// Domain::kQmf. §6.6.6 as written: analyse the downmix into §7.1's 64
// complex subbands, take the per-band linear combination there, synthesise
// each object back. Always kNumChannels5X channels: QmfState::bed is sized
// to it, matching the one downmix width a licensed decoder ever runs this
// domain against (Table 47's 7-channel configurations need Lb/Rb from a
// dependent substream no caller of this function has in hand).
//
// One timeslot at a time rather than a frame at a time. The frame's worth
// of subband values would be 5 channels x 24 timeslots x 64 bins x 2 -
// 123 KB of scratch to hold something each object consumes immediately, so
// nothing here is buffered that does not have to be.
//
// `previous_matrix`/`older_matrix` are kept per QMF SUBBAND, the same
// reasoning reconstruct_mdct_band's own comment gives: a per-object band
// count or data-point count is free to change frame to frame, and
// subband-resolution storage never has to reinterpret what an old value
// meant under a band layout that no longer applies.
[[nodiscard]] std::vector<std::vector<float>> reconstruct_qmf(
    std::span<const std::span<const float>> bed, const FrameParameters& params,
    ReconstructionState& state) {
    static_assert(dsp::kQmfSlotsPerFrame * dsp::kQmfHop == kSamplesPerFrame,
                  "the QMF hop has to divide the frame exactly");

    const int objects = params.objects;

    if (!state.qmf) {
        state.qmf = std::make_unique<ReconstructionState::QmfState>();
    }
    auto& qmf = *state.qmf;
    if (static_cast<int>(qmf.objects.size()) != objects) {
        // Same reasoning as object_history above: a changed object count
        // means index i no longer names the same object, so its filterbank
        // tail is not worth keeping either.
        qmf.objects.assign(static_cast<std::size_t>(objects), dsp::QmfSynthesis{});
    }

    const std::size_t previous_size = static_cast<std::size_t>(objects) *
                                      static_cast<std::size_t>(kNumChannels5X) *
                                      static_cast<std::size_t>(kQmfSubbands);
    const bool has_previous = params.seq_count != 0 && state.previous_objects == objects &&
                              state.previous_channels == kNumChannels5X &&
                              state.previous_matrix.size() == previous_size;
    const bool has_older = has_previous && state.older_matrix.size() == previous_size;
    if (state.previous_matrix.size() != previous_size) {
        state.previous_matrix.assign(previous_size, 0.0);
    }

    std::vector<std::vector<float>> out(
        static_cast<std::size_t>(objects),
        std::vector<float>(static_cast<std::size_t>(kSamplesPerFrame)));

    for (int slot = 0; slot < dsp::kQmfSlotsPerFrame; ++slot) {
        for (int ch = 0; ch < kNumChannels5X; ++ch) {
            const std::span<const float, dsp::kQmfHop> hop{
                bed[static_cast<std::size_t>(ch)].data() + slot * dsp::kQmfHop,
                static_cast<std::size_t>(dsp::kQmfHop)};
            qmf.bed[static_cast<std::size_t>(ch)].push(hop,
                                                       qmf.bed_real[static_cast<std::size_t>(ch)],
                                                       qmf.bed_imag[static_cast<std::size_t>(ch)]);
        }

        // §6.6.5's ramp, but over the timeslots this call actually EMITS
        // rather than the ones it analyses. The pair's kQmfDelay means the
        // first kQmfDelaySlots of them carry the previous frame's audio, so
        // they finish out THAT frame's own ramp; only the rest belong to the
        // frame just parsed. Getting this wrong is silent - it just applies
        // every matrix 576 samples early on 37% of the audio - which is why
        // the two cases are spelled out.
        //
        // The delayed tail (previous_frame) is replayed as a plain linear
        // blend between two stored matrix snapshots, never per-object
        // interpolation: by the time this call runs, the PREVIOUS frame's
        // own ObjectShape (steep? how many data points?) is gone - only the
        // numbers it produced survive in older_matrix/previous_matrix. This
        // is not a regression - a plain blend is what this segment has
        // always used, from before per-object shapes existed.
        const bool previous_frame = slot < dsp::kQmfDelaySlots;
        const double tail_frac =
            static_cast<double>(dsp::kQmfSlotsPerFrame - dsp::kQmfDelaySlots + slot + 1) /
            static_cast<double>(dsp::kQmfSlotsPerFrame);
        // This frame's own content is only ever visible here for its first
        // (kQmfSlotsPerFrame - kQmfDelaySlots) timeslots; the rest of its
        // 24-timeslot window falls into the delayed-tail branch of the NEXT
        // call instead, the same approximation the plain blend above makes.
        const int ts = slot - dsp::kQmfDelaySlots;

        for (int object = 0; object < objects; ++object) {
            const auto shape = params.shape(object);
            auto& synth = qmf.objects[static_cast<std::size_t>(object)];
            if (!shape.present) {
                // Nothing coded this frame: still pull every slot, with a
                // silent contribution, so the filter's own internal state
                // drains through its natural zero-input response instead of
                // being cut off - the QMF-domain equivalent of
                // reconstruct_mdct_band's explicit history[] drain.
                std::ranges::fill(qmf.object_real, 0.0);
                std::ranges::fill(qmf.object_imag, 0.0);
                const std::span<float, dsp::kQmfHop> emitted{
                    out[static_cast<std::size_t>(object)].data() + slot * dsp::kQmfHop,
                    static_cast<std::size_t>(dsp::kQmfHop)};
                synth.pull(qmf.object_real, qmf.object_imag, emitted);
                continue;
            }
            // One offset walk per object per block instead of one per
            // coefficient read - see FrameParameters::ObjectMatrixView.
            const auto view = params.object_view(object);
            const auto& mapping = kSubbandToBand[static_cast<std::size_t>(shape.num_bands_idx)];

            for (int k = 0; k < dsp::kQmfSubbands; ++k) {
                const auto band = mapping[static_cast<std::size_t>(k)];
                double real = 0.0;
                double imag = 0.0;
                for (int ch = 0; ch < kNumChannels5X; ++ch) {
                    const std::size_t index =
                        (static_cast<std::size_t>(object) *
                             static_cast<std::size_t>(kNumChannels5X) +
                         static_cast<std::size_t>(ch)) *
                            static_cast<std::size_t>(kQmfSubbands) +
                        static_cast<std::size_t>(k);
                    // This frame's own transmitted value, used both as the
                    // ramp's ultimate fallback (no history at all) and as
                    // one endpoint of the current-frame segment below.
                    const std::array<double, kMaxDataPoints> dq = {
                        view.at(0, ch, band),
                        shape.data_points > 1 ? view.at(1, ch, band) : view.at(0, ch, band)};
                    const double previous_val =
                        has_previous ? state.previous_matrix[index] : dq[0];
                    double m;
                    if (previous_frame) {
                        // Degenerates to `previous_val` outright when there is
                        // no older snapshot either (older_val == previous_val
                        // in that case), which is the same "nothing to ramp
                        // from" fallback every other domain uses.
                        const double older_val =
                            has_older ? state.older_matrix[index] : previous_val;
                        m = older_val + tail_frac * (previous_val - older_val);
                    } else {
                        m = has_previous
                                ? interpolate(shape, previous_val, dq, ts)
                                : dq[static_cast<std::size_t>(shape.data_points - 1)];
                    }
                    real += m * qmf.bed_real[static_cast<std::size_t>(ch)]
                                            [static_cast<std::size_t>(k)];
                    imag += m * qmf.bed_imag[static_cast<std::size_t>(ch)]
                                            [static_cast<std::size_t>(k)];
                }
                qmf.object_real[static_cast<std::size_t>(k)] = real;
                qmf.object_imag[static_cast<std::size_t>(k)] = imag;
            }

            const std::span<float, dsp::kQmfHop> emitted{
                out[static_cast<std::size_t>(object)].data() + slot * dsp::kQmfHop,
                static_cast<std::size_t>(dsp::kQmfHop)};
            synth.pull(qmf.object_real, qmf.object_imag, emitted);
        }
    }

    // The move keeps this at one matrix copy a frame: older_matrix inherits
    // the buffer previous_matrix is about to give up, then previous_matrix
    // is rebuilt at subband resolution from this frame's own last data
    // point - see reconstruct_mdct_band's own tail for why that resolution,
    // not params.matrix's own (variable, per-object) one.
    state.older_matrix = std::move(state.previous_matrix);
    state.previous_matrix.assign(previous_size, 0.0);
    for (int object = 0; object < objects; ++object) {
        const auto shape = params.shape(object);
        const auto& mapping = kSubbandToBand[static_cast<std::size_t>(shape.num_bands_idx)];
        // One offset walk per object, not one per (channel, subband) -
        // see FrameParameters::ObjectMatrixView.
        const auto wb_view = params.object_view(object);
        for (int ch = 0; ch < kNumChannels5X; ++ch) {
            for (int subband = 0; subband < kQmfSubbands; ++subband) {
                const std::size_t index =
                    (static_cast<std::size_t>(object) *
                         static_cast<std::size_t>(kNumChannels5X) +
                     static_cast<std::size_t>(ch)) *
                        static_cast<std::size_t>(kQmfSubbands) +
                    static_cast<std::size_t>(subband);
                state.previous_matrix[index] =
                    shape.present ? wb_view.at(shape.data_points - 1, ch,
                                               mapping[static_cast<std::size_t>(subband)])
                                  : 0.0;
            }
        }
    }
    state.previous_objects = objects;
    state.previous_channels = kNumChannels5X;

    return out;
}

}  // namespace

std::vector<std::vector<float>> reconstruct(std::span<const std::span<const float>> bed,
                                            const FrameParameters& params,
                                            ReconstructionState& state, bool fast_mdct,
                                            bool fast_imdct, Domain domain) {
    assert(bed.size() == static_cast<std::size_t>(params.channels));
    assert(params.channels >= 1 && params.channels <= kMaxChannels);
    assert(domain != Domain::kQmf || params.channels == kNumChannels5X);
    assert(params.matrix.size() == params.coefficient_count());

    // Each domain function maintains its own previous_matrix/older_matrix/
    // previous_objects/previous_channels internally, at the subband
    // resolution its own comment explains, so there is no generic
    // post-processing step here - unlike params.matrix itself, that state
    // cannot be copied verbatim between calls once band counts and data
    // points are allowed to vary per object.
    return domain == Domain::kQmf ? reconstruct_qmf(bed, params, state)
                                  : reconstruct_mdct_band(bed, params, state, fast_mdct, fast_imdct);
}

}  // namespace ac3::joc
