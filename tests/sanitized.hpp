#pragma once

// Whether this build runs the tests under the sanitizers (cmake/Sanitizers.cmake).
//
// Under ASan and UBSan in a debug build the codecs run many times slower, and
// the sanitizer leg of CI runs ctest serially (the tests share scratch paths),
// so the heaviest tests take less there: fewer frames, shorter signals, a
// stride through their cases, one frame rate or layout where a normal build
// takes each. Each still runs every code path it covers at least once, to the
// same tolerances; a normal build runs every case at full length.
//
// tests/CMakeLists.txt defines AC3FORGE_TEST_SANITIZED as 1 when
// AC3FORGE_SANITIZERS names any and as 0 when it does not, so that no test
// asks the preprocessor which it is.

namespace ac3::test {

inline constexpr bool kSanitized = AC3FORGE_TEST_SANITIZED != 0;

}  // namespace ac3::test
