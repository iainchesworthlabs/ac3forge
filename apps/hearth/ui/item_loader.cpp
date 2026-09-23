#include "item_loader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace ac3::hearth::ui {

namespace {

[[nodiscard]] std::string lowercase_extension(const std::string& path) {
    std::string extension = std::filesystem::path(path).extension().string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension;
}

// list_folder_items()'s recognised set: the two make_file_item_loader() can
// actually decode, plus the container formats the design's own drop-zone
// wording names (docs/hearth/design/screenshots/first-run.png) - not read
// yet, but real enough as files that a folder scan leaving them out would
// look like it had missed something.
constexpr std::array<std::string_view, 5> kFolderMediaExtensions{
    ".ac3", ".ec3", ".mp4", ".mkv", ".ts"};

[[nodiscard]] bool is_folder_media_extension(const std::string& extension) {
    return std::find(kFolderMediaExtensions.begin(), kFolderMediaExtensions.end(), extension) !=
           kFolderMediaExtensions.end();
}

}  // namespace

ac3::hearth::ItemLoader make_file_item_loader() {
    return [](const std::string& path) -> std::expected<LoadedItem, std::string> {
        const std::string extension = lowercase_extension(path);
        if (extension != ".ac3" && extension != ".ec3") {
            // apps/common/container_input.hpp's readers join this loader in a
            // later slice; until then a container or an AC-4 stream is
            // recognised but not playable, which is what this sentence says.
            return std::unexpected("not yet playable: ac3hearth reads raw .ac3/.ec3 only so far");
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

std::vector<std::string> list_folder_items(const std::string& folder) {
    std::vector<std::string> paths;
    std::error_code walk_error;
    std::filesystem::recursive_directory_iterator it(
        folder, std::filesystem::directory_options::skip_permission_denied, walk_error);
    const std::filesystem::recursive_directory_iterator end;
    while (!walk_error && it != end) {
        std::error_code type_error;
        if (it->is_regular_file(type_error) && !type_error &&
            is_folder_media_extension(lowercase_extension(it->path().string()))) {
            paths.push_back(it->path().string());
        }
        it.increment(walk_error);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

}  // namespace ac3::hearth::ui
