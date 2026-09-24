// The POSIX half of the test suite's platform seam (tests/platform/process.hpp).
//
// Every platform ships the same filename under tests/platform/<os>/; CMake
// compiles the directory that matches the target, so there is no #ifdef here -
// the file's path is what says "POSIX".

#include "platform/process.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <string_view>

namespace ac3::test::platform {

std::string process_id() { return std::to_string(::getpid()); }

int run_shell(std::string_view command) {
    // std::system() takes a C string and string_view carries no guarantee of
    // one, so the copy is the interface, not an oversight.
    const std::string terminated{command};
    const int status = std::system(terminated.c_str());

    // No command interpreter available, or it could not be started. Passed
    // through unchanged: it is not an exit code and must not be mistaken for
    // one.
    if (status == -1) {
        return status;
    }

    // The POSIX half of the seam's whole point - see the header. A
    // signal-terminated child (a crash, an abort()) has no exit code of its
    // own to report; 128 + signal is the shell's convention for that, and
    // sits clear of every real ac3cli exit code (0..7 -
    // apps/cli/exit_codes.hpp).
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void set_environment(std::string_view name, std::string_view value) {
    const std::string n{name};
    const std::string v{value};
    ::setenv(n.c_str(), v.c_str(), 1);
}

void unset_environment(std::string_view name) {
    const std::string n{name};
    ::unsetenv(n.c_str());
}

}  // namespace ac3::test::platform
