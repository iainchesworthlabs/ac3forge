#pragma once

// The type src/ac4core's kernels are explicitly instantiated at (the
// `template class Foo<Real>;` lines in dsp/qmf.hpp, dsp/mdct.hpp, dsp/fft.hpp,
// aspx/hf_generator.hpp, acpl/acpl.hpp, ajcc/ajcc.hpp and ajoc/ajoc.hpp) - the
// DOUBLE variant. The float variant is the identically-pathed header under
// src/internal/scalar/float/; src/ac4core/CMakeLists.txt picks the directory
// from ICLFORGE_DECODE_SCALAR, the same cache variable src/ac3/CMakeLists.txt
// resolves its own decode_scalar_t from (planning/ac4.md, "AC-4 joins
// ICLFORGE_DECODE_SCALAR"), so no source file asks which it is with a
// preprocessor conditional (tools/checks/check_platform_macros.ps1's rule).
//
// This is not the same mechanism as iclforge::ac3::internal::decode_scalar_t: that name
// is used directly, as a concrete type, throughout iclforge::ac3's own decoder;
// AC-4's kernels stay templated on `Real` (a template parameter, in scope
// only inside each template's own definition) so that a future phase can
// still instantiate them at iclforge::internal::Fixed32 too. `Real` here is the
// unqualified name every explicit-instantiation line outside those templates
// resolves through ordinary enclosing-namespace lookup: iclforge::ac4::detail::dsp,
// iclforge::ac4::detail::aspx, iclforge::ac4::detail::acpl, iclforge::ac4::detail::ajcc and
// iclforge::ac4::detail::ajoc all nest inside iclforge::ac4::detail, where this alias lives.
//
// double: every build outside the ESP32 boards, and the reference every
// other tier is measured against (planning/arithmetic-tiers.md).

namespace iclforge::ac4::detail {

using Real = double;

}  // namespace iclforge::ac4::detail

// Explicit instantiations a translation unit adds at double beside its
// `template class Foo<Real>;` when Real is not double: the encoder (src/ac4enc)
// always runs at double, decision 34 (planning/ac4.md), and ac4core is one
// archive both ac4dec and ac4enc link, so each kernel the encoder calls is
// instantiated at both. Here Real is double, so the line above is already that
// instantiation and a second one is ill-formed: this expands to nothing. It
// takes the instantiation as its argument, rather than a preprocessor
// conditional around it, for tools/checks/check_platform_macros.ps1's rule.
#define AC4CORE_ALSO_AT_DOUBLE(...)
