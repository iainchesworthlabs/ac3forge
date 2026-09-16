// A flash partition. The default source, and the first one CI runs.
//
// It needs no hardware beyond the flash the application is already sitting in,
// so it works under QEMU - which is what lets the streaming path be exercised
// on target without a board. `idf.py flash` writes stream/sample.ac3 into the
// `audio` partition alongside the application; see the project CMakeLists.

#include "byte_source.hpp"

#include <algorithm>
#include <cstdio>

#include "esp_partition.h"

namespace player {
namespace {

const esp_partition_t* g_audio = nullptr;
std::size_t g_offset = 0;

// How many bytes of the partition are audio, supplied by the build from the
// file's own size (main/CMakeLists.txt).
//
// A partition is the one source with no length of its own. Every other kind
// has one - a file size, a Content-Length - and without it the player reads
// the whole 256 KB partition while the framer skips a quarter of a megabyte of
// erased flash looking for a sync word, on every lap. It "works", and reports
// 251,392 bytes of resynchronisation to say how well.
constexpr std::size_t kStreamBytes = AC3FORGE_STREAM_BYTES;

std::size_t limit() {
    if (g_audio == nullptr) {
        return 0;
    }
    return std::min(kStreamBytes, static_cast<std::size_t>(g_audio->size));
}

}  // namespace

bool source_open() {
    g_audio = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       static_cast<esp_partition_subtype_t>(0x40), "audio");
    if (g_audio == nullptr) {
        std::printf("error: no 'audio' partition - check partitions.csv is the table in use\n");
        return false;
    }
    g_offset = 0;
    std::printf("source: partition '%s' at 0x%lx, %lu bytes of audio in %lu\n", g_audio->label,
                static_cast<unsigned long>(g_audio->address),
                static_cast<unsigned long>(limit()),
                static_cast<unsigned long>(g_audio->size));
    return true;
}

// A partition has one thing in it, flashed with the application. There is
// nowhere else to point it.
bool source_set_location(const char*) { return false; }

const char* source_location() { return "audio"; }

std::size_t source_read(std::span<std::byte> dst) {
    const std::size_t end = limit();
    if (g_offset >= end) {
        return 0;
    }
    // Capped well below the framing buffer on purpose, so the accumulator's
    // "need more input" path runs on a real device and not only in its unit
    // tests. A read that always happened to contain a whole frame would hide
    // every framing bug there is.
    constexpr std::size_t kReadBlock = 2048;
    const std::size_t want = std::min({dst.size(), kReadBlock, end - g_offset});
    if (esp_partition_read(g_audio, g_offset, dst.data(), want) != ESP_OK) {
        std::printf("error: partition read failed at %lu\n", static_cast<unsigned long>(g_offset));
        return 0;
    }
    g_offset += want;
    return want;
}

bool source_rewind() {
    g_offset = 0;
    return true;
}

const char* source_name() { return "partition"; }

std::size_t source_length() { return limit(); }

}  // namespace player
