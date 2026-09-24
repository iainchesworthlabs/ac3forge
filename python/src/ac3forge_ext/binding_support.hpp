#pragma once

// The full caster set, not just pybind11.h, and deliberately here rather than
// in each translation unit that happens to need one.
//
// pybind11's type casters are header-only and per-TU: a .cpp that binds a
// function taking std::vector or returning py::array WITHOUT <pybind11/stl.h>
// or <pybind11/numpy.h> in scope still compiles and still registers, but
// registers a signature that refuses the Python types it should accept - a
// TypeError at call time, not a build error. That is exactly the trap splitting
// bindings.cpp into several translation units opens up, and it bit this split
// once already (containers.mux_matroska rejecting a plain list of bytes). Every
// TU in this extension includes this header, so putting the casters here is
// what makes that failure impossible to reintroduce by omission.
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <span>

#include <cstddef>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// The two pieces of binding-layer machinery that more than one translation
// unit in this extension needs.
//
// Both lived in bindings.cpp's anonymous namespace while that file WAS the
// extension. The optional submodules - signing/ and containers/, each present
// only when the configure that built this also built the library behind it -
// used to be `#ifdef AC3FORGE_PY_HAVE_SIGNING` / `AC3FORGE_PY_HAVE_CONTAINERS`
// blocks inside the same PYBIND11_MODULE body, and so had these to hand. They
// are now their own directory-selected translation units (see
// optional_modules.hpp and python/CMakeLists.txt), which is what makes this a
// header rather than a detail of one .cpp.

namespace ac3::python::detail {

namespace py = pybind11;

// --- kwargs-constructible plain config structs ------------------------------
// Every EncoderConfig/AtmosConfig/DecoderConfig/Profile/HeavyConfig/Position/ObjectPlacement
// field already has a real C++ default, so a Python caller only needs to name what they want to
// change - this is the one piece of binding-layer machinery that gets reused across all of them,
// in place of writing the same "collect kwargs, apply known ones, reject the rest" logic six
// times over. Rejecting unknown keys (rather than silently ignoring them) catches the ordinary
// typo ("EncoderConfig(dialnrm=10)") that a silently-accepted **kwargs would hide as a
// wrong-but-legal default.
template <typename T>
class KwargBinder {
   public:
    explicit KwargBinder(py::kwargs kwargs) : kwargs_(std::move(kwargs)) {}

    template <typename V>
    KwargBinder& field(const char* name, V T::* member) {
        if (kwargs_.contains(name)) {
            // kwargs_[name] is an access through the implicit this-> of a class template, so
            // two-phase lookup treats it as dependent regardless of kwargs_'s own (non-dependent)
            // declared type - Clang enforces the standard's `template` disambiguator here and
            // rejects the call without it; MSVC/GCC merely tolerate the omission as a
            // permissive extension. Confirmed the hard way: this built clean on both of those
            // and only failed in CI's real AppleClang leg.
            value_.*member = kwargs_[name].template cast<V>();
            seen_.insert(name);
        }
        return *this;
    }

    T finish() {
        for (auto item : kwargs_) {
            const auto key = py::cast<std::string>(item.first);
            if (!seen_.contains(key)) {
                throw py::type_error("unexpected keyword argument '" + key + "'");
            }
        }
        return value_;
    }

   private:
    py::kwargs kwargs_;
    T value_{};
    std::set<std::string> seen_;
};

// --- buffer / array plumbing -------------------------------------------------

inline std::vector<std::byte> to_bytes(const py::buffer& buf) {
    const py::buffer_info info = buf.request();
    if (info.ndim != 1) {
        throw py::value_error("expected a 1-D bytes-like object");
    }
    const auto total =
        static_cast<std::size_t>(info.size) * static_cast<std::size_t>(info.itemsize);
    std::vector<std::byte> out(total);
    if (total > 0) {
        std::memcpy(out.data(), info.ptr, total);
    }
    return out;
}

// The span-list twin of to_bytes above, for the read/demux side: every
// container reader hands back spans into its own buffer, and Python wants
// owning bytes objects.
inline std::vector<py::bytes> to_bytes_list(const std::vector<std::span<const std::byte>>& spans) {
    std::vector<py::bytes> out;
    out.reserve(spans.size());
    for (const auto span : spans) {
        out.emplace_back(reinterpret_cast<const char*>(span.data()), span.size());
    }
    return out;
}

}  // namespace ac3::python::detail
