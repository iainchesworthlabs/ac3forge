// The inverse transform's built-in tables (src/ac4core/include/iclforge/ac4core/dsp/transform_tables.hpp,
// planning/ac4.md, the decoder's memory): at the float and fixed-point tiers a transform reads its
// roots, twiddles and window from flash for the lengths of a 2048-sample frame, and every value
// there must be the bits the decoder computes when it builds its own, on the compiler and C
// library at hand, or the decoder's output would move. At double nothing is built in.

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/dsp/fft.hpp"
#include "iclforge/ac4core/dsp/mdct.hpp"
#include "iclforge/ac4core/dsp/synthesis.hpp"
#include "iclforge/ac4core/dsp/transform_tables.hpp"

namespace dsp = iclforge::ac4::detail::dsp;
using iclforge::ac4::detail::Real;

namespace {

// A value's bits, so that -0.0 and +0.0 differ, as they can in what follows them.
template <typename R>
std::uint64_t bits(R value) {
    if constexpr (std::is_same_v<R, float>) {
        return std::bit_cast<std::uint32_t>(value);
    } else if constexpr (std::is_same_v<R, double>) {
        return std::bit_cast<std::uint64_t>(value);
    } else {
        return static_cast<std::uint32_t>(value.raw);
    }
}

// The index of the first value that differs, or the size when none does.
template <typename R>
std::size_t first_difference(std::span<const dsp::Complex<R>> built_in,
                             const std::vector<dsp::Complex<R>>& computed) {
    if (built_in.size() != computed.size()) {
        return 0;
    }
    for (std::size_t i = 0; i < computed.size(); ++i) {
        if (bits(built_in[i].re) != bits(computed[i].re) ||
            bits(built_in[i].im) != bits(computed[i].im)) {
            return i;
        }
    }
    return computed.size();
}

template <typename R>
std::size_t first_difference(std::span<const R> built_in, const std::vector<R>& computed) {
    if (built_in.size() != computed.size()) {
        return 0;
    }
    for (std::size_t i = 0; i < computed.size(); ++i) {
        if (bits(built_in[i]) != bits(computed[i])) {
            return i;
        }
    }
    return computed.size();
}

constexpr std::size_t kLengths[] = {2048, 1024, 512, 256, 128};

}  // namespace

TEST_CASE("The built-in transform tables are the bits the decoder computes",
          "[ac4][ac4core][dsp][transform_tables]") {
    for (const std::size_t n : kLengths) {
        CAPTURE(n);
        const dsp::TransformTable<Real>* const table = dsp::transform_table<Real>(n);
        if constexpr (std::is_same_v<Real, double>) {
            CHECK(table == nullptr);
            continue;
        }
        REQUIRE(table != nullptr);

        const auto roots = dsp::Fft<Real>::computed_roots(n / 2);
        CHECK(first_difference<Real>(table->fft_roots, roots) == roots.size());

        const auto pre = dsp::Imdct<Real>::computed_pre_twiddles(n);
        CHECK(first_difference<Real>(table->pre_twiddle, pre) == pre.size());

        const auto post = dsp::Imdct<Real>::computed_post_twiddles(n);
        CHECK(first_difference<Real>(table->post_twiddle, post) == post.size());
        CHECK(post.empty() == std::is_floating_point_v<Real>);

        const auto kbd = dsp::TransformSet<Real>::computed_kbd_left(static_cast<int>(n), 1);
        CHECK(first_difference<Real>(table->kbd_left, kbd) == kbd.size());
    }
}

TEST_CASE("A length with no built-in table has none", "[ac4][ac4core][dsp][transform_tables]") {
    for (const std::size_t n : {std::size_t{1920}, std::size_t{1536}, std::size_t{96}, std::size_t{4096}}) {
        CAPTURE(n);
        CHECK(dsp::transform_table<Real>(n) == nullptr);
    }
    CHECK(dsp::transform_table<double>(2048) == nullptr);
}
