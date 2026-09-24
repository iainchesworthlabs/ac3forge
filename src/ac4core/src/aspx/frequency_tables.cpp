#include "aspx/frequency_tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace ac4::detail::aspx {
namespace {

// 5.7.6.3.1.1.
constexpr std::array<std::uint8_t, 21> kSbgTemplateLowres = {
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 22, 24, 26, 28, 30, 32, 35, 38, 42, 46};
constexpr std::array<std::uint8_t, 23> kSbgTemplateHighres = {
    18, 19, 20, 21, 22, 23, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 47, 50, 53, 56, 59, 62};

[[nodiscard]] std::uint8_t u8(int value) noexcept {
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

// max(1, floor(noise_sbg * log2(sbz / sbx) + 0.5)), or kMaxSbgNoise + 1 for
// anything above kMaxSbgNoise. The floor is the largest k with k - 0.5 <=
// noise_sbg * log2(sbz / sbx), i.e. with 2^(2k - 1) * sbx^(2 noise_sbg) <=
// sbz^(2 noise_sbg), which integers decide exactly: sbz is at most 62, so the
// powers stay below 2^36 and the shifts below 2^50.
[[nodiscard]] int noise_group_count(int noise_sbg, int sbx, int sbz) noexcept {
    std::uint64_t sbz_power = 1;
    std::uint64_t sbx_power = 1;
    for (int i = 0; i < 2 * noise_sbg; ++i) {
        sbz_power *= static_cast<std::uint64_t>(sbz);
        sbx_power *= static_cast<std::uint64_t>(sbx);
    }
    int count = 0;
    while (count <= kMaxSbgNoise && (sbx_power << (2 * count + 1)) <= sbz_power) {
        ++count;
    }
    return std::max(1, count);
}

}  // namespace

GroupsError derive_subband_groups(const FrequencyConfig& config, SubbandGroups& out) {
    // Pseudocode 67.
    const bool highres = config.master_freq_scale == 1;
    const std::span<const std::uint8_t> sbg_template =
        highres ? std::span<const std::uint8_t>(kSbgTemplateHighres)
                : std::span<const std::uint8_t>(kSbgTemplateLowres);
    const int num_sbg_master = (highres ? 22 : 20) - 2 * config.start_freq - 2 * config.stop_freq;
    const int offset = config.xover_subband_offset;
    if (config.start_freq < 0 || config.stop_freq < 0 || num_sbg_master < 1 || offset < 0 ||
        offset >= num_sbg_master) {
        return GroupsError::kXoverOffset;
    }
    SubbandGroups g;
    g.num_sbg_master = num_sbg_master;
    for (int sbg = 0; sbg <= num_sbg_master; ++sbg) {
        g.sbg_master[at(sbg)] = sbg_template[at(2 * config.start_freq + sbg)];
    }
    g.sba = g.sbg_master[0];
    g.sbz = g.sbg_master[at(num_sbg_master)];

    // Pseudocode 68.
    g.num_sbg_sig_highres = num_sbg_master - offset;
    for (int sbg = 0; sbg <= g.num_sbg_sig_highres; ++sbg) {
        g.sbg_sig_highres[at(sbg)] = g.sbg_master[at(sbg + offset)];
    }
    g.sbx = g.sbg_sig_highres[0];
    g.num_sb_aspx = g.sbg_sig_highres[at(g.num_sbg_sig_highres)] - g.sbx;

    // Pseudocode 69.
    const int num_high = g.num_sbg_sig_highres;
    g.num_sbg_sig_lowres = num_high - num_high / 2;
    g.sbg_sig_lowres[0] = g.sbg_sig_highres[0];
    for (int sbg = 1; sbg <= g.num_sbg_sig_lowres; ++sbg) {
        g.sbg_sig_lowres[at(sbg)] =
            g.sbg_sig_highres[at(num_high % 2 == 0 ? 2 * sbg : 2 * sbg - 1)];
    }

    // Pseudocode 70.
    g.num_sbg_noise = noise_group_count(config.noise_sbg, g.sbx, g.sbz);
    if (g.num_sbg_noise > kMaxSbgNoise) {
        return GroupsError::kNoiseGroups;
    }
    int idx = 0;
    g.sbg_noise[0] = g.sbg_sig_lowres[0];
    for (int sbg = 1; sbg <= g.num_sbg_noise; ++sbg) {
        idx += (g.num_sbg_sig_lowres - idx) / (g.num_sbg_noise + 1 - sbg);
        g.sbg_noise[at(sbg)] = g.sbg_sig_lowres[at(idx)];
    }
    out = g;
    return GroupsError::kNone;
}

bool derive_patch_tables(const SubbandGroups& groups, int master_freq_scale, bool base_48k,
                         PatchTables& out) {
    const auto& master = groups.sbg_master;
    const int sba = groups.sba;
    const int sbx = groups.sbx;
    const int sbz = groups.sbx + groups.num_sb_aspx;

    // Pseudocode 71. Each pass that makes a patch moves usb up; one that makes
    // none sets msb to sbx, and two such passes in a row would repeat forever,
    // so the passes are counted.
    std::array<int, kMaxPatches + 2> num_sb{};
    std::array<int, kMaxPatches + 2> start_sb{};
    int msb = sba;
    int usb = sbx;
    int num_patches = 0;
    const int goal_sb = base_48k ? 43 : 46;
    const int source_band_low = master_freq_scale == 1 ? 4 : 2;
    int sbg = 0;
    if (goal_sb < sbz) {
        for (int i = 0; master[at(i)] < goal_sb; ++i) {
            sbg = i + 1;
        }
    } else {
        sbg = groups.num_sbg_master;
    }
    int sb = 0;
    int passes = 0;
    do {
        if (++passes > 2 * (kMaxPatches + 2) || num_patches > kMaxPatches) {
            return false;
        }
        int j = sbg;
        sb = master[at(j)];
        int odd = (sb - 2 + sba) % 2;
        while (sb > sba - source_band_low + msb - odd) {
            if (--j < 0) {
                return false;
            }
            sb = master[at(j)];
            odd = (sb - 2 + sba) % 2;
        }
        num_sb[at(num_patches)] = std::max(sb - usb, 0);
        start_sb[at(num_patches)] = sba - odd - std::max(sb - usb, 0);
        if (num_sb[at(num_patches)] > 0) {
            usb = sb;
            msb = sb;
            ++num_patches;
        } else {
            msb = sbx;
        }
        if (master[at(sbg)] - sb < 3) {
            sbg = groups.num_sbg_master;
        }
    } while (sb != sbz);
    if (num_patches == 0) {
        return false;
    }
    if (num_sb[at(num_patches - 1)] < 3 && num_patches > 1) {
        --num_patches;
    }
    if (num_patches > kMaxPatches) {
        return false;
    }
    PatchTables t;
    t.num_sbg_patches = num_patches;
    t.sbg_patches[0] = u8(sbx);
    for (int i = 0; i < num_patches; ++i) {
        // A source below subband 0 is a table no stream can use.
        if (start_sb[at(i)] < 0) {
            return false;
        }
        t.sbg_patch_num_sb[at(i)] = u8(num_sb[at(i)]);
        t.sbg_patch_start_sb[at(i)] = u8(start_sb[at(i)]);
        t.sbg_patches[at(i + 1)] = u8(t.sbg_patches[at(i)] + num_sb[at(i)]);
    }

    // Pseudocode 72, the loop that copies the patch borders taken as its body
    // (src/ac4dec/ERRATA.md, "Stray semicolon in the limiter's patch
    // borders"), with Pseudocodes 73 and 74.
    const int num_low = groups.num_sbg_sig_lowres;
    std::array<int, kMaxSbgLim + 1> lim{};
    for (int i = 0; i <= num_low; ++i) {
        lim[at(i)] = groups.sbg_sig_lowres[at(i)];
    }
    for (int i = 1; i < num_patches; ++i) {
        lim[at(i + num_low)] = t.sbg_patches[at(i)];
    }
    int num_lim = num_low + num_patches - 1;
    std::sort(lim.begin(), lim.begin() + num_lim + 1);
    const auto is_patch_border = [&](int subband) {
        for (int i = 0; i <= num_patches; ++i) {
            if (t.sbg_patches[at(i)] == subband) {
                return true;
            }
        }
        return false;
    };
    const auto remove = [&](int index) {
        for (int i = index; i < num_lim; ++i) {
            lim[at(i)] = lim[at(i + 1)];
        }
        --num_lim;
    };
    int i = 1;
    while (i <= num_lim) {
        const double num_octaves =
            std::log2(static_cast<double>(lim[at(i)]) / static_cast<double>(lim[at(i - 1)]));
        if (num_octaves >= 0.245) {
            ++i;
        } else if (lim[at(i)] == lim[at(i - 1)]) {
            remove(i);
        } else if (!is_patch_border(lim[at(i)])) {
            remove(i);
        } else if (is_patch_border(lim[at(i - 1)])) {
            ++i;
        } else {
            remove(i - 1);
        }
    }
    t.num_sbg_lim = num_lim;
    for (int k = 0; k <= num_lim; ++k) {
        t.sbg_lim[at(k)] = u8(lim[at(k)]);
    }
    out = t;
    return true;
}

}  // namespace ac4::detail::aspx
