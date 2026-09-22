#include "../console_encoding.hpp"

#include <windows.h>

// WIN32_LEAN_AND_MEAN and NOMINMAX come from target_compile_definitions in
// this directory's CMakeLists.txt, not from a #define above the include -
// tools/checks/check_platform_macros.ps1's header comment says why, and
// src/audio/CMakeLists.txt is the worked example it points at.

namespace ac3::cli::platform {

namespace {

// Whether the given standard handle is a console screen buffer rather than a
// file or a pipe. GetConsoleMode() succeeds only on a console handle, which
// is the documented way to ask.
//
// Both stdout and stderr are asked, because they part company routinely here:
// `ac3cli encode in.wav - > out.ac3` sends the coded stream to a file and the
// human-readable level report to the console via stderr (support.hpp's
// status_stream() explains that split), so a rule that only looked at stdout
// would leave exactly that case unfixed. If NEITHER is a console then nothing
// this process writes can reach one, and changing the code page would be a
// side effect on a window we are not using - so it is left alone.
bool writes_to_a_console() {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0
           || GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != 0;
}

}  // namespace

unsigned int set_console_utf8() {
    if (!writes_to_a_console()) {
        return 0;
    }
    // 0 means no console is attached to the process at all, which
    // writes_to_a_console() has already ruled out; the check is kept because
    // 0 is also this function's "nothing to restore" answer and returning it
    // by accident would be silent.
    const UINT previous = GetConsoleOutputCP();
    if (previous == 0 || previous == CP_UTF8) {
        return 0;
    }
    if (SetConsoleOutputCP(CP_UTF8) == 0) {
        // Nothing to say and nothing to do. The output is no worse than it
        // was before the attempt, and a warning about the encoding would
        // have to be printed in the encoding that just failed to be set.
        return 0;
    }
    return previous;
}

void restore_console_encoding(unsigned int previous) {
    if (previous == 0) {
        return;
    }
    (void)SetConsoleOutputCP(previous);
}

}  // namespace ac3::cli::platform
