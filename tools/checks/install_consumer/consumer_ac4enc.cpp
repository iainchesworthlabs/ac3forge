// The AC-4 encoder's consumer of an installed package, built by
// tools/checks/check_install_consumer.sh twice over, as consumer_ac4.cpp is: through
// find_package(ac3forge) for each exported encoder target (CMakeLists.txt here), and through
// `pkg-config --cflags --libs ac4enc` alone. It sees the installed headers and libraries and
// nothing of the build tree, so a header the encoder's includes and the install leave out, an
// archive a static encoder calls into and the package does not name, or a symbol the shared
// libraries do not export stops it here.
//
// It encodes a second of a stereo tone at 128 kbps, wraps each frame in a sync frame with its CRC,
// and reads the stream back with the inspector the encoder's package requires: every frame found,
// every CRC good, the first frame an I-frame of one presentation, and a dac4 built from the table
// of contents the encoder reports, as an MP4 writer builds its sample entry. A configuration the
// encoder refuses says why.

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include <ac4/ac4.hpp>
#include <ac4enc/encoder.hpp>

namespace {

int fail(const char* what) {
    std::fprintf(stderr, "consumer_ac4enc: %s\n", what);
    return 1;
}

}  // namespace

int main() {
    constexpr int kRate = 48000;
    std::vector<std::vector<float>> input(2, std::vector<float>(kRate));
    for (std::size_t c = 0; c < input.size(); ++c) {
        const double hz = c == 0 ? 440.0 : 660.0;
        for (std::size_t n = 0; n < input[c].size(); ++n) {
            input[c][n] = static_cast<float>(
                0.25 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kRate));
        }
    }
    const std::vector<std::span<const float>> channels{input[0], input[1]};

    // Four channels are no layout the encoder writes, and it says so.
    if (ac4::Encoder::refusal_reason(ac4::EncoderConfig{.channels = 4}).empty()) {
        return fail("the encoder gave no reason for refusing four channels");
    }
    auto encoder = ac4::Encoder::create(ac4::EncoderConfig{.channels = 2, .bitrate_kbps = 128});
    if (!encoder) {
        return fail("the encoder refused stereo at 128 kbps");
    }
    auto frames = encoder->encode(channels);
    if (!frames) {
        return fail("the encoder did not take the tone");
    }
    auto rest = encoder->flush();
    if (!rest) {
        return fail("the encoder did not finish the stream");
    }
    frames->insert(frames->end(), rest->begin(), rest->end());

    std::vector<std::byte> stream;
    for (const ac4::EncodedFrame& frame : *frames) {
        const std::vector<std::byte> sync = ac4::sync_frame(frame.raw_ac4_frame, true);
        stream.insert(stream.end(), sync.begin(), sync.end());
    }
    const ac4::ScanResult scanned = ac4::scan(stream);
    if (scanned.stopped_at || frames->empty() || scanned.frames.size() != frames->size()) {
        return fail("the inspector did not find every frame the encoder wrote");
    }
    for (const ac4::SyncFrame& frame : scanned.frames) {
        if (frame.crc_ok != true) {
            return fail("a sync frame's CRC did not check");
        }
    }
    const auto first = ac4::parse_raw_frame(scanned.frames.front().raw_ac4_frame);
    if (!first || !first->toc.b_iframe_global || first->toc.n_presentations != 1) {
        return fail("the first frame's table of contents is not an I-frame of one presentation");
    }
    if (ac4::build_dac4(encoder->toc()).empty()) {
        return fail("no dac4 describes the encoder's table of contents");
    }

    const std::string codecs = ac4::rfc6381_codec_string(encoder->toc());
    std::printf("consumer_ac4enc: %zu frames, %zu bytes, codecs %s, delay %d samples\n",
                frames->size(), stream.size(), codecs.c_str(), encoder->delay_samples());
    return 0;
}
