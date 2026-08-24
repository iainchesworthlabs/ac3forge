#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/syntax_trace.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/probe.hpp"

// ac3::io::probe (roadmap IO1) and the two additions it is built on:
// io::read_frame_header and DecoderConfig::skip_reconstruction.
//
// The strongest claim any of this makes is skip_reconstruction's: that a parse
// which skips the inverse transform reads the identical bits a full decode
// does, and therefore reports identical metadata. That is asserted directly
// below (same stream, both ways, field by field) rather than inferred from
// probe's own output agreeing with expectations - if the two ever diverge,
// every figure probe reports is suspect and this is the test that says so.

namespace {

// Real audio, not silence - per this project's own testing convention, a
// silent stream gives false passes a tone does not: exponent strategies
// collapse to reuse, coupling never engages, and half of what probe reports
// would read the same whether or not it was being computed. Three distinct
// tones so the channels are not correlated either.
std::vector<std::vector<float>> tone_channels(int channels, int frames) {
    const auto samples = static_cast<std::size_t>(frames) * ac3::kSamplesPerFrame;
    std::vector<std::vector<float>> out(static_cast<std::size_t>(channels),
                                        std::vector<float>(samples, 0.0f));
    for (int ch = 0; ch < channels; ++ch) {
        const double hz = 220.0 * (1.0 + 0.37 * ch);
        for (std::size_t n = 0; n < samples; ++n) {
            const double t = static_cast<double>(n) / 48000.0;
            out[static_cast<std::size_t>(ch)][n] =
                static_cast<float>(0.45 * std::sin(6.283185307179586 * hz * t) +
                                   0.08 * std::sin(6.283185307179586 * hz * 3.0 * t));
        }
    }
    return out;
}

std::vector<std::byte> encode_ac3(int frames, bool coupling) {
    // Built straight from EncoderConfig rather than through ac3::plan: the
    // subject here is what the BITSTREAM says, so the fewer layers between
    // the field being asserted and the field being set, the better.
    const ac3::EncoderConfig config{.bitrate_kbps = 448,
                                    .acmod = ac3::Acmod::k3_2,
                                    .lfe = true,
                                    .coupling = coupling};
    ac3::FrameEncoder encoder{config};
    const auto pcm = tone_channels(6, frames);
    std::vector<std::span<const float>> views;
    views.reserve(pcm.size());
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    std::vector<std::byte> stream;
    for (int frame = 0; frame < frames; ++frame) {
        std::vector<std::span<const float>> block;
        block.reserve(views.size());
        for (const auto& view : views) {
            block.emplace_back(view.subspan(
                static_cast<std::size_t>(frame) * ac3::kSamplesPerFrame, ac3::kSamplesPerFrame));
        }
        const auto encoded = encoder.encode_frame(block);
        REQUIRE(encoded.has_value());
        stream.insert(stream.end(), encoded->begin(), encoded->end());
    }
    return stream;
}

}  // namespace

TEST_CASE("read_frame_header reports what a syncframe declares", "[io][probe]") {
    const auto stream = encode_ac3(3, true);
    const auto header = ac3::io::read_frame_header(stream);
    REQUIRE(header.has_value());
    CHECK(header->kind == ac3::io::StreamKind::kAc3);
    CHECK(header->bsid == 8);
    CHECK(header->acmod == ac3::Acmod::k3_2);
    CHECK(header->lfe);
    CHECK(header->coded_channels() == 6);
    CHECK(header->sample_rate == ac3::SampleRate::k48000);
    CHECK(header->bitrate_kbps == 448);
    CHECK(header->bytes > 0);
    CHECK_FALSE(header->reduced_rate);
    // The same walk scan() makes, so the two must agree about the frame it
    // describes - this is the property that let scan() stop keeping its own
    // private copy of the parse.
    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    CHECK(scanned->bsid == header->bsid);
    CHECK(scanned->bsmod == header->bsmod);
    CHECK(scanned->acmod == header->acmod);
    CHECK(scanned->lfe == header->lfe);
    CHECK(scanned->access_units.front().size() == header->bytes);

    SECTION("a span with no sync word is refused rather than misread") {
        std::vector<std::byte> noise(64, std::byte{0x5A});
        CHECK_FALSE(ac3::io::read_frame_header(noise).has_value());
    }
    SECTION("a span too short to hold a header is truncated, not guessed at") {
        CHECK(ac3::io::read_frame_header(std::span{stream}.first(4)).error() ==
              ac3::io::ScanError::kTruncated);
    }
}

TEST_CASE("skip_reconstruction reads the identical bits a full decode does",
          "[decoder][probe]") {
    const auto stream = encode_ac3(4, true);
    const auto frames = ac3::split_frames(stream);
    REQUIRE(frames.has_value());

    ac3::FrameDecoder full;
    ac3::FrameSyntax syntax;
    ac3::DecoderConfig parse_only;
    parse_only.skip_reconstruction = true;
    parse_only.syntax = &syntax;
    ac3::FrameDecoder parse{parse_only};

    for (const auto& frame : *frames) {
        const auto whole = full.decode_frame(frame);
        const auto parsed = parse.decode_frame(frame);
        REQUIRE(whole.has_value());
        REQUIRE(parsed.has_value());
        // Every metadata field, not a sample of them: the claim is that the
        // two paths differ in nothing but the transform.
        CHECK(parsed->sample_rate == whole->sample_rate);
        CHECK(parsed->bitrate_kbps == whole->bitrate_kbps);
        CHECK(parsed->bsid == whole->bsid);
        CHECK(parsed->bsmod == whole->bsmod);
        CHECK(parsed->acmod == whole->acmod);
        CHECK(parsed->lfe == whole->lfe);
        CHECK(parsed->dialnorm == whole->dialnorm);
        CHECK(parsed->compr == whole->compr);
        CHECK(parsed->dynrng == whole->dynrng);
        CHECK(parsed->dialnorm2 == whole->dialnorm2);
        CHECK(parsed->compr2 == whole->compr2);
        CHECK(parsed->dynrng2 == whole->dynrng2);
        CHECK(parsed->blksw == whole->blksw);
        // ...and no audio, which is the whole point of asking for it.
        CHECK(parsed->channels.empty());
        CHECK_FALSE(whole->channels.empty());

        // The syntax trace saw the frame it was given.
        CHECK(syntax.valid);
        CHECK(syntax.fbw_channels == 5);
        CHECK(syntax.lfe);
        CHECK(syntax.block_count == ac3::kBlocksPerFrame);
        for (int blk = 0; blk < ac3::kBlocksPerFrame; ++blk) {
            CHECK(syntax.blocks[static_cast<std::size_t>(blk)].entered);
        }
    }
}

TEST_CASE("the syntax trace reports the tools the encoder was told to use",
          "[decoder][probe]") {
    // Coupling is the one AC-3 tool a caller can switch, so it is the one that
    // can be asserted in both directions - a trace that reported "coupling"
    // unconditionally would pass a one-sided check.
    for (const bool coupling : {false, true}) {
        const auto stream = encode_ac3(2, coupling);
        const auto report = ac3::io::probe(stream);
        REQUIRE(report.has_value());
        CHECK(report->tools.blocks == 2 * ac3::kBlocksPerFrame);
        if (coupling) {
            CHECK(report->tools.coupling > 0);
        } else {
            CHECK(report->tools.coupling == 0);
        }
        // An AC-3 stream has none of Annex E's own tools, whatever else it does.
        CHECK(report->tools.spectral_extension == 0);
        CHECK(report->tools.enhanced_coupling == 0);
        CHECK(report->tools.aht_frames == 0);
        // Every block coded six streams (five fbw + LFE), plus the coupling
        // channel where it was in use - and every one of them reported a
        // strategy, so the four counts have to add up to exactly that.
        const auto strategies = report->tools.exp_strategy;
        const std::uint64_t total =
            strategies[0] + strategies[1] + strategies[2] + strategies[3];
        CHECK(total == report->tools.blocks * 6 + report->tools.coupling);
        // Block 0 may not reuse (§5.4.3.6), so a real strategy was sent.
        CHECK(strategies[1] + strategies[2] + strategies[3] > 0);
    }
}

TEST_CASE("probe describes an AC-3 stream off the wire", "[io][probe]") {
    constexpr int kFrames = 6;
    const auto stream = encode_ac3(kFrames, true);
    const auto report = ac3::io::probe(stream);
    REQUIRE(report.has_value());

    CHECK(report->kind == ac3::io::StreamKind::kAc3);
    CHECK(report->bsid == 8);
    CHECK(report->sample_rate == ac3::SampleRate::k48000);
    CHECK(report->acmod == ac3::Acmod::k3_2);
    CHECK(report->lfe);
    CHECK(report->coded_channels == 6);
    CHECK(report->rendered_channels == 6);
    CHECK(report->layout.count == 6);
    CHECK(report->access_units == kFrames);
    CHECK(report->syncframes == kFrames);
    CHECK(report->bytes == stream.size());
    CHECK(report->substreams.size() == 1);
    CHECK(report->substreams.front().strmtyp == ac3::eac3::StreamType::kIndependent);
    CHECK(report->substreams.front().syncframes == kFrames);
    CHECK(report->substreams_per_unit == 1);

    // 448 kbit/s at 48 kHz is a whole number of bytes per frame, so the
    // measured rate is exact and the stream is genuinely constant.
    CHECK(report->nominal_bitrate_kbps == 448);
    CHECK(report->bitrate_kbps > 447.9);
    CHECK(report->bitrate_kbps < 448.1);
    CHECK_FALSE(report->variable_bitrate);
    CHECK(report->min_access_unit_bytes == report->max_access_unit_bytes);
    const double expected =
        static_cast<double>(kFrames) * ac3::kSamplesPerFrame / 48000.0;
    CHECK(report->duration_seconds > expected - 1e-9);
    CHECK(report->duration_seconds < expected + 1e-9);

    // Nothing was corrupted, so nothing may be reported as corrupt - the
    // failure counters are the command's own exit code, so a false positive
    // here would break a CI gate built on it.
    CHECK(report->crc_failures == 0);
    CHECK(report->parse_failures == 0);
    CHECK_FALSE(report->first_parse_error.has_value());

    // No object audio anywhere in a plain AC-3 stream.
    CHECK_FALSE(report->oamd);
    CHECK_FALSE(report->joc);
    CHECK(report->emdf_payload_ids.empty());
    CHECK_FALSE(report->program.has_value());
    CHECK_FALSE(report->oba_complexity_index.has_value());
    CHECK(report->authenticity_tagged_frames == 0);

    // dialnorm is mandatory; compr and dynrng are not, and this encode sent
    // neither - "absent" and "present but zero" have to stay distinguishable.
    CHECK(report->dialnorm.seen);
    CHECK(report->dialnorm.constant());
    CHECK_FALSE(report->compr.seen);
    CHECK_FALSE(report->dynrng.seen);
}

TEST_CASE("probe reports a bad CRC without refusing the stream", "[io][probe]") {
    auto stream = encode_ac3(4, true);
    const auto frames = ac3::split_frames(stream);
    REQUIRE(frames.has_value());
    REQUIRE(frames->size() == 4);
    // Corrupt one byte of the SECOND frame's payload, well past its header:
    // the point of the test is that the frame's declared shape still reports
    // correctly while its checksum does not.
    const auto offset = static_cast<std::size_t>(frames->at(1).data() - stream.data()) + 200;
    stream[offset] ^= std::byte{0xFF};

    const auto report = ac3::io::probe(stream);
    REQUIRE(report.has_value());
    CHECK(report->access_units == 4);
    CHECK(report->crc_failures == 1);
    // The other three still describe themselves, and the stream-level facts
    // are unchanged - which is the whole reason the CRC check sits beside the
    // header parse rather than gating it.
    CHECK(report->acmod == ac3::Acmod::k3_2);
    CHECK(report->syncframes == 4);
    CHECK(report->substreams.front().syncframes == 4);
}

TEST_CASE("probe walks an E-AC-3 stream's Annex E tools", "[io][probe][eac3]") {
    // Coupling and spectral extension are tested in SEPARATE encodes, not one
    // configured with both. Asking this encoder for `cpl+spx` produces spx and
    // no coupling at every rate checked (192 and 384 kbit/s at 5.1) - spectral
    // extension takes the high band coupling would have shared, leaving it no
    // sub-bands to work in. Which is exactly the kind of thing probe exists to
    // tell you, and exactly why a test that asserted "both tools are in use"
    // because both were requested would have been testing the request rather
    // than the stream.
    const auto encode = [](bool coupling, bool spx) {
        const ac3::eac3::FrameConfig config{.bitrate_kbps = 384,
                                            .acmod = ac3::Acmod::k3_2,
                                            .lfe = true,
                                            .coupling = coupling,
                                            .spx = spx};
        ac3::eac3::FrameEncoder encoder{config};
        const auto pcm = tone_channels(6, 4);
        std::vector<std::byte> stream;
        for (int frame = 0; frame < 4; ++frame) {
            std::vector<std::span<const float>> block;
            block.reserve(pcm.size());
            for (const auto& channel : pcm) {
                block.emplace_back(
                    std::span{channel}.subspan(static_cast<std::size_t>(frame) *
                                                   ac3::kSamplesPerFrame,
                                               ac3::kSamplesPerFrame));
            }
            const auto encoded = encoder.encode_frame(block);
            REQUIRE(encoded.has_value());
            stream.insert(stream.end(), encoded->begin(), encoded->end());
        }
        return stream;
    };

    SECTION("coupling") {
        const auto report = ac3::io::probe(encode(true, false));
        REQUIRE(report.has_value());
        CHECK(report->kind == ac3::io::StreamKind::kEac3);
        CHECK(report->bsid == ac3::eac3::kBsid);
        CHECK(report->numblkscod == 3);
        CHECK(report->access_units == 4);
        CHECK(report->crc_failures == 0);
        CHECK(report->parse_failures == 0);
        // E-AC-3 has no declared-rate field at all, unlike AC-3's frmsizecod.
        CHECK(report->nominal_bitrate_kbps == std::nullopt);
        CHECK(report->tools.coupling > 0);
        // Standard coupling, not the enhanced form - a trace that could not
        // tell them apart would pass an assertion on `coupling` alone.
        CHECK(report->tools.enhanced_coupling == 0);
        CHECK(report->tools.spectral_extension == 0);
    }

    SECTION("spectral extension") {
        const auto report = ac3::io::probe(encode(false, true));
        REQUIRE(report.has_value());
        CHECK(report->crc_failures == 0);
        CHECK(report->parse_failures == 0);
        CHECK(report->tools.spectral_extension > 0);
        CHECK(report->tools.coupling == 0);
        CHECK(report->tools.enhanced_coupling == 0);
    }
}

TEST_CASE("the detail callback sees every access unit exactly once", "[io][probe]") {
    constexpr int kFrames = 5;
    const auto stream = encode_ac3(kFrames, true);
    std::vector<std::uint64_t> indices;
    std::vector<std::uint64_t> offsets;
    ac3::io::ProbeOptions options;
    options.detail = true;
    options.on_access_unit = [&](const ac3::io::ProbeAccessUnit& unit) {
        indices.push_back(unit.index);
        offsets.push_back(unit.byte_offset);
        REQUIRE(unit.syncframes.size() == 1);
        CHECK(unit.syncframes.front().syntax.valid);
        CHECK(unit.syncframes.front().crc_valid);
        CHECK(unit.syncframes.front().byte_offset == unit.byte_offset);
    };
    const auto report = ac3::io::probe(stream, options);
    REQUIRE(report.has_value());
    REQUIRE(indices.size() == kFrames);
    for (int frame = 0; frame < kFrames; ++frame) {
        CHECK(indices[static_cast<std::size_t>(frame)] == static_cast<std::uint64_t>(frame));
    }
    // Offsets ascend and the last one plus its unit accounts for the file.
    CHECK(std::ranges::is_sorted(offsets));
    CHECK(offsets.front() == 0);
    CHECK(offsets.back() < stream.size());
}

namespace {

// Walks `stream` through an AccessUnitReader and checks every unit against
// `expected`, byte for byte and offset for offset. A stringstream stands in
// for the file: the reader only ever pulls forward, which is the property
// that lets it work on a pipe.
void check_reader_against(std::span<const std::byte> stream,
                          std::span<const std::span<const std::byte>> expected) {
    std::string raw(reinterpret_cast<const char*>(stream.data()), stream.size());  // NOLINT
    std::istringstream in{raw};
    ac3::io::AccessUnitReader reader{in};

    std::size_t seen = 0;
    std::uint64_t offset = 0;
    while (true) {
        const auto unit = reader.next();
        REQUIRE(unit.has_value());
        if (unit->empty()) {
            break;
        }
        REQUIRE(seen < expected.size());
        const auto& want = expected[seen];
        CHECK(unit->size() == want.size());
        CHECK(std::ranges::equal(*unit, want));
        CHECK(reader.byte_offset() == offset);
        offset += unit->size();
        ++seen;
    }
    CHECK(seen == expected.size());
    CHECK(offset == stream.size());
}

}  // namespace

TEST_CASE("AccessUnitReader delimits access units the way the format defines them",
          "[io][probe]") {
    SECTION("AC-3: one syncframe is one access unit") {
        // Checked against split_frames, NOT split_access_units. The latter
        // reads byte 2's top bits as strmtyp, which only means strmtyp in an
        // Annex E frame - in an AC-3 one those bits are part of crc1, so its
        // answer here would depend on a checksum. That is a known property
        // rather than a discovery (apps/android/.../file_replay.cpp documents
        // it against a real disc, and deliberately works around it locally
        // rather than changing the shared path), and every caller of it in
        // this repo already branches on bsid. AccessUnitReader is not affected
        // - it settles the generation through read_frame_header first - so
        // one syncframe per access unit is the right expectation here.
        const auto stream = encode_ac3(7, true);
        const auto expected = ac3::split_frames(stream);
        REQUIRE(expected.has_value());
        REQUIRE(expected->size() == 7);
        check_reader_against(stream, *expected);
    }

    SECTION("E-AC-3: an independent substream plus the dependents that follow it") {
        // The case the boundary rule actually exists for. A 7.1 access unit is
        // an independent 5.1 bed plus one dependent carrying the rear pair, so
        // a reader that treated every syncframe as its own unit - or that
        // failed to close a unit at the next independent - would disagree here
        // and nowhere else.
        namespace cm = ac3::eac3::chanmap;
        const ac3::eac3::AccessUnitConfig config{
            .independent = {.bitrate_kbps = 640, .acmod = ac3::Acmod::k3_2, .lfe = true},
            .dependents = {{.bitrate_kbps = 320,
                            .acmod = ac3::Acmod::k2_2,
                            .chanmap = cm::k71Rear}}};
        ac3::eac3::AccessUnitEncoder encoder{config};
        const auto pcm = tone_channels(10, 4);
        std::vector<std::byte> stream;
        for (int frame = 0; frame < 4; ++frame) {
            std::vector<std::span<const float>> views;
            views.reserve(pcm.size());
            for (const auto& channel : pcm) {
                views.emplace_back(std::span{channel}.subspan(
                    static_cast<std::size_t>(frame) * ac3::kSamplesPerFrame,
                    ac3::kSamplesPerFrame));
            }
            const auto unit = encoder.encode_access_unit(views);
            REQUIRE(unit.has_value());
            stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
        }

        const auto expected = ac3::split_access_units(stream);
        REQUIRE(expected.has_value());
        REQUIRE(expected->size() == 4);
        check_reader_against(stream, *expected);

        // ...and probe reports the shape those units have: two substreams per
        // unit, 8 syncframes over 4 access units, and a 7.1 render from a 5.1
        // bed - the §E3.8.2 union, not the bed's own channel count.
        const auto report = ac3::io::probe(stream);
        REQUIRE(report.has_value());
        CHECK(report->access_units == 4);
        CHECK(report->syncframes == 8);
        CHECK(report->substreams_per_unit == 2);
        REQUIRE(report->substreams.size() == 2);
        CHECK(report->substreams[0].strmtyp == ac3::eac3::StreamType::kIndependent);
        CHECK(report->substreams[1].strmtyp == ac3::eac3::StreamType::kDependent);
        CHECK(report->substreams[1].chanmap == cm::k71Rear);
        CHECK(report->coded_channels == 6);
        CHECK(report->rendered_channels == 8);
        CHECK(report->layout.count == 8);
    }
}

TEST_CASE("MinMax keeps absent and zero apart", "[io][probe]") {
    ac3::io::MinMax range;
    CHECK_FALSE(range.seen);
    CHECK_FALSE(range.constant());
    range.add(0);
    CHECK(range.seen);
    CHECK(range.constant());
    CHECK(range.min == 0);
    CHECK(range.max == 0);
    range.add(-4);
    range.add(9);
    CHECK_FALSE(range.constant());
    CHECK(range.min == -4);
    CHECK(range.max == 9);
}
