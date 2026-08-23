#include "audio_io.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/audio/capture.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/audio/watchdog.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "recording_sink.hpp"

namespace ac3cli::commands {

namespace plan = ac3::plan;

int run_devices() {
    const auto devices = ac3::audio::enumerate_devices();
    if (!devices) {
        std::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return kExitUnavailable;
    }
    if (devices->empty()) {
        std::println("no active capture endpoints found");
        return 0;
    }
    std::println("{:>3}  {:<9} {:>7}  {:>3}  {}", "idx", "kind", "rate", "ch", "name");
    for (std::size_t i = 0; i < devices->size(); ++i) {
        const auto& d = (*devices)[i];
        std::println("{:>3}  {:<9} {:>7}  {:>3}  {}{}", i,
                     d.kind == ac3::audio::DeviceKind::kInput ? "input" : "loopback",
                     d.sample_rate, d.channels, d.name, d.is_default ? "  [default]" : "");
    }
    return 0;
}

int run_record(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
               int device_index, const Options& meta) {
    const auto devices = ac3::audio::enumerate_devices();
    if (!devices) {
        std::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return kExitUnavailable;
    }
    if (devices->empty()) {
        std::println(stderr, "error: no capture endpoints available");
        return kExitUnavailable;
    }
    if (device_index < 0 || static_cast<std::size_t>(device_index) >= devices->size()) {
        std::println(stderr, "error: device index {} out of range (see 'ac3cli devices')",
                     device_index);
        return kExitUsage;
    }
    const auto& device = (*devices)[static_cast<std::size_t>(device_index)];

    ac3::SampleRate sr{};
    switch (device.sample_rate) {
        case 48000: sr = ac3::SampleRate::k48000; break;
        case 44100: sr = ac3::SampleRate::k44100; break;
        case 32000: sr = ac3::SampleRate::k32000; break;
        default:
            std::println(stderr,
                         "error: device runs at {} Hz; AC-3 needs 32, 44.1 or 48 kHz "
                         "(change the endpoint's shared-mode format in Windows sound settings)",
                         device.sample_rate);
            return kExitUnavailable;
    }

    // layout=/codec= (roadmap IO9). Before this, 'record' was stereo AC-3 and
    // nothing else, while the GUI recorded any layout the format allows - and
    // the two shared a capture path, an encoder and a container writer, so the
    // gap was entirely in what the CLI would let you ask for.
    const auto take = resolve_take_plan(meta, bitrate, sr);
    if (!take) {
        return kExitUsage;
    }
    const auto channel_plan = plan::resolve(take->plan);

    ac3::audio::Capture capture;
    const auto started = capture.start(device.id, device.kind);
    if (!started) {
        std::println(stderr, "error: {}", ac3::audio::describe(started.error()));
        return kExitUnavailable;
    }
    const auto channels = capture.channels();
    const auto rate_hz = capture.sample_rate();

    // The captured channels are placed onto the take's coded channels by
    // DIRECTION, not by index - a two-channel microphone recorded onto 5.1
    // fills L/R and leaves the rest silent, exactly as a two-channel WAV
    // encoded onto 5.1 does (plan::route's own model). A source wider than the
    // target folds down per 7.8.
    const auto routing = plan::route(channel_plan, channels, meta.p.cmixlev, meta.p.surmixlev);
    if (!routing) {
        std::println(stderr, "error: {} capture channels - {}", channels,
                     plan::describe(plan::PlanError::kNoSourceLayout));
        return kExitUsage;
    }

    const auto status = status_stream();
    status_println(status, "recording from \"{}\" ({} Hz, {} ch) to {} {} for {} s...",
                   device.name, rate_hz, channels,
                   plan::codec_label(take->plan.codec), take->label, seconds);

    // Heap-allocated: both encoders carry several KB of MDCT scratch/history
    // state, and this function only constructs them once (PREfast's C6262).
    // Both are declared, only one is built: which, is settled by take->eac3
    // before the loop rather than tested per frame.
    std::unique_ptr<ac3::FrameEncoder> ac3_encoder;
    std::unique_ptr<ac3::eac3::AccessUnitEncoder> eac3_encoder;
    if (take->eac3) {
        eac3_encoder =
            std::make_unique<ac3::eac3::AccessUnitEncoder>(plan::eac3_config(take->plan));
    } else {
        ac3_encoder = std::make_unique<ac3::FrameEncoder>(plan::ac3_config(take->plan));
    }
    // Meters what the encoder is fed, not what the endpoint delivers: a needle
    // that moves on a channel the stream never carries would be a lie. The
    // bed's own acmod/lfe, widened to the coded count where a dependent adds
    // channels past it - the same meter shape the GUI's live session builds.
    ac3::analysis::LevelMeter meter{channel_plan.bed_acmod, channel_plan.bed_lfe, rate_hz,
                                    routing->coded_channels};
    const std::uint64_t target_frames =
        (static_cast<std::uint64_t>(seconds) * rate_hz + ac3::kSamplesPerFrame - 1) /
        ac3::kSamplesPerFrame;

    // Streamed to its container as it is produced (roadmap IO9), through the
    // same RecordingSink the GUI's own takes go through - so a take of any
    // length costs one frame of memory rather than the whole session, and a
    // crash an hour in no longer loses the hour.
    RecordingSink sink;
    if (const auto why = sink.open(std::string{out_path}, take_sink_config(meta, *take, rate_hz));
        !why.empty()) {
        std::println(stderr, "error: {}: {}", out_path, why);
        return kExitOutput;
    }

    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    std::vector<float> interleaved(static_cast<std::size_t>(ac3::kSamplesPerFrame) * channels);
    std::vector<std::vector<float>> source(channels,
                                           std::vector<float>(ac3::kSamplesPerFrame, 0.0F));
    std::vector<std::vector<float>> block(nchans,
                                          std::vector<float>(ac3::kSamplesPerFrame, 0.0F));
    std::vector<std::span<const float>> in(channels);
    std::vector<std::span<float>> out(nchans);
    std::vector<std::span<const float>> views(nchans);
    for (std::size_t c = 0; c < channels; ++c) {
        in[c] = source[c];
    }
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }

    // A device that vanishes (unplugged, disabled, torn down under us) reads
    // as an endless run of zero-byte reads, which the old `while (got == 0)
    // sleep 2ms` loop could not tell from "briefly starved" - so a recording
    // sat there looking healthy with nothing coming in. Same class, same 3 s
    // default and the same "stop the session the first time it fires" rule as
    // the GUI's live session; watchdog=0 turns it off.
    ac3::audio::SilenceWatchdog watchdog{meta.watchdog};
    watchdog.reset(std::chrono::steady_clock::now());
    const bool watching = meta.watchdog.count() > 0;
    bool device_lost = false;
    std::uint64_t frames_written = 0;

    while (frames_written < target_frames && !device_lost) {
        // Block until a whole frame of interleaved samples is available.
        std::size_t filled = 0;
        while (filled < interleaved.size()) {
            const auto got = capture.buffer()->read(
                std::span{interleaved}.subspan(filled, interleaved.size() - filled));
            filled += got;
            const auto read_at = std::chrono::steady_clock::now();
            watchdog.on_read(got, read_at);
            if (got == 0) {
                if (watching && watchdog.timed_out(read_at)) {
                    device_lost = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        if (device_lost) {
            break;
        }
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const std::size_t base = static_cast<std::size_t>(i) * channels;
            for (std::size_t ch = 0; ch < channels; ++ch) {
                source[ch][static_cast<std::size_t>(i)] = interleaved[base + ch];
            }
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
        meter.process(views);

        std::vector<std::byte> unit_bytes;
        if (take->eac3) {
            const auto unit = eac3_encoder->encode_access_unit(views);
            if (!unit) {
                std::println(stderr, "error: the encoder cannot express this configuration");
                std::ignore = sink.close();
                return kExitUsage;
            }
            unit_bytes = unit->bytes;
        } else {
            auto frame = ac3_encoder->encode_frame(views);
            if (!frame) {
                std::println(stderr, "error: bitrate must be a legal AC-3 rate");
                std::ignore = sink.close();
                return kExitUsage;
            }
            unit_bytes = std::move(*frame);
        }
        if (const auto why = sink.push(unit_bytes); !why.empty()) {
            std::println(stderr, "error: {}: {}", out_path, why);
            std::ignore = sink.close();
            return kExitOutput;
        }
        ++frames_written;
        // One frame is 32 ms at 48 kHz, so the meter redraws about 30 times a
        // second without any throttling of its own.
        print_live_meter(meter, static_cast<double>(frames_written * ac3::kSamplesPerFrame) /
                                    rate_hz);
    }
    status_println(status);

    capture.stop();
    const auto stats = capture.stats();
    // Finalized whether or not the device dropped: everything already pushed
    // is on disk and playable, which is the whole reason a take streams. A
    // close() complaint is reported, but a lost device is the more useful
    // diagnosis of the two and wins the exit code - a take that captured
    // nothing before the device vanished ends as a device failure, not as a
    // disk one.
    const auto close_problem = sink.close();
    if (!close_problem.empty() && !device_lost) {
        std::println(stderr, "error: {}: {}", out_path, close_problem);
        return kExitOutput;
    }
    if (device_lost) {
        std::println(stderr,
                     "error: \"{}\" stopped delivering audio for {} ms; the take was stopped and "
                     "what had already been written is kept (watchdog=0 disables this){}",
                     device.name, meta.watchdog.count(),
                     close_problem.empty() ? "" : " - " + close_problem);
        return kExitRuntime;
    }
    status_println(status, "wrote {} {} ({} kbps, {}) to {}{}", frames_written,
                   take->eac3 ? "access units" : "frames", bitrate, take->label, out_path,
                   container_note(meta.container));
    status_println(status, "captured {} frames, {} silence-filled, {} dropped",
                   stats.frames_captured, stats.frames_silence_filled, stats.frames_dropped);
    print_channel_summary(meter, status);
    return kExitOk;
}

int run_outputs() {
    const auto devices = ac3::audio::enumerate_render_devices();
    if (!devices) {
        std::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return kExitUnavailable;
    }
    if (devices->empty()) {
        std::println("no active render endpoints found");
        return 0;
    }
    std::println("{:>3}  {:<9}  {:<9}  {:<9}  {}", "idx", "AC-3", "E-AC-3", "excl PCM", "name");
    for (std::size_t i = 0; i < devices->size(); ++i) {
        const auto& d = (*devices)[i];
        std::println("{:>3}  {:<9}  {:<9}  {:<9}  {}{}", i, d.supports_ac3_passthrough ? "yes" : "no",
                     d.supports_eac3_passthrough ? "yes" : "no",
                     d.supports_exclusive_pcm ? "yes" : "no", d.name,
                     d.is_default ? "  [default]" : "");
    }
    std::println("");
    std::println("AC-3     the endpoint accepted an IEC 61937 AC-3 format in exclusive mode.");
    std::println("E-AC-3   the same, for Dolby Digital Plus (and Atmos riding inside it - there");
    std::println("         is no separate passthrough format for Atmos).");
    std::println("excl PCM the same endpoint accepted ordinary 16-bit stereo PCM exclusively.");
    std::println("");
    std::println("PCM yes + AC-3/E-AC-3 no means the device simply cannot bitstream - analog");
    std::println("outputs cannot; only S/PDIF (TOSLINK/coax) and HDMI can. Enable Dolby Digital");
    std::println("under Sound > Playback > Properties > Supported Formats for such a device.");
    std::println("All no means exclusive mode itself is unavailable (disabled for the device,");
    std::println("or another application currently holds it).");
    return 0;
}

int run_play(std::string_view in_path, int device_index) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        std::println(stderr, "error: cannot read {}", in_path);
        return kExitInput;
    }
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        std::println(stderr, "error: {} is too short to hold a syncframe", in_path);
        return kExitInput;
    }
    const bool eac3 = *bsid > 8;

    std::vector<std::span<const std::byte>> units;
    std::uint32_t content_rate = 0;
    if (eac3) {
        const auto split = ac3::split_access_units(stream);
        if (!split || split->empty()) {
            std::println(stderr, "error: {} is not a valid E-AC-3 stream", in_path);
            return kExitInput;
        }
        units = *split;
        content_rate =
            sample_rate_hz(static_cast<ac3::SampleRate>(
                std::to_integer<std::uint32_t>(units[0][4]) >> 6));
    } else {
        const auto split = ac3::split_frames(stream);
        if (!split || split->empty()) {
            std::println(stderr, "error: {} is not a valid AC-3 stream", in_path);
            return kExitInput;
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
        std::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return kExitUnavailable;
    }
    if (devices->empty()) {
        std::println(stderr, "error: no render endpoints available");
        return kExitUnavailable;
    }
    std::string device_id;
    std::string device_name = "default endpoint";
    if (device_index >= 0) {
        if (static_cast<std::size_t>(device_index) >= devices->size()) {
            std::println(stderr, "error: device index {} out of range (see 'ac3cli outputs')",
                         device_index);
            return kExitUsage;
        }
        const auto& chosen = (*devices)[static_cast<std::size_t>(device_index)];
        device_id = chosen.id;
        device_name = chosen.name;
        const bool supported =
            eac3 ? chosen.supports_eac3_passthrough : chosen.supports_ac3_passthrough;
        if (!supported) {
            std::println(stderr,
                         "error: \"{}\" does not accept {} over IEC 61937 (see 'ac3cli outputs')",
                         chosen.name, eac3 ? "E-AC-3" : "AC-3");
            return kExitUnavailable;
        }
    }

    ac3::audio::PassthroughSink sink;
    const auto started = sink.start(
        device_id, content_rate,
        eac3 ? ac3::audio::BitstreamFormat::kEac3 : ac3::audio::BitstreamFormat::kAc3);
    if (!started) {
        std::println(stderr, "error: {}", ac3::audio::describe(started.error()));
        return kExitUnavailable;
    }
    status_println(status_stream(), "streaming {} {} to \"{}\" ({} Hz{})…", units.size(),
                 eac3 ? "access units" : "frames", device_name, content_rate,
                 eac3 ? ", carrier 4x that" : " carrier");

    ac3::iec61937::Eac3BurstPacker eac3_packer;
    for (const auto& unit : units) {
        std::vector<std::byte> burst;
        if (eac3) {
            auto result = eac3_packer.push(unit);
            if (!result) {
                std::println(stderr, "error: burst wrap failed");
                return kExitRuntime;
            }
            if (!*result) {
                continue;  // accumulating; nothing to submit yet
            }
            burst = std::move(**result);
        } else {
            const auto wrapped = ac3::iec61937::wrap_frame(unit);
            if (!wrapped) {
                std::println(stderr, "error: burst wrap failed");
                return kExitRuntime;
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
    status_println(status_stream(), "submitted {} bursts, rendered {}, {} underruns", stats.bursts_submitted,
                 stats.bursts_rendered, stats.underruns);
    return 0;
}

}  // namespace ac3cli::commands
