#include "optional_modules.hpp"

// The variant of the `ac3.containers` submodule compiled when this configure
// did not build matroska/mp4/mpegts - see optional_modules.hpp for the pair,
// and python/CMakeLists.txt for the selection.
//
// Registers nothing, deliberately, on the same reasoning as the signing
// absent/ variant beside it.

namespace ac3::python {

void register_containers(pybind11::module_& /*m*/) {}

}  // namespace ac3::python
