#pragma once

#include <string>
#include <string_view>

// The test suite's own platform seam: the two things a test needs from the
// operating system that the standard library does not portably provide.
//
// Both used to be written inline, behind `#ifdef _WIN32`, in every test file
// that needed them - nineteen copies of the process-id branch and eight of
// the std::system() one, each with its own drifting copy of the explanatory
// comment. That is exactly the arrangement the rest of this repository does
// not have: src/audio/src/backend/<backend>/, apps/cli/platform/<os>/ and
// tests/crt/<runtime>/ all ship one filename per directory and let CMake
// compile the directory that matches the target, so no source file has to ask
// which platform it is on. tools/checks/check_platform_macros.ps1 holds that
// line over src/ and apps/; this directory is what lets it hold over tests/
// too, which is where every remaining violation lived.
//
// See this directory's WIN32/else split in tests/CMakeLists.txt for the
// selection itself.

namespace ac3::test::platform {

// This process's OS-assigned id, rendered as a string.
//
// Every test that writes a file puts its artefacts under a leaf of
// AC3FORGE_TEST_SCRATCH_DIR (see tests/CMakeLists.txt), and that root is
// keyed to the build tree rather than to the process - so two ac3tests
// runs against one build tree (a concurrent re-run, or two sessions sharing
// a tree) would otherwise race on the same directory, one's
// remove_all/create_directories/file-open colliding with the other's
// mid-test. Folding this on top of the leaf name gives each process its own.
//
// A process id is the right token for that and there is no portable way to
// ask for one: POSIX spells it getpid() in <unistd.h>, the Windows CRT
// spells it _getpid() in <process.h>. Hence the seam. Returned already
// converted, because a string is what all nineteen call sites want and
// std::to_string at each of them is nineteen more chances to differ.
std::string process_id();

// Runs `command` through the system command interpreter and returns the
// child's OWN exit code - 0..7 for ac3cli (apps/cli/exit_codes.hpp), or
// 128 + signal where a POSIX child was killed rather than exiting, or -1
// where the interpreter could not be started at all.
//
// This is deliberately NOT std::system()'s return value, which is two
// different things on the two platforms: the child's exit code on Windows,
// but the raw wait() status word on POSIX, where exit code 1 arrives as 256
// (1 << 8) until WIFEXITED/WEXITSTATUS unpack it. A test that compares
// std::system()'s result against anything other than zero is therefore a
// Windows-only check wearing a portable face. Both implementations return
// the same number here so a caller can compare it against a real exit code.
//
// Quoting is the other half. Windows hands `command` to `cmd.exe /c`, and
// where the command both contains spaces and starts with its own quoted
// executable path - which every ac3cli invocation in this suite does - the
// CRT's argument quoting backslash-escapes the embedded quotes on the way,
// and cmd.exe does not read \" as an escaped quote, so what runs is
// corrupted ("The filename, directory name, or volume label syntax is
// incorrect"). Wrapping the whole string in one more quote pair is the
// standard workaround: cmd.exe's own "strip a matching outer pair" rule
// removes exactly that pair and hands the layer beneath it through intact.
// POSIX `sh -c` has no such rule - the same extra pair there would make sh
// read the entire command, redirections included, as one quoted word. So the
// wrapping belongs on the Windows side of this seam and nowhere else.
int run_shell(std::string_view command);

// Sets (or, with unset_environment, removes) a variable in this process's own
// environment - the seam for a test that has to steer code reading
// std::getenv(). POSIX spells these setenv()/unsetenv(); the Windows CRT has
// neither and uses _putenv_s(), where an empty value removes the variable.
void set_environment(std::string_view name, std::string_view value);
void unset_environment(std::string_view name);

}  // namespace ac3::test::platform
