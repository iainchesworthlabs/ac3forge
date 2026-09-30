#pragma once

#include <vector>

// Kaiser-Bessel derived windows, ETSI TS 103 190-1 V1.4.1 clause 5.5.3.
//
// KBD_LEFT(N, n) = sqrt( sum_{p=0..n} W(N, p, a) / sum_{p=0..N} W(N, p, a) ),  0 <= n < N
// KBD_RIGHT(N, n) = sqrt( sum_{p=0..2N-n-1} W(N, p, a) / sum_{p=0..N} W(N, p, a) ),  N <= n < 2N
// W(N, n, a) = I0(pi a sqrt(1 - (2n/N - 1)^2)) / I0(pi a)
//
// with the alpha of Table 186 for the transform length. The sums run to
// p = N, one past the n < N the text gives W for; W is symmetric about N/2
// and defined there (src/ac4dec/ERRATA.md, "The KBD kernel is summed to
// p = N"). The right half is the left half reversed,
// KBD_RIGHT(N, N + n) = KBD_LEFT(N, N - 1 - n), and the halves that overlap
// meet the Princen-Bradley condition KBD_LEFT(N, n)^2 + KBD_RIGHT(N, N + n)^2
// = 1, since W(N, p) = W(N, N - p).

namespace ac4::detail::dsp {

// Table 186's alpha for a transform length of `length` samples at a sampling
// frequency `rate_multiplier` times the 44.1/48 kHz base (1, 2 or 4). 0 for a
// length the table does not list.
[[nodiscard]] double kbd_alpha(int length, int rate_multiplier) noexcept;

// KBD_LEFT(length, n) for n = 0 .. length - 1, in double. Empty for a length
// of 0 or less.
[[nodiscard]] std::vector<double> kbd_left(int length, double alpha);

// The zeroth-order modified Bessel function of the first kind, by the series
// the clause gives: sum_{k>=0} ((x/2)^k / k!)^2.
[[nodiscard]] double bessel_i0(double x) noexcept;

}  // namespace ac4::detail::dsp
