#include "pcm/asf_reconstruct.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "tables/sfb_tables.hpp"

namespace ac4::detail {
namespace {

// The note under clause 5.1.3.1's quant_spec: 8 191 is the largest magnitude,
// which is also the most ext_code (Pseudocode 20, 21 bits) can escape to.
constexpr std::size_t kMaxQuant = 8191;

// sign(q) |q|^(4/3), clause 5.1.3.2.
[[nodiscard]] double reconstruct_line(std::int32_t q) noexcept {
    static const std::array<double, kMaxQuant + 1> kPow43 = [] {
        std::array<double, kMaxQuant + 1> table{};
        for (std::size_t m = 0; m < table.size(); ++m) {
            table[m] = std::pow(static_cast<double>(m), 4.0 / 3.0);
        }
        return table;
    }();
    const std::int64_t wide = q;
    const auto magnitude = static_cast<std::uint64_t>(wide < 0 ? -wide : wide);
    const double value = magnitude < kPow43.size() ? kPow43[static_cast<std::size_t>(magnitude)]
                                                   : std::pow(static_cast<double>(magnitude), 4.0 / 3.0);
    return q < 0 ? -value : value;
}

// The sum of squares Pseudocodes 22 and 23 call band_rms before dividing it
// by the band's line count over every window of its group.
[[nodiscard]] double band_energy(std::span<const double> scaled, std::size_t begin, std::size_t end) noexcept {
    double sum = 0.0;
    for (std::size_t k = begin; k < end; ++k) {
        sum += scaled[k] * scaled[k];
    }
    return sum;
}

}  // namespace

ParseResult reconstruct_track(const SfInfo& info, const SfData& data, RandGenState& noise,
                              std::vector<double>& scaled) {
    const AsfPsyInfo& psy = info.psy;
    scaled.assign(data.quant_spec.size(), 0.0);

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
            const double sf_gain = std::pow(2.0, 0.25 * static_cast<double>(scale_factor - 100));
            const std::size_t begin = data.sect_sfb_offset[gi][si];
            const std::size_t end = data.sect_sfb_offset[gi][si + 1];
            for (std::size_t k = begin; k < end; ++k) {
                scaled[k] = sf_gain * reconstruct_line(data.quant_spec[k]);
            }
        }
    }

    if (!data.b_snf_data_exists) {
        return {};
    }
    // Pseudocode 22: the reference level is that of the first band with any
    // energy. 1.44269504 is the text's own rounding of 1/ln 2.
    constexpr double kLog2E = 1.44269504;
    double previous_rms = -1000.0;
    for (int g = 0; g < psy.num_window_groups && previous_rms == -1000.0; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        for (int sfb = 0; sfb < data.max_sfb[gi]; ++sfb) {
            const auto si = static_cast<std::size_t>(sfb);
            const std::size_t begin = data.sect_sfb_offset[gi][si];
            const std::size_t end = data.sect_sfb_offset[gi][si + 1];
            const double band_rms = band_energy(scaled, begin, end);
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
            const double band_rms = band_energy(scaled, begin, end);
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
                scaled[k] = static_cast<double>(get_random_noise_value(noise)) * amplitude;
            }
        }
    }
    return {};
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
             std::span<const double> scaled, std::vector<double>& spec_reord) {
    std::size_t total = 0;
    std::vector<std::size_t> win_offset;
    win_offset.reserve(lengths.size());
    for (const int length : lengths) {
        win_offset.push_back(total);
        total += static_cast<std::size_t>(length);
    }
    spec_reord.assign(total, 0.0);
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

}  // namespace ac4::detail
