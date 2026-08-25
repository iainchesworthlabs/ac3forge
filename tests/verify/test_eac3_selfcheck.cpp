#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/verify/eac3_mirror.hpp"
#include "ac3/verify/eac3_selfcheck.hpp"

// ac3::verify's E-AC-3 half - the Annex E encoder/decoder mirror check. See
// ac3/verify/eac3_mirror.hpp for what it compares and why it matters more
// here than it does for AC-3: for ecpl, tpn, fscod2 and 7.1.4 the in-repo
// round trip is the ONLY check there is, and a round trip cannot see a
// misreading of the spec that both sides share.
//
// Two kinds of test live here, the same split tests/verify/test_selfcheck.cpp
// makes. The compare() cases plant a divergence in a pair of hand-built
// traces and check the right substream, block, stream and field come back out
// - they are what proves the check can FAIL, without needing a broken encoder
// to prove it with. The end-to-end cases then run real programme material
// through Eac3MirrorEncoder across the tool matrix and require silence.

namespace {

using ac3::verify::Eac3Field;

// A trace of one substream that both sides agree on: `streams` coded streams
// and `channels` full-bandwidth channels per block, every array a constant.
// A test then perturbs one copy.
ac3::verify::Eac3SubstreamTrace flat_substream(int fbw, int coded, int streams) {
    ac3::verify::Eac3SubstreamTrace trace;
    trace.fbw_channels = fbw;
    trace.coded_channels = coded;
    trace.blocks_coded = ac3::kBlocksPerFrame;
    for (int block = 0; block < ac3::kBlocksPerFrame; ++block) {
        auto& b = trace.blocks[static_cast<std::size_t>(block)];
        b.entered = true;
        b.allocated = true;
        b.bit_offset = static_cast<std::size_t>(1000 + 500 * block);
        b.cplinu = true;
        b.cplstrtmant = 73;
        b.cplendmant = 253;
        b.streams.assign(static_cast<std::size_t>(streams), {});
        for (auto& stream : b.streams) {
            stream.exponents.assign(64, 7);
            stream.bap.assign(64, 3);
            stream.endmant = 64;
        }
        b.channels.assign(static_cast<std::size_t>(fbw), {});
        for (auto& channel : b.channels) {
            channel.in_coupling = true;
            channel.cplco.assign(15, 0.5);
        }
    }
    return trace;
}

// One access unit's worth: a bed plus `dependents` dependent substreams, all
// the same shape.
ac3::verify::Eac3AccessUnitTrace flat_unit(int dependents) {
    ac3::verify::Eac3AccessUnitTrace trace;
    for (int i = 0; i <= dependents; ++i) {
        auto& slot = trace.begin_substream(i == 0);
        slot = flat_substream(2, 2, 3);
        slot.strmtyp = i == 0 ? ac3::eac3::StreamType::kIndependent
                              : ac3::eac3::StreamType::kDependent;
        slot.substreamid = i == 0 ? 0 : i - 1;
    }
    return trace;
}

std::vector<std::vector<float>> golden_audio(const std::string& name) {
    auto wav = ac3::io::read_wav(std::string{AC3FORGE_GOLDEN_AUDIO_DIR} + "/" + name);
    REQUIRE(wav.has_value());
    return wav->channels;
}

// Both sides' traces really were written, and to the shape this plan implies.
// Returns an empty string when they were.
std::string trace_is_populated(const ac3::verify::Eac3MirrorEncoder& encoder,
                               std::size_t substreams) {
    const auto sides = {std::pair{"encoder", &encoder.encoder_trace()},
                        std::pair{"decoder", &encoder.decoder_trace()}};
    for (const auto& [name, trace] : sides) {
        if (trace->size() != substreams) {
            return std::string{name} + " trace has " + std::to_string(trace->size()) +
                   " substreams, expected " + std::to_string(substreams);
        }
        for (const auto& substream : trace->substreams()) {
            for (int blk = 0; blk < substream.blocks_coded; ++blk) {
                const auto& block = substream.blocks[static_cast<std::size_t>(blk)];
                if (!block.entered || !block.allocated || block.streams.empty()) {
                    return std::string{name} + " trace block " + std::to_string(blk) +
                           " was never filled";
                }
            }
        }
    }
    return {};
}

// Encodes `frames` access units of `source` through the mirror and returns the
// first frame's findings as text, or an empty string when every frame was
// clean. `source` is in WAVE order; the plan's own routing puts it onto the
// target layout's coded channels, exactly as the CLI does.
std::string mirror_encode(const ac3::plan::Plan& plan,
                          const std::vector<std::vector<float>>& source, std::size_t frames) {
    const auto routing =
        ac3::plan::route(ac3::plan::resolve(plan), source.size(), plan.meta.cmixlev,
                         plan.meta.surmixlev);
    REQUIRE(routing.has_value());
    const auto coded = static_cast<std::size_t>(routing->coded_channels);

    ac3::verify::Eac3MirrorEncoder encoder{ac3::plan::eac3_config(plan)};
    std::vector<std::vector<float>> block(coded,
                                          std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
    std::vector<std::span<const float>> in(source.size());
    std::vector<std::span<float>> out(coded);
    std::vector<std::span<const float>> views(coded);
    for (std::size_t c = 0; c < coded; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }

    // What a filled trace must look like for this plan, checked below on
    // every frame: a trace that silently never got written would make every
    // comparison in this file vacuously true, which is the one way a
    // self-check can be worse than no check.
    const auto substreams = ac3::plan::eac3_config(plan).dependents.size() + 1;

    const std::size_t available = source.front().size() / ac3::kSamplesPerFrame;
    for (std::size_t frame = 0; frame < frames && frame < available; ++frame) {
        for (std::size_t c = 0; c < source.size(); ++c) {
            in[c] = std::span{source[c]}.subspan(frame * ac3::kSamplesPerFrame,
                                                 ac3::kSamplesPerFrame);
        }
        ac3::plan::render(*routing, in, out, ac3::kSamplesPerFrame);
        const auto checked = encoder.encode_access_unit(views);
        if (!checked) {
            return "frame " + std::to_string(frame) + ": encode failed";
        }
        if (const auto shape = trace_is_populated(encoder, substreams); !shape.empty()) {
            return "frame " + std::to_string(frame) + ": " + shape;
        }
        if (checked->ok()) {
            continue;
        }
        std::string report = encoder.last_report();
        if (checked->decode_error) {
            // The refusal is the symptom; whatever compare() found above it is
            // the cause, and the gap between the two is the point.
            if (!report.empty()) {
                report += "\n";
            }
            report += "frame " + std::to_string(frame) + ": decoder refused a substream (" +
                      std::string{ac3::describe(*checked->decode_error)} + ")";
        }
        return report;
    }
    return {};
}

ac3::plan::Plan eac3_plan(ac3::plan::LayoutId layout, std::uint32_t kbps,
                          const std::string& tools) {
    ac3::plan::Plan plan;
    plan.codec = ac3::plan::Codec::kEac3;
    plan.layout = layout;
    plan.bitrate_kbps = kbps;
    REQUIRE(ac3::plan::parse_tools(tools, plan.tools));
    return plan;
}

// Long enough to leave the encoder's first-frame transients behind: the
// recorded lesson from this project's own history is that silence and frame 0
// both give false passes, so nothing here checks fewer than a handful of
// frames of real programme material.
constexpr std::size_t kFrames = 10;
constexpr std::size_t kWideFrames = 5;

}  // namespace

TEST_CASE("verify::compare passes an E-AC-3 encoder and decoder that agree", "[verify]") {
    const auto encoder = flat_unit(2);
    const auto decoder = flat_unit(2);
    CHECK(ac3::verify::compare(encoder, decoder, 0).empty());
}

TEST_CASE("verify::compare reports the block boundary an E-AC-3 desync starts at", "[verify]") {
    auto encoder = flat_unit(0);
    auto decoder = flat_unit(0);
    // A decoder that sized one field differently arrives at block 3 short. It
    // stays wrong for every block after that, which is exactly what a real
    // desync does - and the report must still name block 3.
    for (int block = 3; block < ac3::kBlocksPerFrame; ++block) {
        decoder.substream(0).blocks[static_cast<std::size_t>(block)].bit_offset -= 17;
    }

    const auto found = ac3::verify::compare(encoder, decoder, 12);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().frame == 12);
    CHECK(found.front().substream == 0);
    CHECK(found.front().block == 3);
    CHECK(found.front().field == Eac3Field::kBitOffset);
    CHECK(found.front().encoder == 2500);
    CHECK(found.front().decoder == 2483);
    for (const auto& mismatch : found) {
        CHECK(mismatch.block == 3);
    }
}

TEST_CASE("verify::compare names an AHT gain divergence", "[verify]") {
    auto encoder = flat_unit(0);
    auto decoder = flat_unit(0);
    // The AHT case the round trip cannot see on its own: both sides agree on
    // every transmitted field and on the whole allocation, and differ only on
    // the gain one of them recovered from the gain words - which silently
    // rescales that bin's mantissas rather than desynchronising anything.
    for (auto& block : encoder.substream(0).blocks) {
        for (auto& stream : block.streams) {
            stream.aht = true;
        }
    }
    for (auto& block : decoder.substream(0).blocks) {
        for (auto& stream : block.streams) {
            stream.aht = true;
        }
    }
    encoder.substream(0).blocks[0].streams[1].gain.assign(64, 2);
    decoder.substream(0).blocks[0].streams[1].gain.assign(64, 2);
    decoder.substream(0).blocks[0].streams[1].gain[9] = 4;

    const auto found = ac3::verify::compare(encoder, decoder, 0);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().block == 0);
    CHECK(found.front().stream == 1);
    CHECK_FALSE(found.front().channel);
    CHECK(found.front().index == 9);
    CHECK(found.front().field == Eac3Field::kAhtGain);
    CHECK(found.front().encoder == 2);
    CHECK(found.front().decoder == 4);
}

TEST_CASE("verify::compare names a coupling coordinate divergence", "[verify]") {
    auto encoder = flat_unit(0);
    auto decoder = flat_unit(0);
    decoder.substream(0).blocks[2].channels[1].cplco[6] = 0.25;

    const auto found = ac3::verify::compare(encoder, decoder, 3);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().block == 2);
    CHECK(found.front().stream == 1);
    CHECK(found.front().channel);
    CHECK(found.front().index == 6);
    CHECK(found.front().field == Eac3Field::kCouplingCoordinate);
    // The text form names the channel rather than the internal index, and
    // prints a coordinate as a coordinate rather than as an integer.
    const auto text = ac3::verify::report(found, encoder);
    CHECK(text.find("frame 3 substream 0 block 2 channel 1: cplco[6]") == 0);
    CHECK(text.find("0.5") != std::string::npos);
}

TEST_CASE("verify::compare reports a substream count disagreement on its own", "[verify]") {
    const auto encoder = flat_unit(2);
    const auto decoder = flat_unit(1);
    const auto found = ac3::verify::compare(encoder, decoder, 0);
    REQUIRE(found.size() == 1);
    CHECK(found.front().field == Eac3Field::kSubstreamCount);
    CHECK(found.front().encoder == 3);
    CHECK(found.front().decoder == 2);
}

TEST_CASE("verify::compare reports a dependent substream's own divergence", "[verify]") {
    const auto encoder = flat_unit(2);
    auto decoder = flat_unit(2);
    decoder.substream(2).blocks[1].streams[0].bap[4] = 9;

    const auto found = ac3::verify::compare(encoder, decoder, 0);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().substream == 2);
    CHECK(found.front().block == 1);
    CHECK(found.front().field == Eac3Field::kBap);
}

TEST_CASE("verify::compare reports transient pre-noise state before any block", "[verify]") {
    auto encoder = flat_unit(0);
    auto decoder = flat_unit(0);
    encoder.substream(0).transproce = true;
    encoder.substream(0).chintransproc = {true, false};
    encoder.substream(0).transprocloc = {512, 0};
    encoder.substream(0).transproclen = {256, 0};
    decoder.substream(0).transproce = true;
    decoder.substream(0).chintransproc = {true, false};
    decoder.substream(0).transprocloc = {508, 0};
    decoder.substream(0).transproclen = {256, 0};

    const auto found = ac3::verify::compare(encoder, decoder, 0);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().field == Eac3Field::kTransientProcLocation);
    CHECK(found.front().block == -1);
    CHECK(found.front().index == 0);
    CHECK(found.front().encoder == 512);
    CHECK(found.front().decoder == 508);
}

TEST_CASE("every E-AC-3 mismatch field has a name", "[verify]") {
    // A field added without a describe() case would fall through to "unknown
    // field" and make a report unreadable at exactly the moment it matters.
    for (int raw = 0; raw <= static_cast<int>(Eac3Field::kSpxBlend); ++raw) {
        const auto field = static_cast<Eac3Field>(raw);
        CAPTURE(raw);
        CHECK(ac3::verify::describe(field) != "unknown field");
        CHECK_FALSE(ac3::verify::describe(field).empty());
    }
}

// --- end to end -------------------------------------------------------------

TEST_CASE("E-AC-3 encoder and decoder agree on stereo programme material",
          "[verify][golden]") {
    const auto channels = golden_audio("reference_stereo.wav");
    for (const std::uint32_t kbps : {96u, 128u, 192u, 256u}) {
        CAPTURE(kbps);
        const auto failure =
            mirror_encode(eac3_plan(ac3::plan::LayoutId::kStereo, kbps, "none"), channels,
                          kFrames);
        INFO(failure);
        CHECK(failure.empty());
    }
}

TEST_CASE("E-AC-3 encoder and decoder agree across the Annex E tool matrix",
          "[verify][golden]") {
    const auto channels = golden_audio("reference_51.wav");
    // Every token the codec matrix runs, at the rate that matrix uses. ecpl
    // and tpn are the two with no external oracle at all - not even the
    // partial one 7.1.4 gets - so they are the reason this test exists.
    for (const std::string tools : {"none", "cpl", "spx", "aht", "aht:0", "spx+aht",
                                    "cpl:4+spx:5", "cpl+ecpl", "tpn", "cpl+ecpl+tpn", "all",
                                    "auto", "auto+spx:5", "all+noatten", "all+nofastmdct"}) {
        CAPTURE(tools);
        const auto failure =
            mirror_encode(eac3_plan(ac3::plan::LayoutId::k51, 192, tools), channels, kFrames);
        INFO(failure);
        CHECK(failure.empty());
    }
}

TEST_CASE("E-AC-3 encoder and decoder agree on the coupling channel's own delta",
          "[verify][golden]") {
    // Regression test: the decoder's mirror trace hardcoded an empty
    // DeltaSegments for any stream past the full-bandwidth channels, on the
    // (stale, pre delta-under-coupling) assumption that only a fbw channel
    // ever carries one. `delta[kCplStream]` was being parsed correctly all
    // along - only the trace was throwing it away - so this only ever showed
    // up as a false "encoder and decoder disagree" once the coupling
    // channel's own cost/rate-fit comparison actually chose a nonzero
    // cpldeltbae, which the codec matrix's short fixtures never ran long
    // enough to reach.
    //
    // Reproducing it needs the exact shape tools/ci/run_codec_matrix.sh's
    // mirror self-check feeds `eac3-encode`: one second of reference_51.wav
    // (48000 samples - 31.25 frames), padded to a whole 32 frames by HOLDING
    // the last real sample rather than dropping to zero (run_encode's own
    // padding rule, encode.cpp, so a discontinuity that exists only because
    // the clip ends mid-frame does not itself cost a block-switch). That held
    // plateau is what makes the coupling channel's own rate-fit comparison
    // choose a real, nonzero cpldeltbae for the first time, mid-frame, at
    // access unit 31 - plain silence padding or more (unpadded) seconds of
    // programme material both left every frame's coupling delta at zero and
    // never reached this at all.
    auto channels = golden_audio("reference_51.wav");
    constexpr std::size_t kOneSecond = 48000;
    constexpr std::size_t kPaddedFrames = 32;
    for (auto& channel : channels) {
        channel.resize(kOneSecond);
        const float hold = channel.back();
        channel.resize(kPaddedFrames * ac3::kSamplesPerFrame, hold);
    }
    const auto failure = mirror_encode(eac3_plan(ac3::plan::LayoutId::k51, 192, "cpl+ecpl"),
                                       channels, kPaddedFrames);
    INFO(failure);
    CHECK(failure.empty());
}

TEST_CASE("E-AC-3 encoder and decoder agree at every layout", "[verify][golden]") {
    const auto channels = golden_audio("reference_51.wav");
    // 7.1.4 is the layout with no external oracle at all: FFmpeg refuses a
    // second dependent substream in every container, so encoder and decoder
    // are checked against each other and nothing else. Both dependents'
    // traces are compared here, which is what makes that self-check mean
    // something.
    for (const auto layout : {ac3::plan::LayoutId::kMono, ac3::plan::LayoutId::kStereo,
                              ac3::plan::LayoutId::k51, ac3::plan::LayoutId::k71,
                              ac3::plan::LayoutId::k512, ac3::plan::LayoutId::k514,
                              ac3::plan::LayoutId::k714}) {
        CAPTURE(ac3::plan::layout(layout).name);
        for (const std::string tools : {"none", "all"}) {
            CAPTURE(tools);
            const auto failure = mirror_encode(eac3_plan(layout, 256, tools), channels,
                                               kWideFrames);
            INFO(failure);
            CHECK(failure.empty());
        }
    }
}

TEST_CASE("E-AC-3 encoder and decoder agree on 1+1 dual mono", "[verify][golden]") {
    const auto channels = golden_audio("reference_stereo.wav");
    auto plan = eac3_plan(ac3::plan::LayoutId::kDualMono, 192, "none");
    plan.meta.dialnorm2 = 24;
    const auto failure = mirror_encode(plan, channels, kFrames);
    INFO(failure);
    CHECK(failure.empty());
}

TEST_CASE("E-AC-3 encoder and decoder agree at the fscod2 half rates",
          "[verify][golden]") {
    const auto channels = golden_audio("reference_51.wav");
    // fscod2 audio is refused by FFmpeg AND by Dolby's own Reference Player
    // (docs/verification.md), so this round trip is the only check the coded
    // audio has - which is exactly the case a shared misreading survives.
    // The source is played out at the reduced rate rather than resampled:
    // what is under test is the syntax and the allocation tables the rate
    // selects, not the audio's pitch.
    for (const auto rate : {ac3::SampleRate::k24000, ac3::SampleRate::k22050,
                            ac3::SampleRate::k16000}) {
        CAPTURE(ac3::sample_rate_hz(rate));
        for (const std::string tools : {"none", "all"}) {
            CAPTURE(tools);
            auto plan = eac3_plan(ac3::plan::LayoutId::k51, 96, tools);
            plan.sample_rate = rate;
            const auto failure = mirror_encode(plan, channels, kFrames);
            INFO(failure);
            CHECK(failure.empty());
        }
    }
}

TEST_CASE("E-AC-3 encoder and decoder agree under VBR", "[verify][golden]") {
    const auto channels = golden_audio("reference_51.wav");
    for (const std::string spec : {"q:0.3", "q:0.6,min:96,max:256"}) {
        CAPTURE(spec);
        auto plan = eac3_plan(ac3::plan::LayoutId::k51, 192, "all");
        REQUIRE(ac3::plan::parse_vbr(spec, plan.vbr));
        const auto failure = mirror_encode(plan, channels, kFrames);
        INFO(failure);
        CHECK(failure.empty());
    }
}

TEST_CASE("the E-AC-3 mirror check is off unless a trace is attached", "[verify][golden]") {
    ac3::eac3::FrameConfig config;
    CHECK(config.trace == nullptr);
    ac3::DecoderConfig decoder_config;
    CHECK(decoder_config.eac3_trace == nullptr);

    // And attaching one changes nothing about the output: the trace reads
    // state the encoder already has, it never steers a decision. A regression
    // here would make the checked build a different encoder from the shipped
    // one, which would make the check worthless.
    const auto channels = golden_audio("reference_stereo.wav");
    std::vector<std::span<const float>> views{
        std::span{channels[0]}.first(ac3::kSamplesPerFrame),
        std::span{channels[1]}.first(ac3::kSamplesPerFrame)};
    config.acmod = ac3::Acmod::k2_0;
    // Coupling without spectral extension: §E3.3.1 derives cplendf from
    // spxbegf when both are on, which at 2/0's rate default can leave no
    // coupling region at all - and this case wants the coupling stream
    // present, to prove it is traced.
    config.coupling = true;
    config.aht = true;

    ac3::eac3::FrameEncoder plain{config};
    const auto without = plain.encode_frame(views);
    REQUIRE(without.has_value());

    ac3::verify::Eac3SubstreamTrace trace;
    config.trace = &trace;
    ac3::eac3::FrameEncoder traced{config};
    const auto with = traced.encode_frame(views);
    REQUIRE(with.has_value());

    CHECK(*without == *with);
    CHECK(trace.fbw_channels == 2);
    CHECK(trace.coded_channels == 2);
    CHECK(trace.blocks.front().entered);
    CHECK(trace.blocks.back().allocated);
    // The coupling stream sits one past the coded channels, the numbering
    // ac3/verify/mirror.hpp already uses.
    CHECK(trace.blocks.front().cplinu);
    CHECK(trace.blocks.front().streams.size() == 3);
    CHECK(trace.blocks.front().channels.size() == 2);
    CHECK(trace.blocks.front().channels.front().cplco.size() ==
          trace.blocks.front().channels.back().cplco.size());
}
