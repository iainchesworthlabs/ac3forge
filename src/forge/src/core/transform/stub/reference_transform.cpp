#include "reference_transform.hpp"

#include <cassert>
#include <cstddef>
#include <span>

#include "ac3/internal/profile.hpp"

// The variant of src/core/reference_transform.hpp that carries NO direct-form
// tables - what the minimum-footprint decoder profile compiles
// (AC3FORGE_MINIMAL_DECODER, roadmap PF7). Selecting this file instead of
// src/core/transform/reference/reference_transform.cpp removes 1,900,544
// bytes of .bss from the link: the four (k, n) matrices §8.2.3.2's forward
// MDCT and §7.9.4.2 step 3's inverse sums need. See the header for the
// per-table byte counts and how they were measured.
//
// None of these bodies can run. ac3::internal::kReferenceTransformAvailable
// is false in this profile, every call site checks it before dispatching, and
// the public API refuses the configuration that would need the direct form
// (DecoderConfig::fast_imdct == false yields DecodeError::kUnsupported) rather
// than silently running the fast path in its place - substituting a different
// arithmetic for the one the caller asked to validate against would defeat the
// only reason that switch exists. The asserts are the backstop for a future
// call site added without the guard; the zero fill is what an NDEBUG build
// does next, and it is deliberately a wrong answer rather than a plausible
// one, so such a bug shows up immediately in a round trip rather than as a
// slight quality change nobody attributes.

namespace ac3::internal {

static_assert(!kReferenceTransformAvailable,
              "this translation unit is only for the profile that declares the direct-form "
              "transform absent; the other variant carries the tables");

namespace {

void unreachable_fill(std::span<double> a, std::span<double> b) {
    for (double& value : a) {
        value = 0.0;
    }
    for (double& value : b) {
        value = 0.0;
    }
}

}  // namespace

// NOLINTNEXTLINE(cert-dcl03-c,misc-static-assert) - a runtime guard, not a
// compile-time one: reaching here at all is the bug being reported.
void reference_mdct512_forward(std::span<const double, 512> /*windowed*/,
                               std::span<double, 256> coeffs) {
    assert(false && "direct-form MDCT is absent from this build (AC3FORGE_MINIMAL_DECODER)");
    unreachable_fill(coeffs, {});
}

// NOLINTNEXTLINE(cert-dcl03-c,misc-static-assert)
void reference_mdct256_forward_first(std::span<const double, 256> /*windowed*/,
                                     std::span<double, 128> coeffs) {
    assert(false && "direct-form MDCT is absent from this build (AC3FORGE_MINIMAL_DECODER)");
    unreachable_fill(coeffs, {});
}

// NOLINTNEXTLINE(cert-dcl03-c,misc-static-assert)
void reference_mdct256_forward_second(std::span<const double, 256> /*windowed*/,
                                      std::span<double, 128> coeffs) {
    assert(false && "direct-form MDCT is absent from this build (AC3FORGE_MINIMAL_DECODER)");
    unreachable_fill(coeffs, {});
}

// NOLINTNEXTLINE(cert-dcl03-c,misc-static-assert)
void reference_inner_sum_128(std::span<const double, 128> /*z_re*/,
                             std::span<const double, 128> /*z_im*/, std::span<double, 128> t_re,
                             std::span<double, 128> t_im) {
    assert(false && "direct-form IMDCT is absent from this build (AC3FORGE_MINIMAL_DECODER)");
    unreachable_fill(t_re, t_im);
}

// NOLINTNEXTLINE(cert-dcl03-c,misc-static-assert)
void reference_inner_sum_64(std::span<const double, 64> /*z_re*/,
                            std::span<const double, 64> /*z_im*/, std::span<double, 64> t_re,
                            std::span<double, 64> t_im) {
    assert(false && "direct-form IMDCT is absent from this build (AC3FORGE_MINIMAL_DECODER)");
    unreachable_fill(t_re, t_im);
}

}  // namespace ac3::internal
