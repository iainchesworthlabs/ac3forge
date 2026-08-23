#pragma once

#include <string>
#include <string_view>

// Decimal <-> double for the scene file formats, portably.
//
// Neither of the obvious tools is available everywhere this library is built.
// <charconv>'s FLOATING-POINT from_chars is absent from some libc++ builds this
// project targets - the NDK r26 libc++ implements only the integer overloads
// (see docs/platforms/android.md and parse_unit_double's identical note in
// encoder/plan.cpp), and the macOS wheel's own deployment target puts both the
// double from_chars and the to_chars behind std::format's floating-point
// formatter out of reach ("'from_chars' is unavailable: introduced in macOS
// 26.0", "'to_chars' is unavailable: introduced in macOS 13.3"). Integer
// from_chars is fine everywhere and is still used directly.
//
// So: reading is strtod, the same locale-independent, reject-all-trailing-
// garbage contract plan.cpp and encoder/assignment.cpp already hand-roll; and
// writing is an ostringstream imbued with the classic locale, which cannot be
// disturbed by a host application's locale at all.

namespace ac3::oba {

// The whole of `token` as a double, or false: no leading/trailing spaces, no
// trailing characters, no out-of-range magnitude. A leading '+' is accepted,
// which the keyframe grammar's original istringstream extraction also was.
[[nodiscard]] bool read_double(std::string_view token, double& out);

// The shortest decimal that read_double() turns back into exactly this double
// - found by asking for one significant digit more until the round trip holds,
// so writer and reader agree by construction rather than by assertion. A scene
// file therefore survives a save/load cycle bit-exactly, and a diff of one
// shows the values that changed rather than a reflowed field width.
[[nodiscard]] std::string write_double(double value);

}  // namespace ac3::oba
