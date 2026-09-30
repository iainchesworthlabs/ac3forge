#pragma once

// Which committed AC-4 streams a heavy test plays under the sanitizers
// (tests/sanitized.hpp), and how much of each. A normal build plays every
// stream to its end. Under the sanitizers a test plays one stream of each kind,
// and of a long stream its first kSanitizedFrames frames. A kind is a frame
// rate, the channels and substream roles of the presentation decode() plays,
// how the stream codes its objects, and the codec modes of its first frame:
// every layout, codec mode, object coding and frame rate the streams show is
// still played.
//
// The tests that play the committed streams share it:
// tests/ac4dec/test_ac4dec_api.cpp and tests/hearth/test_ac4_engine.cpp.

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4/syntax.hpp"
#include "ac4dec/decoder.hpp"
#include "sanitized.hpp"

namespace ac3::test {

// The frames of each stream the sanitizers play. The constructed streams have 4,
// and the hand-built object streams send their second I-frame as their fifth.
inline constexpr std::size_t kSanitizedFrames = 5;

// The first `frames` sync frames of `stream`, as the bytes of a stream of their
// own; all of it where it has no more.
inline std::span<const std::byte> first_frames(std::span<const std::byte> stream,
                                               std::size_t frames) {
    const ac4::ScanResult scanned = ac4::scan(stream);
    return frames < scanned.frames.size() ? stream.first(scanned.frames[frames].offset) : stream;
}

// What a stream shows an engine that another of its kind does not: the frame
// rate, the channels and substream roles of the presentation decode() plays, how
// the objects of its groups are coded, and the codec modes its first frame
// codes. Only the traits matter, as a key.
inline std::set<std::string> kind_of(std::span<const std::byte> first_frame) {
    std::set<std::string> kind;
    ac4::DecoderConfig config;
    config.syntax = [&kind](const ac4::SyntaxRecord& record) {
        if (record.name.find("codec_mode") != std::string_view::npos) {
            kind.insert(std::string{record.name} + "=" + std::to_string(record.value));
        }
    };
    ac4::Decoder decoder(config);
    REQUIRE(decoder.parse(first_frame).has_value());
    const auto raw = ac4::parse_raw_frame(first_frame);
    REQUIRE(raw.has_value());
    kind.insert("bitstream_version " + std::to_string(raw->toc.bitstream_version));
    kind.insert("frame_rate_index " + std::to_string(raw->toc.frame_rate_index));

    const std::span<const ac4::PresentationInfo> presentations = decoder.presentations();
    const std::optional<std::size_t> selected = decoder.metadata().presentation;
    if (selected && *selected < presentations.size()) {
        for (const ac4::Speaker speaker : presentations[*selected].speakers) {
            kind.insert("speaker " + std::string{ac4::describe(speaker)});
        }
        for (const ac4::PresentationMember& member : presentations[*selected].members) {
            kind.insert("role " + std::string{ac4::describe(member.role)});
        }
    }
    for (const ac4::SubstreamGroupInfo& group : raw->toc.substream_groups) {
        for (const ac4::GroupSubstream& substream : group.substreams) {
            if (substream.ajoc) {
                kind.insert("A-JOC objects");
            } else if (substream.obj) {
                kind.insert(substream.obj->b_dynamic_objects ? "direct dynamic objects"
                                                             : "direct static objects");
            }
        }
    }
    return kind;
}

// The streams a test plays: all of them, or under the sanitizers one of each
// kind, the one with the fewest frames to play and, among equals, the first in
// path order.
inline std::vector<std::filesystem::path> streams_to_play(
    const std::vector<std::filesystem::path>& streams) {
    if (!kSanitized) {
        return streams;
    }
    struct Choice {
        std::filesystem::path path;
        std::size_t frames = 0;
    };
    std::map<std::set<std::string>, Choice> kinds;
    for (const std::filesystem::path& path : streams) {
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.good());
        const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(chars.size());
        std::ranges::transform(chars, bytes.begin(),
                               [](char c) { return static_cast<std::byte>(c); });
        const ac4::ScanResult scanned = ac4::scan(bytes);
        REQUIRE_FALSE(scanned.frames.empty());
        const std::size_t cost = std::min(scanned.frames.size(), kSanitizedFrames);
        const auto [it, added] =
            kinds.try_emplace(kind_of(scanned.frames.front().raw_ac4_frame), Choice{path, cost});
        if (!added && cost < it->second.frames) {
            it->second = Choice{path, cost};
        }
    }
    // The streams show 45 kinds. A key that stopped telling them apart would
    // leave a few streams to play and no failure to say so.
    REQUIRE(kinds.size() >= 40);
    std::vector<std::filesystem::path> chosen;
    for (const auto& kind : kinds) {
        chosen.push_back(kind.second.path);
    }
    std::ranges::sort(chosen);
    return chosen;
}

}  // namespace ac3::test
