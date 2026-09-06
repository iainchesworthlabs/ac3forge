#pragma once

// Makes a Windows console render what ac3cli actually prints.
//
// The CLI's own text is UTF-8 and always has been. Every source file here is
// UTF-8, the root CMakeLists.txt gives MSVC /utf-8 (so both its source and
// its execution character set are UTF-8, and a narrow literal reaches the
// binary as the bytes the file holds; GCC and Clang default to the same), and
// a little over a hundred characters outside ASCII sit in the string
// literals of usage.cpp, support.cpp, the commands/ tree and main.cpp's own
// command table - 106 of them, counted 2026-09-06. Eighty are the section
// sign in a specification citation - "A/52 §7.8", "§E2.3.1.2" - which is how
// this project cites a clause everywhere else too, in the docs and in the
// comments. The remaining twenty-six are eighteen em dashes, five ellipses
// closing a progress line ("streaming ..." in audio_io.cpp, "monitoring ..."
// and "spatial: ..." in live_audio.cpp), and three degree signs: one in the
// soundfield azimuth readout, two in the help text for §7.8.2's 90° surround
// phase shift.
//
// A Windows console does not read those bytes as UTF-8 unless it is told to.
// Left on the system's own code page - 850 or 437 on a stock English install,
// 1252 where the OEM/ANSI setting has been changed - the two bytes of § come
// out as two unrelated glyphs, and a spec citation reads as line noise. So
// every command that prints one, which is most of them, has been printing
// rubbish on the platform this project's own installer targets.
//
// SetConsoleOutputCP(CP_UTF8) is the whole fix, and it is deliberately the
// ONLY thing done here:
//
//   - It changes how the console translates the bytes a process writes to it.
//     It does not touch the bytes. When stdout is redirected to a file or a
//     pipe the writes never go through the console at all, so a redirect
//     keeps exactly the UTF-8 bytes it produces today and a `> out.txt` or a
//     `| findstr` is byte-identical either side of this change. That is the
//     property that ruled out the alternatives: _setmode(_O_U8TEXT) on stdout
//     demands wide-character writes and aborts on a narrow one, and a
//     setlocale() call changes how the CRT converts, which redirection does
//     see.
//   - It is a process-wide, one-time call rather than something each command
//     opts into. The CLI has around forty commands, and a fix that a new
//     command has to remember is a fix that lasts until the next command.
//   - It says nothing about console INPUT (SetConsoleCP) or about how the CRT
//     decoded argv, which is a separate question with a separate answer: this
//     tool reads no text from the console, and a non-ASCII path passed as an
//     argument is a problem this does not claim to have solved.
//
// The code page belongs to the console, not to the process, so it outlives
// the run: setting it and leaving would silently change how every LATER
// command in that window renders its own output. Hence the pair - the
// call site holds the returned value and gives it back on the way out (see
// main.cpp's ConsoleEncoding guard). A run killed with Ctrl-C or terminated
// outright skips that, which is the same bargain every console tool that
// touches the code page makes.
//
// The second entry in ac3cli's platform seam, alongside stdio_binary.hpp,
// and selected the same way: see this directory's CMakeLists.txt entry for
// the WIN32/else split, and tools/checks/check_platform_macros.ps1 for why an
// #ifdef in main.cpp is not an option.

namespace ac3::cli::platform {

// Switches the attached console to UTF-8 for output, and returns the code
// page that was in force so it can be put back. Returns 0 when there is
// nothing to restore - no console, output already UTF-8, or the call failed -
// in which case restore_console_encoding() does nothing.
unsigned int set_console_utf8();

// Undoes set_console_utf8(). Passing 0 is the documented no-op.
void restore_console_encoding(unsigned int previous);

}  // namespace ac3::cli::platform
