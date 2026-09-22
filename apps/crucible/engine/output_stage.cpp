#include "output_stage.hpp"

#include "platform_services.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <deque>
#include <optional>
#include <thread>
#include <utility>

#include "ac3/audio/spatial.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"

namespace ac3::crucible {

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

// The bed's LFE is not an object, so it never goes through JOC reconstruction
// - but the dynamic objects submit()'s kHeadphones branch places beside it
// did, and that costs ac3::oba::joc::reconstruction_delay(domain) samples the
// LFE does not pay (docs/library/decoding.md, "Atmos objects lag the bed").
// Submitted to the spatial sink as soon as each unit decodes, the LFE would
// reach the room that far ahead of the objects beside it, so it goes through
// a delay line of that length first. Same class, same reasoning, as
// ac3cli's identical run_spatial - see live_audio.cpp's own LfeDelayLine.
class LfeDelayLine {
public:
    explicit LfeDelayLine(std::size_t delay_samples) : pending_(delay_samples, 0.0F) {}

    std::vector<float> process(std::span<const float> in) {
        pending_.insert(pending_.end(), in.begin(), in.end());
        std::vector<float> out(in.size());
        for (float& sample : out) {
            sample = pending_.front();
            pending_.pop_front();
        }
        return out;
    }

private:
    std::deque<float> pending_;
};

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
    std::shared_ptr<AudioDevices> devices;
    // Exactly one of these is live at a time, per mode.
    std::unique_ptr<BurstSink> passthrough;
    std::unique_ptr<PcmSink> monitor;
    std::unique_ptr<ObjectSink> spatial;
    bool spatial_started = false;

    std::unique_ptr<ac3::iec61937::Eac3BurstPacker> packer;  // Atmos / DD+
    std::unique_ptr<ac3::FrameEncoder> ac3_encoder;           // DD 5.1
    std::unique_ptr<ac3::Eac3Decoder> decoder;                // the decoded modes

    std::vector<float> interleaved;
    std::vector<ac3::audio::DynamicObjectUpdate> dynamic_updates;
    std::vector<ac3::audio::StaticObjectUpdate> static_updates;
    // kHeadphones only - see LfeDelayLine's own comment. Re-armed alongside
    // `decoder` whenever that mode (re)starts (apply()), so a later mode
    // switch back to headphones never plays a stale tail left over from an
    // earlier session through it.
    std::optional<LfeDelayLine> lfe_delay;
    std::vector<float> delayed_lfe;

    // Whether the sink `mode` currently owns still has its device, for
    // apply()'s "nothing changed" check below. kHeadphones reads true before
    // its first submit() - the spatial sink is opened lazily (ensure_spatial)
    // and is not itself lost just because it has never been asked to start -
    // and kNone has no sink to ask, so it reads true too (apply() only
    // consults this once it already knows `choice.mode` is not kNone).
    [[nodiscard]] bool sink_running(OutputMode mode) const {
        switch (mode) {
            case OutputMode::kAtmos:
            case OutputMode::kDdPlus51:
            case OutputMode::kDd51: return !passthrough || passthrough->running();
            case OutputMode::kPcmSurround:
            case OutputMode::kStereo: return !monitor || monitor->running();
            case OutputMode::kHeadphones: return !spatial_started || spatial->running();
            case OutputMode::kNone: return true;
        }
        return true;
    }

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
        lfe_delay.reset();
    }
};

OutputStage::OutputStage(OutputStageConfig config)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)) {
    impl_->devices = config_.devices ? config_.devices : platform_audio_devices();
}

OutputStage::~OutputStage() {
    stop();
}

void OutputStage::set_pinned(std::optional<OutputMode> pinned) {
    config_.pinned = pinned;
}

void OutputStage::set_preferred_endpoint(std::string id) {
    config_.preferred_endpoint_id = std::move(id);
}

void OutputStage::set_null_sink_substring(std::string substring) {
    config_.null_sink_substring = std::move(substring);
}

void OutputStage::stop() {
    impl_->teardown();
    status_.running = false;
    status_.sink_queue_frames = 0;
}

std::vector<EndpointFacts> OutputStage::enumerate() const {
    std::vector<EndpointFacts> facts;
    const std::string needle = lower(config_.null_sink_substring);
    for (const auto& device : impl_->devices->render_devices(config_.sample_rate)) {
        facts.push_back({.id = device.id,
                         .name = device.name,
                         .is_default = device.is_default,
                         .is_null_sink = !needle.empty() &&
                                         lower(device.name).find(needle) != std::string::npos,
                         .accepts_eac3 = device.accepts_eac3,
                         .accepts_ac3 = device.accepts_ac3,
                         .shared_channels = device.shared_channels,
                         .spatial = device.spatial,
                         .spatial_max_objects = device.spatial_max_objects});
    }
    return facts;
}

const OutputStatus& OutputStage::reprobe(bool signing_key_loaded) {
    return apply(enumerate(), signing_key_loaded);
}

const OutputStatus& OutputStage::apply(std::vector<EndpointFacts> facts, bool signing_key_loaded) {
    const auto choice = choose_output({.endpoints = facts,
                                       .signing_key_loaded = signing_key_loaded,
                                       .pinned = config_.pinned,
                                       .preferred_endpoint_id = config_.preferred_endpoint_id});
    status_.endpoints = std::move(facts);
    status_.reason = choice.reason;

    // Not just "did the policy's answer change": an endpoint that lost its
    // stream (unplugged, taken by another exclusive app, reset by its
    // driver) can still be re-enumerated with the same id and the same
    // accepted formats, so the policy answers exactly as before and this
    // would otherwise conclude nothing needs doing - keeping the dead sink
    // forever, since nothing else ever revisits this decision. running()
    // is what tells the two cases apart.
    const bool unchanged = status_.running && choice.mode == status_.mode &&
                           choice.endpoint_id == status_.endpoint_id &&
                           impl_->sink_running(status_.mode);
    if (unchanged) {
        return status_;
    }

    impl_->teardown();
    status_.running = false;
    status_.sink_queue_frames = 0;
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
            impl_->passthrough = impl_->devices->burst_sink();
            const auto started = impl_->passthrough->start(choice.endpoint_id, config_.sample_rate, true);
            if (!started.has_value()) {
                return refuse(started.error());
            }
            impl_->packer = std::make_unique<ac3::iec61937::Eac3BurstPacker>();
            break;
        }
        case OutputMode::kDd51: {
            impl_->passthrough = impl_->devices->burst_sink();
            const auto started = impl_->passthrough->start(choice.endpoint_id, config_.sample_rate, false);
            if (!started.has_value()) {
                return refuse(started.error());
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
            impl_->monitor = impl_->devices->pcm_sink();
            const auto started =
                impl_->monitor->start(choice.endpoint_id, config_.sample_rate, 6, kMask51, config_.low_latency);
            if (!started.has_value()) {
                return refuse(started.error());
            }
            impl_->decoder = std::make_unique<ac3::Eac3Decoder>(ac3::DecoderConfig{});
            break;
        }
        case OutputMode::kStereo: {
            impl_->monitor = impl_->devices->pcm_sink();
            const auto started =
                impl_->monitor->start(choice.endpoint_id, config_.sample_rate, 2, kMaskStereo, config_.low_latency);
            if (!started.has_value()) {
                return refuse(started.error());
            }
            impl_->decoder = std::make_unique<ac3::Eac3Decoder>(
                ac3::DecoderConfig{.output = {.target = ac3::DownmixTarget::kLoRo}});
            break;
        }
        case OutputMode::kHeadphones: {
            // The spatial sink is started on the first decoded unit, which
            // is when the object count and the LFE's presence are known.
            impl_->spatial = impl_->devices->object_sink();
            // Named rather than a temporary passed straight to the decoder:
            // lfe_delay below reads .joc_domain back off it, so the two can
            // never disagree on which domain this session actually decodes
            // with.
            const ac3::DecoderConfig decoder_config{};
            impl_->decoder = std::make_unique<ac3::Eac3Decoder>(decoder_config);
            impl_->lfe_delay.emplace(static_cast<std::size_t>(
                ac3::oba::joc::reconstruction_delay(decoder_config.joc_domain)));
            break;
        }
        case OutputMode::kNone: return status_;
    }
    status_.running = true;
    return status_;
}

void OutputStage::set_bypass(bool on) {
    config_.bypass_codec = on;
}

bool OutputStage::ensure_spatial(bool has_lfe, std::size_t objects) {
    auto& impl = *impl_;
    if (impl.spatial_started) {
        return true;
    }
    const auto started = impl.spatial->start(
        status_.endpoint_id, config_.sample_rate, has_lfe ? kSpeakerLowFrequency : 0U,
        static_cast<std::uint32_t>(std::max<std::size_t>(objects, 1)));
    if (!started.has_value()) {
        status_.reason += "; spatial sink refused: " + started.error();
        impl.teardown();
        status_.running = false;
        status_.mode = OutputMode::kNone;
        return false;
    }
    impl.spatial_started = true;
    return true;
}

// The bypass: the decoded modes fed from the engine's own frame. Headphones
// render every object slot where the encoder placed it, with the bed's LFE
// as the one static channel; PCM surround takes the bed by channel; stereo
// takes an ITU-R BS.775 fold of it (centre and surrounds at -3 dB, LFE
// dropped), normalised so full scale on every channel cannot clip.
void OutputStage::submit_raw(const RawFrame& raw) {
    auto& impl = *impl_;
    if (status_.mode == OutputMode::kHeadphones) {
        const bool has_lfe = raw.bed.size() >= 6;
        if (!ensure_spatial(has_lfe, raw.objects.size())) {
            return;
        }
        impl.dynamic_updates.clear();
        for (std::size_t i = 0; i < raw.objects.size() && i < raw.placements.size(); ++i) {
            const auto xyz = to_windows_spatial(raw.placements[i].position);
            impl.dynamic_updates.push_back({.pcm = raw.objects[i],
                                            .x = xyz.x,
                                            .y = xyz.y,
                                            .z = xyz.z,
                                            .gain = static_cast<float>(raw.placements[i].gain)});
        }
        impl.static_updates.clear();
        if (has_lfe) {
            impl.static_updates.push_back({.pcm = raw.bed[5], .channel = kSpeakerLowFrequency});
        }
        submit_with_patience(*impl.spatial, status_.underruns,
                             std::span<const ac3::audio::DynamicObjectUpdate>(impl.dynamic_updates),
                             std::span<const ac3::audio::StaticObjectUpdate>(impl.static_updates));
        return;
    }
    if (raw.bed.size() < 6) {
        return;
    }
    const std::size_t frames = raw.bed[0].size();
    if (status_.mode == OutputMode::kPcmSurround) {
        constexpr std::size_t kCodedForWave[6] = {0, 2, 1, 5, 3, 4};
        impl.interleaved.resize(frames * 6);
        for (std::size_t i = 0; i < frames; ++i) {
            for (std::size_t w = 0; w < 6; ++w) {
                impl.interleaved[i * 6 + w] = raw.bed[kCodedForWave[w]][i];
            }
        }
    } else {
        constexpr float kMinus3dB = 0.70710678F;
        constexpr float kNormalise = 1.0F / (1.0F + kMinus3dB + kMinus3dB);
        impl.interleaved.resize(frames * 2);
        for (std::size_t i = 0; i < frames; ++i) {
            const float c = raw.bed[1][i] * kMinus3dB;
            impl.interleaved[i * 2] = (raw.bed[0][i] + c + raw.bed[3][i] * kMinus3dB) * kNormalise;
            impl.interleaved[i * 2 + 1] = (raw.bed[2][i] + c + raw.bed[4][i] * kMinus3dB) * kNormalise;
        }
    }
    submit_with_patience(*impl.monitor, status_.underruns,
                         std::span<const float>(impl.interleaved));
    status_.sink_queue_frames = impl.monitor->queued_frames();
}

void OutputStage::submit(std::span<const std::byte> unit, const RawFrame& raw) {
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
            const auto frame = impl.ac3_encoder->encode_frame(raw.bed);
            if (!frame.has_value()) {
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

    status_.bypassed = config_.bypass_codec;
    if (config_.bypass_codec) {
        submit_raw(raw);
        return;
    }

    const auto decoded = impl.decoder->decode_access_unit(unit);
    if (!decoded || !decoded->has_value()) {
        return;  // a decode error, or §3.7's held-back unit
    }
    const auto& out = **decoded;

    if (status_.mode == OutputMode::kHeadphones) {
        const bool has_lfe = out.object_metadata && ac3::oba::has_lfe(out.object_metadata->program);
        if (!ensure_spatial(has_lfe, out.object_audio.size())) {
            return;
        }
        impl.dynamic_updates.clear();
        if (out.object_metadata.has_value()) {
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
            // Delayed to arrive with the dynamic objects above, not ahead of
            // them - see LfeDelayLine's own comment.
            impl.delayed_lfe = impl.lfe_delay->process(out.channels.back());
            impl.static_updates.push_back(
                {.pcm = impl.delayed_lfe, .channel = kSpeakerLowFrequency});
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
    status_.sink_queue_frames = impl.monitor->queued_frames();
}

}  // namespace ac3::crucible
