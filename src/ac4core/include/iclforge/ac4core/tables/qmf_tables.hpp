#pragma once

#include <array>

// ETSI TS 103 190-1 V1.4.1 Annex D.3, QWIN, and Annex D.2, ASPX_NOISE. GENERATED
// by tools/generators/gen_ac4_tables.py from the attachment ts_103190_tables.c;
// do not edit by hand.

namespace ac4::detail::tables {

// The window of the QMF analysis and synthesis banks (clauses 5.7.3 and
// 5.7.4), in the float the attachment declares. It carries its own signs.
extern const std::array<float, 640> kQwin;

// NoiseTable of A-SPX's noise generator (clause 5.7.6.4.3): complex numbers
// of random phase and mean energy 1, {real, imaginary}, in the float the
// attachment declares.
extern const std::array<std::array<float, 2>, 512> kAspxNoise;

}  // namespace ac4::detail::tables
