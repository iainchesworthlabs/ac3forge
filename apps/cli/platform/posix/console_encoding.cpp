#include "../console_encoding.hpp"

// A POSIX terminal has no per-console code page to set: the encoding is the
// locale's, every distribution and every macOS install this project targets
// ships a UTF-8 one, and a terminal has been reading a byte stream as UTF-8
// without being asked since long before any of this. The section signs and em
// dashes the CLI prints have always rendered correctly here - the defect this
// seam exists for is Windows-only.
//
// This file exists rather than folding a no-op into main.cpp for the reason
// stdio_binary.cpp's POSIX half gives: the call site never asks which
// platform it is running on.

namespace ac3::cli::platform {

unsigned int set_console_utf8() {
    return 0;
}

void restore_console_encoding(unsigned int /*previous*/) {
    // Nothing to restore: set_console_utf8() above never changed anything.
}

}  // namespace ac3::cli::platform
