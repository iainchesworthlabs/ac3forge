#include "optional_modules.hpp"

// The variant of the `ac3.ac4` submodule compiled when this configure did not
// build ac4::ac4/ac4::decoder/ac4::encoder - see optional_modules.hpp for the
// pair, and python/CMakeLists.txt for the selection.
//
// Registers nothing, deliberately, on the same reasoning as the signing and
// containers absent/ variants beside it. `ac3.ac4` is simply absent from the
// module, so a caller reaching for it gets Python's own AttributeError.

namespace ac3::python {

void register_ac4(pybind11::module_& /*m*/) {}

}  // namespace ac3::python
