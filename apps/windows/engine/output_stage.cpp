#include "output_stage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <thread>
#include <utility>

#include "ac3/audio/monitor.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/spatial.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/oba/oamd.hpp"

namespace ac3::windemo {

namespace {

// WAVEFORMATEXTENSIBLE speaker masks, spelled as the SPEAKER_* bits so this
// file needs no ksmedia.h (src/audio's Windows backend does the same).
constexpr std::uint32_t kMaskStereo = 0x3;
constexpr std::uint32_t kMask51 = 0x3f;
constexpr std::uint32_t kSpeakerLowFrequency = 0x8;

// How long submit() is prepared to wait for a full sink before giving the
// frame up: a sink that has died must not hold the encode loop forever.
constexpr auto kSubmitPatience = std::chrono::milliseconds(200);

std::string lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// TS 103 420 §4.2.1's room cube to ISpatialAudioObject's listener-relative
// metres: +x right, +y up, +z behind. Same mapping as ac3cli spatial.
struct SpatialXyz {
    float x, y, z;
};

SpatialXyz to_windows_spatial(const ac3::oba::Position& p) {
    constexpr float kHalfWidthM = 2.0F;
    constexpr float kHalfDepthM = 2.0F;
    constexpr float kHeightM = 1.0F;
    return {.x = (static_cast<float>(p.x) - 0.5F) * kHalfWidthM,
            .y = static_cast<float>(p.z) * kHeightM,
            .z = (static_cast<float>(p.y) - 0.5F) * kHalfDepthM};
}

template <typename Sink, typename... Args>
bool submit_with_patience(Sink& sink, std::uint64_t& underruns, Args&&... args) {
    const auto deadline = std::chrono::steady_clock::now() + kSubmitPatience;
    while (!sink.submit(std::forward<Args>(args)...)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            ++underruns;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

}  // namespace

struct OutputStage::Impl {
    // Exactly one of these is live at a time, per mode.
    std::unique_ptr<ac3::audio::PassthroughSink> passthrough;
    std::unique_ptr<ac3::audio::MonitorSink> monitor;
    std::unique_ptr<ac3::audio::SpatialObjectSink> spatial;
    bool spatial_started = false;

    std::unique_ptr<ac3::iec61937::Eac3BurstPacker> packer;  // Atmos / DD+
    std::unique_ptr<ac3::FrameEncoder> ac3_encoder;           // DD 5.1
    std::unique_ptr<ac3::Eac3Decoder> decoder;                // the decoded modes

    std::vector<float> interleaved;
    std::vector<ac3::audio::DynamicObjectUpdate> dynamic_updates;
    std::vector<ac3::audio::StaticObjectUpdate> static_updates;

    void teardown() {
        if (passthrough) {
            passthrough->stop();
        }
        if (monitor) {
            monitor->stop();
        }
        if (spatial) {
            spatial->stop();
        }
        passthrough.reset();
        monitor.reset();
        spatial.reset();
        spatial_started = false;
        packer.reset();
        ac3_encoder.reset();
        decoder.reset();
    }
};

OutputStage::OutputStage(OutputStageConfig config)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)) {}

OutputStage::~OutputStage() {
    stop();
}

void OutputStage::set_pinned(std::optional<OutputMode> pinned) {
    config_.pinned = pinned;
}

void OutputStage::set_null_sink_substring(std::string substring) {
    config_.null_sink_substring = std::move(substring);
}

void OutputStage::stop() {
    impl_->teardown();
    status_.running = false;
}

const OutputStatus& OutputStage::reprobe(bool signing_key_loaded) {
    std::vector<EndpointFacts> facts;
    const auto devices = ac3::audio::enumerate_render_devices(config_.sample_rate);
    if (devices) {
        const std::string needle = lower(config_.null_sink_substring);
        for (const auto& device : *devices) {
            EndpointFacts f{.id = device.id,
                            .name = device.name,
                            .is_default = device.is_default,
                            .is_null_sink = !needle.empty() &&
                                            lower(device.name).find(needle) != std::string::npos,
                            .accepts_eac3 = device.supports_eac3_passthrough,
                            .accepts_ac3 = device.supports_ac3_passthrough,
                            .shared_channels = device.channels};
            if (const auto spatial = ac3::audio::probe_spatial_capability(device.id);
                spatial && spatial->available) {
                f.spatial = true;
                f.spatial_max_objects = spatial->max_dynamic_objects;
            }
            facts.push_back(std::move(f));
        }
    }

    const auto choice = choose_output(
        {.endpoints = facts, .signing_key_loaded = signing_key_loaded, .pinned = config_.pinned});
    status_.endpoints = std::move(facts);
    status_.reason = choice.reason;

    const bool unchanged = status_.running && choice.mode == status_.mode &&
                           choice.endpoint_id == status_.endpoint_id;
    if (unchanged) {
        return status_;
    }

    impl_->teardown();
    status_.running = false;
    status_.mode = choice.mode;
    status_.endpoint_id = choice.endpoint_id;
    status_.endpoint_name = choice.endpoint_name;

    const auto refuse = [&](std::string why) -> const OutputStatus& {
        impl_->teardown();
        status_.reason += "; could not start: " + why;
        status_.mode = OutputMode::kNone;
        return status_;
    };

    switch (choice.mode) {
        case OutputMode::kAtmos:
        case OutputMode::kDdPlus51: {
            impl_->passthrough = std::make_unique<ac3::audio::PassthroughSink>();
            const auto started = impl_->passthrough->start(choice.endpoint_id, config_.sample_rate,
                                                           ac3::audio::BitstreamFormat::kEac3);
            if (!started) {
                return refuse(std::string(ac3::audio::describe(started.error())));
            }
            impl_->packer = std::make_unique<ac3::iec61937::Eac3BurstPacker>();
            break;
        }
        case OutputMode::kDd51: {
            impl_->passthrough = std::make_unique<ac3::audio::PassthroughSink>();
            const auto started = impl_->passthrough->start(choice.endpoint_id, config_.sample_rate,
                                                           ac3::audio::BitstreamFormat::kAc3);
            if (!started) {
                return refuse(std::string(ac3::audio::describe(started.error())));
            }
            impl_->ac3_encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
                .sample_rate = ac3::SampleRate::k48000,
                .bitrate_kbps = ac3::clamp_to_legal_ac3_bitrate(config_.ac3_bitrate_kbps),
                .dialnorm = 31,
                .acmod = ac3::Acmod::k3_2,
                .lfe = true});
            break;
        }
        case OutputMode::kPcmSurround: {
            impl_->monitor = std::make_unique<ac3::audio::MonitorSink>();
            const auto started =
                impl_->monitor->start(choice.endpoint_id, config_.sample_rate, 6, kMask51);
            if (!started) {
                return refuse(std::string(ac3::audio::describe(started.error())));
            }
            impl_->decoder = std::make_unique<ac3::Eac3Decoder>(ac3::DecoderConfig{});
            break;
        }
        case OutputMode::kStereo: {
            impl_->monitor = std::make_unique<ac3::audio::MonitorSink>();
            const auto started =
                impl_->monitor->start(choice.endpoint_id, config_.sample_rate, 2, kMaskStereo);
            if (!started) {
                return refuse(std::string(ac3::audio::describe(started.error())));
            }
            impl_->decoder = std::make_unique<ac3::Eac3Decoder>(
                ac3::DecoderConfig{.output = {.target = ac3::DownmixTarget::kLoRo}});
            break;
        }
        case OutputMode::kHeadphones: {
            // The spatial sink is started on the first decoded unit, which
            // is when the object count and the LFE's presence are known.
            impl_->spatial = std::make_unique<ac3::audio::SpatialObjectSink>();
            impl_->decoder = std::make_unique<ac3::Eac3Decoder>(ac3::DecoderConfig{});
            break;
        }
        case OutputMode::kNone: return status_;
    }
    status_.running = true;
    return status_;
}

void OutputStage::submit(std::span<const std::byte> unit,
                         std::span<const std::span<const float>> bed) {
    if (!status_.running) {
        return;
    }
    ++status_.units_submitted;
    auto& impl = *impl_;

    switch (status_.mode) {
        case OutputMode::kAtmos:
        case OutputMode::kDdPlus51: {
            auto packed = impl.packer->push(unit);
            if (packed && *packed) {
                submit_with_patience(*impl.passthrough, status_.underruns,
                                     std::span<const std::byte>(**packed));
            }
            return;
        }
        case OutputMode::kDd51: {
            const auto frame = impl.ac3_encoder->encode_frame(bed);
            if (!frame) {
                return;
            }
            if (const auto wrapped = ac3::iec61937::wrap_frame(*frame)) {
                submit_with_patience(*impl.passthrough, status_.underruns,
                                     std::span<const std::byte>(*wrapped));
            }
            return;
        }
        case OutputMode::kPcmSurround:
        case OutputMode::kStereo:
        case OutputMode::kHeadphones: break;
        case OutputMode::kNone: return;
    }

    const auto decoded = impl.decoder->decode_access_unit(unit);
    if (!decoded || !decoded->has_value()) {
        return;  // a decode error, or §3.7's held-back unit
    }
    const auto& out = **decoded;

    if (status_.mode == OutputMode::kHeadphones) {
        if (!impl.spatial_started) {
            const bool has_lfe = out.object_metadata && ac3::oba::has_lfe(out.object_metadata->program);
            const auto started = impl.spatial->start(
                status_.endpoint_id, config_.sample_rate, has_lfe ? kSpeakerLowFrequency : 0U,
                static_cast<std::uint32_t>(std::max<std::size_t>(out.object_audio.size(), 1)));
            if (!started) {
                status_.reason += "; spatial sink refused: " +
                                  std::string(ac3::audio::describe(started.error()));
                impl.teardown();
                status_.running = false;
                status_.mode = OutputMode::kNone;
                return;
            }
            impl.spatial_started = true;
        }
        impl.dynamic_updates.clear();
        if (out.object_metadata) {
            const auto positions = ac3::oba::describe_objects(*out.object_metadata);
            for (std::size_t i = 0; i < out.object_audio.size() && i < positions.size(); ++i) {
                const auto xyz = to_windows_spatial(positions[i].position);
                impl.dynamic_updates.push_back(
                    {.pcm = out.object_audio[i],
                     .x = xyz.x,
                     .y = xyz.y,
                     .z = xyz.z,
                     .gain = static_cast<float>(std::pow(10.0, positions[i].gain_db / 20.0))});
            }
        }
        impl.static_updates.clear();
        if (out.object_metadata && ac3::oba::has_lfe(out.object_metadata->program) &&
            !out.channels.empty()) {
            impl.static_updates.push_back({.pcm = out.channels.back(), .channel = kSpeakerLowFrequency});
        }
        submit_with_patience(*impl.spatial, status_.underruns,
                             std::span<const ac3::audio::DynamicObjectUpdate>(impl.dynamic_updates),
                             std::span<const ac3::audio::StaticObjectUpdate>(impl.static_updates));
        return;
    }

    // PCM surround: the decoder's coded order for 3/2+LFE is L C R Ls Rs LFE
    // (Table 5.8, LFE last); WAVEFORMATEXTENSIBLE 5.1 is L R C LFE Ls Rs.
    // Stereo: the Lo/Ro fold already produced L then R.
    const std::size_t frames = out.channels.empty() ? 0 : out.channels[0].size();
    if (status_.mode == OutputMode::kPcmSurround) {
        if (out.channels.size() < 6) {
            return;
        }
        constexpr std::size_t kCodedForWave[6] = {0, 2, 1, 5, 3, 4};
        impl.interleaved.resize(frames * 6);
        for (std::size_t i = 0; i < frames; ++i) {
            for (std::size_t w = 0; w < 6; ++w) {
                impl.interleaved[i * 6 + w] = out.channels[kCodedForWave[w]][i];
            }
        }
    } else {
        if (out.channels.size() < 2) {
            return;
        }
        impl.interleaved.resize(frames * 2);
        for (std::size_t i = 0; i < frames; ++i) {
            impl.interleaved[i * 2] = out.channels[0][i];
            impl.interleaved[i * 2 + 1] = out.channels[1][i];
        }
    }
    submit_with_patience(*impl.monitor, status_.underruns,
                         std::span<const float>(impl.interleaved));
}

}  // namespace ac3::windemo
