#pragma once

#include <string_view>

// Decimal -> double for the scene file formats, portably.
//
// Writing has no equivalent helper here: fmt::format("{}", value) already IS
// the shortest decimal that reads back as the same double - the same
// guarantee std::format makes, since {fmt} is the library it was standardized
// from - and {fmt} is safe everywhere this project builds (see
// CONTRIBUTING.md's code-conventions section), so scene.cpp/scene_json.cpp
// call it directly rather than through a wrapper.
//
// Reading is the one direction {fmt} does not cover: it formats text, it does
// not parse it. <charconv>'s FLOATING-POINT from_chars is absent from some
// libc++ builds this project targets - the NDK r26 libc++ implements only the
// integer overloads (see docs/platforms/android.md and
// parse_unit_double's identical note in encoder/plan.cpp), and the macOS
// wheel's own deployment target puts it out of reach too ("'from_chars' is
// unavailable: introduced in macOS 26.0"). So reading goes through strtod
// instead - the same locale-independent, reject-all-trailing-garbage contract
// plan.cpp and encoder/assignment.cpp already hand-roll for the identical
// reason.

namespace ac3::oba {

// The whole of `token` as a double, or false: no leading/trailing spaces, no
// trailing characters, no out-of-range magnitude. A leading '+' is accepted,
// which the keyframe grammar's original istringstream extraction also was.
[[nodiscard]] bool read_double(std::string_view token, double& out);

}  // namespace ac3::oba
