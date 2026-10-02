#include "pcm/asf_reconstruct.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "pcm/pow43.hpp"
#include "iclforge/ac4core/dsp/scalar_traits.hpp"
#include "iclforge/ac4core/tables/sfb_tables.hpp"

namespace iclforge::ac4::detail {
namespace {

using dsp::MantExp;

// sign(q) |q|^(4/3), clause 5.1.3.2, from pcm/pow43.hpp's table in read-only
// data. The note under clause 5.1.3.1's quant_spec makes 8 191 the largest
// magnitude, and asf.cpp refuses a longer ext_code; the clamp is for a caller that
// builds an SfData by hand.
template <typename R>
[[nodiscard]] R reconstruct_line(std::int32_t q) noexcept {
    const std::int64_t wide = q;
    const auto magnitude = static_cast<std::uint64_t>(wide < 0 ? -wide : wide);
    const R value =
        kPow43<R>[static_cast<std::size_t>(std::min<std::uint64_t>(magnitude, kMaxQuant))];
    return q < 0 ? -value : value;
}

// The sum of squares Pseudocodes 22 and 23 call band_rms before dividing it
// by the band's line count over every window of its group.
template <typename R>
[[nodiscard]] R band_energy(std::span<const R> scaled, std::size_t begin, std::size_t end) noexcept {
    R sum{};
    for (std::size_t k = begin; k < end; ++k) {
        sum += scaled[k] * scaled[k];
    }
    return sum;
}

// Pseudocodes 21 to 23 at double and float.
template <typename R>
ParseResult reconstruct_track_floating(const SfInfo& info, const SfData& data,
                                       const std::array<R, 256>& sf_gain_table, RandGenState& noise,
                                       std::vector<R>& scaled) {
    const AsfPsyInfo& psy = info.psy;
    scaled.assign(data.quant_spec.size(), R{});

    // Pseudocode 21: the first band with a scale factor takes
    // reference_scale_factor; each later one adds its codeword's index less 60.
    int scale_factor = data.reference_scale_factor;
    bool first_scf_found = false;
    for (int g = 0; g < psy.num_window_groups; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        for (int sfb = 0; sfb < data.max_sfb[gi]; ++sfb) {
            const auto si = static_cast<std::size_t>(sfb);
            if (!data.scale_factor_present[gi][si]) {
                continue;
            }
            if (first_scf_found) {
                scale_factor += data.dpcm_sf[gi][si] - 60;
            } else {
                first_scf_found = true;
            }
            if (scale_factor < 0 || scale_factor > 255) {
                return fail(DecodeError::kInvalidStream, "a scale factor outside 0 to 255");
            }
            const R sf_gain = sf_gain_table[static_cast<std::size_t>(scale_factor)];
            const std::size_t begin = data.sect_sfb_offset[gi][si];
            const std::size_t end = data.sect_sfb_offset[gi][si + 1];
            for (std::size_t k = begin; k < end; ++k) {
                scaled[k] = sf_gain * reconstruct_line<R>(data.quant_spec[k]);
            }
        }
    }

    if (!data.b_snf_data_exists) {
        return {};
    }
    // Pseudocode 22: the reference level is that of the first band with any
    // energy. 1.44269504 is the text's own rounding of 1/ln 2.
    // The noise floor's tracking (previous_rms, band_rms and amplitude) stays
    // double regardless of R: it is a once-per-band control value, not a
    // per-sample one, in the same "computed in double, narrowed once" shape
    // as a downmix or DRC gain (band_energy's own sum of squares is R, the
    // actual per-sample precision, then widened once for std::log).
    constexpr double kLog2E = 1.44269504;
    double previous_rms = -1000.0;
    for (int g = 0; g < psy.num_window_groups && previous_rms == -1000.0; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        for (int sfb = 0; sfb < data.max_sfb[gi]; ++sfb) {
            const auto si = static_cast<std::size_t>(sfb);
            const std::size_t begin = data.sect_sfb_offset[gi][si];
            const std::size_t end = data.sect_sfb_offset[gi][si + 1];
            const auto band_rms = static_cast<double>(band_energy<R>(scaled, begin, end));
            if (band_rms > 0.0) {
                previous_rms = kLog2E * std::log(band_rms / static_cast<double>(end - begin));
                break;
            }
        }
    }
    // Pseudocode 23.
    for (int g = 0; g < psy.num_window_groups; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        for (int sfb = 0; sfb < data.max_sfb[gi]; ++sfb) {
            const auto si = static_cast<std::size_t>(sfb);
            const std::size_t begin = data.sect_sfb_offset[gi][si];
            const std::size_t end = data.sect_sfb_offset[gi][si + 1];
            const auto band_rms = static_cast<double>(band_energy<R>(scaled, begin, end));
            if (band_rms > 0.0) {
                previous_rms = kLog2E * std::log(band_rms / static_cast<double>(end - begin));
                continue;
            }
            // A band with no energy is one asf_snf_data() read a codeword for
            // (sfb_cb 0 or max_quant_idx 0); the two conditions coincide.
            if (!data.snf_present[gi][si]) {
                continue;
            }
            const int delta = data.dpcm_snf[gi][si] - 17;
            if (delta == -17) {
                continue;  // the escape: no noise, and the level is not updated
            }
            const double noise_rms = previous_rms + static_cast<double>(delta);
            previous_rms = noise_rms;
            const double amplitude = std::pow(2.0, 0.5 * noise_rms);
            for (std::size_t k = begin; k < end; ++k) {
                scaled[k] = static_cast<R>(static_cast<double>(get_random_noise_value(noise)) * amplitude);
            }
        }
    }
    return {};
}


// The fixed-point tier's reconstruction (planning/ac4.md, D14d): the same Pseudocodes 21 to 23,
// each line's value sign(q) |q|^(4/3) 2^((sf - 100) / 4) formed as a mantissa and a power of two
// (dsp::MantExp) and the noise fill's levels from the bands' exact energies, by MantExp's own
// log2 and exp2. The track is walked twice with the same arithmetic: once for the largest
// magnitude any line can take, which fixes the track's exponent, and once to write the lines,
// each of which is then below 1, with the format's seven bits above it for the stereo and
// multichannel matrices. Each line is rounded once, to 2^-24 of that bound: with the bound at
// 1/8 the rounding of a frame's many small lines held a tone's spectrum to 107 dB from the
// double decoder's.
class FixedTrack {
   public:
    FixedTrack(const SfInfo& info, const SfData& data) : psy_(info.psy), data_(data) {}

    [[nodiscard]] ParseResult run(RandGenState& noise, std::vector<dsp::Fixed32>& scaled, int& exponent) {
        scaled.assign(data_.quant_spec.size(), dsp::Fixed32{});
        exponent = 0;
        RandGenState unused = noise;
        if (auto ok = walk(false, unused, scaled); !ok) {
            return ok;
        }
        if (largest_.is_zero()) {
            return {};
        }
        // Every line below 2^largest_.e, and written at 2^-largest_.e.
        exponent = largest_.e;
        return walk(true, noise, scaled);
    }

   private:
    // 2^(n / 4) for n in 0 to 3.
    static constexpr std::array<MantExp, 4> kQuarterPowers = {
        MantExp{1.0}, MantExp{1.1892071150027210667}, MantExp{1.4142135623730950488},
        MantExp{1.6817928305074290861}};
    // 1.44269504 ln 2: the text's rounding of 1/ln 2, applied to a natural log, as a factor on log2.
    static constexpr MantExp kLog2EOnLog2 = MantExp{1.44269504 * 0.69314718055994530942};
    // The noise fill's values are a sum of two table entries, each at most 1.87 in magnitude.
    static constexpr int kNoiseBits = 2;

    [[nodiscard]] static MantExp gain(int scale_factor) noexcept {
        const int n = scale_factor - 100;
        const int whole = n >= 0 ? n / 4 : -((-n + 3) / 4);
        return kQuarterPowers[static_cast<std::size_t>(n - 4 * whole)].scaled_by_pow2(whole);
    }

    void note(MantExp bound) noexcept {
        if (!bound.is_zero() && (largest_.is_zero() || bound.e > largest_.e)) {
            largest_ = bound;
        }
    }

    [[nodiscard]] dsp::Fixed32 stored(MantExp value) const noexcept {
        return value.scaled_by_pow2(-largest_.e).to_fixed();
    }

    [[nodiscard]] ParseResult walk(bool write, RandGenState& noise, std::vector<dsp::Fixed32>& scaled) {
        const auto& pow43 = kPow43<MantExp>;
        const auto magnitude = [](std::int32_t q) {
            const std::int64_t wide = q;
            return static_cast<std::size_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(wide < 0 ? -wide : wide), kMaxQuant));
        };
        // Pseudocode 21, with each coded band's energy kept for Pseudocodes 22 and 23.
        std::array<std::array<MantExp, kMaxSfb>, kMaxWindows> energy{};
        int scale_factor = data_.reference_scale_factor;
        bool first_scf_found = false;
        for (int g = 0; g < psy_.num_window_groups; ++g) {
            const auto gi = static_cast<std::size_t>(g);
            for (int sfb = 0; sfb < data_.max_sfb[gi]; ++sfb) {
                const auto si = static_cast<std::size_t>(sfb);
                if (!data_.scale_factor_present[gi][si]) {
                    continue;
                }
                if (first_scf_found) {
                    scale_factor += data_.dpcm_sf[gi][si] - 60;
                } else {
                    first_scf_found = true;
                }
                if (scale_factor < 0 || scale_factor > 255) {
                    return fail(DecodeError::kInvalidStream, "a scale factor outside 0 to 255");
                }
                const MantExp sf_gain = gain(scale_factor);
                const std::size_t begin = data_.sect_sfb_offset[gi][si];
                const std::size_t end = data_.sect_sfb_offset[gi][si + 1];
                MantExp sum{};
                std::size_t largest_q = 0;
                for (std::size_t k = begin; k < end; ++k) {
                    const std::size_t m = magnitude(data_.quant_spec[k]);
                    largest_q = std::max(largest_q, m);
                    const MantExp value = sf_gain * pow43[m];
                    sum += value * value;
                    if (write) {
                        scaled[k] = stored(data_.quant_spec[k] < 0 ? -value : value);
                    }
                }
                energy[gi][si] = sum;
                note(sf_gain * pow43[largest_q]);
            }
        }
        if (!data_.b_snf_data_exists) {
            return {};
        }
        const auto band_energy = [&](std::size_t gi, std::size_t si) {
            // Only a band with a scale factor has lines Pseudocode 21 set.
            return data_.scale_factor_present[gi][si] ? energy[gi][si] : MantExp{};
        };
        const auto level = [&](MantExp band_rms, std::size_t lines) {
            return scalar_log2(band_rms / MantExp{static_cast<int>(lines)}) * kLog2EOnLog2;
        };
        // Pseudocode 22.
        bool have_previous = false;
        MantExp previous_rms{};
        for (int g = 0; g < psy_.num_window_groups && !have_previous; ++g) {
            const auto gi = static_cast<std::size_t>(g);
            for (int sfb = 0; sfb < data_.max_sfb[gi]; ++sfb) {
                const auto si = static_cast<std::size_t>(sfb);
                const MantExp band_rms = band_energy(gi, si);
                if (band_rms > MantExp{}) {
                    previous_rms = level(band_rms, data_.sect_sfb_offset[gi][si + 1] - data_.sect_sfb_offset[gi][si]);
                    have_previous = true;
                    break;
                }
            }
        }
        if (!have_previous) {
            previous_rms = MantExp{-1000};
        }
        // Pseudocode 23.
        for (int g = 0; g < psy_.num_window_groups; ++g) {
            const auto gi = static_cast<std::size_t>(g);
            for (int sfb = 0; sfb < data_.max_sfb[gi]; ++sfb) {
                const auto si = static_cast<std::size_t>(sfb);
                const std::size_t begin = data_.sect_sfb_offset[gi][si];
                const std::size_t end = data_.sect_sfb_offset[gi][si + 1];
                const MantExp band_rms = band_energy(gi, si);
                if (band_rms > MantExp{}) {
                    previous_rms = level(band_rms, end - begin);
                    continue;
                }
                if (!data_.snf_present[gi][si]) {
                    continue;
                }
                const int delta = data_.dpcm_snf[gi][si] - 17;
                if (delta == -17) {
                    continue;
                }
                const MantExp noise_rms = previous_rms + MantExp{delta};
                previous_rms = noise_rms;
                const MantExp amplitude = scalar_exp2(noise_rms.scaled_by_pow2(-1));
                note(amplitude.scaled_by_pow2(kNoiseBits));
                if (write) {
                    for (std::size_t k = begin; k < end; ++k) {
                        scaled[k] = stored(MantExp{static_cast<double>(get_random_noise_value(noise))} * amplitude);
                    }
                }
            }
        }
        return {};
    }

    const AsfPsyInfo& psy_;
    const SfData& data_;
    MantExp largest_{};
};

template <typename R>
ParseResult reconstruct_track_at(const SfInfo& info, const SfData& data, const std::array<R, 256>& sf_gain,
                                 RandGenState& noise, std::vector<R>& scaled, int& exponent) {
    if constexpr (dsp::kFixed<R>) {
        return FixedTrack(info, data).run(noise, scaled, exponent);
    } else {
        exponent = 0;
        return reconstruct_track_floating<R>(info, data, sf_gain, noise, scaled);
    }
}

}  // namespace

ParseResult reconstruct_track(const SfInfo& info, const SfData& data, const ScaleFactorGains& sf_gain,
                              RandGenState& noise, std::vector<Real>& scaled, int& exponent) {
    return reconstruct_track_at<Real>(info, data, sf_gain, noise, scaled, exponent);
}

ScaleFactorGains scale_factor_gains() {
    ScaleFactorGains gains{};
    if (dsp::kFixed<Real>) {
        return gains;  // the fixed tier forms each gain as a MantExp (FixedTrack)
    }
    for (std::size_t sf = 0; sf < gains.size(); ++sf) {
        gains[sf] = static_cast<Real>(
            std::pow(2.0, 0.25 * static_cast<double>(static_cast<int>(sf) - 100)));
    }
    return gains;
}

ParseResult window_lengths(const SubstreamContext& ctx, const AsfPsyInfo& psy, std::vector<int>& lengths) {
    lengths.clear();
    if (psy.b_long_frame && ctx.frame_len_base >= 1536) {
        lengths.push_back(ctx.frame_len_base);
        return {};
    }
    int total = 0;
    for (int w = 0; w < psy.num_windows; ++w) {
        const int g = psy.window_to_group[static_cast<std::size_t>(w)];
        const int length = transform_length_samples(ctx, get_transf_length(ctx, psy, g));
        if (length <= 0) {
            return fail(DecodeError::kInvalidStream, "a transform length the frame length does not have");
        }
        lengths.push_back(length);
        total += length;
    }
    if (total != ctx.frame_len_base) {
        return fail(DecodeError::kInvalidStream, "the frame's blocks do not add up to its length");
    }
    return {};
}

void ungroup(const SubstreamContext& ctx, const AsfPsyInfo& psy, const SfData& data, std::span<const int> lengths,
             std::span<const Real> scaled, std::vector<Real>& spec_reord) {
    std::size_t total = 0;
    std::vector<std::size_t> win_offset;
    win_offset.reserve(lengths.size());
    for (const int length : lengths) {
        win_offset.push_back(total);
        total += static_cast<std::size_t>(length);
    }
    spec_reord.assign(total, Real{});
    std::size_t k = 0;
    std::size_t win = 0;
    for (int g = 0; g < psy.num_window_groups; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        const std::span<const std::uint16_t> offsets =
            tables::sfb_offsets_48(transform_length_samples(ctx, get_transf_length(ctx, psy, g)));
        const std::size_t windows = psy.num_win_in_group[gi];
        for (int sfb = 0; sfb < data.max_sfb[gi]; ++sfb) {
            const auto si = static_cast<std::size_t>(sfb);
            for (std::size_t w = 0; w < windows; ++w) {
                for (std::size_t l = offsets[si]; l < offsets[si + 1]; ++l) {
                    spec_reord[win_offset[win + w] + l] = scaled[k++];
                }
            }
        }
        win += windows;
    }
}

bool ungroup_in_place(const SubstreamContext& ctx, const AsfPsyInfo& psy, const SfData& data,
                      std::span<const int> lengths, std::vector<Real>& scaled,
                      std::vector<Real>& spec_reord) {
    if (lengths.size() != 1 || psy.num_window_groups != 1 || psy.num_win_in_group[0] != 1) {
        return false;
    }
    const std::span<const std::uint16_t> offsets =
        tables::sfb_offsets_48(transform_length_samples(ctx, get_transf_length(ctx, psy, 0)));
    const auto bands = static_cast<std::size_t>(std::max(data.max_sfb[0], 0));
    const auto total = static_cast<std::size_t>(std::max(lengths[0], 0));
    // The lines the bands cover are the first ones, one for one: offsets[0] is 0 and the bands
    // follow each other.
    if (offsets.empty() || offsets[0] != 0 || bands >= offsets.size() ||
        scaled.size() != offsets[bands] || scaled.size() > total) {
        return false;
    }
    std::swap(spec_reord, scaled);
    spec_reord.resize(total, Real{});
    return true;
}

}  // namespace iclforge::ac4::detail
