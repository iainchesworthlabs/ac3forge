#include "iclforge/ac4core/dsp/transform_tables.hpp"

#include <array>
#include <cstddef>
#include <type_traits>

#include "iclforge/ac4core/tables/transform_tables.hpp"

namespace iclforge::ac4::detail::dsp {
namespace {

// {re, im} pairs narrowed as Fft and Imdct narrow theirs: each double cast to the scalar once.
template <typename R, std::size_t M>
constexpr std::array<Complex<R>, M / 2> complexes(const std::array<double, M>& values) {
    std::array<Complex<R>, M / 2> out{};
    for (std::size_t i = 0; i < M / 2; ++i) {
        out[i] = Complex<R>(static_cast<R>(values[2 * i]), static_cast<R>(values[2 * i + 1]));
    }
    return out;
}

// Imdct's post-twiddle at Fixed32: the pre-twiddle's doubles times 2^post_shift / N, with
// 2^(post_shift + 1) the least power of two not below N, then narrowed.
template <typename R, std::size_t N, std::size_t M>
constexpr std::array<Complex<R>, M / 2> post_twiddles(const std::array<double, M>& pre) {
    unsigned shift = 0;
    while ((std::size_t{1} << shift) < N) {
        ++shift;
    }
    const double factor =
        static_cast<double>(std::size_t{1} << (shift - 1)) / static_cast<double>(N);
    std::array<Complex<R>, M / 2> out{};
    for (std::size_t i = 0; i < M / 2; ++i) {
        out[i] = Complex<R>(R(pre[2 * i] * factor), R(pre[2 * i + 1] * factor));
    }
    return out;
}

template <typename R, std::size_t N>
constexpr std::array<R, N> reals(const std::array<double, N>& values) {
    std::array<R, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = static_cast<R>(values[i]);
    }
    return out;
}

template <typename R, std::size_t N, std::size_t Roots, std::size_t Pre>
struct Built {
    static constexpr bool kFixed = !std::is_floating_point_v<R>;
    std::array<Complex<R>, Roots / 2> roots;
    std::array<Complex<R>, Pre / 2> pre;
    std::array<Complex<R>, kFixed ? Pre / 2 : 0> post;
    std::array<R, N> kbd;

    [[nodiscard]] constexpr TransformTable<R> view() const {
        return TransformTable<R>{roots, pre, post, kbd};
    }
};

template <typename R, std::size_t N, std::size_t Roots, std::size_t Pre>
constexpr Built<R, N, Roots, Pre> build(const std::array<double, Roots>& roots,
                                        const std::array<double, Pre>& pre,
                                        const std::array<double, N>& kbd) {
    Built<R, N, Roots, Pre> out{complexes<R>(roots), complexes<R>(pre), {}, reals<R>(kbd)};
    if constexpr (Built<R, N, Roots, Pre>::kFixed) {
        out.post = post_twiddles<R, N>(pre);
    }
    return out;
}

// Function-local constexpr, so a build whose scalar is double, which never calls this,
// neither evaluates nor links any of it. Constant-initialised: no guard.
template <typename R>
const TransformTable<R>* lookup(std::size_t length) noexcept {
    static constexpr auto k2048 =
        build<R>(tables::kFftRoots2048, tables::kPreTwiddle2048, tables::kKbdLeft2048);
    static constexpr auto k1024 =
        build<R>(tables::kFftRoots1024, tables::kPreTwiddle1024, tables::kKbdLeft1024);
    static constexpr auto k512 =
        build<R>(tables::kFftRoots512, tables::kPreTwiddle512, tables::kKbdLeft512);
    static constexpr auto k256 =
        build<R>(tables::kFftRoots256, tables::kPreTwiddle256, tables::kKbdLeft256);
    static constexpr auto k128 =
        build<R>(tables::kFftRoots128, tables::kPreTwiddle128, tables::kKbdLeft128);
    static constexpr std::array<TransformTable<R>, 5> kTables{k2048.view(), k1024.view(), k512.view(),
                                                              k256.view(), k128.view()};
    for (const TransformTable<R>& table : kTables) {
        if (table.kbd_left.size() == length) {
            return &table;
        }
    }
    return nullptr;
}

}  // namespace

const TransformTable<Real>* built_in_transform_table(std::size_t length) noexcept {
    return []<typename R>(std::size_t n) -> const TransformTable<R>* {
        if constexpr (std::is_same_v<R, double>) {
            (void)n;
            return nullptr;
        } else {
            return lookup<R>(n);
        }
    }.template operator()<Real>(length);
}

}  // namespace iclforge::ac4::detail::dsp
