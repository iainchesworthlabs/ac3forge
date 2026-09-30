#pragma once

#include <array>
#include <cstdint>

// A-SPX's subband group tables: ETSI TS 103 190-1 V1.4.1 clause 5.7.6.3.1,
// Pseudocodes 67 to 74. Every table holds QMF subbands, the lower border of
// each group followed by the upper border of the last, so a table of n groups
// has n + 1 entries.
//
// The decoder's syntax reads with the counts (how many envelope values an
// aspx_data element carries), its PCM stage with the tables; the encoder
// derives the same ones.

namespace ac4::detail::aspx {

inline constexpr int kMaxSbgMaster = 22;  // sbg_template_highres spans 22 groups
inline constexpr int kMaxSbgNoise = 5;    // 5.7.6.3.1.3
inline constexpr int kMaxPatches = 5;     // 5.7.6.3.1.4
// num_sbg_lim is num_sbg_sig_lowres + num_sbg_patches - 1 at most.
inline constexpr int kMaxSbgLim = kMaxSbgMaster / 2 + 1 + kMaxPatches - 1;

// What aspx_config() and an aspx_data element's aspx_xover_subband_offset say
// about frequency.
struct FrequencyConfig {
    int master_freq_scale = 0;  // aspx_master_freq_scale: 1 for the high resolution template
    int start_freq = 0;         // aspx_start_freq, 0 to 7
    int stop_freq = 0;          // aspx_stop_freq, 0 to 3
    int noise_sbg = 0;          // aspx_noise_sbg, 0 to 3
    int xover_subband_offset = 0;
};

// Pseudocodes 67 to 70.
struct SubbandGroups {
    int num_sbg_master = 0;
    std::array<std::uint8_t, kMaxSbgMaster + 1> sbg_master{};
    int sba = 0;  // sbg_master[0]
    int sbz = 0;  // sbg_master[num_sbg_master]
    int sbx = 0;  // the crossover: the first subband A-SPX recreates
    int num_sb_aspx = 0;
    int num_sbg_sig_highres = 0;
    std::array<std::uint8_t, kMaxSbgMaster + 1> sbg_sig_highres{};
    int num_sbg_sig_lowres = 0;
    std::array<std::uint8_t, kMaxSbgMaster + 1> sbg_sig_lowres{};
    int num_sbg_noise = 0;
    std::array<std::uint8_t, kMaxSbgNoise + 1> sbg_noise{};
};

enum class GroupsError : std::uint8_t {
    kNone,
    // aspx_xover_subband_offset at or past num_sbg_master: Pseudocode 68 would
    // index past sbg_master, or leave no group.
    kXoverOffset,
    // num_sbg_noise above 5 (5.7.6.3.1.3).
    kNoiseGroups,
};

// Every table of Pseudocodes 67 to 70 for `config`. However master_reset
// falls, the master table follows from aspx_master_freq_scale,
// aspx_start_freq and aspx_stop_freq, so it is built from them each time.
// num_sbg_noise, max(1, floor(aspx_noise_sbg * log2(sbz / sbx) + 0.5)), is
// decided in integers (src/ac4dec/ERRATA.md, "Counts computed exactly").
[[nodiscard]] GroupsError derive_subband_groups(const FrequencyConfig& config, SubbandGroups& out);

// Pseudocodes 71 to 74.
struct PatchTables {
    int num_sbg_patches = 0;
    std::array<std::uint8_t, kMaxPatches + 1> sbg_patch_num_sb{};
    std::array<std::uint8_t, kMaxPatches + 1> sbg_patch_start_sb{};
    std::array<std::uint8_t, kMaxPatches + 1> sbg_patches{};  // the patches' borders from sbx
    int num_sbg_lim = 0;
    std::array<std::uint8_t, kMaxSbgLim + 1> sbg_lim{};
};

// The patches and the limiter's groups for `groups`. `base_48k` is true at a
// base sampling frequency of 48 kHz (fs_index 1), false at 44.1 kHz. False
// when the patch construction of Pseudocode 71 stops making progress before
// it reaches sbz, or makes more than five patches, or none: tables no
// conforming stream selects.
[[nodiscard]] bool derive_patch_tables(const SubbandGroups& groups, int master_freq_scale,
                                       bool base_48k, PatchTables& out);

}  // namespace ac4::detail::aspx
