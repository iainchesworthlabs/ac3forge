// The AC-4 object renderer of apps/common/ac4_object_render.hpp - ac3cli's,
// through the layout renderer Hearth plays E-AC-3's objects with - on the
// constructed object streams of ac4dec_objects.hpp, whose object 0 moves from
// the left wall to the right one over the stream. Each speaker's output is
// each object's essence at the gain the layout renderer gives the object's
// position at that moment: tone by tone, the speaker's component is the sum
// of the objects' components at their gains.
//
// With AC4DEC_WRITE_LISTENING set to a directory, this writes the moving
// cases ten seconds long there, to decode with ac3cli and listen to.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4_object_render.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4dec_objects.hpp"

namespace {

namespace fs = std::filesystem;
using ac4dec_test::BuiltObjectStream;
using ac4dec_test::ObjectCase;
using S = ac4::Speaker;

// The component of `samples` at `hz`, through a Hann window, scaled so that a
// sine of amplitude a has magnitude a.
std::complex<double> tone_component(std::span<const float> samples, double hz) {
    const double w = 2.0 * std::numbers::pi * hz / 48000.0;
    const auto n = static_cast<double>(samples.size());
    std::complex<double> sum{};
    double weights = 0.0;
    for (std::size_t k = 0; k < samples.size(); ++k) {
        const double window =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(k) / (n - 1.0));
        sum +=
            window * static_cast<double>(samples[k]) * std::polar(1.0, -w * static_cast<double>(k));
        weights += window;
    }
    return 2.0 * sum / weights;
}

ObjectCase committed(const std::string& name) {
    ObjectCase found;
    for (const ObjectCase& c : ac4dec_test::committed_object_cases()) {
        if (c.name == name) {
            found = c;
        }
    }
    REQUIRE(found.name == name);
    return found;
}

// The properties of `object` half way through its frame.
ac4::ObjectProperties at_middle(const ac4::DecodedObject& object) {
    ac4::ObjectProperties p = object.properties;
    for (const ac4::ObjectUpdate& update : object.updates) {
        if (update.sample <= object.samples.size() / 2) {
            p = update.properties;
        }
    }
    return p;
}

std::vector<double> tones_of(const BuiltObjectStream& stream) {
    std::vector<double> tones;
    for (const auto& list : {stream.full, stream.core}) {
        for (const ac4dec_test::ExpectedObject& object : list) {
            for (const auto& tone : object.tones) {
                if (std::ranges::find(tones, tone[0]) == tones.end()) {
                    tones.push_back(tone[0]);
                }
            }
        }
    }
    return tones;
}

}  // namespace

TEST_CASE(
    "rendered AC-4 objects reach each speaker at the layout renderer's gain for their position",
    "[ac4dec][objects]") {
    struct Leg {
        std::string name;
        ac4::DecodingMode mode;
    };
    const std::vector<Leg> legs = {{"direct-dynamic", ac4::DecodingMode::kFull},
                                   {"ajoc-2to4-coarse", ac4::DecodingMode::kFull},
                                   {"ajoc-2to4-coarse", ac4::DecodingMode::kCore}};
    for (const Leg& leg : legs) {
        CAPTURE(leg.name, ac4::describe(leg.mode));
        // Four seconds, so that object 0 moves little within a frame.
        const BuiltObjectStream stream = ac4dec_test::build_objects(committed(leg.name), 96);
        const std::vector<double> tones = tones_of(stream);
        ac4::DecoderConfig config;
        config.decoding = leg.mode;
        ac4::Decoder decoder(config);
        ac3::apps::Ac4ObjectRenderer renderer(ac4::DownmixTarget::k7X4);
        ac3::apps::Ac4ObjectRenderer reference(ac4::DownmixTarget::k7X4);
        const std::span<const S> speakers = renderer.speakers();
        REQUIRE(speakers.size() == 12);
        const std::size_t left = 0;
        const std::size_t right = 1;
        REQUIRE(speakers[left] == S::kLeft);
        REQUIRE(speakers[right] == S::kRight);
        std::vector<std::vector<float>> out;
        std::vector<std::vector<float>>
            last_first;                        // each object's gains as the last frame started
        std::vector<double> left_minus_right;  // object 0's tone, in dB, frame by frame
        for (std::size_t f = 0; f < stream.frames.size(); ++f) {
            const auto decoded = decoder.decode(stream.frames[f]);
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->has_value());
            const ac4::DecodedFrame& frame = **decoded;
            renderer.render(frame, out);
            REQUIRE(out.size() == speakers.size());
            // Each object's gains half way through the frame, and how far
            // from them its gains stand at any other time in it: those in
            // force as the frame starts, as the last frame started (a ramp can
            // carry over), and at each update.
            std::vector<std::vector<float>> gains;
            std::vector<std::vector<float>> spread;
            for (std::size_t o = 0; o < frame.objects.size(); ++o) {
                const ac4::DecodedObject& object = frame.objects[o];
                REQUIRE((object.lfe || object.kind == ac4::ObjectKind::kDyn));
                const auto gains_at = [&](const ac4::ObjectProperties& p) {
                    return object.lfe ? std::vector<float>(speakers.size(), 0.0F)
                                      : reference.object_gains(p);
                };
                std::vector<std::vector<float>> seen = {gains_at(object.properties)};
                for (const ac4::ObjectUpdate& update : object.updates) {
                    seen.push_back(gains_at(update.properties));
                }
                if (o < last_first.size()) {
                    seen.push_back(last_first[o]);
                }
                gains.push_back(gains_at(at_middle(object)));
                std::vector<float> s(speakers.size(), 0.0F);
                for (const std::vector<float>& g : seen) {
                    for (std::size_t slot = 0; slot < s.size(); ++slot) {
                        s[slot] = std::max(s[slot], std::abs(g[slot] - gains.back()[slot]));
                    }
                }
                spread.push_back(std::move(s));
            }
            last_first.clear();
            for (const ac4::DecodedObject& object : frame.objects) {
                last_first.push_back(object.lfe ? std::vector<float>(speakers.size(), 0.0F)
                                                : reference.object_gains(object.properties));
            }
            if (f < 4) {
                continue;  // the decoder's delay and the first metadata
            }
            // Each tone at each speaker is the sum of the objects' components
            // at those gains, to 3 %, and to what the objects' gains move
            // within the frame: a moving object's gain at a speaker changes
            // the faster the nearer the object is to it.
            for (const double hz : tones) {
                std::vector<std::complex<double>> components;
                for (const ac4::DecodedObject& object : frame.objects) {
                    components.push_back(tone_component(object.samples, hz));
                }
                for (std::size_t slot = 0; slot < speakers.size(); ++slot) {
                    if (speakers[slot] == S::kLfe) {
                        continue;
                    }
                    std::complex<double> want{};
                    double moved = 0.0;
                    for (std::size_t o = 0; o < components.size(); ++o) {
                        want += static_cast<double>(gains[o][slot]) * components[o];
                        moved += static_cast<double>(spread[o][slot]) * std::abs(components[o]);
                    }
                    const std::complex<double> got = tone_component(out[slot], hz);
                    CAPTURE(f, hz, slot, std::abs(want), std::abs(got), moved);
                    CHECK(std::abs(got - want) < 0.03 * std::abs(want) + moved + 2e-4);
                }
            }
            // Object 0 (after the LFE of the direct-coded case) moves along
            // the front wall from the left to the right.
            const std::size_t moving = frame.objects.front().lfe ? 1 : 0;
            const double hz = [&] {
                const std::vector<float>& x = frame.objects[moving].samples;
                double best = 0.0;
                double at = 0.0;
                for (const double t : tones) {
                    const double a = std::abs(tone_component(x, t));
                    if (a > best) {
                        best = a;
                        at = t;
                    }
                }
                return at;
            }();
            left_minus_right.push_back(20.0 * std::log10(std::abs(tone_component(out[left], hz)) /
                                                         std::abs(tone_component(out[right], hz))));
        }
        REQUIRE(left_minus_right.size() >= 10);
        CAPTURE(left_minus_right);
        CHECK(left_minus_right.front() > 0.0);
        CHECK(left_minus_right.back() < 0.0);
        CHECK(left_minus_right.front() - left_minus_right.back() > 20.0);
    }
}

TEST_CASE("the moving object streams for listening are written where AC4DEC_WRITE_LISTENING says",
          "[ac4dec][objects]") {
    // Ten seconds of each moving case, for ac3cli decode and the ear: object
    // 0's tone crosses the front from the left wall to the right one.
    const char* dir = std::getenv("AC4DEC_WRITE_LISTENING");
    if (dir == nullptr) {
        SKIP("AC4DEC_WRITE_LISTENING does not name a directory");
    }
    fs::create_directories(dir);
    for (const char* name :
         {"direct-dynamic", "ajoc-2to4-coarse", "ajoc-5lfe-aspx-decorr-sparse"}) {
        const BuiltObjectStream stream = ac4dec_test::build_objects(committed(name), 240);
        const std::vector<std::byte> bytes = ac4dec_test::sync_framed(stream);
        std::ofstream out(fs::path{dir} / ("listen-" + std::string{name} + ".ac4"),
                          std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        REQUIRE(out.good());
    }
}
