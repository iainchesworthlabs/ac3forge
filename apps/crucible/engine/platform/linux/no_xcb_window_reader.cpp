#include <memory>
#include <optional>

#include "x11_foreground.hpp"

// The X11WindowReader for a build configured without libxcb
// (apps/crucible/CMakeLists.txt: AC3FORGE_CRUCIBLE_X11=OFF, or AUTO on a
// machine without the headers). Inert rather than absent, the same rule as
// tests/crucible/platform_services_stub.cpp: a platform that cannot do
// something says so through its own support(), so the X11 Foreground above
// this never tests for a null reader, and the UI prints what is missing.

namespace ac3::crucible {

namespace {

class NoXcbWindowReader final : public X11WindowReader {
public:
    const char* connect() override {
        return "this build has no X11 support: it was configured without libxcb (install "
               "libxcb1-dev and reconfigure); the full-screen rule is off";
    }
    std::optional<X11ActiveWindow> active_window() override { return std::nullopt; }
};

}  // namespace

std::unique_ptr<X11WindowReader> make_x11_window_reader() {
    return std::make_unique<NoXcbWindowReader>();
}

}  // namespace ac3::crucible
