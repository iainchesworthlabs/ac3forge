#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>

// The part of a file-backed source that is not about the block device.
//
// source/sd/ and source/fatfs/ differ in exactly one thing: where the FAT
// volume lives - an SD card over SDMMC, or a partition in the flash the
// application is already running from. Everything after the mount is fopen,
// fread, ftell and rewind, and that is what this header holds.
//
// WHY THAT SPLIT IS THE POINT. QEMU has no SD host, so source/sd/ can only ever
// be compiled by CI. It CAN mount FAT from flash, so source/fatfs/ runs - and
// because both go through the code below, running one exercises the file layer
// of the other. What stays unverified is the SDMMC host itself, which is
// Espressif's driver rather than ours.
//
// That is a mock of the boundary rather than a replacement for the thing behind
// it: the same VFS, the same FATFS, the same stdio, a different block device.

namespace player {
namespace file_source {

inline std::FILE* g_file = nullptr;
inline std::size_t g_length = 0;
// The path the next open() uses: the configured default until a control
// surface points the source at another file.
inline char g_path[256] = {};

inline void set_default_path(const char* path) {
    if (g_path[0] == '\0') {
        std::strncpy(g_path, path, sizeof(g_path) - 1);
    }
}

// False when the location does not fit; the caller reports it.
[[nodiscard]] inline bool set_path(const char* path) {
    if (path == nullptr || path[0] == '\0' || std::strlen(path) >= sizeof(g_path)) {
        return false;
    }
    std::strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';
    return true;
}

// Opens the current path and learns its length. A file has one, unlike a
// partition - which is what stops the framer reading past the audio into
// whatever follows. A file already open from a previous run is closed first,
// so a source can be opened again for each thing it plays.
[[nodiscard]] inline bool open() {
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_length = 0;
    g_file = std::fopen(g_path, "rb");
    if (g_file == nullptr) {
        std::printf("error: %s could not be opened\n", g_path);
        return false;
    }
    if (std::fseek(g_file, 0, SEEK_END) == 0) {
        const auto end = std::ftell(g_file);
        g_length = end > 0 ? static_cast<std::size_t>(end) : 0;
        std::rewind(g_file);
    }
    return true;
}

[[nodiscard]] inline std::size_t read(std::span<std::byte> dst) {
    if (g_file == nullptr) {
        return 0;
    }
    // Capped well below the framing buffer on purpose, so the accumulator's
    // "need more input" path runs rather than every read happening to contain a
    // whole frame - which would hide every framing bug there is.
    constexpr std::size_t kReadBlock = 2048;
    const std::size_t want = dst.size() < kReadBlock ? dst.size() : kReadBlock;
    return std::fread(dst.data(), 1, want, g_file);
}

[[nodiscard]] inline bool rewind() {
    if (g_file == nullptr) {
        return false;
    }
    std::rewind(g_file);
    return true;
}

[[nodiscard]] inline std::size_t length() { return g_length; }

[[nodiscard]] inline const char* path() { return g_path; }

}  // namespace file_source
}  // namespace player
