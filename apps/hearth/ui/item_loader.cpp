#include "item_loader.hpp"

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>

namespace ac3::hearth::ui {

namespace {

[[nodiscard]] std::string lowercase_extension(const std::string& path) {
    std::string extension = std::filesystem::path(path).extension().string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension;
}

}  // namespace

ac3::hearth::ItemLoader make_file_item_loader() {
    return [](const std::string& path) -> std::expected<LoadedItem, std::string> {
        const std::string extension = lowercase_extension(path);
        // .ac4 reads its bytes but is never playable here (io::scan() only
        // knows AC-3 and E-AC-3, so Session::open() fails on them with its
        // own sentence) - accepted at this gate anyway so a MediaInspector
        // reading the same loader (hearth_controller.cpp) can still describe
        // an AC-4 item's table of contents for the Media page, which is a
        // question about the bytes rather than about playback.
        if (extension != ".ac3" && extension != ".ec3" && extension != ".ac4") {
            // apps/common/container_input.hpp's readers join this loader in a
            // later slice; until then a container is recognised but not
            // playable, which is what this sentence says.
            return std::unexpected("not yet playable: ac3hearth reads raw .ac3/.ec3/.ac4 only so far");
        }
        std::error_code sized;
        const std::uintmax_t size = std::filesystem::file_size(path, sized);
        if (sized) {
            return std::unexpected(sized.message());
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::unexpected("could not open the file");
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (!bytes.empty()) {
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file) {
                return std::unexpected("could not read the file");
            }
        }
        return LoadedItem{.bytes = std::move(bytes)};
    };
}

}  // namespace ac3::hearth::ui
