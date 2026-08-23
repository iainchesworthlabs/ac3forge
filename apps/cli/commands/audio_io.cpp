#include "audio_io.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fmt/base.h>
#include <fmt/format.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/audio/capture.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "matroska/matroska.hpp"

namespace ac3cli::commands {

int run_devices() {
    const auto devices = ac3::audio::enumerate_devices();
    if (!devices) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return 1;
    }
    if (devices->empty()) {
        fmt::println("no active capture endpoints found");
        return 0;
    }
    fmt::println("{:>3}  {:<9} {:>7}  {:>3}  {}", "idx", "kind", "rate", "ch", "name");
    for (std::size_t i = 0; i < devices->size(); ++i) {
        const auto& d = (*devices)[i];
        fmt::println("{:>3}  {:<9} {:>7}  {:>3}  {}{}", i,
                     d.kind == ac3::audio::DeviceKind::kInput ? "input" : "loopback",
                     d.sample_rate, d.channels, d.name, d.is_default ? "  [default]" : "");
    }
    return 0;
}

namespace {

// Record what a bitstreaming source is actually sending, rather than
// encoding it (roadmap IO3).
//
// An endpoint fed IEC 61937 hands its bursts over as ordinary PCM - the
// capture API has no way to say "this is Dolby Digital" - so a recorder that
// takes them at face value encodes noise. Once PassthroughDetector has said
// otherwise, the useful output is the elementary stream inside, which is what
// this writes: bit-identical to what the player sent, no re-encode at all.
//
// `detector` arrives holding the carrier already gone past, so the recording
// starts at the first burst rather than a quarter-second into it.
int record_passthrough(std::string_view out_path, std::uint32_t seconds,
                       ac3::audio::Capture& capture,
                       ac3::iec61937::PassthroughDetector& detector, const Options& meta) {
    const auto channels = capture.channels();
    const bool eac3 = detector.detected() == ac3::iec61937::BurstDataType::kEac3;
    fmt::println("");
    fmt::println("capture is bitstreaming {}, not PCM: recording the elementary stream",
                 eac3 ? "Dolby Digital Plus (data type 0x15)" : "Dolby Digital (data type 0x01)");
    if (meta.matroska_container) {
        // Said rather than silently ignored: container=mkv needs the frame
        // boundaries write_frames_or_mux muxes on, and this path never has
        // frames - it has a byte stream nothing here re-parsed. 'mkv' turns
        // the result into Matroska in one further step.
        fmt::println("container=mkv does not apply to a passthrough capture: writing the bare");
        fmt::println("elementary stream, which 'ac3cli mkv' will wrap if you want a container.");
    }

    EncodedStreamSink sink;
    if (!sink.open(out_path, meta.keep_partial)) {
        return 1;
    }
    ac3::iec61937::BurstReader reader;
    std::vector<std::byte> payload;
    std::uint64_t elementary_bytes = 0;
    const auto drain = [&](std::span<const std::byte> carrier) {
        payload.clear();
        const auto pushed = reader.push(carrier, payload);
        if (!pushed) {
            fmt::println(stderr, "error: {}", ac3::iec61937::describe(pushed.error()));
            return false;
        }
        if (payload.empty()) {
            return true;
        }
        elementary_bytes += payload.size();
        return sink.push(payload);
    };

    if (!drain(detector.buffered())) {
        sink.abort();
        return 1;
    }
    detector.clear_buffer();

    // The carrier's own clock, not the content's: an E-AC-3 burst period
    // spans 6144 sample frames at the 4x rate, an AC-3 one 1536 at 1x, and
    // both come to the same 32 ms of programme.
    const auto rate = capture.sample_rate();
    const std::uint64_t target_frames = static_cast<std::uint64_t>(seconds) * rate;
    std::uint64_t captured = 0;
    std::vector<float> interleaved(static_cast<std::size_t>(ac3::kSamplesPerFrame) * channels);
    std::vector<std::byte> carrier;
    while (captured < target_frames) {
        std::size_t filled = 0;
        while (filled < interleaved.size()) {
            const auto got = capture.buffer()->read(
                std::span{interleaved}.subspan(filled, interleaved.size() - filled));
            filled += got;
            if (got == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        captured += static_cast<std::uint64_t>(ac3::kSamplesPerFrame);
        carrier.clear();
        ac3::iec61937::carrier_from_capture(interleaved, channels, carrier);
        if (!drain(carrier)) {
            sink.abort();
            return 1;
        }
        fmt::print("\r  {} burst{} captured ({:.1f} s)  ", reader.bursts(),
                   reader.bursts() == 1 ? "" : "s",
                   static_cast<double>(captured) / static_cast<double>(rate));
    }
    fmt::println("");

    capture.stop();
    if (reader.bursts() == 0) {
        sink.abort();
        fmt::println(stderr, "error: the bursts stopped before a whole one was captured");
        return 1;
    }
    if (!sink.close()) {
        return 1;
    }
    const auto stats = capture.stats();
    fmt::println("wrote {} {} bursts ({} bytes) to {}", reader.bursts(),
                 eac3 ? "E-AC-3" : "AC-3", elementary_bytes, out_path);
    fmt::println("captured {} frames, {} silence-filled, {} dropped", stats.frames_captured,
                 stats.frames_silence_filled, stats.frames_dropped);
    if (reader.skipped_bursts() > 0 || reader.false_syncs() > 0) {
        fmt::println("{} burst(s) of another data type skipped, {} false sync(s) resynced past",
                     reader.skipped_bursts(), reader.false_syncs());
    }
    fmt::println("no re-encode happened: this is what the source sent, byte for byte.");
    return 0;
}

}  // namespace

int run_record(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
               int device_index, const Options& meta) {
    const auto devices = ac3::audio::enumerate_devices();
    if (!devices) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return 1;
    }
    if (devices->empty()) {
        fmt::println(stderr, "error: no capture endpoints available");
        return 1;
    }
    if (device_index < 0 || static_cast<std::size_t>(device_index) >= devices->size()) {
        fmt::println(stderr, "error: device index {} out of range (see 'ac3cli devices')",
                     device_index);
        return 1;
    }
    const auto& device = (*devices)[static_cast<std::size_t>(device_index)];

    ac3::SampleRate sr{};
    bool encodable_rate = true;
    switch (device.sample_rate) {
        case 48000: sr = ac3::SampleRate::k48000; break;
        case 44100: sr = ac3::SampleRate::k44100; break;
        case 32000: sr = ac3::SampleRate::k32000; break;
        // Not an error on its own any more: a bitstreaming endpoint routinely
        // runs at a rate AC-3 cannot encode at - 192 kHz is exactly the
        // E-AC-3 carrier's 4x - so the rate gate is now the PCM path's, and
        // is applied below only once detection has ruled a bitstream out.
        default: encodable_rate = false; break;
    }

    ac3::audio::Capture capture;
    const auto started = capture.start(device.id, device.kind);
    if (!started) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(started.error()));
        return 1;
    }
    const auto channels = capture.channels();
    fmt::println("recording from \"{}\" ({} Hz, {} ch) for {} s…", device.name,
                 capture.sample_rate(), channels, seconds);

    // A rate AC-3 cannot encode at leaves only one thing this can be. Listen
    // until the detector decides, then either record the bursts or say what
    // was wrong with the endpoint - which is both reasons at once, since
    // neither alone explains why nothing can be done with it.
    if (!encodable_rate) {
        ac3::iec61937::PassthroughDetector detector;
        std::vector<float> probe(static_cast<std::size_t>(ac3::kSamplesPerFrame) * channels);
        while (!detector.decided()) {
            std::size_t filled = 0;
            while (filled < probe.size()) {
                const auto got = capture.buffer()->read(
                    std::span{probe}.subspan(filled, probe.size() - filled));
                filled += got;
                if (got == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            detector.push(probe, channels);
        }
        if (detector.detected()) {
            return record_passthrough(out_path, seconds, capture, detector, meta);
        }
        capture.stop();
        fmt::println(stderr,
                     "error: device runs at {} Hz; AC-3 needs 32, 44.1 or 48 kHz "
                     "(change the endpoint's shared-mode format in Windows sound settings), "
                     "and it is not bitstreaming IEC 61937 either",
                     device.sample_rate);
        return 1;
    }

    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state, and this function only constructs it once (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
        .sample_rate = sr, .bitrate_kbps = bitrate, .fast_mdct = meta.fast_mdct});
    // Meters what the encoder is fed, not what the endpoint delivers: a
    // needle that moves on a channel the stream never carries would be a lie.
    ac3::analysis::LevelMeter meter{ac3::Acmod::k2_0, false, capture.sample_rate()};
    const std::uint64_t target_frames =
        (static_cast<std::uint64_t>(seconds) * capture.sample_rate() + ac3::kSamplesPerFrame - 1) /
        ac3::kSamplesPerFrame;

    std::vector<float> interleaved(static_cast<std::size_t>(ac3::kSamplesPerFrame) * channels);
    std::vector<std::vector<float>> planar(2,
                                           std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
    std::vector<std::span<const float>> views{planar[0], planar[1]};
    std::vector<std::vector<std::byte>> frames;
    frames.reserve(static_cast<std::size_t>(target_frames));

    // Runs alongside the encode for the first quarter-second or so, then
    // costs nothing at all. Encoding continues meanwhile rather than the
    // session pausing to listen first: an ordinary microphone - which is
    // what this almost always is - must not lose its opening.
    ac3::iec61937::PassthroughDetector detector;

    while (frames.size() < target_frames) {
        // Block until a whole AC-3 frame of interleaved samples is available.
        std::size_t filled = 0;
        while (filled < interleaved.size()) {
            const auto got = capture.buffer()->read(
                std::span{interleaved}.subspan(filled, interleaved.size() - filled));
            filled += got;
            if (got == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        if (!detector.decided()) {
            detector.push(interleaved, channels);
            if (detector.detected()) {
                // Everything encoded so far was burst data read as audio.
                // Drop it and record what the source is actually sending.
                frames.clear();
                frames.shrink_to_fit();
                return record_passthrough(out_path, seconds, capture, detector, meta);
            }
        }
        // Deinterleave to stereo: take the first two channels, or duplicate a
        // mono source across both.
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const std::size_t base = static_cast<std::size_t>(i) * channels;
            planar[0][static_cast<std::size_t>(i)] = interleaved[base];
            planar[1][static_cast<std::size_t>(i)] =
                channels > 1 ? interleaved[base + 1] : interleaved[base];
        }
        meter.process(views);
        auto frame = encoder->encode_frame(views);
        if (!frame) {
            fmt::println(stderr, "error: bitrate must be a legal AC-3 rate");
            return 1;
        }
        frames.push_back(std::move(*frame));
        // One frame is 32 ms at 48 kHz, so the meter redraws about 30 times a
        // second without any throttling of its own.
        print_live_meter(meter, static_cast<double>(frames.size() * ac3::kSamplesPerFrame) /
                                    capture.sample_rate());
    }
    fmt::println("");

    capture.stop();
    const auto stats = capture.stats();
    // record is always plain AC-3 stereo (see the deinterleave above, which
    // only ever fills `planar`'s two channels) - the same track shape 'mkv'
    // would derive by scanning this file back, just already known here.
    const matroska::AudioTrack track{.codec_id = std::string{matroska::kCodecAc3},
                                     .sample_rate = capture.sample_rate(),
                                     .channels = 2,
                                     .samples_per_frame = ac3::kSamplesPerFrame};
    if (!write_frames_or_mux(out_path, meta.matroska_container, track, frames)) {
        return 1;
    }
    fmt::println("wrote {} frames ({} kbps) to {}{}", frames.size(), bitrate, out_path,
                 meta.matroska_container ? " (Matroska)" : "");
    fmt::println("captured {} frames, {} silence-filled, {} dropped", stats.frames_captured,
                 stats.frames_silence_filled, stats.frames_dropped);
    print_channel_summary(meter);
    return 0;
}

int run_outputs() {
    const auto devices = ac3::audio::enumerate_render_devices();
    if (!devices) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return 1;
    }
    if (devices->empty()) {
        fmt::println("no active render endpoints found");
        return 0;
    }
    fmt::println("{:>3}  {:<9}  {:<9}  {:<9}  {}", "idx", "AC-3", "E-AC-3", "excl PCM", "name");
    for (std::size_t i = 0; i < devices->size(); ++i) {
        const auto& d = (*devices)[i];
        fmt::println("{:>3}  {:<9}  {:<9}  {:<9}  {}{}", i, d.supports_ac3_passthrough ? "yes" : "no",
                     d.supports_eac3_passthrough ? "yes" : "no",
                     d.supports_exclusive_pcm ? "yes" : "no", d.name,
                     d.is_default ? "  [default]" : "");
    }
    fmt::println("");
    fmt::println("AC-3     the endpoint accepted an IEC 61937 AC-3 format in exclusive mode.");
    fmt::println("E-AC-3   the same, for Dolby Digital Plus (and Atmos riding inside it - there");
    fmt::println("         is no separate passthrough format for Atmos).");
    fmt::println("excl PCM the same endpoint accepted ordinary 16-bit stereo PCM exclusively.");
    fmt::println("");
    fmt::println("PCM yes + AC-3/E-AC-3 no means the device simply cannot bitstream - analog");
    fmt::println("outputs cannot; only S/PDIF (TOSLINK/coax) and HDMI can. Enable Dolby Digital");
    fmt::println("under Sound > Playback > Properties > Supported Formats for such a device.");
    fmt::println("All no means exclusive mode itself is unavailable (disabled for the device,");
    fmt::println("or another application currently holds it).");
    return 0;
}

int run_play(std::string_view in_path, int device_index) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        fmt::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        fmt::println(stderr, "error: {} is too short to hold a syncframe", in_path);
        return 1;
    }
    const bool eac3 = *bsid > 8;

    std::vector<std::span<const std::byte>> units;
    std::uint32_t content_rate = 0;
    if (eac3) {
        const auto split = ac3::split_access_units(stream);
        if (!split || split->empty()) {
            fmt::println(stderr, "error: {} is not a valid E-AC-3 stream", in_path);
            return 1;
        }
        units = *split;
        content_rate =
            sample_rate_hz(static_cast<ac3::SampleRate>(
                std::to_integer<std::uint32_t>(units[0][4]) >> 6));
    } else {
        const auto split = ac3::split_frames(stream);
        if (!split || split->empty()) {
            fmt::println(stderr, "error: {} is not a valid AC-3 stream", in_path);
            return 1;
        }
        units = *split;
        content_rate =
            sample_rate_hz(static_cast<ac3::SampleRate>(
                std::to_integer<std::uint32_t>(units[0][4]) >> 6));
    }

    const auto devices = ac3::audio::enumerate_render_devices(content_rate);
    // Enumeration failing and enumeration finding nothing are different
    // answers: the first is the backend saying it could not look, the second
    // is it looking and seeing no endpoints. Reporting both as "none
    // available" sent people hunting for a missing sound device when the real
    // answer was a COM failure.
    if (!devices) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return 1;
    }
    if (devices->empty()) {
        fmt::println(stderr, "error: no render endpoints available");
        return 1;
    }
    std::string device_id;
    std::string device_name = "default endpoint";
    if (device_index >= 0) {
        if (static_cast<std::size_t>(device_index) >= devices->size()) {
            fmt::println(stderr, "error: device index {} out of range (see 'ac3cli outputs')",
                         device_index);
            return 1;
        }
        const auto& chosen = (*devices)[static_cast<std::size_t>(device_index)];
        device_id = chosen.id;
        device_name = chosen.name;
        const bool supported =
            eac3 ? chosen.supports_eac3_passthrough : chosen.supports_ac3_passthrough;
        if (!supported) {
            fmt::println(stderr,
                         "error: \"{}\" does not accept {} over IEC 61937 (see 'ac3cli outputs')",
                         chosen.name, eac3 ? "E-AC-3" : "AC-3");
            return 1;
        }
    }

    ac3::audio::PassthroughSink sink;
    const auto started = sink.start(
        device_id, content_rate,
        eac3 ? ac3::audio::BitstreamFormat::kEac3 : ac3::audio::BitstreamFormat::kAc3);
    if (!started) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(started.error()));
        return 1;
    }
    fmt::println("streaming {} {} to \"{}\" ({} Hz{})…", units.size(),
                 eac3 ? "access units" : "frames", device_name, content_rate,
                 eac3 ? ", carrier 4x that" : " carrier");

    ac3::iec61937::Eac3BurstPacker eac3_packer;
    for (const auto& unit : units) {
        std::vector<std::byte> burst;
        if (eac3) {
            auto result = eac3_packer.push(unit);
            if (!result) {
                fmt::println(stderr, "error: burst wrap failed");
                return 1;
            }
            if (!*result) {
                continue;  // accumulating; nothing to submit yet
            }
            burst = std::move(**result);
        } else {
            const auto wrapped = ac3::iec61937::wrap_frame(unit);
            if (!wrapped) {
                fmt::println(stderr, "error: burst wrap failed");
                return 1;
            }
            burst = *wrapped;
        }
        // Wait for room rather than racing ahead: the render thread consumes
        // in real time, one burst per burst period.
        while (!sink.submit(burst)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
    // Let the queue drain before tearing the endpoint down.
    while (sink.stats().bursts_rendered < sink.stats().bursts_submitted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stats = sink.stats();
    sink.stop();
    fmt::println("submitted {} bursts, rendered {}, {} underruns", stats.bursts_submitted,
                 stats.bursts_rendered, stats.underruns);
    return 0;
}

}  // namespace ac3cli::commands
