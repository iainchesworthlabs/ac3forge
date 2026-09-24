#include "optional_modules.hpp"

#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"

#include "binding_support.hpp"

// The variant of the `ac3.signing` submodule compiled when ac3::signing is in this build.
//
// This is the body that used to sit inside bindings.cpp's PYBIND11_MODULE
// behind `#ifdef AC3FORGE_PY_HAVE_SIGNING`, moved verbatim. See
// optional_modules.hpp for why it is a translation unit now, and
// python/CMakeLists.txt for the selection that picks this file over the
// absent/ one beside it.

namespace ac3::python {

namespace py = pybind11;
using detail::to_bytes;
using detail::to_bytes_list;
using detail::KwargBinder;

void register_signing(py::module_& m) {
    // --- Object signing (Python bindings completeness) --------------------------------------
    auto signing = m.def_submodule(
        "signing",
        "EMDF object-layer signing - see docs/concepts/object-signing.md. Sign an encoded "
        "Atmos stream's frames, detect tags, verify with the matching key.");

    py::class_<ac3::signing::SigningKey>(signing, "SigningKey",
                                         "Owns the key bytes; zeroizes them on destruction.")
        .def(py::init([](const py::buffer& content) {
                 auto decoded = ac3::signing::decode_signing_key(to_bytes(content));
                 if (!decoded) {
                     throw py::value_error("empty signing key");
                 }
                 return *decoded;
             }),
             py::arg("content"),
             "Decode a key from bytes: base64 when the content is valid base64 (the "
             "CI/secret transport form), raw key bytes otherwise - the same single decode "
             "every other front end uses.")
        .def_property_readonly("empty", &ac3::signing::SigningKey::empty);

    signing.def(
        "load_signing_key",
        [](const std::string& explicit_path) {
            auto key = ac3::signing::load_signing_key(explicit_path);
            if (!key) {
                throw py::value_error(key.error().message);
            }
            return *key;
        },
        py::arg("path") = std::string{},
        "Resolve a key from `path` if given, else $AC3FORGE_SIGNING_KEY_FILE, else "
        "$AC3FORGE_SIGNING_KEY - the CLI's own resolution order. Raises ValueError with the "
        "loader's message when nothing usable was found.");

    signing.def(
        "sign_atmos_stream",
        [](const py::buffer& stream, const ac3::signing::SigningKey& key) {
            auto bytes = to_bytes(stream);
            int signed_count = 0;
            {
                py::gil_scoped_release release;
                signed_count = ac3::signing::sign_atmos_stream(bytes, key);
            }
            return py::make_tuple(
                py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                signed_count);
        },
        py::arg("stream"), py::arg("key"),
        "Sign every Atmos frame in an elementary stream. Returns (signed_stream, "
        "frames_signed) - the input is not modified (Python bytes are immutable; the C++ "
        "in-place form signs a copy here).");

    signing.def(
        "has_authenticity_tag",
        [](const py::buffer& frame) {
            return ac3::signing::has_authenticity_tag(to_bytes(frame));
        },
        py::arg("frame"),
        "Whether this frame carries an authenticity tag - answerable without any key.");

    py::class_<ac3::signing::VerifySummary>(signing, "VerifySummary")
        .def_readonly("valid", &ac3::signing::VerifySummary::valid)
        .def_readonly("mismatch", &ac3::signing::VerifySummary::mismatch)
        .def_readonly("no_container", &ac3::signing::VerifySummary::no_container)
        .def_property_readonly("all_valid", [](const ac3::signing::VerifySummary& s) {
            return s.valid > 0 && s.mismatch == 0;
        });

    signing.def(
        "verify_atmos_stream",
        [](const py::buffer& stream, const ac3::signing::SigningKey& key) {
            const auto bytes = to_bytes(stream);
            py::gil_scoped_release release;
            return ac3::signing::verify_atmos_stream(bytes, key);
        },
        py::arg("stream"), py::arg("key"),
        "Verify every frame's tag against `key`. See VerifySummary.");
}

}  // namespace ac3::python
