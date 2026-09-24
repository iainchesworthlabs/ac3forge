#include "optional_modules.hpp"

// The variant of the `ac3.signing` submodule compiled when this configure did
// not build ac3::signing - see optional_modules.hpp for the pair, and
// python/CMakeLists.txt for the selection.
//
// Registers nothing, deliberately. `ac3.signing` is simply absent from the
// module, so a caller reaching for it gets Python's own AttributeError, which
// is what the `#ifdef AC3FORGE_PY_HAVE_SIGNING` this replaced also produced -
// the behaviour is unchanged, only where the decision is written down.

namespace ac3::python {

void register_signing(pybind11::module_& /*m*/) {}

}  // namespace ac3::python
