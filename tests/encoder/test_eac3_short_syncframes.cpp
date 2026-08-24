#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"

// §E2.3.1.4 short syncframes (numblkscod 0-2) and §E2.3.1.64 convsync
// (roadmap EQ11). The encoder always wrote numblkscod == 0x3 before this;
// the decoder's numblkscod != 0x3 path (parse_audfrm/parse_bsi) is spec-
// derived from the same source EQ1's per-block machinery already exercises
// at six blocks, but had never been driven by a real encoded stream. These
// tests are that first real stream, both ways.

namespace {

using ac3::eac3::blocks_per_syncframe;

std::uint8_t u8(std::span<const std::byte> bytes, std::size_t index) {
    return std::to_integer<std::uint8_t>(bytes[index]);
}

// Real audio, not silence: a tone whose frequency differs per channel, so a
// swapped or dropped channel is audible in the SNR rather than merely in the
// byte count. `samples` is the WHOLE test signal's length, sliced into
// encoder.samples_per_frame()-sized calls.
std::vector<std::vector<float>> tones(int channels, std::size_t samples) {
    std::vector<std::vector<float>> out(static_cast<std::size_t>(channels),
                                        std::vector<float>(samples));
    for (int ch = 0; ch < channels; ++ch) {
        const double hz = 300.0 + 400.0 * ch;
        for (std::size_t n = 0; n < samples; ++n) {
            out[static_cast<std::size_t>(ch)][n] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0));
        }
    }
    return out;
}

// Direct sample SNR with the 256-sample encode+decode delay, skipping a
// generous warm-up at each end - generous because a short syncframe's own
// analysis window still spans 512 samples regardless of how few new samples
// the frame itself contributes, so settling takes more FRAMES the shorter
// numblkscod makes each one.
double snr_db(const std::vector<float>& input, const std::vector<float>& decoded) {
    constexpr std::size_t kDelay = 256;
    constexpr std::size_t kSkip = 2048;
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = kSkip; i + kSkip < input.size() && i + kSkip < decoded.size(); ++i) {
        const double x = static_cast<double>(input[i - kDelay]);
        const double d = static_cast<double>(decoded[i]) - x;
        signal += x * x;
        noise += d * d;
    }
    return 10.0 * std::log10(signal / std::max(noise, 1e-30));
}

struct ShortRoundTrip {
    std::vector<std::vector<float>> source;    // per coded channel, full length
    std::vector<std::vector<float>> rendered;  // per rendered channel, full length
    ac3::eac3::chanmap::Layout layout;         // rendered[i]'s location is layout[i]
    int numblkscod = 3;
};

// Every coded channel's Location, in the transmission order
// AccessUnitEncoder::encode_access_unit wants channels handed to it: the
// independent's own (Table 5.8 fbw order, LFE last) then each dependent's, in
// ITS chanmap's Table E2.5 bit order - chanmap::expand's own order, which is
// what a dependent's channels are actually transmitted in. A dependent with
// no explicit chanmap renders the same way its acmod/lfeon would as an
// independent, so it takes the same acmod_map fallback.
std::vector<ac3::eac3::chanmap::Location> coded_locations(
    const ac3::eac3::AccessUnitConfig& config) {
    namespace cm = ac3::eac3::chanmap;
    std::vector<cm::Location> out;
    const auto append = [&](const ac3::eac3::FrameConfig& sub) {
        const auto map = sub.chanmap ? *sub.chanmap : cm::acmod_map(sub.acmod, sub.lfe);
        for (const auto location : cm::expand(map)) {
            out.push_back(location);
        }
    };
    append(config.independent);
    for (const auto& dep : config.dependents) {
        append(dep);
    }
    return out;
}

// Encodes `frame_calls` calls to encode_frame's worth of real audio - one
// distinct tone per LOCATION, not per coded index, since a dependent's
// chanmap can put a coded channel somewhere the independent already has a
// slot for (§E3.8.2 overwrite) - and decodes it back through
// split_access_units + Eac3Decoder, so the framing is exercised too, not just
// the frames the encoder happened to hand over.
ShortRoundTrip round_trip(const ac3::eac3::AccessUnitConfig& config, int frame_calls) {
    namespace cm = ac3::eac3::chanmap;
    ac3::eac3::AccessUnitEncoder encoder{config};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    const auto locations = coded_locations(config);
    REQUIRE(locations.size() == nchans);
    const auto samples_per_frame = static_cast<std::size_t>(
        blocks_per_syncframe(config.independent.numblkscod) * ac3::kSamplesPerBlock);
    const auto total_samples = samples_per_frame * static_cast<std::size_t>(frame_calls);

    // One tone per location that actually appears, keyed by the same
    // std::to_underlying-shaped index every Location already has. LFE/LFE2
    // get their own low-frequency tone instead of tones()'s generic
    // 300 + 400*index scale: at index 20-21 that scale would land at
    // 8.3-8.7 kHz, which §7.3.1 kLfeEndmant's 7-bin coded bandwidth (a few
    // hundred Hz) cannot carry at all - the encoder would legitimately code
    // that as silence, which is not what this test is checking.
    const auto max_location = static_cast<std::size_t>(cm::kMaxChannels);
    auto pcm_for = tones(static_cast<int>(max_location), total_samples);
    for (const auto lfe : {cm::Location::kLfe, cm::Location::kLfe2}) {
        for (std::size_t n = 0; n < total_samples; ++n) {
            pcm_for[static_cast<std::size_t>(lfe)][n] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * 60.0 * static_cast<double>(n) / 48000.0));
        }
    }

    ShortRoundTrip rt;
    rt.source.resize(nchans);
    for (std::size_t ch = 0; ch < nchans; ++ch) {
        rt.source[ch] = pcm_for[static_cast<std::size_t>(locations[ch])];
    }

    std::vector<std::byte> stream;
    for (int f = 0; f < frame_calls; ++f) {
        std::vector<std::span<const float>> views(nchans);
        for (std::size_t ch = 0; ch < nchans; ++ch) {
            views[ch] = std::span{rt.source[ch]}.subspan(
                static_cast<std::size_t>(f) * samples_per_frame, samples_per_frame);
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == static_cast<std::size_t>(frame_calls));

    ac3::Eac3Decoder decoder;
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        rt.numblkscod = (*decoded)->numblkscod;
        rt.layout = (*decoded)->layout;
        if (rt.rendered.empty()) {
            rt.rendered.resize((*decoded)->channels.size());
        }
        REQUIRE((*decoded)->channels.size() == rt.rendered.size());
        for (std::size_t ch = 0; ch < (*decoded)->channels.size(); ++ch) {
            rt.rendered[ch].insert(rt.rendered[ch].end(), (*decoded)->channels[ch].begin(),
                                   (*decoded)->channels[ch].end());
        }
    }
    return rt;
}

// The tone rt.rendered[ch] should carry: whichever coded channel shares
// rt.layout[ch]'s location - the last one transmitted, since §E3.8.2's
// overwrite is what a real decoder applies too.
const std::vector<float>& expected_for(const ShortRoundTrip& rt,
                                       const std::vector<ac3::eac3::chanmap::Location>& locations,
                                       int rendered_index) {
    const auto location = rt.layout[rendered_index];
    int last = -1;
    for (std::size_t ch = 0; ch < locations.size(); ++ch) {
        if (locations[ch] == location) {
            last = static_cast<int>(ch);
        }
    }
    REQUIRE(last >= 0);
    return rt.source[static_cast<std::size_t>(last)];
}

}  // namespace

TEST_CASE("frame_words scales with numblkscod", "[eac3][numblkscod]") {
    // Table E2.4: 1/2/3/6 blocks - a code-0 frame is a sixth of a code-3
    // frame's words at the same rate, exactly, since both are the same exact
    // bit budget rounded to whole 16-bit words.
    const auto six = ac3::eac3::frame_words(ac3::SampleRate::k48000, 192, 6);
    STATIC_CHECK(ac3::eac3::frame_words(ac3::SampleRate::k48000, 192, 1) * 6 == 384);
    CHECK(six == 384);
    CHECK(ac3::eac3::frame_words(ac3::SampleRate::k48000, 192, 2) * 3 == six);
    CHECK(ac3::eac3::frame_words(ac3::SampleRate::k48000, 192, 3) * 2 == six);
}

TEST_CASE("numblkscod rejects what Annex E cannot express", "[eac3][numblkscod]") {
    // Table E2.4 only defines codes 0-3.
    {
        ac3::eac3::FrameConfig config{.bitrate_kbps = 192, .numblkscod = 4};
        const auto frame = ac3::eac3::build_silent_frame(config);
        CHECK_FALSE(frame.has_value());
    }
    // §E2.3.1.3: fscod2 replaces numblkscod outright at a reduced rate, so a
    // caller cannot ask for both a reduced rate and a short syncframe.
    {
        ac3::eac3::FrameConfig config{
            .sample_rate = ac3::SampleRate::k24000, .bitrate_kbps = 192, .numblkscod = 0};
        const auto frame = ac3::eac3::build_silent_frame(config);
        CHECK_FALSE(frame.has_value());
    }
    // Table E1.3: ahte is implied 0 below a six-block syncframe, so AHT has
    // no bit to switch it on with.
    {
        ac3::eac3::FrameConfig config{
            .bitrate_kbps = 192, .numblkscod = 1, .aht = true};
        const auto frame = ac3::eac3::build_silent_frame(config);
        CHECK_FALSE(frame.has_value());
    }
    {
        ac3::eac3::FrameConfig config{
            .bitrate_kbps = 192, .numblkscod = 1, .auto_tools = true};
        const auto frame = ac3::eac3::build_silent_frame(config);
        CHECK_FALSE(frame.has_value());
    }
    // The default, six blocks, is unaffected.
    {
        ac3::eac3::FrameConfig config{.bitrate_kbps = 192, .numblkscod = 3, .aht = true};
        const auto frame = ac3::eac3::build_silent_frame(config);
        CHECK(frame.has_value());
    }
}

TEST_CASE("an access unit refuses substreams with different numblkscod",
          "[eac3][numblkscod]") {
    // AccessUnitConfig's own contract: every substream codes the same
    // samples, so they cannot disagree about how many blocks a syncframe is.
    ac3::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 640, .acmod = ac3::Acmod::k3_2, .lfe = true,
                       .numblkscod = 1},
        .dependents = {{.bitrate_kbps = 320,
                       .acmod = ac3::Acmod::k2_2,
                       .numblkscod = 3,
                       .chanmap = ac3::eac3::chanmap::k71Rear}}};
    ac3::eac3::AccessUnitEncoder encoder{config};
    CHECK(encoder.channel_count() == 0);  // the constructor rejected the layout
    const auto audio = tones(8, static_cast<std::size_t>(ac3::kSamplesPerBlock));
    std::vector<std::span<const float>> views(audio.begin(), audio.end());
    const auto unit = encoder.encode_access_unit(views);
    CHECK_FALSE(unit.has_value());
}

TEST_CASE("numblkscod is written where Annex E puts it, and nowhere else",
          "[eac3][numblkscod]") {
    // byte 4: fscod(2) | numblkscod(2) | acmod(3) | lfeon(1). acmod 2/0
    // stereo (010) with lfeon clear, fscod 0 (48 kHz): the top nibble is the
    // only thing under test here.
    for (const int code : {0, 1, 2, 3}) {
        CAPTURE(code);
        const auto frame =
            ac3::eac3::build_silent_frame({.bitrate_kbps = 192, .numblkscod = code});
        REQUIRE(frame.has_value());
        const auto top = u8(*frame, 4) >> 4;
        CHECK(top == static_cast<std::uint8_t>(code));
    }
}

TEST_CASE("a silent frame at every numblkscod decodes cleanly", "[eac3][numblkscod]") {
    // Table E1.3's numblkscod-conditioned fields - expstre/ahte implied at
    // code 3 only, blkstrtinfoe absent only at code 0, convexpstre a real bit
    // below code 3, convsync present below code 3 - all have to be placed
    // exactly right for ANY of these frames to parse at all: a single
    // misplaced bit desyncs every field after it, and a decoder reading
    // exponents or mantissas at the wrong offset is exactly the failure mode
    // EQ1's own deltbaie bug surfaced as (an out-of-range exponent, several
    // fields downstream of the actual mistake). Decoding successfully is
    // therefore a strong, whole-header check that does not require hand-
    // walking every field the way a bit-position test would - see the
    // round-trip tests below for the same guarantee on real (not silent)
    // audio, including a dependent substream.
    for (const int code : {0, 1, 2, 3}) {
        CAPTURE(code);
        const auto frame = ac3::eac3::build_silent_frame(
            {.bitrate_kbps = 192, .acmod = ac3::Acmod::k3_2, .lfe = true, .numblkscod = code});
        REQUIRE(frame.has_value());
        const auto units = ac3::split_access_units(*frame);
        REQUIRE(units.has_value());
        REQUIRE(units->size() == 1);
        ac3::Eac3Decoder decoder;
        const auto decoded = decoder.decode_access_unit((*units)[0]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        CHECK((*decoded)->numblkscod == code);
        CHECK((*decoded)->channels.size() == 6);  // L C R Ls Rs LFE
    }
}

TEST_CASE("convsync marks the first frame of every 6/numblkscod group",
          "[eac3][numblkscod]") {
    // §E2.3.1.64, via the PES-packaging text (§8.2's own worked description):
    // a converter accumulating short syncframes into a six-block AC-3 frame
    // needs to know which one starts a fresh accumulation. This project makes
    // no converter, so what is under test is only that the encoder's own
    // counter cycles right: 1 on the first frame of every group of
    // 6/numblkscod frames, 0 on the rest, forever.
    for (const int code : {0, 1, 2}) {
        CAPTURE(code);
        const int group = 6 / blocks_per_syncframe(code);
        ac3::eac3::FrameEncoder encoder{
            {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0, .numblkscod = code}};
        const auto samples =
            static_cast<std::size_t>(blocks_per_syncframe(code) * ac3::kSamplesPerBlock);
        const auto audio = tones(2, samples);
        std::vector<std::span<const float>> views(audio.begin(), audio.end());
        for (int f = 0; f < group * 3; ++f) {
            const auto frame = encoder.encode_frame(views);
            REQUIRE(frame.has_value());
            ac3::BitReader r{*frame};
            r.skip(16 + 2 + 3 + 11 + 2 + 2 + 3 + 1 + 5 + 5);  // up to compre
            REQUIRE(r.read(1) == 0);  // compre
            REQUIRE(r.read(1) == 0);  // mixmdate
            REQUIRE(r.read(1) == 0);  // infomdate
            const bool convsync = r.read(1) != 0;
            CHECK(convsync == (f % group == 0));
        }
    }
}

TEST_CASE("a short syncframe round-trips real audio", "[eac3][numblkscod][decoder]") {
    // The decoder's numblkscod != 3 path (parse_bsi/parse_audfrm's own
    // branches) had never been driven by a real encoded stream before this -
    // it existed only because it is spec-derived from the same machinery the
    // six-block path already exercises. This is that stream.
    for (const int code : {0, 1, 2, 3}) {
        CAPTURE(code);
        const ac3::eac3::AccessUnitConfig config{
            {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0, .numblkscod = code}};
        const auto locations = coded_locations(config);
        // More calls at a shorter frame, so every layout covers the same real
        // duration of audio - about 200 ms - and the SNR bar below means the
        // same thing at every code.
        const int frame_calls = 6 * blocks_per_syncframe(3) / blocks_per_syncframe(code);
        const auto rt = round_trip(config, std::max(frame_calls, 6));
        REQUIRE(rt.numblkscod == code);
        REQUIRE(rt.rendered.size() == 2);
        for (int ch = 0; ch < 2; ++ch) {
            CAPTURE(ch);
            CHECK(snr_db(expected_for(rt, locations, ch), rt.rendered[static_cast<std::size_t>(ch)]) >
                 25.0);
        }
    }
}

TEST_CASE("a short syncframe round-trips a dependent substream too",
          "[eac3][numblkscod][decoder]") {
    // The access-unit path, not just one substream: both the independent bed
    // and a dependent share numblkscod (AccessUnitConfig's own contract,
    // checked above), and both have to decode correctly framed together.
    // k71Rear's own §E3.8.2 overwrite (the dependent's Ls/Rs replace the
    // bed's) is exercised deliberately, not sidestepped - expected_for above
    // is what makes comparing across that overwrite possible at all.
    const ac3::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 320, .acmod = ac3::Acmod::k3_2, .lfe = true,
                       .numblkscod = 1},
        .dependents = {{.bitrate_kbps = 160,
                       .acmod = ac3::Acmod::k2_2,
                       .numblkscod = 1,
                       .chanmap = ac3::eac3::chanmap::k71Rear}}};
    const auto locations = coded_locations(config);
    const auto rt = round_trip(config, 24);
    REQUIRE(rt.numblkscod == 1);
    REQUIRE(rt.rendered.size() == 8);  // L C R Ls Rs Lrs Rrs LFE
    for (int ch = 0; ch < static_cast<int>(rt.rendered.size()); ++ch) {
        CAPTURE(ch);
        CHECK(snr_db(expected_for(rt, locations, ch), rt.rendered[static_cast<std::size_t>(ch)]) >
             20.0);
    }
}
