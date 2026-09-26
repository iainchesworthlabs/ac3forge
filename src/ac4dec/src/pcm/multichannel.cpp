#include "pcm/multichannel.hpp"

#include <algorithm>
#include <cstddef>

#include "tables/sfb_tables.hpp"

namespace ac4::detail {
namespace {

constexpr int kMatselCount = 12;

// The parameter sets the matrix of N channels takes: Table 178's two for
// three channels, clause 5.3.3.4's four for four and Table 179's five for five.
template <std::size_t N>
inline constexpr std::size_t kSets = N == 3 ? 2 : N;

// A tile's parameter sets, a, b, c and d each.
template <std::size_t Sets>
[[nodiscard]] std::array<Abcd, Sets> tile(std::span<const StereoParameters> parameters, std::size_t g,
                                          std::size_t sfb) noexcept {
    std::array<Abcd, Sets> out{};
    for (std::size_t i = 0; i < Sets; ++i) {
        out[i] = parameters[i].abcd[g][sfb];
    }
    return out;
}

template <std::size_t N>
void multiply(const Matrix<N>& m, std::span<std::vector<double>* const> tracks, std::size_t begin,
              std::size_t end) {
    std::array<double, N> in{};
    for (std::size_t k = begin; k < end; ++k) {
        for (std::size_t i = 0; i < N; ++i) {
            in[i] = (*tracks[i])[k];
        }
        for (std::size_t o = 0; o < N; ++o) {
            double sum = 0.0;
            for (std::size_t i = 0; i < N; ++i) {
                sum += m[o][i] * in[i];
            }
            (*tracks[o])[k] = sum;
        }
    }
}

template <std::size_t N>
[[nodiscard]] std::optional<Matrix<N>> matrix_of(int chel_matsel, const std::array<Abcd, kSets<N>>& p) {
    if constexpr (N == 3) {
        return three_channel_matrix(chel_matsel, p[0], p[1]);
    } else if constexpr (N == 4) {
        return four_channel_matrix(p);
    } else {
        return five_channel_matrix(chel_matsel, p);
    }
}

template <std::size_t N>
[[nodiscard]] ParseResult apply(const SfInfo& info, const SfData& layout, int chel_matsel,
                                std::span<const StereoParameters> parameters,
                                std::span<std::vector<double>* const> tracks) {
    for (int g = 0; g < info.psy.num_window_groups; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        for (int sfb = 0; sfb < layout.max_sfb[gi]; ++sfb) {
            const auto si = static_cast<std::size_t>(sfb);
            const auto m = matrix_of<N>(chel_matsel, tile<kSets<N>>(parameters, gi, si));
            if (!m) {
                return fail(DecodeError::kInvalidStream, "a chel_matsel Tables 178 and 179 do not define");
            }
            multiply<N>(*m, tracks, layout.sect_sfb_offset[gi][si], layout.sect_sfb_offset[gi][si + 1]);
        }
    }
    return {};
}

}  // namespace

std::optional<Matrix<3>> three_channel_matrix(int chel_matsel, const Abcd& p0, const Abcd& p1) {
    const auto [a0, b0, c0, d0] = p0;
    const auto [a1, b1, c1, d1] = p1;
    switch (chel_matsel) {
        case 0:
            return Matrix<3>{{{a0 * a1, b0 * a1, b1}, {c0, d0, 0.0}, {a0 * c1, b0 * c1, d1}}};
        case 1:
            return Matrix<3>{{{d0, c0, 0.0}, {b0 * a1, a0 * a1, b1}, {b0 * c1, a0 * c1, d1}}};
        case 2:
            return Matrix<3>{{{a0 * a1, b1, b0 * a1}, {a0 * c1, d1, b0 * c1}, {c0, 0.0, d0}}};
        case 3:
            return Matrix<3>{{{a1, c0 * b1, d0 * b1}, {0.0, a0, b0}, {c1, c0 * d1, d0 * d1}}};
        case 4:
            return Matrix<3>{{{a0, 0.0, b0}, {c0 * b1, a1, d0 * b1}, {c0 * d1, c1, d0 * d1}}};
        case 5:
            return Matrix<3>{{{a1, d0 * b1, c0 * b1}, {c1, d0 * d1, c0 * d1}, {0.0, b0, a0}}};
        case 6:
            return Matrix<3>{{{d0 * d1, c0 * d1, c1}, {b0, a0, 0.0}, {d0 * b1, c0 * b1, a1}}};
        case 7:
            return Matrix<3>{{{a0, b0, 0.0}, {c0 * d1, d0 * d1, c1}, {c0 * b1, d0 * b1, a1}}};
        case 8:
            return Matrix<3>{{{d0 * d1, c1, c0 * d1}, {d0 * b1, a1, c0 * b1}, {b0, 0.0, a0}}};
        case 9:
            return Matrix<3>{{{d1, b0 * c1, a0 * c1}, {0.0, d0, c0}, {b1, b0 * a1, a0 * a1}}};
        case 10:
            return Matrix<3>{{{d0, 0.0, c0}, {b0 * c1, d1, a0 * c1}, {b0 * a1, b1, a0 * a1}}};
        case 11:
            return Matrix<3>{{{d1, a0 * c1, b0 * c1}, {b1, a0 * a1, b0 * a1}, {0.0, c0, d0}}};
        default:
            return std::nullopt;
    }
}

Matrix<4> four_channel_matrix(std::span<const Abcd, 4> p) {
    const auto [a0, b0, c0, d0] = p[0];
    const auto [a1, b1, c1, d1] = p[1];
    const auto [a2, b2, c2, d2] = p[2];
    const auto [a3, b3, c3, d3] = p[3];
    return Matrix<4>{{{a0 * a2, b0 * a2, a1 * b2, b1 * b2},
                      {c0 * a3, d0 * a3, c1 * b3, d1 * b3},
                      {a0 * c2, b0 * c2, a1 * d2, b1 * d2},
                      {c0 * c3, d0 * c3, c1 * d3, d1 * d3}}};
}

std::optional<Matrix<5>> five_channel_matrix(int chel_matsel, std::span<const Abcd, 5> p) {
    const auto t = three_channel_matrix(chel_matsel, p[0], p[1]);
    if (!t) {
        return std::nullopt;
    }
    const auto [a2, b2, c2, d2] = p[2];
    const auto [a3, b3, c3, d3] = p[3];
    const auto [a4, b4, c4, d4] = p[4];
    const auto& [t0, t1, t2] = *t;
    return Matrix<5>{{{a3 * t0[0], a3 * t0[1], a3 * t0[2], b3 * a2, b3 * b2},
                      {a4 * t1[0], a4 * t1[1], a4 * t1[2], b4 * c2, b4 * d2},
                      {t2[0], t2[1], t2[2], 0.0, 0.0},
                      {c3 * t0[0], c3 * t0[1], c3 * t0[2], d3 * a2, d3 * b2},
                      {c4 * t1[0], c4 * t1[1], c4 * t1[2], d4 * c2, d4 * d2}}};
}

ParseResult apply_channel_data(const SfInfo& info, const SfData& layout, int chel_matsel,
                               std::span<const StereoParameters> parameters,
                               std::span<std::vector<double>* const> tracks) {
    switch (tracks.size()) {
        case 3:
            if (parameters.size() != 2) {
                break;
            }
            if (chel_matsel < 0 || chel_matsel >= kMatselCount) {
                return fail(DecodeError::kInvalidStream, "a chel_matsel Table 178 does not define");
            }
            return apply<3>(info, layout, chel_matsel, parameters, tracks);
        case 4:
            if (parameters.size() != 4) {
                break;
            }
            return apply<4>(info, layout, chel_matsel, parameters, tracks);
        case 5:
            if (parameters.size() != 5) {
                break;
            }
            if (chel_matsel < 0 || chel_matsel >= kMatselCount) {
                return fail(DecodeError::kInvalidStream, "a chel_matsel Table 179 does not define");
            }
            return apply<5>(info, layout, chel_matsel, parameters, tracks);
        default:
            break;
    }
    return fail(DecodeError::kInvalidStream, "a channel data element without its chparam_info()s");
}

ParseResult apply_additional_pair(const SubstreamContext& ctx, const AsfPsyInfo& base,
                                  const StereoParameters& parameters, std::span<const int> base_lengths,
                                  std::span<const int> other_lengths, std::span<double> base_lines,
                                  std::span<double> other_lines) {
    if (!std::ranges::equal(base_lengths, other_lengths)) {
        return fail(DecodeError::kInvalidStream,
                    "a step between channel data elements whose channels are transformed unlike "
                    "each other");
    }
    std::size_t window_start = 0;
    std::size_t window = 0;
    for (int g = 0; g < base.num_window_groups; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        const std::span<const std::uint16_t> offsets =
            tables::sfb_offsets_48(transform_length_samples(ctx, get_transf_length(ctx, base, g)));
        const int max_sfb = std::min(get_max_sfb(ctx, base, g, false), kMaxSfb);
        for (std::size_t w = 0; w < base.num_win_in_group[gi]; ++w, ++window) {
            if (window >= base_lengths.size()) {
                return fail(DecodeError::kInvalidStream, "more windows in the groups than in the frame");
            }
            for (int sfb = 0; sfb < max_sfb && static_cast<std::size_t>(sfb) + 1 < offsets.size(); ++sfb) {
                const auto si = static_cast<std::size_t>(sfb);
                const auto [a, b, c, d] = parameters.abcd[gi][si];
                for (std::size_t l = offsets[si]; l < offsets[si + 1]; ++l) {
                    const std::size_t k = window_start + l;
                    const double i0 = base_lines[k];
                    const double i1 = other_lines[k];
                    base_lines[k] = a * i0 + b * i1;
                    other_lines[k] = c * i0 + d * i1;
                }
            }
            window_start += static_cast<std::size_t>(base_lengths[window]);
        }
    }
    return {};
}

}  // namespace ac4::detail
