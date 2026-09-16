#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "bit_reader.hpp"
#include "syntax/context.hpp"

// The audio spectral frontend's syntax (ETSI TS 103 190-1 V1.4.1 clauses 4.2.7
// and 4.2.8, semantics 4.3.6) and stereo audio processing's (4.2.10, 4.3.8):
// sf_info, sf_info_lfe, sf_data, chparam_info and sap_data.
//
// The speech spectral frontend is refused where sf_info or sf_data would
// select it (DecodeError::kUnsupported); see ac4dec/decoder.hpp.

namespace ac4::detail {

inline constexpr int kMaxWindows = 16;       // 4.3.6.2.6: num_windows is at most 16
inline constexpr int kMaxSfb = 64;           // max_sfb is at most 6 bits
inline constexpr int kLongFrameIndex = 4;    // Pseudocode 2's "long frame" transform length index

// asf_transform_info() and asf_psy_info() with Pseudocodes 2 to 5's helpers.
struct AsfPsyInfo {
    // 4.2.8.1. transf_length[] holds transform length INDICES (Tables 100 to
    // 105), not sample counts; get_transf_length() returns 4 for a long frame.
    bool b_long_frame = true;
    std::array<int, 2> transf_length{};
    // Set when frame_len_base < 1536, where one index covers the whole frame.
    bool single_transf_length = false;

    // 4.2.8.2
    bool b_dual_maxsfb = false;
    bool b_side_limited = false;
    bool b_different_framing = false;
    std::array<int, 2> max_sfb{};
    std::array<int, 2> max_sfb_side{};
    int n_grp_bits = 0;
    std::array<std::uint8_t, kMaxWindows> scale_factor_grouping{};

    // Pseudocode 3 and 4.
    int num_windows = 1;
    int num_window_groups = 1;
    std::array<std::uint8_t, kMaxWindows> window_to_group{};
    std::array<std::uint8_t, kMaxWindows> num_win_in_group{};
};

struct SfInfo {
    bool is_lfe = false;       // read by sf_info_lfe()
    int spec_frontend = 0;     // 0 = ASF; SSF is refused before an SfInfo is kept
    AsfPsyInfo psy{};
};

// Pseudocode 2: the transform length index of window group g.
[[nodiscard]] int get_transf_length(const SubstreamContext& ctx, const AsfPsyInfo& psy, int g) noexcept;

// The transform length in samples at the internal rate for an index at this
// frame_len_base (Tables 99, 100 and 103).
[[nodiscard]] int transform_length_samples(const SubstreamContext& ctx, int index) noexcept;

// Pseudocode 5. `side_channel` is Pseudocode 5's b_side_channel: set while the
// side channel of an ASPX_ACPL_1 pair is read.
[[nodiscard]] int get_max_sfb(const SubstreamContext& ctx, const AsfPsyInfo& psy, int g,
                              bool side_channel) noexcept;

struct AsfSection {
    std::uint8_t cb = 0;
    std::uint8_t start = 0;  // scale factor band
    std::uint8_t end = 0;    // one past the last band
};

// One sf_data() of the ASF: 4.2.8.3 to 4.2.8.6.
struct SfData {
    // Per window group.
    std::array<std::vector<AsfSection>, kMaxWindows> sections{};
    std::array<int, kMaxWindows> num_sec_lsf{};
    std::array<std::array<std::uint8_t, kMaxSfb>, kMaxWindows> sfb_cb{};
    // sect_sfb_offset[g][sfb] for sfb <= max_sfb(g) (Pseudocode 4, with the
    // entry at max_sfb that section ends need).
    std::array<std::array<std::uint16_t, kMaxSfb + 1>, kMaxWindows> sect_sfb_offset{};
    std::array<int, kMaxWindows> max_sfb{};

    // 4.2.8.4: every quantised spectral line of the frame, in the order the
    // syntax reads them, signed, escapes resolved.
    std::vector<std::int32_t> quant_spec;
    std::array<std::array<std::uint16_t, kMaxSfb>, kMaxWindows> max_quant_idx{};

    // 4.2.8.5. dpcm_sf holds the codebook index (Table A.1) of each delta read;
    // `scale_factor_present` marks the bands that have a scale factor at all.
    int reference_scale_factor = 0;
    std::array<std::array<std::int16_t, kMaxSfb>, kMaxWindows> dpcm_sf{};
    std::array<std::array<bool, kMaxSfb>, kMaxWindows> scale_factor_present{};

    // 4.2.8.6, likewise with Table A.13's indices.
    bool b_snf_data_exists = false;
    std::array<std::array<std::int16_t, kMaxSfb>, kMaxWindows> dpcm_snf{};
    std::array<std::array<bool, kMaxSfb>, kMaxWindows> snf_present{};
};

// sf_info(spec_frontend, b_dual_maxsfb, b_side_limited), 4.2.7.1.
[[nodiscard]] ParseResult parse_sf_info(BitReader& r, const SubstreamContext& ctx, int spec_frontend,
                                        bool b_dual_maxsfb, bool b_side_limited, SfInfo& out);

// sf_info_lfe(), 4.2.7.2.
[[nodiscard]] ParseResult parse_sf_info_lfe(BitReader& r, const SubstreamContext& ctx, SfInfo& out);

// sf_data(spec_frontend), 4.2.7.3, against the sf_info that governs it.
[[nodiscard]] ParseResult parse_sf_data(BitReader& r, const SubstreamContext& ctx, const SfInfo& info,
                                        bool side_channel, SfData& out);

// chparam_info() and sap_data(), 4.2.10.
struct ChparamInfo {
    int sap_mode = 0;
    std::array<std::array<bool, kMaxSfb>, kMaxWindows> ms_used{};
    bool sap_coeff_all = false;
    std::array<std::array<bool, kMaxSfb>, kMaxWindows> sap_coeff_used{};
    bool delta_code_time = false;
    std::array<std::array<std::int16_t, kMaxSfb>, kMaxWindows> dpcm_alpha_q{};  // Table A.1 indices
};

[[nodiscard]] ParseResult parse_chparam_info(BitReader& r, const SubstreamContext& ctx, const SfInfo& info,
                                             ChparamInfo& out);

// n_side_bits for a transform length index at this frame_len_base (Table 106).
[[nodiscard]] int n_side_bits(const SubstreamContext& ctx, int transform_index) noexcept;

}  // namespace ac4::detail
