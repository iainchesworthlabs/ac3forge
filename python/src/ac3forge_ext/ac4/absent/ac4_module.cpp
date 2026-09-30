#include "optional_modules.hpp"

// The variant of the `ac3.ac4` submodule compiled when this configure did not
// build iclforge::ac4/iclforge::ac4dec/iclforge::ac4enc - see optional_modules.hpp for the
// pair, and python/CMakeLists.txt for the selection.
//
// Registers nothing, deliberately, on the same reasoning as the signing and
// containers absent/ variants beside it. `ac3.ac4` is simply absent from the
// module, so a caller reaching for it gets Python's own AttributeError.

namespace iclforge::python {

void register_ac4(pybind11::module_& /*m*/) {}

}  // namespace iclforge::python
