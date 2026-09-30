#include "optional_modules.hpp"

#include "iclforge/signing/emdf_atmos_signer.hpp"
#include "iclforge/signing/signing_key.hpp"

#include "binding_support.hpp"

// The variant of the `ac3.signing` submodule compiled when iclforge::signing is in this build.
//
// This is the body that used to sit inside bindings.cpp's PYBIND11_MODULE
// behind `#ifdef ICLFORGE_PY_HAVE_SIGNING`, moved verbatim. See
// optional_modules.hpp for why it is a translation unit now, and
// python/CMakeLists.txt for the selection that picks this file over the
// absent/ one beside it.

namespace iclforge::python {

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

    py::class_<iclforge::signing::SigningKey>(signing, "SigningKey",
                                         "Owns the key bytes; zeroizes them on destruction.")
        .def(py::init([](const py::buffer& content) {
                 auto decoded = iclforge::signing::decode_signing_key(to_bytes(content));
                 if (!decoded) {
                     throw py::value_error("empty signing key");
                 }
                 return *decoded;
             }),
             py::arg("content"),
             "Decode a key from bytes: base64 when the content is valid base64 (the "
             "CI/secret transport form), raw key bytes otherwise - the same single decode "
             "every other front end uses.")
        .def_property_readonly("empty", &iclforge::signing::SigningKey::empty);

    signing.def(
        "load_signing_key",
        [](const std::string& explicit_path) {
            auto key = iclforge::signing::load_signing_key(explicit_path);
            if (!key) {
                throw py::value_error(key.error().message);
            }
            return *key;
        },
        py::arg("path") = std::string{},
        "Resolve a key from `path` if given, else $ICLFORGE_SIGNING_KEY_FILE, else "
        "$ICLFORGE_SIGNING_KEY - the CLI's own resolution order. Raises ValueError with the "
        "loader's message when nothing usable was found.");

    signing.def(
        "sign_atmos_stream",
        [](const py::buffer& stream, const iclforge::signing::SigningKey& key) {
            auto bytes = to_bytes(stream);
            int signed_count = 0;
            {
                py::gil_scoped_release release;
                signed_count = iclforge::signing::sign_atmos_stream(bytes, key);
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
            return iclforge::signing::has_authenticity_tag(to_bytes(frame));
        },
        py::arg("frame"),
        "Whether this frame carries an authenticity tag - answerable without any key.");

    py::class_<iclforge::signing::VerifySummary>(signing, "VerifySummary")
        .def_readonly("valid", &iclforge::signing::VerifySummary::valid)
        .def_readonly("mismatch", &iclforge::signing::VerifySummary::mismatch)
        .def_readonly("no_container", &iclforge::signing::VerifySummary::no_container)
        .def_property_readonly("all_valid", [](const iclforge::signing::VerifySummary& s) {
            return s.valid > 0 && s.mismatch == 0;
        });

    signing.def(
        "verify_atmos_stream",
        [](const py::buffer& stream, const iclforge::signing::SigningKey& key) {
            const auto bytes = to_bytes(stream);
            py::gil_scoped_release release;
            return iclforge::signing::verify_atmos_stream(bytes, key);
        },
        py::arg("stream"), py::arg("key"),
        "Verify every frame's tag against `key`. See VerifySummary.");
}

}  // namespace iclforge::python
