#include "audio_io.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fmt/base.h>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/audio/capture.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/sink_capabilities.hpp"
#include "ac3/audio/watchdog.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "live_audio.hpp"
#include "recording_sink.hpp"
#include "stream_tools.hpp"

namespace ac3cli::commands {

namespace plan = ac3::plan;

int run_devices() {
    const auto devices = ac3::audio::enumerate_devices();
    if (!devices) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return kExitUnavailable;
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
    const auto status = status_stream(out_path);
    status_println(status, "");
    status_println(status, "capture is bitstreaming {}, not PCM: recording the elementary stream",
                   eac3 ? "Dolby Digital Plus (data type 0x15)" : "Dolby Digital (data type 0x01)");
    if (meta.container != RecordingSink::Container::kElementary) {
        // Said rather than silently ignored: mkv/ts/spdif/fmp4 all need the
        // frame boundaries RecordingSink works from, and this path never has
        // them - it has a byte stream nothing here re-parsed. 'mkv'/'ts'/
        // 'spdif'/'fmp4' turn the result into a container in one further
        // step.
        std::string_view name;
        switch (meta.container) {
            case RecordingSink::Container::kMatroska: name = "mkv"; break;
            case RecordingSink::Container::kMpegts: name = "ts"; break;
            case RecordingSink::Container::kSpdif: name = "spdif"; break;
            case RecordingSink::Container::kFmp4: name = "fmp4"; break;
            case RecordingSink::Container::kElementary: break;
        }
        status_println(status,
                       "container={} does not apply to a passthrough capture: writing the bare",
                       name);
        status_println(status,
                       "elementary stream, which 'ac3cli {}' will wrap if you want a container.",
                       name);
    }

    EncodedStreamSink sink;
    if (!sink.open(out_path, meta.keep_partial)) {
        return kExitOutput;
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
        return kExitInput;
    }
    detector.clear_buffer();

    // The carrier's own clock, not the content's: an E-AC-3 burst period
    // spans 6144 sample frames at the 4x rate, an AC-3 one 1536 at 1x, and
    // both come to the same 32 ms of programme. Watched by the same
    // SilenceWatchdog run_record's own PCM path uses, so a bitstreaming
    // device that vanishes mid-take is caught here too.
    const auto rate = capture.sample_rate();
    const std::uint64_t target_frames = static_cast<std::uint64_t>(seconds) * rate;
    std::uint64_t captured = 0;
    std::vector<float> interleaved(static_cast<std::size_t>(ac3::kSamplesPerFrame) * channels);
    std::vector<std::byte> carrier;
    ac3::audio::SilenceWatchdog watchdog{meta.watchdog};
    watchdog.reset(std::chrono::steady_clock::now());
    const bool watching = meta.watchdog.count() > 0;
    bool device_lost = false;
    while (captured < target_frames && !device_lost) {
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
        captured += static_cast<std::uint64_t>(ac3::kSamplesPerFrame);
        carrier.clear();
        ac3::iec61937::carrier_from_capture(interleaved, channels, carrier);
        if (!drain(carrier)) {
            sink.abort();
            return kExitInput;
        }
        if (!quiet_mode()) {
            fmt::print("\r  {} burst{} captured ({:.1f} s)  ", reader.bursts(),
                       reader.bursts() == 1 ? "" : "s",
                       static_cast<double>(captured) / static_cast<double>(rate));
        }
    }
    status_println(status);

    capture.stop();
    if (reader.bursts() == 0 && !device_lost) {
        sink.abort();
        fmt::println(stderr, "error: the bursts stopped before a whole one was captured");
        return kExitInput;
    }
    const bool closed = sink.close();
    if (!closed && !device_lost) {
        return kExitOutput;
    }
    if (device_lost) {
        fmt::println(stderr,
                     "error: capture stopped delivering audio for {} ms; the take was stopped "
                     "and what had already been written is kept (watchdog=0 disables this)",
                     meta.watchdog.count());
        return kExitRuntime;
    }
    const auto stats = capture.stats();
    status_println(status, "wrote {} {} bursts ({} bytes) to {}", reader.bursts(),
                   eac3 ? "E-AC-3" : "AC-3", elementary_bytes, out_path);
    status_println(status, "captured {} frames, {} silence-filled, {} dropped",
                   stats.frames_captured, stats.frames_silence_filled, stats.frames_dropped);
    if (reader.skipped_bursts() > 0 || reader.false_syncs() > 0) {
        status_println(status,
                       "{} burst(s) of another data type skipped, {} false sync(s) resynced past",
                       reader.skipped_bursts(), reader.false_syncs());
    }
    status_println(status, "no re-encode happened: this is what the source sent, byte for byte.");
    return kExitOk;
}

}  // namespace

int run_record(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
               int device_index, const Options& meta) {
    const auto devices = ac3::audio::enumerate_devices();
    if (!devices) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return kExitUnavailable;
    }
    if (devices->empty()) {
        fmt::println(stderr, "error: no capture endpoints available");
        return kExitUnavailable;
    }
    if (device_index < 0 || static_cast<std::size_t>(device_index) >= devices->size()) {
        fmt::println(stderr, "error: device index {} out of range (see 'ac3cli devices')",
                     device_index);
        return kExitUsage;
    }
    const auto& device = (*devices)[static_cast<std::size_t>(device_index)];

    ac3::SampleRate sr{};
    bool encodable_rate = true;
    switch (device.sample_rate) {
        case 48000: sr = ac3::SampleRate::k48000; break;
        case 44100: sr = ac3::SampleRate::k44100; break;
        case 32000: sr = ac3::SampleRate::k32000; break;
        // Not an error on its own: a bitstreaming endpoint routinely runs at a
        // rate AC-3 cannot encode at - 192 kHz is exactly the E-AC-3 carrier's
        // 4x - so the rate gate below is the PCM path's own, applied only
        // once detection has ruled a bitstream out.
        default: encodable_rate = false; break;
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
        fmt::println(stderr, "error: {}", ac3::audio::describe(started.error()));
        return kExitUnavailable;
    }
    const auto channels = capture.channels();
    const auto rate_hz = capture.sample_rate();

    // A device that vanishes (unplugged, disabled, torn down under us) reads
    // as an endless run of zero-byte reads, which a plain "sleep 2ms on
    // got==0" loop cannot tell from "briefly starved" - so a recording sat
    // there looking healthy with nothing coming in. Same class, same 3 s
    // default and the same "stop the session the first time it fires" rule
    // as the GUI's live session; watchdog=0 turns it off. Shared by every
    // capture.buffer()->read() loop below, including the bitstream probe.
    ac3::audio::SilenceWatchdog watchdog{meta.watchdog};
    watchdog.reset(std::chrono::steady_clock::now());
    const bool watching = meta.watchdog.count() > 0;
    bool device_lost = false;
    const auto read_frame = [&](std::span<float> interleaved) {
        std::size_t filled = 0;
        while (filled < interleaved.size()) {
            const auto got = capture.buffer()->read(interleaved.subspan(filled));
            filled += got;
            const auto read_at = std::chrono::steady_clock::now();
            watchdog.on_read(got, read_at);
            if (got == 0) {
                if (watching && watchdog.timed_out(read_at)) {
                    device_lost = true;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    };

    // Bitstream passthrough auto-detection (roadmap IO3): an endpoint fed
    // IEC 61937 hands its bursts over as ordinary PCM - the capture API has
    // no way to say "this is Dolby Digital" - so encoding them at face value
    // produces noise. A device whose advertised rate AC-3 cannot encode at
    // (encodable_rate false - typically an E-AC-3 carrier's 192 kHz 4x rate)
    // can only be this or an unusable device, so detection is mandatory
    // there; an encodable-rate device still gets a detection window, since
    // an AC-3 carrier rides at an ordinary 1x rate indistinguishable from
    // real PCM until the header bytes are parsed - see the encode loop below
    // for how that briefer, opportunistic check works.
    if (!encodable_rate) {
        ac3::iec61937::PassthroughDetector detector;
        std::vector<float> probe(static_cast<std::size_t>(ac3::kSamplesPerFrame) * channels);
        while (!detector.decided() && !device_lost) {
            read_frame(probe);
            if (device_lost) {
                break;
            }
            detector.push(probe, channels);
        }
        if (device_lost) {
            capture.stop();
            fmt::println(stderr,
                         "error: capture stopped delivering audio for {} ms before its format "
                         "could be determined (watchdog=0 disables this)",
                         meta.watchdog.count());
            return kExitRuntime;
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
        return kExitUnavailable;
    }

    // The captured channels are placed onto the take's coded channels by
    // DIRECTION, not by index - a two-channel microphone recorded onto 5.1
    // fills L/R and leaves the rest silent, exactly as a two-channel WAV
    // encoded onto 5.1 does (plan::route's own model). A source wider than the
    // target folds down per 7.8.
    const auto routing = plan::route(channel_plan, channels, meta.p.cmixlev, meta.p.surmixlev);
    if (!routing) {
        fmt::println(stderr, "error: {} capture channels - {}", channels,
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
    // crash an hour in no longer loses the hour. Opening is DEFERRED, though
    // (see `pending` below): an encodable-rate device still gets a brief,
    // opportunistic bitstream check, and until that decides, this is not yet
    // known to be real PCM worth committing to disk.
    RecordingSink sink;
    bool sink_open = false;
    const auto open_sink = [&] {
        if (const auto why = sink.open(std::string{out_path}, take_sink_config(meta, *take, rate_hz));
            !why.empty()) {
            fmt::println(stderr, "error: {}: {}", out_path, why);
            return false;
        }
        sink_open = true;
        return true;
    };

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

    // Runs alongside the encode for the first quarter-second or so, then
    // costs nothing at all (PassthroughDetector::decided() latches true).
    // Encoding continues meanwhile rather than the session pausing to listen
    // first - an ordinary microphone, which is what this almost always is,
    // must not lose its opening - but nothing reaches the sink until decided:
    // `pending` holds the handful of units encoded during that window, which
    // either get discarded (a bitstream after all - they were noise) or
    // flushed into the sink once opened (see below). Bounded to a fraction of
    // a second's worth of frames, not the whole session, so IO9's
    // bounded-memory property still holds for everything after this window.
    ac3::iec61937::PassthroughDetector detector;
    std::vector<std::vector<std::byte>> pending;
    std::uint64_t frames_written = 0;

    while (frames_written < target_frames && !device_lost) {
        // Block until a whole frame of interleaved samples is available.
        read_frame(interleaved);
        if (device_lost) {
            break;
        }
        if (!detector.decided()) {
            detector.push(interleaved, channels);
            if (detector.detected()) {
                // Everything encoded so far (`pending`) was burst data read
                // as audio - discard it and record what the source is
                // actually sending. Nothing was ever written to `sink`,
                // since it is only opened once decided() rules this out.
                return record_passthrough(out_path, seconds, capture, detector, meta);
            }
            if (detector.decided() && !sink_open && !open_sink()) {
                return kExitOutput;
            }
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
                fmt::println(stderr, "error: the encoder cannot express this configuration");
                if (sink_open) {
                    std::ignore = sink.close();
                }
                return kExitUsage;
            }
            unit_bytes = unit->bytes;
        } else {
            auto frame = ac3_encoder->encode_frame(views);
            if (!frame) {
                fmt::println(stderr, "error: bitrate must be a legal AC-3 rate");
                if (sink_open) {
                    std::ignore = sink.close();
                }
                return kExitUsage;
            }
            unit_bytes = std::move(*frame);
        }
        if (sink_open) {
            if (const auto why = sink.push(unit_bytes); !why.empty()) {
                fmt::println(stderr, "error: {}: {}", out_path, why);
                return kExitOutput;
            }
        } else {
            pending.push_back(std::move(unit_bytes));
        }
        ++frames_written;
        // One frame is 32 ms at 48 kHz, so the meter redraws about 30 times a
        // second without any throttling of its own.
        print_live_meter(meter, static_cast<double>(frames_written * ac3::kSamplesPerFrame) /
                                    rate_hz);
    }
    status_println(status);

    // The detector never decided within the whole take (a session shorter
    // than its own detection window) - open now and flush whatever is
    // pending, exactly as the mid-loop path does once decided() goes true.
    if (!sink_open && !pending.empty() && !open_sink()) {
        return kExitOutput;
    }
    if (sink_open) {
        for (auto& unit_bytes : pending) {
            if (const auto why = sink.push(unit_bytes); !why.empty()) {
                fmt::println(stderr, "error: {}: {}", out_path, why);
                return kExitOutput;
            }
        }
    }

    capture.stop();
    const auto stats = capture.stats();
    // Finalized whether or not the device dropped: everything already pushed
    // is on disk and playable, which is the whole reason a take streams. A
    // close() complaint is reported, but a lost device is the more useful
    // diagnosis of the two and wins the exit code - a take that captured
    // nothing before the device vanished ends as a device failure, not as a
    // disk one.
    const auto close_problem = sink_open ? sink.close() : std::string{};
    if (!close_problem.empty() && !device_lost) {
        fmt::println(stderr, "error: {}: {}", out_path, close_problem);
        return kExitOutput;
    }
    if (device_lost) {
        fmt::println(stderr,
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
        fmt::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return kExitUnavailable;
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

namespace {

// Reads `path` as a bare elementary stream and splits it into passthrough
// units - the one thing both the original file and the AC-3 fallback's own
// temp file need done to them the same way, so 'play' does not carry two
// slightly different copies of this. Owns `bytes` itself (rather than the
// caller keeping a separate buffer alive) since `units` is only ever spans
// into it - the fallback path in particular has nowhere else convenient to
// keep the temp file's bytes alive for the duration of the play loop.
struct SplitStream {
    std::vector<std::byte> bytes;
    std::vector<std::span<const std::byte>> units;
    std::uint32_t content_rate = 0;
};

[[nodiscard]] std::optional<SplitStream> split_playable_stream(std::string_view path, bool eac3) {
    SplitStream result;
    result.bytes = read_elementary_stream(path);
    if (result.bytes.empty()) {
        return std::nullopt;
    }
    if (eac3) {
        const auto split = ac3::split_access_units(result.bytes);
        if (!split || split->empty()) {
            fmt::println(stderr, "error: {} is not a valid E-AC-3 stream", path);
            return std::nullopt;
        }
        result.units = *split;
    } else {
        const auto split = ac3::split_frames(result.bytes);
        if (!split || split->empty()) {
            fmt::println(stderr, "error: {} is not a valid AC-3 stream", path);
            return std::nullopt;
        }
        result.units = *split;
    }
    result.content_rate = sample_rate_hz(
        static_cast<ac3::SampleRate>(std::to_integer<std::uint32_t>(result.units[0][4]) >> 6));
    return result;
}

// Wraps `units` into IEC 61937 bursts and feeds them to `sink`, already
// started - the tail every 'play' path shares: native passthrough, and
// roadmap UX9's AC-3 transcode fallback.
int submit_units_to_sink(ac3::audio::PassthroughSink& sink,
                         std::span<const std::span<const std::byte>> units, bool eac3) {
    ac3::iec61937::Eac3BurstPacker eac3_packer;
    for (const auto& unit : units) {
        std::vector<std::byte> burst;
        if (eac3) {
            auto result = eac3_packer.push(unit);
            if (!result) {
                fmt::println(stderr, "error: burst wrap failed");
                return kExitRuntime;
            }
            if (!*result) {
                continue;  // accumulating; nothing to submit yet
            }
            burst = std::move(**result);
        } else {
            const auto wrapped = ac3::iec61937::wrap_frame(unit);
            if (!wrapped) {
                fmt::println(stderr, "error: burst wrap failed");
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
    return kExitOk;
}

// A path unique across concurrent processes and repeated calls within one -
// same technique src/ac3adm/src/adm.cpp's make_temp_path() uses (a
// high-resolution clock reading XORed with a random_device draw and an
// in-process counter), duplicated locally rather than shared across modules
// for one temp file each.
[[nodiscard]] std::filesystem::path make_temp_ac3_path() {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device rd;
    const auto unique =
        (static_cast<std::uint64_t>(rd()) << 32) ^
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
        counter.fetch_add(1, std::memory_order_relaxed);
    return std::filesystem::temp_directory_path() / ("ac3play_" + std::to_string(unique) + ".ac3");
}

// Roadmap UX9's transcode-to-passthrough leg: DC9's transcode produces an
// AC-3 file the sink already confirmed it accepts, then that file plays
// exactly the way a plain AC-3 source file already does - the two commands
// this was "two commands and knowing why" before, run back to back with the
// middle file held in a temp path instead of one the operator has to name
// and clean up themselves. 448 kbps matches the project's own transcode
// examples throughout docs/cli/commands.md; the metadata options 'play's own
// caller gave (dialnorm=, drc=, ...) still apply, carried through `meta`
// exactly as they would to a direct 'transcode' invocation.
int play_via_ac3_transcode(std::string_view in_path, const std::string& device_id,
                           std::string_view device_name, const Options& meta) {
    const auto temp_path = make_temp_ac3_path();
    const auto temp_path_str = temp_path.string();
    const auto transcoded = run_transcode(in_path, temp_path_str, 448, "", meta);
    if (transcoded != 0) {
        std::error_code ec;
        std::filesystem::remove(temp_path, ec);
        return kExitRuntime;
    }

    const auto split = split_playable_stream(temp_path_str, /*eac3=*/false);
    if (!split) {
        std::error_code ec;
        std::filesystem::remove(temp_path, ec);
        return kExitRuntime;
    }

    ac3::audio::PassthroughSink sink;
    const auto started =
        sink.start(device_id, split->content_rate, ac3::audio::BitstreamFormat::kAc3);
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
    if (!started) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(started.error()));
        return kExitUnavailable;
    }
    status_println(status_stream(), "streaming {} frames to \"{}\" ({} Hz carrier)…",
                   split->units.size(), device_name, split->content_rate);
    const auto result = submit_units_to_sink(sink, split->units, /*eac3=*/false);
    const auto stats = sink.stats();
    sink.stop();
    status_println(status_stream(), "submitted {} bursts, rendered {}, {} underruns",
                   stats.bursts_submitted, stats.bursts_rendered, stats.underruns);
    return result;
}

}  // namespace

int run_play(std::string_view in_path, int device_index, const Options& meta) {
    const auto stream = read_elementary_stream(in_path);
    if (stream.empty()) {
        return kExitInput;
    }
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        fmt::println(stderr, "error: {} is too short to hold a syncframe", in_path);
        return kExitInput;
    }
    const bool eac3 = *bsid > 8;

    std::vector<std::span<const std::byte>> units;
    std::uint32_t content_rate = 0;
    if (eac3) {
        const auto split = ac3::split_access_units(stream);
        if (!split || split->empty()) {
            fmt::println(stderr, "error: {} is not a valid E-AC-3 stream", in_path);
            return kExitInput;        }
        units = *split;
        content_rate =
            sample_rate_hz(static_cast<ac3::SampleRate>(
                std::to_integer<std::uint32_t>(units[0][4]) >> 6));
    } else {
        const auto split = ac3::split_frames(stream);
        if (!split || split->empty()) {
            fmt::println(stderr, "error: {} is not a valid AC-3 stream", in_path);
            return kExitInput;        }
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
        return kExitUnavailable;
    }
    if (devices->empty()) {
        fmt::println(stderr, "error: no render endpoints available");
        return kExitUnavailable;
    }
    std::string device_id;
    std::string device_name = "default endpoint";
    const ac3::audio::RenderDeviceInfo* chosen = nullptr;
    if (device_index >= 0) {
        if (static_cast<std::size_t>(device_index) >= devices->size()) {
            fmt::println(stderr, "error: device index {} out of range (see 'ac3cli outputs')",
                         device_index);
            return kExitUsage;
        }
        chosen = &(*devices)[static_cast<std::size_t>(device_index)];
        device_id = chosen->id;
        device_name = chosen->name;
    }

    // Roadmap UX9: what the chosen sink actually accepts. The default
    // endpoint (chosen == nullptr) is taken at its word, exactly as before -
    // its capabilities were never probed either, and there is no id to read
    // EDID from. EDID first (real only on ALSA today -
    // sink_capabilities.hpp's own comment says why the others fall back), a
    // live probe otherwise - either way the sink's own answer, not a guess.
    bool takes_native = true;
    bool takes_ac3 = false;
    bool takes_pcm = false;
    if (chosen != nullptr) {
        const auto edid = ac3::audio::read_sink_capabilities(chosen->id);
        if (edid) {
            takes_native = eac3 ? edid->eac3 : edid->ac3;
            takes_ac3 = edid->ac3;
            takes_pcm = edid->pcm;
        } else {
            status_println(status_stream(),
                           "note: could not read \"{}\"'s EDID ({}); using a live probe instead",
                           chosen->name, ac3::audio::describe(edid.error()));
            takes_native =
                eac3 ? chosen->supports_eac3_passthrough : chosen->supports_ac3_passthrough;
            takes_ac3 = chosen->supports_ac3_passthrough;
            takes_pcm = chosen->supports_exclusive_pcm;
        }
    }

    if (chosen != nullptr && !takes_native) {
        const bool ac3_leg = eac3 && takes_ac3 && meta.follow_sink;
        const bool pcm_leg = !ac3_leg && takes_pcm && meta.follow_sink;
        if (ac3_leg) {
            status_println(status_stream(),
                           "\"{}\" does not accept E-AC-3 over IEC 61937; transcoding to AC-3 "
                           "instead (roadmap DC9/UX9)",
                           chosen->name);
            return play_via_ac3_transcode(in_path, device_id, device_name, meta);
        }
        if (pcm_leg) {
            status_println(status_stream(),
                           "\"{}\" does not accept {} over IEC 61937; falling back to decoded "
                           "PCM",
                           chosen->name, eac3 ? "E-AC-3" : "AC-3");
            return run_monitor(in_path, device_index, meta);
        }
        fmt::println(stderr,
                     "error: \"{}\" does not accept {} over IEC 61937 (see 'ac3cli outputs'){}",
                     chosen->name, eac3 ? "E-AC-3" : "AC-3",
                     !meta.follow_sink && (takes_ac3 || takes_pcm)
                         ? " (drop follow=off to let play fall back instead of refusing)"
                         : "");
        return kExitUnavailable;
    }

    ac3::audio::PassthroughSink sink;
    const auto started = sink.start(
        device_id, content_rate,
        eac3 ? ac3::audio::BitstreamFormat::kEac3 : ac3::audio::BitstreamFormat::kAc3);
    if (!started) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(started.error()));
        return kExitUnavailable;
    }
    status_println(status_stream(), "streaming {} {} to \"{}\" ({} Hz{})…", units.size(),
                 eac3 ? "access units" : "frames", device_name, content_rate,
                 eac3 ? ", carrier 4x that" : " carrier");

    const auto result = submit_units_to_sink(sink, units, eac3);
    const auto stats = sink.stats();
    sink.stop();
    status_println(status_stream(), "submitted {} bursts, rendered {}, {} underruns",
                   stats.bursts_submitted, stats.bursts_rendered, stats.underruns);
    return result;
}

}  // namespace ac3cli::commands
