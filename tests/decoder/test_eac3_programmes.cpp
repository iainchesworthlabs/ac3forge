#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"

// Multiple independent substreams (§E2.3.1.2's I0-I7): the multi-language and
// associated-service shape of broadcast DD+, where one elementary stream
// carries a main programme and the alternatives a receiver picks between.
//
// The thing under test is that they stay APART. Two programmes are not two
// layers of a soundfield the way an independent substream and its dependents
// are - they are separate pieces of audio that happen to share a frame period
// - so every stage has to keep them separate: the framing must not splice
// their access units into one timeline, the decoder must render one without
// the other's frames touching its state, and the scanner must describe each
// on its own terms rather than unioning them into a programme that exists
// nowhere.
//
// No external oracle reaches here. FFmpeg's ff_ac3_parse_header rejects any
// frame with substreamid != 0, so it cannot read a second independent
// substream any more than it can read a second dependent one (see
// docs/verification.md's oracle table); the in-repo decoder and
// tools/references/eac3_parse.py are what check this.

namespace {

constexpr double kAmplitude = 0.4;
// Deliberately far apart, and both well clear of the other's neighbourhood:
// the whole point is to tell one programme's audio from the other's, so a
// decode that quietly rendered the wrong one has to be visible as a tone, not
// as a level.
constexpr double kMainTone = 1000.0;
constexpr double kCommentaryTone = 300.0;
// The LFE is band-limited to 120 Hz (§5.4.3.4), so it cannot carry either
// programme's identifying tone and is checked for level rather than pitch.
constexpr double kLfeTone = 60.0;

ac3::eac3::FrameConfig bed(std::uint32_t kbps, int dialnorm) {
    return {.bitrate_kbps = kbps, .acmod = ac3::Acmod::k3_2, .lfe = true, .dialnorm = dialnorm};
}

// I0 5.1 at dialnorm 27, I1 mono at dialnorm 20 - two different levels,
// because a commentary or description track is levelled independently of the
// mix it plays against and nothing here may quietly share one measurement.
ac3::eac3::AccessUnitConfig two_programme_config() {
    ac3::eac3::AccessUnitConfig config;
    config.independent = bed(448, 27);
    config.additional.push_back(
        {.independent = {.bitrate_kbps = 96, .acmod = ac3::Acmod::k1_0, .dialnorm = 20}});
    return config;
}

// A sine at `hz`, continuing from sample `n0` so successive frames join
// without a discontinuity the encoder would spend a block switch on.
void fill_tone(std::vector<float>& dest, double hz, std::uint64_t n0) {
    for (std::size_t i = 0; i < dest.size(); ++i) {
        dest[i] = static_cast<float>(
            kAmplitude * std::sin(2.0 * std::numbers::pi * hz *
                                  static_cast<double>(n0 + i) / 48000.0));
    }
}

// One elementary stream carrying `frames` access units of every programme in
// `config`. Each programme's channels all carry that programme's own tone, so
// the two are distinguishable channel by channel after decoding.
std::vector<std::byte> encode(const ac3::eac3::AccessUnitConfig& config, int frames,
                              std::span<const double> tone_per_programme) {
    ac3::eac3::AccessUnitEncoder encoder{config};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    REQUIRE(nchans > 0);

    // Which programme each coded channel belongs to, so the right tone goes
    // into it - the same first-programme-then-the-rest order
    // encode_access_unit takes its spans in.
    std::vector<double> tone;
    tone.reserve(nchans);
    const auto append = [&](const ac3::eac3::ProgrammeConfig& programme, double hz) {
        const auto take = [&](const ac3::eac3::FrameConfig& sub) {
            tone.insert(tone.end(),
                        static_cast<std::size_t>(ac3::fullbw_channel_count(sub.acmod)), hz);
            if (sub.lfe) {
                tone.push_back(kLfeTone);
            }
        };
        take(programme.independent);
        for (const auto& dep : programme.dependents) {
            take(dep);
        }
    };
    append({config.independent, config.dependents}, tone_per_programme[0]);
    for (std::size_t i = 0; i < config.additional.size(); ++i) {
        append(config.additional[i], tone_per_programme[i + 1]);
    }
    REQUIRE(tone.size() == nchans);

    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::byte> stream;
    std::uint64_t n0 = 0;
    for (int f = 0; f < frames; ++f) {
        for (std::size_t ch = 0; ch < nchans; ++ch) {
            fill_tone(block[ch], tone[ch], n0);
            views[ch] = block[ch];
        }
        n0 += ac3::kSamplesPerFrame;
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    return stream;
}

// Coarse spectral peak over the steady-state middle, the same measurement the
// layout tests use - a decode that rendered the wrong programme's audio shows
// up here as the wrong frequency, not as a small error.
double dominant_freq_hz(const std::vector<float>& x) {
    double best_f = 0.0;
    double best_m = -1.0;
    const std::size_t n0 = 2048;
    REQUIRE(x.size() > n0);
    const std::size_t len = std::min<std::size_t>(8192, x.size() - n0);
    for (double f = 40.0; f <= 2000.0; f += 5.0) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t i = 0; i < len; ++i) {
            const double phase = 2.0 * std::numbers::pi * f * static_cast<double>(i) / 48000.0;
            re += static_cast<double>(x[n0 + i]) * std::cos(phase);
            im += static_cast<double>(x[n0 + i]) * std::sin(phase);
        }
        const double mag = re * re + im * im;
        if (mag > best_m) {
            best_m = mag;
            best_f = f;
        }
    }
    return best_f;
}

struct Decoded {
    std::vector<std::vector<float>> channels;
    ac3::eac3::chanmap::Layout layout;
    int dialnorm = 0;
    int programme = -1;
    int units = 0;
};

// Decodes one programme. `select` is what goes into
// DecoderConfig::programme; `units` is the span list to feed, which is where
// the two halves of the fix meet - a caller can either pre-filter the framing
// or let the decoder skip, and both must give the same audio.
Decoded decode_programme(std::span<const std::span<const std::byte>> units,
                         std::optional<int> select) {
    ac3::Eac3Decoder decoder{{.programme = select}};
    Decoded out;
    for (const auto& unit : units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            continue;
        }
        const auto& got = **decoded;
        if (out.channels.empty()) {
            out.channels.resize(got.channels.size());
            out.layout = got.layout;
            out.dialnorm = got.dialnorm;
            out.programme = got.programme;
        }
        REQUIRE(got.channels.size() == out.channels.size());
        REQUIRE(got.programme == out.programme);
        ++out.units;
        for (std::size_t ch = 0; ch < got.channels.size(); ++ch) {
            out.channels[ch].insert(out.channels[ch].end(), got.channels[ch].begin(),
                                    got.channels[ch].end());
        }
    }
    return out;
}

}  // namespace

TEST_CASE("a second independent substream is a second programme, not more frames",
          "[eac3][programmes]") {
    constexpr int kFrames = 5;
    const std::array<double, 2> tones{kMainTone, kCommentaryTone};
    const auto stream = encode(two_programme_config(), kFrames, tones);

    SECTION("the framing enumerates both programmes and keeps their units apart") {
        const auto ids = ac3::programme_ids(stream);
        REQUIRE(ids.has_value());
        REQUIRE(*ids == std::vector<int>{0, 1});

        // Unfiltered: every unit of both programmes, interleaved one frame
        // period at a time. This is the count that used to be mistaken for
        // one programme running at twice the frame rate.
        const auto all = ac3::split_access_units(stream);
        REQUIRE(all.has_value());
        CHECK(all->size() == static_cast<std::size_t>(kFrames) * 2);

        for (const int id : *ids) {
            CAPTURE(id);
            const auto own = ac3::split_access_units(stream, id);
            REQUIRE(own.has_value());
            CHECK(own->size() == static_cast<std::size_t>(kFrames));
        }
        // Asking for a programme the stream does not carry is an empty
        // answer, not an error - that is how a caller finds out.
        const auto missing = ac3::split_access_units(stream, 5);
        REQUIRE(missing.has_value());
        CHECK(missing->empty());
    }

    SECTION("each programme decodes to its own audio, at its own dialnorm") {
        const auto all = ac3::split_access_units(stream);
        REQUIRE(all.has_value());

        const auto main = decode_programme(*all, 0);
        const auto commentary = decode_programme(*all, 1);

        // Both programmes released every access unit of their own: neither
        // was starved by the other's frames arriving in between.
        CHECK(main.units == kFrames);
        CHECK(commentary.units == kFrames);
        CHECK(main.programme == 0);
        CHECK(commentary.programme == 1);

        // 5.1 against mono - the second programme is a different SHAPE, so a
        // decode that had spliced the two would not even agree on width.
        CHECK(main.channels.size() == 6);
        CHECK(commentary.channels.size() == 1);

        // Levelled independently (§5.4.2.8), which is the whole reason a
        // receiver treats them as separate programmes.
        CHECK(main.dialnorm == 27);
        CHECK(commentary.dialnorm == 20);

        // Every full-bandwidth channel of the main programme carries the
        // main programme's tone - so a decode that had let the commentary's
        // frames through would show 300 Hz somewhere here.
        for (int ch = 0; ch < main.layout.count; ++ch) {
            CAPTURE(ch, ac3::eac3::chanmap::name(main.layout[ch]));
            const auto& channel = main.channels[static_cast<std::size_t>(ch)];
            const double want =
                main.layout[ch] == ac3::eac3::chanmap::Location::kLfe ? kLfeTone : kMainTone;
            CHECK(std::abs(dominant_freq_hz(channel) - want) < 10.0);
        }
        CHECK(std::abs(dominant_freq_hz(commentary.channels[0]) - kCommentaryTone) < 10.0);
    }

    SECTION("pre-filtered framing and decoder-side selection agree") {
        for (const int id : {0, 1}) {
            CAPTURE(id);
            const auto all = ac3::split_access_units(stream);
            const auto own = ac3::split_access_units(stream, id);
            REQUIRE(all.has_value());
            REQUIRE(own.has_value());
            // Same audio whether the caller filtered the units itself or
            // handed the decoder everything and let it skip.
            const auto by_decoder = decode_programme(*all, id);
            const auto by_framing = decode_programme(*own, std::nullopt);
            REQUIRE(by_decoder.channels.size() == by_framing.channels.size());
            CHECK(by_framing.programme == id);
            for (std::size_t ch = 0; ch < by_decoder.channels.size(); ++ch) {
                CHECK(by_decoder.channels[ch] == by_framing.channels[ch]);
            }
        }
    }

    SECTION("scan describes each programme on its own terms") {
        const auto scanned = ac3::io::scan(stream);
        REQUIRE(scanned.has_value());
        REQUIRE(scanned->programmes.size() == 2);

        CHECK(scanned->programmes[0].substreamid == 0);
        CHECK(scanned->programmes[0].acmod == ac3::Acmod::k3_2);
        CHECK(scanned->programmes[0].lfe);
        CHECK(scanned->programmes[0].channels == 6);
        CHECK(scanned->programmes[0].access_units.size() ==
              static_cast<std::size_t>(kFrames));

        CHECK(scanned->programmes[1].substreamid == 1);
        CHECK(scanned->programmes[1].acmod == ac3::Acmod::k1_0);
        CHECK_FALSE(scanned->programmes[1].lfe);
        CHECK(scanned->programmes[1].channels == 1);
        CHECK(scanned->programmes[1].access_units.size() ==
              static_cast<std::size_t>(kFrames));

        // The scalar summary is the FIRST programme's, and access_units is
        // its units alone - never both programmes' spliced together, which is
        // what a muxer would otherwise write into one track.
        CHECK(scanned->acmod == ac3::Acmod::k3_2);
        CHECK(scanned->channels == 6);
        CHECK(scanned->access_units.size() == static_cast<std::size_t>(kFrames));
        CHECK(scanned->access_units.front().data() ==
              scanned->programmes[0].access_units.front().data());
    }

    SECTION("the dec3 box declares both independent substreams") {
        const auto scanned = ac3::io::scan(stream);
        REQUIRE(scanned.has_value());
        const auto box = ac3::io::build_codec_config_box(*scanned);
        REQUIRE(box.size() >= 2);
        // §F.6: data_rate(13) then num_ind_sub(3), which counts one less than
        // the substreams - so two programmes read back as 1.
        const auto low = std::to_integer<std::uint32_t>(box[1]);
        CHECK((low & 0x07) == 1);
        // Each programme contributes its own per-substream block, so the box
        // grew by one of them (3 bytes for a dependent-free programme) over
        // the single-programme shape.
        const auto single = ac3::io::scan(encode(
            {.independent = bed(448, 27)}, 2, std::array<double, 1>{kMainTone}));
        REQUIRE(single.has_value());
        const auto single_box = ac3::io::build_codec_config_box(*single);
        CHECK(box.size() == single_box.size() + 3);
    }
}

TEST_CASE("a single-programme stream is unchanged by the programme layer",
          "[eac3][programmes]") {
    constexpr int kFrames = 4;
    const auto stream =
        encode({.independent = bed(448, 31)}, kFrames, std::array<double, 1>{kMainTone});

    const auto ids = ac3::programme_ids(stream);
    REQUIRE(ids.has_value());
    CHECK(*ids == std::vector<int>{0});

    const auto all = ac3::split_access_units(stream);
    const auto own = ac3::split_access_units(stream, 0);
    REQUIRE(all.has_value());
    REQUIRE(own.has_value());
    CHECK(all->size() == static_cast<std::size_t>(kFrames));
    // Selecting the only programme there is changes nothing at all.
    REQUIRE(own->size() == all->size());
    for (std::size_t i = 0; i < own->size(); ++i) {
        CHECK((*own)[i].data() == (*all)[i].data());
        CHECK((*own)[i].size() == (*all)[i].size());
    }

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->programmes.size() == 1);
    CHECK(scanned->programmes[0].substreamid == 0);
    CHECK(scanned->access_units.size() == static_cast<std::size_t>(kFrames));

    // And an unset DecoderConfig::programme still renders whatever arrives,
    // reporting programme 0 - the behaviour every existing caller has.
    const auto decoded = decode_programme(*all, std::nullopt);
    CHECK(decoded.units == kFrames);
    CHECK(decoded.programme == 0);
    CHECK(decoded.channels.size() == 6);
}

TEST_CASE("an AC-3 stream reports one programme whatever crc1 happens to hold",
          "[eac3][programmes]") {
    // §E2.3.1.2's substreamid lives in byte 2 of an ANNEX E syncframe; in an
    // AC-3 one that byte is part of crc1, which varies per frame. Reading it
    // as strmtyp/substreamid there aliases to "dependent" about a quarter of
    // the time and swallows runs of frames into one group - so the framing
    // gates on each frame's own bsid instead, and every AC-3 frame is its own
    // access unit of the one programme AC-3 can have.
    ac3::EncoderConfig config{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0};
    ac3::FrameEncoder encoder{config};
    constexpr int kFrames = 24;
    std::vector<std::vector<float>> block(2, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(2);
    std::vector<std::byte> stream;
    std::uint64_t n0 = 0;
    for (int f = 0; f < kFrames; ++f) {
        for (std::size_t ch = 0; ch < block.size(); ++ch) {
            fill_tone(block[ch], kMainTone, n0);
            views[ch] = block[ch];
        }
        n0 += ac3::kSamplesPerFrame;
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        stream.insert(stream.end(), frame->begin(), frame->end());
    }

    const auto ids = ac3::programme_ids(stream);
    REQUIRE(ids.has_value());
    CHECK(*ids == std::vector<int>{0});

    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    CHECK(units->size() == static_cast<std::size_t>(kFrames));

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->programmes.size() == 1);
    CHECK(scanned->programmes[0].substreamid == 0);
    CHECK(scanned->programmes[0].access_units.size() == static_cast<std::size_t>(kFrames));
}
