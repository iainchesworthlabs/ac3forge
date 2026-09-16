#include "ac3/sendspin/discovery.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace ac3::sendspin::discovery {

std::optional<std::string> Service::txt_value(std::string_view key) const {
    const auto found = std::find_if(txt.begin(), txt.end(), [key](const TxtEntry& entry) { return entry.key == key; });
    if (found == txt.end()) {
        return std::nullopt;
    }
    return found->value;
}

std::optional<std::string> Service::url() const {
    if (addresses.empty() || port == 0) {
        return std::nullopt;
    }
    std::string path = txt_value("path").value_or("/sendspin");
    if (path.empty() || path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    return "ws://" + addresses.front() + ":" + std::to_string(port) + path;
}

}  // namespace ac3::sendspin::discovery
