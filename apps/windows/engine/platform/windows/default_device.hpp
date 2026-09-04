#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

// The system default render endpoint: reading it, moving it to the null
// sink, and putting it back on exit (docs/platforms/windows-demo.md,
// "Rerouting: explicit, like FxSound").
//
// Windows documents no API for setting the default. What every
// default-switcher uses is IPolicyConfig::SetDefaultEndpoint, an interface
// on the PolicyConfigClient class that has been stable since Vista and is
// declared here by hand (the SDK has no header for it). It is still
// unsupported, so a refusal is not an error: open_sound_settings() is the
// fallback, and the DeviceWatcher tells the engine when the user has made
// the change by hand. Windows-only, this directory only.

namespace ac3::windemo {

struct RenderEndpoint {
    std::string id;
    std::string name;
    bool is_default = false;
};

// Every active render endpoint, default first.
[[nodiscard]] std::vector<RenderEndpoint> render_endpoints();

// The console default's endpoint id, or empty when there is none.
[[nodiscard]] std::string default_render_id();

// Makes `endpoint_id` the default for every role. Returns a one-line reason
// on refusal.
[[nodiscard]] std::expected<void, std::string> set_default_render(std::string_view endpoint_id);

// The endpoint whose friendly name contains `name_substring`
// (case-insensitive), or empty.
[[nodiscard]] std::string find_render_endpoint(std::string_view name_substring);

// The fallback: the Settings app's Sound page.
void open_sound_settings();

}  // namespace ac3::windemo
