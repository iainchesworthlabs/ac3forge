#pragma once

#include <pybind11/pybind11.h>

// The extension's two optional submodules, behind declarations so the one
// PYBIND11_MODULE body can register them without knowing whether they have
// anything to register.
//
// `ac3.signing` and `ac3.containers` exist only when the configure that built
// this extension also built the libraries behind them - ac3::signing, and the
// matroska/mp4/mpegts trio (see python/CMakeLists.txt, and the options'
// comments in the root CMakeLists.txt). A trimmed developer build - codec only
// - still produces a working extension without them; the wheel build
// (python/pyproject.toml) turns every one of those targets ON, so a published
// wheel always carries the full surface.
//
// That used to be two `#ifdef AC3FORGE_PY_HAVE_*` blocks, about three hundred
// lines of them, inside bindings.cpp's module body. Each is now its own
// translation unit in a {present,absent} directory pair, selected by CMake on
// exactly the same condition that decides the compile definition did - which
// is how every other either/or in this repository is answered (see
// src/core/transform/{reference,stub}/, src/internal/avx2/none/,
// tests/core/avx2/{present,absent}/ and tools/checks/check_platform_macros.ps1
// for the rule itself).
//
// The absent variants are not empty files: each registers nothing and says so.
// A caller reaching for ac3.signing in a build without it gets Python's own
// AttributeError on the missing submodule, exactly as before.

namespace ac3::python {

// Adds the `signing` submodule to `m`, or does nothing in a build with no
// ac3::signing behind it.
void register_signing(pybind11::module_& m);

// Adds the `containers` submodule to `m`, or does nothing in a build with no
// matroska/mp4/mpegts behind it.
void register_containers(pybind11::module_& m);

}  // namespace ac3::python
