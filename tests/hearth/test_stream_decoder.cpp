#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/render/layout.hpp"
#include "decoder_settings.hpp"
#include "stream_decoder.hpp"

// ac3::hearth::StreamDecoder (apps/hearth/engine/stream_decoder.cpp): access
// units in, rendered blocks out, and nothing lost at the end of a stream.
//
// The count that matters for A3's gapless exit is the one checked here: every
// unit's samples arrive, once, in blocks the output layout's width - including
// a unit transient pre-noise processing is still holding back when the stream
// ends, which a player that forgets Eac3Decoder::flush() silently drops. The
// streams are encoded in the test, so nothing depends on a fixture file.

namespace {

using ac3::hearth::StreamDecoder;

// A second of a tone, or of silence, framed as the encoders want it.
std::vector<float> tone(double hz, double level, std::size_t frames, std::size_t offset) {
    std::vector<float> out(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        out[n] = static_cast<float>(
            level * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n + offset) / 48000.0));
    }
    return out;
}

// `count` E-AC-3 syncframes of a steady tone, all channels alike.
std::vector<std::vector<std::byte>> eac3_frames(ac3::Acmod acmod, bool lfe, int count,
                                                 bool transient_at_end = false) {
    ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 384;
    config.acmod = acmod;
    config.lfe = lfe;
    // With the tool on, a transient engages §3.7's hold-back, and once engaged
    // it stays engaged for the rest of the stream - so the last frame of
    // such a stream is the one only a flush releases.
    config.transient_prenoise = transient_at_end;
    ac3::eac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());

    std::vector<std::vector<std::byte>> out;
    for (int f = 0; f < count; ++f) {
        std::vector<float> samples;
        if (transient_at_end && f == count - 2) {
            // Silence, then a sharp onset late in the frame: the shape the
            // encoder's own heuristic signals a correction for.
            samples.assign(ac3::kSamplesPerFrame, 0.0F);
            for (int n = 960; n < ac3::kSamplesPerFrame; ++n) {
                samples[static_cast<std::size_t>(n)] = static_cast<float>(
                    0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 * n / 48000.0));
            }
        } else if (transient_at_end) {
            samples.assign(ac3::kSamplesPerFrame, 0.0F);
        } else {
            samples = tone(440.0, 0.3, ac3::kSamplesPerFrame,
                           static_cast<std::size_t>(f) * ac3::kSamplesPerFrame);
        }
        const std::vector<std::span<const float>> views(channels, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

std::vector<std::vector<std::byte>> ac3_frames(ac3::Acmod acmod, bool lfe, int count) {
    ac3::EncoderConfig config;
    config.bitrate_kbps = 384;
    config.acmod = acmod;
    config.lfe = lfe;
    ac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::vector<std::byte>> out;
    for (int f = 0; f < count; ++f) {
        const std::vector<float> samples = tone(
            440.0, 0.3, ac3::kSamplesPerFrame, static_cast<std::size_t>(f) * ac3::kSamplesPerFrame);
        const std::vector<std::span<const float>> views(channels, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

// Decodes every unit and the end of the stream, and reports what came out.
struct Played {
    std::size_t frames = 0;
    std::size_t blocks = 0;
    std::size_t widest_block = 0;
    std::size_t slots = 0;
    double energy = 0.0;
    std::vector<std::size_t> per_call;
};

Played play(StreamDecoder& decoder, const std::vector<std::vector<std::byte>>& units) {
    Played out;
    const auto deliver = [&out](std::span<const std::span<const float>> slots,
                                std::size_t frames) {
        ++out.blocks;
        out.frames += frames;
        out.widest_block = std::max(out.widest_block, frames);
        out.slots = slots.size();
        for (const auto slot : slots) {
            REQUIRE(slot.size() == frames);
            for (const float v : slot) {
                out.energy += static_cast<double>(v) * static_cast<double>(v);
            }
        }
    };
    for (const auto& unit : units) {
        const auto delivered = decoder.decode(unit, deliver);
        REQUIRE(delivered.has_value());
        out.per_call.push_back(*delivered);
    }
    out.per_call.push_back(decoder.finish(deliver));
    return out;
}

}  // namespace

TEST_CASE("stream decoder: every E-AC-3 sample arrives, a block at a time, at the layout's width",
          "[hearth][stream-decoder]") {
    const auto layout = ac3::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    const auto units = eac3_frames(ac3::Acmod::k3_2, /*lfe=*/true, 20);
    const Played played = play(decoder, units);

    CHECK(played.frames == 20 * ac3::kSamplesPerFrame);
    CHECK(played.blocks == 20 * ac3::kBlocksPerFrame);
    CHECK(played.widest_block == ac3::kSamplesPerBlock);
    CHECK(played.slots == 6);
    // Something audible came out, not just the right number of zeros.
    CHECK(played.energy > 1.0);
    // Nothing held back: each unit delivered its own frame in its own call,
    // and the end of the stream had nothing left to release.
    CHECK(played.per_call.back() == 0);
}

TEST_CASE("stream decoder: AC-3 frames go through the AC-3 decoder",
          "[hearth][stream-decoder]") {
    const auto layout = ac3::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    const auto units = ac3_frames(ac3::Acmod::k3_2, /*lfe=*/true, 12);
    const Played played = play(decoder, units);

    CHECK(played.frames == 12 * ac3::kSamplesPerFrame);
    CHECK(played.slots == 6);
    CHECK(played.energy > 1.0);
}

TEST_CASE("stream decoder: a stereo layout folds a 5.1 stream and loses no samples",
          "[hearth][stream-decoder]") {
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};
    // A two-speaker layout is served by the decoder's own §7.8 fold rather
    // than by the renderer placing six channels onto two.
    REQUIRE(decoder.serving().fold.has_value());

    const Played played = play(decoder, eac3_frames(ac3::Acmod::k3_2, /*lfe=*/true, 10));
    CHECK(played.frames == 10 * ac3::kSamplesPerFrame);
    CHECK(played.slots == 2);
    CHECK(played.energy > 1.0);

    // And the AC-3 path under the same fold.
    StreamDecoder ac3_decoder{*layout, 48000};
    const Played ac3_played = play(ac3_decoder, ac3_frames(ac3::Acmod::k3_2, /*lfe=*/true, 10));
    CHECK(ac3_played.frames == 10 * ac3::kSamplesPerFrame);
    CHECK(ac3_played.slots == 2);
}

TEST_CASE("stream decoder: a frame still held back at the end of the stream is not lost",
          "[hearth][stream-decoder]") {
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    // Stereo with transient pre-noise processing, a transient in the
    // second-to-last frame: from there on the decoder runs a frame behind.
    const auto units = eac3_frames(ac3::Acmod::k2_0, /*lfe=*/false, 8, /*transient_at_end=*/true);
    const Played played = play(decoder, units);

    // Everything still arrives - the last frame through finish().
    CHECK(played.frames == 8 * ac3::kSamplesPerFrame);
    REQUIRE(played.per_call.size() == units.size() + 1);
    // The transient's own frame delivered nothing when it was decoded (it
    // was held back), and the end of the stream delivered the frame the
    // decoder was still holding.
    CHECK(played.per_call[units.size() - 2] == 0);
    CHECK(played.per_call.back() == ac3::kSamplesPerFrame);
    // The transient itself reached the output.
    CHECK(played.energy > 1.0);
}

TEST_CASE("stream decoder: a reset starts the next stream clean", "[hearth][stream-decoder]") {
    const auto layout = ac3::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    const auto first = eac3_frames(ac3::Acmod::k3_2, /*lfe=*/true, 4);
    const auto second = ac3_frames(ac3::Acmod::k2_0, /*lfe=*/false, 4);
    CHECK(play(decoder, first).frames == 4 * ac3::kSamplesPerFrame);
    // finish() leaves the decoder ready for another stream, of another
    // codec and another layout.
    CHECK(play(decoder, second).frames == 4 * ac3::kSamplesPerFrame);
}

TEST_CASE("stream decoder: a unit that is not a stream is reported, not played",
          "[hearth][stream-decoder]") {
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    const std::vector<std::byte> garbage(64, std::byte{0x5A});
    std::size_t delivered = 0;
    const auto result = decoder.decode(
        garbage, [&delivered](std::span<const std::span<const float>>, std::size_t frames) {
            delivered += frames;
        });
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(result.error().empty());
    CHECK(delivered == 0);

    // And the decoder carries on with a real stream afterwards.
    CHECK(play(decoder, eac3_frames(ac3::Acmod::k2_0, false, 3)).frames ==
          3 * ac3::kSamplesPerFrame);
}

namespace {

// What decoding a stream with reports turned on gave: the frames each call
// delivered, and the reports in order, each with the frames delivered before
// it in its call.
struct Reported {
    std::vector<ac3::hearth::UnitReport> reports;
    std::vector<std::size_t> frames_before;
    std::size_t frames = 0;
};

Reported play_reported(StreamDecoder& decoder, const std::vector<std::vector<std::byte>>& units) {
    Reported out;
    std::size_t in_call = 0;
    const auto deliver = [&](std::span<const std::span<const float>>, std::size_t frames) {
        in_call += frames;
        out.frames += frames;
    };
    const auto reported = [&](const ac3::hearth::UnitReport& report) {
        out.reports.push_back(report);
        out.frames_before.push_back(in_call);
    };
    for (const auto& unit : units) {
        in_call = 0;
        REQUIRE(decoder.decode(unit, deliver, reported).has_value());
    }
    in_call = 0;
    static_cast<void>(decoder.finish(deliver, reported));
    return out;
}

}  // namespace

TEST_CASE("stream decoder: an AC-3 unit's report follows its blocks", "[hearth][stream-decoder]") {
    const auto layout = ac3::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};
    ac3::EncoderConfig config;
    config.bitrate_kbps = 448;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = true;
    config.dialnorm = 20;
    config.heavy = ac3::meta::HeavyConfig{};
    config.cmixlev = ac3::meta::CentreMixLevel::kMinus3dB;
    config.info.bsmod = ac3::meta::BitstreamMode::kCommentary;
    ac3::FrameEncoder encoder{config};
    std::vector<std::vector<std::byte>> units;
    for (int f = 0; f < 4; ++f) {
        const std::vector<float> samples = tone(440.0, 0.3, ac3::kSamplesPerFrame,
                                                static_cast<std::size_t>(f) * ac3::kSamplesPerFrame);
        const std::vector<std::span<const float>> views(6, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        units.push_back(std::move(*frame));
    }

    const Reported played = play_reported(decoder, units);
    REQUIRE(played.reports.size() == 4);
    for (std::size_t k = 0; k < played.reports.size(); ++k) {
        INFO("unit " << k);
        const auto& report = played.reports[k];
        // After the unit's own six blocks.
        CHECK(played.frames_before[k] == static_cast<std::size_t>(ac3::kSamplesPerFrame));
        CHECK(report.acmod == ac3::Acmod::k3_2);
        CHECK(report.lfe);
        CHECK(report.substreams == 1);
        CHECK(report.layout.count == 6);
        CHECK(report.bsmod == 5);
        CHECK(report.dialnorm == 20);
        CHECK(report.compr.has_value());
        CHECK(report.blocks == ac3::kBlocksPerFrame);
        CHECK(report.short_blocks == 0);
        CHECK(report.levels.loro_clev == ac3::meta::level::kMinus3dB);
        CHECK_FALSE(report.concealed.has_value());
        CHECK_FALSE(report.objects.has_value());
    }
}

TEST_CASE("stream decoder: an E-AC-3 unit's report comes with the call that delivers it",
          "[hearth][stream-decoder]") {
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());

    SECTION("mixing metadata") {
        StreamDecoder decoder{*layout, 48000};
        ac3::eac3::FrameConfig config;
        config.bitrate_kbps = 384;
        config.acmod = ac3::Acmod::k3_2;
        config.lfe = true;
        config.dialnorm = 24;
        ac3::meta::MixMetadata mix;
        mix.dmixmod = ac3::meta::DownmixMode::kLtRt;
        mix.lfemixlevcod = 10;
        config.mixing = mix;
        ac3::eac3::FrameEncoder encoder{config};
        std::vector<std::vector<std::byte>> units;
        for (int f = 0; f < 3; ++f) {
            const std::vector<float> samples = tone(440.0, 0.3, ac3::kSamplesPerFrame, 0);
            const std::vector<std::span<const float>> views(6, samples);
            auto frame = encoder.encode_frame(views);
            REQUIRE(frame.has_value());
            units.push_back(std::move(*frame));
        }
        const Reported played = play_reported(decoder, units);
        REQUIRE(played.reports.size() == 3);
        const auto& report = played.reports.front();
        CHECK(report.dialnorm == 24);
        CHECK(report.layout.count == 6);
        CHECK(report.lfe);
        CHECK_FALSE(report.bsmod.has_value());
        CHECK_FALSE(report.short_blocks.has_value());
        CHECK(report.levels.preferred == ac3::meta::DownmixMode::kLtRt);
        CHECK(report.levels.lfe_mix_level_db == 0.0);
    }

    SECTION("a unit held back is reported by the call that releases it, the last by finish()") {
        StreamDecoder decoder{*layout, 48000};
        const auto units = eac3_frames(ac3::Acmod::k2_0, /*lfe=*/false, 8, /*transient_at_end=*/true);
        const Reported played = play_reported(decoder, units);
        CHECK(played.frames == 8 * ac3::kSamplesPerFrame);
        // One report for every unit, each after a whole unit's frames.
        REQUIRE(played.reports.size() == units.size());
        for (const std::size_t before : played.frames_before) {
            CHECK(before == static_cast<std::size_t>(ac3::kSamplesPerFrame));
        }
        CHECK(played.reports.back().layout.count == 2);
    }
}

TEST_CASE("stream decoder: a unit's objects are in its report", "[hearth][stream-decoder]") {
    ac3::oba::AtmosEncoder encoder{
        {.bitrate_kbps = 448, .num_bands_idx = 4, .emit_object_metadata = true}, 1};
    const std::array<ac3::oba::ObjectPlacement, 1> placement{{{}}};
    std::vector<std::span<const float>> views(1);
    std::vector<std::vector<std::byte>> units;
    for (int f = 0; f < 3; ++f) {
        const std::vector<float> essence = tone(440.0, 0.3, ac3::kSamplesPerFrame,
                                                static_cast<std::size_t>(f) * ac3::kSamplesPerFrame);
        views[0] = essence;
        auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());
        units.push_back(std::move(unit->bytes));
    }
    // Whether or not the objects are reconstructed, the report carries them.
    for (const char* name : {"2.0", "7.1.4"}) {
        INFO("layout " << name);
        const auto layout = ac3::render::OutputLayout::parse(name);
        REQUIRE(layout.has_value());
        StreamDecoder decoder{*layout, 48000};
        const Reported played = play_reported(decoder, units);
        REQUIRE(played.reports.size() == 3);
        for (const auto& report : played.reports) {
            REQUIRE(report.objects.has_value());
            CHECK(report.objects->program.dynamic_objects == 1);
            CHECK_FALSE(report.objects->blocks.empty());
        }
    }
}

TEST_CASE("stream decoder: dual mono plays the programme the settings choose",
          "[hearth][stream-decoder]") {
    using ac3::hearth::DualMonoChoice;
    // Two unrelated programmes, one per channel, told apart by their tones.
    ac3::EncoderConfig config;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::kDualMono;
    config.dialnorm2 = 31;  // required for 1+1
    ac3::FrameEncoder encoder{config};
    REQUIRE(encoder.channel_count() == 2);
    std::vector<std::vector<std::byte>> units;
    for (int f = 0; f < 6; ++f) {
        const auto offset = static_cast<std::size_t>(f) * ac3::kSamplesPerFrame;
        const std::vector<float> first = tone(440.0, 0.3, ac3::kSamplesPerFrame, offset);
        const std::vector<float> second = tone(1000.0, 0.3, ac3::kSamplesPerFrame, offset);
        const std::vector<std::span<const float>> views{first, second};
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        units.push_back(std::move(*frame));
    }

    // A stereo room, served by the decoder's fold (which leaves 1+1 alone),
    // and a 5.1 room, where the renderer places the bed's left and right.
    for (const char* name : {"2.0", "5.1"}) {
        INFO("layout " << name);
        const auto layout = ac3::render::OutputLayout::parse(name);
        REQUIRE(layout.has_value());
        const int left = layout->index_of(ac3::eac3::chanmap::Location::kLeft);
        const int right = layout->index_of(ac3::eac3::chanmap::Location::kRight);
        REQUIRE(left >= 0);
        REQUIRE(right >= 0);

        const auto heard = [&](DualMonoChoice choice) {
            ac3::hearth::DecoderSettings settings;
            settings.dual_mono = choice;
            StreamDecoder decoder{*layout, 48000, settings};
            std::vector<std::vector<float>> slots(layout->slots());
            const auto deliver = [&slots](std::span<const std::span<const float>> rendered,
                                          std::size_t frames) {
                for (std::size_t slot = 0; slot < rendered.size() && slot < slots.size(); ++slot) {
                    slots[slot].insert(slots[slot].end(), rendered[slot].begin(),
                                       rendered[slot].begin() + static_cast<std::ptrdiff_t>(frames));
                }
            };
            for (const auto& unit : units) {
                REQUIRE(decoder.decode(unit, deliver).has_value());
            }
            decoder.finish(deliver);
            return std::pair{slots[static_cast<std::size_t>(left)],
                             slots[static_cast<std::size_t>(right)]};
        };

        // Compared with ranges::equal so a failure prints a verdict, not
        // thousands of samples.
        const auto [both_left, both_right] = heard(DualMonoChoice::kBoth);
        REQUIRE(both_left.size() == 6 * ac3::kSamplesPerFrame);
        // Both programmes: one each side, and they are not the same audio.
        CHECK_FALSE(std::ranges::equal(both_left, both_right));

        const auto [first_left, first_right] = heard(DualMonoChoice::kFirst);
        CHECK(std::ranges::equal(first_left, both_left));
        CHECK(std::ranges::equal(first_right, both_left));

        const auto [second_left, second_right] = heard(DualMonoChoice::kSecond);
        CHECK(std::ranges::equal(second_left, both_right));
        CHECK(std::ranges::equal(second_right, both_right));
    }
}

TEST_CASE("stream decoder: fast inverse transform reaches the decoder, closely matching the "
          "reference transform",
          "[hearth][stream-decoder]") {
    const auto layout = ac3::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    const auto units = eac3_frames(ac3::Acmod::k3_2, /*lfe=*/true, 8);

    const auto heard = [&](bool fast) {
        ac3::hearth::DecoderSettings settings;
        settings.fast_inverse_transform = fast;
        StreamDecoder decoder{*layout, 48000, settings};
        std::vector<float> left;
        const auto deliver = [&left](std::span<const std::span<const float>> slots,
                                     std::size_t frames) {
            REQUIRE_FALSE(slots.empty());
            left.insert(left.end(), slots[0].begin(),
                       slots[0].begin() + static_cast<std::ptrdiff_t>(frames));
        };
        for (const auto& unit : units) {
            REQUIRE(decoder.decode(unit, deliver).has_value());
        }
        decoder.finish(deliver);
        return left;
    };

    const std::vector<float> fast = heard(true);
    const std::vector<float> reference = heard(false);
    REQUIRE(fast.size() == reference.size());
    REQUIRE(fast.size() == 8 * ac3::kSamplesPerFrame);

    // The setting must reach DecoderConfig::fast_imdct rather than the same
    // path running twice (decoder_settings.cpp's decoder_setup()) - but both
    // remain a correct decode of the same signal: tests/decoder/test_decoder.cpp's
    // own fast_imdct test pins the two transform paths' agreement above 200 dB SNR.
    CHECK_FALSE(std::ranges::equal(fast, reference));
    double squared_diff = 0.0;
    double squared_signal = 0.0;
    for (std::size_t n = 0; n < fast.size(); ++n) {
        const double diff = static_cast<double>(fast[n]) - static_cast<double>(reference[n]);
        squared_diff += diff * diff;
        squared_signal += static_cast<double>(reference[n]) * static_cast<double>(reference[n]);
    }
    REQUIRE(squared_diff > 0.0);
    const double snr_db = 10.0 * std::log10(squared_signal / squared_diff);
    CHECK(snr_db > 100.0);
}

TEST_CASE("stream decoder: an independent-only decoder plays a unit's first substream alone",
          "[hearth][stream-decoder]") {
    // 7.1: a 5.1 independent substream and a dependent that adds to it.
    ac3::plan::Plan plan;
    plan.codec = ac3::plan::Codec::kEac3;
    plan.layout = ac3::plan::LayoutId::k71;
    plan.bitrate_kbps = 384;
    ac3::eac3::AccessUnitEncoder encoder{ac3::plan::eac3_config(plan)};
    const auto coded = static_cast<std::size_t>(encoder.channel_count());
    REQUIRE(coded > 6);
    std::vector<std::vector<std::byte>> units;
    std::vector<std::vector<std::byte>> cores;
    for (int f = 0; f < 6; ++f) {
        const auto offset = static_cast<std::size_t>(f) * ac3::kSamplesPerFrame;
        std::vector<std::vector<float>> channels;
        for (std::size_t c = 0; c < coded; ++c) {
            channels.push_back(tone(200.0 + (150.0 * static_cast<double>(c)), 0.2,
                                    ac3::kSamplesPerFrame, offset));
        }
        const std::vector<std::span<const float>> views(channels.begin(), channels.end());
        auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        REQUIRE(unit->substream_count() > 1);
        const auto core = unit->substream(0);
        cores.emplace_back(core.begin(), core.end());
        units.push_back(std::move(unit->bytes));
    }

    const auto collect = [](StreamDecoder& decoder, const std::vector<std::vector<std::byte>>& in) {
        std::vector<float> out;
        const auto deliver = [&out](std::span<const std::span<const float>> slots,
                                    std::size_t frames) {
            for (const auto slot : slots) {
                const auto part = slot.first(frames);
                out.insert(out.end(), part.begin(), part.end());
            }
        };
        for (const auto& unit : in) {
            REQUIRE(decoder.decode(unit, deliver).has_value());
        }
        decoder.finish(deliver);
        return out;
    };
    const auto layout = ac3::render::OutputLayout::named("5.1");
    REQUIRE(layout.has_value());
    const auto settings = ac3::hearth::transcode_settings({});
    StreamDecoder independent{*layout, 48000, settings, ac3::hearth::Substreams::kIndependent};
    StreamDecoder first_only{*layout, 48000, settings};
    StreamDecoder whole{*layout, 48000, settings};
    CHECK(independent.substreams() == ac3::hearth::Substreams::kIndependent);
    CHECK(whole.substreams() == ac3::hearth::Substreams::kAll);

    const auto from_units = collect(independent, units);
    REQUIRE(from_units.size() == 6 * 6 * ac3::kSamplesPerFrame);
    // Compared with ranges::equal so a failure prints a verdict, not
    // thousands of samples.
    CHECK(std::ranges::equal(from_units, collect(first_only, cores)));
    CHECK_FALSE(std::ranges::equal(from_units, collect(whole, units)));
}
