// The Windows half of the test suite's platform seam (tests/platform/process.hpp).
//
// Every platform ships the same filename under tests/platform/<os>/; CMake
// compiles the directory that matches the target, so there is no #ifdef here -
// the file's path is what says "Windows".

#include "platform/process.hpp"

#include <process.h>

#include <cstdlib>
#include <string>
#include <string_view>

namespace ac3::test::platform {

std::string process_id() { return std::to_string(::_getpid()); }

int run_shell(std::string_view command) {
    // The extra outer quote pair, and why cmd.exe needs one where sh must not
    // have it: see the header's run_shell comment. Building it here rather
    // than at each call site is the reason this seam exists - the eight test
    // files that used to do it inline each carried their own copy of that
    // reasoning, and a ninth would have had to know to.
    const std::string wrapped = "\"" + std::string{command} + "\"";

    // std::system()'s return value on Windows already IS the child's own exit
    // code for a plain (non-shell-builtin) invocation through cmd.exe /c, so
    // there is nothing to unpack on this side - which is exactly the asymmetry
    // the POSIX file's WIFEXITED/WEXITSTATUS pass exists to cancel out. -1
    // still means no interpreter and passes through as itself.
    return std::system(wrapped.c_str());
}

void set_environment(std::string_view name, std::string_view value) {
    const std::string n{name};
    const std::string v{value};
    ::_putenv_s(n.c_str(), v.c_str());
}

void unset_environment(std::string_view name) {
    // _putenv_s with an empty value is how the CRT removes a variable.
    const std::string n{name};
    ::_putenv_s(n.c_str(), "");
}

}  // namespace ac3::test::platform
