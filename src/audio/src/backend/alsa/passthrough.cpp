#include "ac3/audio/passthrough.hpp"

// The ALSA passthrough backend. CMake compiles this directory's
// passthrough.cpp on a Linux host whose libasound development headers are
// present and another directory's everywhere else, so there is no #ifdef -
// the file's path is what says "ALSA".
//
// ---------------------------------------------------------------------------
// Why this is the backend that decided which Linux audio API to use
// ---------------------------------------------------------------------------
// Capture is an ordinary PCM stream and every Linux audio API can do it.
// Bitstreaming is not, and ALSA is the layer where it is expressed:
//
//   * ALSA does it by opening a plain 16-bit stereo PCM whose IEC 60958
//     channel status has the non-audio bit set. The bit travels with the
//     samples down the S/PDIF or HDMI link and is what makes the receiver
//     decode rather than reproduce them. device_names.hpp is that bit.
//   * PulseAudio does it with PA_STREAM_PASSTHROUGH and an AC-3 pa_format_info
//     - a real capability, but one that ends in the same ALSA call, made by
//     the daemon instead of by us.
//   * PipeWire does it with SPA_MEDIA_SUBTYPE_iec958 and an AC-3 codec, and
//     likewise finishes in ALSA.
//
// So ALSA is not merely the lowest common denominator here; it is the layer
// the other two are implemented on top of, it is present on every Linux system
// including ones with no sound server at all, and its device string is what
// gives a caller direct, unmixed access to the hardware. The cost is
// coexistence: opening a hw: device takes it exclusively, so a running sound
// server has to have released it - which is the same bargain WASAPI exclusive
// mode strikes, and for the same reason. A PipeWire backend is the sensible
// second one to add, as a sibling directory selected the same way; it would
// buy politeness, not capability.
//
// ---------------------------------------------------------------------------
// Exclusive mode, ALSA-style
// ---------------------------------------------------------------------------
// There is no share-mode flag to set. The `iec958` and `hdmi` device names
// resolve to the hardware device directly, with none of dmix's mixing,
// resampling or volume scaling in the way - which is precisely what exclusive
// mode buys on Windows. A device already held by another process simply fails
// to open, and that is reported as kExclusiveUnavailable.
//
// ---------------------------------------------------------------------------
// AC-3 and E-AC-3
// ---------------------------------------------------------------------------
// One difference runs through every function here: a Dolby Digital Plus burst
// is four times the size of a Dolby Digital one and covers the same span of
// time, so its link runs four times as fast. 48 kHz content therefore opens
// the device at 192 kHz - which is why most S/PDIF outputs enumerate with
// supports_eac3_passthrough false while supports_ac3_passthrough is true. An
// optical or coaxial link is not specified past 96 kHz; HDMI is where E-AC-3
// actually goes.
//
// AC-4 (IEC 61937-14) needs nothing ALSA does not already do: the channel
// status says "not audio" whatever the codec, so AC-4's bursts go out the way
// AC-3's do, on a link at the content rate, and its HBR4 bursts at 4x the way
// E-AC-3's do. An AC-4 burst is as long as its frame, which follows the
// stream's frame rate, so the queue holds bursts of different lengths and the
// counts below are kept in bytes. AC-4 HBR16 needs the eight-channel
// high-bit-rate link, which this backend does not open, and is refused.

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <thread>
#include <vector>

#include "ac3/audio/playback_counter.hpp"
#include "ac3/audio/ring_buffer.hpp"
#include "ac3/audio/speakers.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "alsa_support.hpp"
#include "candidates.hpp"
#include "device_names.hpp"

namespace ac3::audio {

namespace {

using alsa::Candidate;
using alsa::DigitalOutput;
using alsa::find_candidates;
using alsa::HwParams;
using alsa::Pcm;
using alsa::SwParams;

// The IEC 61937 carrier is a 2-channel 16-bit stream whatever rides inside it,
// so a burst's length in bytes and in sample-frames differ by a constant 4.
// One AC-3 burst is 6144 bytes = 1536 frames; one E-AC-3 burst is four times
// that in both, and travels over a link clocked four times as fast, so it
// still covers the same span of time.
constexpr unsigned kCarrierChannels = 2;
constexpr snd_pcm_format_t kCarrierFormat = SND_PCM_FORMAT_S16_LE;
constexpr std::size_t kCarrierFrameBytes = 4;
constexpr unsigned kPeriodsPerBuffer = 4;
constexpr int kWaitMs = 100;
// How many recoveries one burst's write may need before the rest of it is
// given up on; see MonitorSink's ALSA backend.
constexpr int kWriteRetries = 4;

// Why snd_pcm_open() said no.
//
// alsa-lib returns a negated errno, and the two answers worth telling apart
// are "that device is not here" and "that device is here and someone else has
// it" - the second is the one a user can do something about, by stopping
// whatever holds it. Guessing between them from a single failure code is how
// this backend originally reported a misspelt device name as a busy one.
PassthroughError open_failure(int error) {
    switch (-error) {
        // The name did not resolve to a device, or resolved to one that is
        // not present: a card that has been unplugged, an HDMI output with no
        // display attached, a typo in a device string.
        case ENOENT:
        case ENODEV:
        case ENXIO:
        case EINVAL:
            return PassthroughError::kDeviceNotFound;
        // Present, and not ours to have. EACCES is the same answer with a
        // different cause - the device is there but this user cannot open it,
        // usually for want of membership of the `audio` group.
        case EBUSY:
        case EAGAIN:
        case EACCES:
        case EPERM:
            return PassthroughError::kExclusiveUnavailable;
        default:
            return PassthroughError::kComFailure;
    }
}

// Configure an open PCM for the IEC 61937 carrier: 16-bit stereo at the link
// rate, no conversion of any kind in the path.
//
// `commit` distinguishes the two callers. Enumeration only wants to know
// whether the parameters would be accepted, and stops before installing them;
// start() installs them and then sets up the software parameters too, sizing
// the period to one whole burst, and learns whether the hardware can pause.
bool configure(snd_pcm_t* pcm, std::uint32_t carrier, std::size_t burst_frames, bool commit,
               bool* can_pause = nullptr) {
    HwParams params;
    if (!params || snd_pcm_hw_params_any(pcm, params.get()) < 0) {
        return false;
    }
    if (snd_pcm_hw_params_set_access(pcm, params.get(), SND_PCM_ACCESS_RW_INTERLEAVED) < 0 ||
        snd_pcm_hw_params_set_format(pcm, params.get(), kCarrierFormat) < 0 ||
        snd_pcm_hw_params_set_channels(pcm, params.get(), kCarrierChannels) < 0) {
        return false;
    }
    // set_rate, not set_rate_near: a burst stream at the wrong rate is not a
    // slightly wrong burst stream, it is noise. If the device will not run the
    // link at this rate, this output cannot carry this format - which for
    // E-AC-3 means most S/PDIF outputs, since 192 kHz is beyond what an
    // optical or coaxial link is specified for.
    if (snd_pcm_hw_params_set_rate(pcm, params.get(), carrier, 0) < 0) {
        return false;
    }
    if (!commit) {
        return true;
    }

    auto period = static_cast<snd_pcm_uframes_t>(burst_frames);
    int direction = 0;
    if (snd_pcm_hw_params_set_period_size_near(pcm, params.get(), &period, &direction) < 0) {
        return false;
    }
    snd_pcm_uframes_t buffer = period * kPeriodsPerBuffer;
    if (snd_pcm_hw_params_set_buffer_size_near(pcm, params.get(), &buffer) < 0) {
        return false;
    }
    if (snd_pcm_hw_params(pcm, params.get()) < 0) {
        return false;
    }
    if (can_pause != nullptr) {
        *can_pause = snd_pcm_hw_params_can_pause(params.get()) == 1;
    }

    SwParams software;
    if (software && snd_pcm_sw_params_current(pcm, software.get()) >= 0) {
        // Start once the buffer is full rather than on the first write, so the
        // link comes up with a whole buffer of bursts behind it instead of
        // one, and the receiver has the best chance of locking first time.
        snd_pcm_sw_params_set_start_threshold(pcm, software.get(), buffer);
        snd_pcm_sw_params_set_avail_min(pcm, software.get(), period);
        snd_pcm_sw_params(pcm, software.get());
    }
    return true;
}

// Open `name`, check it takes the carrier format, close it again.
//
// ALSA has no IsFormatSupported(): the only way to ask whether a device will
// accept a format is to open it and offer it. So this probe is intrusive in a
// way the WASAPI one is not - it briefly holds the device - and it answers
// "no" for a device that is merely busy. Both are stated in the header's
// wording for supports_exclusive_pcm, which exists to tell those two apart.
bool probe(const std::string& name, std::uint32_t carrier) {
    const alsa::QuietErrors quiet;
    snd_pcm_t* handle = nullptr;
    if (snd_pcm_open(&handle, name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) {
        return false;
    }
    const Pcm owned{handle};
    return configure(handle, carrier, /*burst_frames=*/0, /*commit=*/false);
}

// The SPEAKER_* bit an ALSA channel position names (ac3::audio::speakers.hpp).
// 0 for the positions that are not a speaker - SND_CHMAP_NA (a channel to
// leave alone), MONO, and the two "unknown" values - and for a position
// WAVEFORMATEXTENSIBLE has no bit for.
std::uint32_t speaker_of_position(unsigned int position) {
    switch (position) {
        case SND_CHMAP_FL: return kSpeakerFrontLeft;
        case SND_CHMAP_FR: return kSpeakerFrontRight;
        case SND_CHMAP_FC: return kSpeakerFrontCentre;
        case SND_CHMAP_LFE: return kSpeakerLowFrequency;
        case SND_CHMAP_RL: return kSpeakerBackLeft;
        case SND_CHMAP_RR: return kSpeakerBackRight;
        case SND_CHMAP_FLC: return kSpeakerFrontLeftOfCentre;
        case SND_CHMAP_FRC: return kSpeakerFrontRightOfCentre;
        case SND_CHMAP_RC: return kSpeakerBackCentre;
        case SND_CHMAP_SL: return kSpeakerSideLeft;
        case SND_CHMAP_SR: return kSpeakerSideRight;
        case SND_CHMAP_TC: return kSpeakerTopCentre;
        case SND_CHMAP_TFL: return kSpeakerTopFrontLeft;
        case SND_CHMAP_TFC: return kSpeakerTopFrontCentre;
        case SND_CHMAP_TFR: return kSpeakerTopFrontRight;
        case SND_CHMAP_TRL: return kSpeakerTopBackLeft;
        case SND_CHMAP_TRC: return kSpeakerTopBackCentre;
        case SND_CHMAP_TRR: return kSpeakerTopBackRight;
        default: return 0;
    }
}

// What the endpoint itself renders, for RenderDeviceInfo's channels, speakers
// and sample_rates. All three come from one open: this probe is intrusive
// (see probe() above - ALSA has no IsFormatSupported), so the device is held
// once rather than three times over.
//
// The width comes from the hardware parameter space rather than from a mix
// format, so it is the device's own maximum rather than whatever a shared
// mixer happens to be running at - the right number for "is a decoded
// programme wider than this output?". Everything stays at its "cannot say"
// value on any failure, including a device that is merely busy: 0 never means
// "no channels", and an empty rate list never means "no rates".
struct EndpointFacts {
    std::uint16_t channels = 0;
    std::uint32_t speakers = 0;
    std::vector<std::uint32_t> sample_rates;
};

EndpointFacts endpoint_facts(const std::string& name) {
    EndpointFacts facts;
    const alsa::QuietErrors quiet;
    snd_pcm_t* handle = nullptr;
    if (snd_pcm_open(&handle, name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) {
        return facts;
    }
    const Pcm owned{handle};
    const HwParams params;
    if (!params || snd_pcm_hw_params_any(handle, params.get()) < 0) {
        return facts;
    }

    unsigned int channels = 0;
    if (snd_pcm_hw_params_get_channels_max(params.get(), &channels) == 0) {
        // ALSA reports a plug device's maximum as something absurd (1024 or
        // more) because the plug layer will invent any width asked of it. That
        // is not an endpoint width, so it is reported as unknown rather than
        // as a number no downmix decision should be made from.
        facts.channels = channels > 0 && channels <= 64 ? static_cast<std::uint16_t>(channels) : 0;
    }

    for (const std::uint32_t rate : {44100U, 48000U, 88200U, 96000U, 176400U, 192000U}) {
        if (snd_pcm_hw_params_test_rate(handle, params.get(), rate, 0) == 0) {
            facts.sample_rates.push_back(rate);
        }
    }

    // The driver's channel maps, one per width it can be configured in: the
    // one for this endpoint's own width says which speaker each channel is.
    // HDMI drivers fill these in; many others answer nothing, which stays
    // "cannot say".
    if (snd_pcm_chmap_query_t** maps = snd_pcm_query_chmaps(handle); maps != nullptr) {
        for (snd_pcm_chmap_query_t** entry = maps; *entry != nullptr; ++entry) {
            const snd_pcm_chmap_t& map = (*entry)->map;
            if (facts.channels != 0 && map.channels != facts.channels) {
                continue;
            }
            std::uint32_t speakers = 0;
            for (unsigned int i = 0; i < map.channels; ++i) {
                speakers |= speaker_of_position(map.pos[i]);
            }
            if (speaker_count(speakers) == map.channels) {
                facts.speakers = speakers;
                // A map found while the width was unknown is itself the
                // width: reporting a mask for eight speakers beside a channel
                // count of "cannot say" would let a caller pair the two with
                // a stream of some third width. The two figures come from the
                // same query, so they agree by construction.
                if (facts.channels == 0) {
                    facts.channels = static_cast<std::uint16_t>(map.channels);
                }
                break;
            }
        }
        snd_pcm_free_chmaps(maps);
    }
    return facts;
}

// Whether `base` will carry `format` at `content_rate`: the device name with
// the right channel status for the link rate that format needs, opened and
// offered the carrier parameters.
bool probe_format(std::string_view base, BitstreamFormat format, std::uint32_t content_rate) {
    const std::uint32_t carrier = alsa::carrier_rate(format, content_rate);
    const auto name = alsa::passthrough_device_name(base, carrier);
    return name.has_value() && probe(*name, carrier);
}

}  // namespace

std::string_view describe(PassthroughError error) {
    switch (error) {
        case PassthroughError::kNoBackend: return "no passthrough backend on this platform";
        case PassthroughError::kComFailure: return "an ALSA call failed";
        case PassthroughError::kDeviceNotFound:
            return "no such output: either the named ALSA device does not exist, or none was "
                   "named and this machine has no S/PDIF or HDMI output for ALSA to find";
        case PassthroughError::kFormatRejected:
            return "the output will not carry this bitstream over IEC 61937 at this rate "
                   "(E-AC-3 needs a 4x link clock, which usually means HDMI rather than S/PDIF, "
                   "and an HDMI output needs a display connected and awake before its audio "
                   "device accepts anything)";
        case PassthroughError::kExclusiveUnavailable:
            return "the device could not be opened directly (PipeWire, PulseAudio or another "
                   "application holds it - a bitstream cannot share a device with a mixer - or "
                   "this user has no permission on it, which means the `audio` group)";
        case PassthroughError::kAlreadyRunning: return "passthrough is already running";
        case PassthroughError::kNotRunning: return "passthrough is not running";
        case PassthroughError::kUnsupportedFormat:
            return "AC-4 HBR16 travels on an eight-channel high-bit-rate link, and this backend "
                   "opens a two-channel one";
    }
    return "unknown passthrough error";
}

std::expected<std::vector<RenderDeviceInfo>, PassthroughError> enumerate_render_devices(
    std::uint32_t sample_rate) {
    const int preferred_card = alsa::default_card();
    // Which entry gets is_default, decided after the walk: the first DIGITAL
    // output on the configured default card, since this list's default is
    // what 'play' aims a bitstream at, and an analogue jack cannot carry one.
    // A machine with no digital output at all falls back to its first entry.
    // (A player asking for the default DECODED output passes no device name
    // at all and gets ALSA's own "default" PCM, which is a different
    // question and the user's own configuration to answer.)
    std::size_t default_index = 0;
    bool marked_default = false;
    bool any_digital = false;

    std::vector<RenderDeviceInfo> devices;
    for (const auto& candidate : find_candidates(alsa::Include::kEveryPlaybackPcm)) {
        EndpointFacts facts = endpoint_facts(candidate.hw_name);
        // An output that is neither HDMI nor S/PDIF is not probed for
        // passthrough at all, and this is load-bearing rather than an
        // optimisation. Such an output is named through plug, and plug accepts
        // ANY format, width and rate by construction - it would resample a
        // burst rather than refuse it, so the probe would answer yes for every
        // analogue jack on the machine. `play` believes that answer: it would
        // hand IEC 61937 bursts to a resampler with no non-audio bit set, which
        // is full-scale noise out of the speakers, the exact outcome
        // device_names.hpp's header exists to prevent. The probe-decides
        // reasoning in that header's DigitalOutput comment holds only for a
        // name that would carry channel status, which a plug name cannot.
        const bool digital = candidate.kind != DigitalOutput::kNone;
        const bool ac3_link =
            digital && probe_format(candidate.name, BitstreamFormat::kAc3, sample_rate);
        RenderDeviceInfo info{
            .id = candidate.name,
            .name = candidate.friendly,
            .is_default = false,
            .supports_ac3_passthrough = ac3_link,
            .supports_eac3_passthrough =
                digital && probe_format(candidate.name, BitstreamFormat::kEac3, sample_rate),
            // AC-4's link is AC-3's - two channels at the content rate, the
            // same channel status - so one probe answers both.
            .supports_ac4_passthrough = ac3_link,
            // The control probe: the same carrier format on the raw hardware
            // device, with no channel status. A device that takes this but
            // neither of the above cannot bitstream; one that takes none of
            // the three is in use by something else.
            .supports_exclusive_pcm = probe(candidate.hw_name, sample_rate),
            .channels = facts.channels,
            .speakers = facts.speakers,
            .sample_rates = std::move(facts.sample_rates),
        };

        // A digital output on the configured default card wins; failing that,
        // the first digital output anywhere; failing that, entry zero, which
        // is what default_index starts as.
        if (candidate.kind != DigitalOutput::kNone && !marked_default) {
            const bool preferred = candidate.card == preferred_card;
            if (preferred || !any_digital) {
                default_index = devices.size();
            }
            any_digital = true;
            marked_default = preferred;
        }
        devices.push_back(std::move(info));
    }
    // Nothing digital on the configured default card, or no configuration to
    // read: the first output found is as good a default as exists.
    if (!devices.empty()) {
        devices[default_index].is_default = true;
    }
    return devices;
}

struct PassthroughSink::Impl {
    std::unique_ptr<ByteRingBuffer> queue;
    std::jthread worker;
    snd_pcm_t* pcm = nullptr;
    // Set by start() and read by submit(): the AC-3 and E-AC-3 burst sizes are
    // four times apart, and a caller that hands over the wrong one is handing
    // over a frame boundary in the wrong place rather than a slightly odd
    // length. An AC-4 burst is any whole number of link frames up to the
    // longest (burst_size_fits()).
    BitstreamFormat format = BitstreamFormat::kAc3;
    std::size_t burst_bytes = iec61937::kBurstBytes;
    // Link frames to a content frame (carrier_ratio()), for position().
    std::uint32_t ratio = 1;
    // Raised by start(). Lowered by stop(), or by the render thread itself
    // when the device goes away under it (see the end of its loop).
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    // In bytes, the bursts not all being one length: stats() turns them into
    // bursts.
    std::atomic<std::uint64_t> submitted_bytes{0};
    std::atomic<std::uint64_t> rendered_bytes{0};
    std::atomic<std::uint64_t> underruns{0};
    // What the render thread last read from the device, in link frames, for
    // position(): snd_pcm_delay() against the frames handed over. Only the
    // render thread touches the handle, so position() reads the counter.
    PlaybackCounter counter;
    // Whether the hardware can pause without losing what it holds; see
    // MonitorSink's ALSA backend for what happens when it cannot.
    bool can_pause = false;
    std::atomic_bool paused{false};
    std::atomic_bool flushing{false};
    std::atomic<std::uint64_t> flushes{0};
    // How far the queue had been written when the flush was asked for: what
    // the render thread drops.
    std::atomic<std::size_t> flush_mark{0};
};

PassthroughSink::PassthroughSink() : impl_(std::make_unique<Impl>()) {}

PassthroughSink::~PassthroughSink() {
    stop();
}

bool PassthroughSink::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

PassthroughStats PassthroughSink::stats() const {
    const std::uint64_t submitted = impl_->submitted.load();
    const std::uint64_t submitted_bytes = impl_->submitted_bytes.load();
    const std::uint64_t rendered_bytes = impl_->rendered_bytes.load();
    // Whole bursts heard. Exact for AC-3 and E-AC-3, whose bursts are all one
    // length, and for AC-4 once everything submitted has been heard, which is
    // what a caller draining the queue waits for; part-way, by their mean
    // length.
    const std::uint64_t rendered = submitted_bytes == 0 ? 0
                                   : rendered_bytes >= submitted_bytes
                                       ? submitted
                                       : rendered_bytes * submitted / submitted_bytes;
    return {.bursts_submitted = submitted,
            .bursts_rendered = rendered,
            .underruns = impl_->underruns.load()};
}

std::optional<MonitorPosition> PassthroughSink::position() const {
    if (!running() || !impl_->queue) {
        return std::nullopt;
    }
    const std::uint64_t queued_here = impl_->queue->available() / kCarrierFrameBytes;
    // snd_pcm_delay() already counts the whole path out of the machine, as
    // the device's queue; there is no latency left to add.
    return per_content_frame(impl_->counter.position(queued_here, /*latency=*/0), impl_->ratio);
}

void PassthroughSink::flush() {
    if (!running()) {
        return;
    }
    const std::uint64_t done = impl_->flushes.load(std::memory_order_acquire);
    impl_->flush_mark.store(impl_->queue->write_mark(), std::memory_order_release);
    impl_->flushing.store(true, std::memory_order_release);
    for (int waited = 0; waited < 200; ++waited) {
        if (impl_->flushes.load(std::memory_order_acquire) != done || !running()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // The render thread did not get to it - a device that has stopped
    // answering. The flush is left for the thread to make when it next runs.
    // It drops only what was queued before the mark, so bursts submitted
    // after this call returned are kept. A device whose recovery failed has
    // ended the thread instead, which lowered `running` and ended the wait
    // at once.
}

std::expected<void, PassthroughError> PassthroughSink::pause() {
    if (!running()) {
        return std::unexpected(PassthroughError::kNotRunning);
    }
    // Hardware that cannot pause is dropped and prepared again instead, which
    // loses the bursts it held - up to a buffer's worth, as a flush() would.
    // The queue survives either way.
    impl_->paused.store(true, std::memory_order_release);
    return {};
}

std::expected<void, PassthroughError> PassthroughSink::resume() {
    if (!running()) {
        return std::unexpected(PassthroughError::kNotRunning);
    }
    impl_->paused.store(false, std::memory_order_release);
    return {};
}

bool PassthroughSink::paused() const {
    // A pause is a property of a running stream, so one whose device has
    // gone is not paused either.
    return running() && impl_->paused.load(std::memory_order_acquire);
}

bool PassthroughSink::can_submit() const {
    if (!running() || !impl_->queue) {
        return false;
    }
    return impl_->queue->capacity() - impl_->queue->available() > impl_->burst_bytes;
}

bool PassthroughSink::submit(std::span<const std::byte> burst) {
    if (!running() || !impl_->queue || !burst_size_fits(impl_->format, burst.size())) {
        return false;
    }
    if (!can_submit()) {
        return false;
    }
    const auto wrote = impl_->queue->write(burst);
    if (wrote != burst.size()) {
        return false;
    }
    impl_->submitted.fetch_add(1);
    impl_->submitted_bytes.fetch_add(burst.size());
    return true;
}

void PassthroughSink::stop() {
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    if (impl_->pcm != nullptr) {
        snd_pcm_close(impl_->pcm);
        impl_->pcm = nullptr;
    }
    // A pause is a property of a running stream, so it does not outlive one.
    impl_->paused.store(false, std::memory_order_relaxed);
    impl_->flushing.store(false, std::memory_order_relaxed);
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, PassthroughError> PassthroughSink::start(const std::string& device_id,
                                                             std::uint32_t sample_rate,
                                                             BitstreamFormat format_kind) {
    if (running()) {
        return std::unexpected(PassthroughError::kAlreadyRunning);
    }
    if (format_kind == BitstreamFormat::kAc4Hbr16) {
        return std::unexpected(PassthroughError::kUnsupportedFormat);
    }
    // A render thread that ended because its device went away still has the
    // device open, and a hw: device opens for one process at a time. stop()
    // joins the thread and closes the handle; with nothing started it does
    // nothing.
    stop();

    // The link rate, not the content rate: the same for AC-3 and AC-4, and 4x
    // it for E-AC-3 and AC-4 HBR4. Everything below - the channel status, the
    // device parameters, the burst size - is expressed in the carrier's terms
    // from here on. For AC-4 the burst size is the longest one, which the
    // period and the write chunk are sized to; shorter bursts run on in the
    // queue behind each other.
    const std::uint32_t carrier = alsa::carrier_rate(format_kind, sample_rate);
    const std::size_t burst_bytes = max_burst_bytes(format_kind);
    const std::size_t burst_frames = burst_bytes / kCarrierFrameBytes;

    // Pick the device before touching it. An empty id means "the default
    // output", which for a bitstream can only mean a digital one that has
    // already said yes - there is no system-wide "default S/PDIF" setting to
    // consult, and defaulting to the analog output would emit full-scale noise
    // from the speakers.
    std::string base = device_id;
    if (base.empty()) {
        const auto candidates = find_candidates();
        if (candidates.empty()) {
            return std::unexpected(PassthroughError::kDeviceNotFound);
        }
        for (const auto& candidate : candidates) {
            if (probe_format(candidate.name, format_kind, sample_rate)) {
                base = candidate.name;
                break;
            }
        }
        if (base.empty()) {
            return std::unexpected(PassthroughError::kFormatRejected);
        }
    }

    const auto name = alsa::passthrough_device_name(base, carrier);
    if (!name) {
        // Reached only for a carrier rate IEC 60958 has no frequency code for.
        // AC-3's three rates all have one; E-AC-3 at 32 kHz would want a
        // 128 kHz link, and that does not.
        return std::unexpected(PassthroughError::kFormatRejected);
    }

    snd_pcm_t* handle = nullptr;
    if (const int opened = snd_pcm_open(&handle, name->c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        opened < 0) {
        return std::unexpected(open_failure(opened));
    }
    Pcm opened{handle};

    bool can_pause = false;
    if (!configure(handle, carrier, burst_frames, /*commit=*/true, &can_pause)) {
        return std::unexpected(PassthroughError::kFormatRejected);
    }
    if (snd_pcm_prepare(handle) < 0) {
        return std::unexpected(PassthroughError::kComFailure);
    }

    // Room for roughly a second of bursts, so a caller encoding slightly
    // ahead of real time never has to spin. Counted in bursts rather than
    // bytes so an E-AC-3 session gets the same second, not a quarter of one.
    impl_->queue = std::make_unique<ByteRingBuffer>(burst_bytes * 40);
    impl_->format = format_kind;
    impl_->burst_bytes = burst_bytes;
    impl_->ratio = carrier_ratio(format_kind);
    impl_->submitted.store(0);
    impl_->submitted_bytes.store(0);
    impl_->rendered_bytes.store(0);
    impl_->underruns.store(0);
    impl_->counter.restart();
    impl_->paused.store(false);
    impl_->flushing.store(false);
    impl_->flushes.store(0);
    impl_->can_pause = can_pause;
    impl_->running.store(true, std::memory_order_release);
    impl_->pcm = opened.release();

    impl_->worker = std::jthread([this, burst_bytes, burst_frames](const std::stop_token& stop) {
        snd_pcm_t* pcm = impl_->pcm;
        std::vector<std::byte> chunk(burst_bytes);
        std::uint64_t handed_over = 0;
        bool device_paused = false;
        // Whether the pause in force was made by dropping rather than by
        // snd_pcm_pause, which decides how it is undone.
        bool dropped_to_pause = false;
        // Set when the loop ends because the device did, as MonitorSink's
        // ALSA backend sets it.
        bool lost = false;

        while (!stop.stop_requested()) {
            // As in MonitorSink's ALSA backend: the device and the queue
            // belong to this thread, and pause() and flush() raise flags. A
            // hardware pause keeps what the device holds; a device that cannot
            // pause, or refuses to from PREPARED, is dropped and prepared
            // again, which loses it.
            const bool wanted_pause = impl_->paused.load(std::memory_order_acquire);
            if (wanted_pause != device_paused) {
                if (wanted_pause) {
                    const bool held = impl_->can_pause && snd_pcm_pause(pcm, 1) == 0;
                    if (!held) {
                        snd_pcm_drop(pcm);
                        snd_pcm_prepare(pcm);
                        // What was dropped will never be heard, so what the
                        // device was given is what it played.
                        handed_over = impl_->counter.played();
                        impl_->counter.report(handed_over, 0);
                    }
                    dropped_to_pause = !held;
                } else if (!dropped_to_pause) {
                    snd_pcm_pause(pcm, 0);
                } else {
                    dropped_to_pause = false;
                }
                device_paused = wanted_pause;
            }
            if (device_paused && !impl_->flushing.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (impl_->flushing.exchange(false, std::memory_order_acq_rel)) {
                // drop discards what the device holds; prepare puts the stream
                // back in a state that can be written to.
                snd_pcm_drop(pcm);
                snd_pcm_prepare(pcm);
                impl_->queue->discard_to(impl_->flush_mark.load(std::memory_order_acquire));
                handed_over = 0;
                impl_->counter.restart();
                impl_->rendered_bytes.store(0, std::memory_order_relaxed);
                impl_->submitted_bytes.store(0, std::memory_order_relaxed);
                impl_->submitted.store(0, std::memory_order_relaxed);
                impl_->flushes.fetch_add(1, std::memory_order_release);
                if (device_paused) {
                    // Dropped, so a resume starts the stream by writing.
                    dropped_to_pause = true;
                    continue;
                }
            }
            snd_pcm_sframes_t delay = 0;
            if (snd_pcm_delay(pcm, &delay) == 0 && delay >= 0) {
                impl_->counter.report(handed_over, static_cast<std::uint64_t>(delay));
            }
            const int ready = snd_pcm_wait(pcm, kWaitMs);
            if (ready < 0) {
                // snd_pcm_recover mends an underrun or a suspend; anything
                // else it hands back - -ENODEV for a card unplugged - is the
                // device's end, and the stream's.
                if (snd_pcm_recover(pcm, ready, /*silent=*/1) < 0) {
                    lost = true;
                    break;
                }
                continue;
            }
            if (ready == 0) {
                continue;  // nothing wanted yet; go back and re-check the stop
            }

            const auto got = impl_->queue->read(chunk);
            if (got < chunk.size()) {
                // Nothing queued: emit silence for the remainder. A receiver
                // that sees a gap in the burst stream usually drops lock, so
                // this is counted, not hidden. The count starts at the moment
                // the device is opened, so a caller that opens the sink before
                // it has any bursts ready will see the first few periods
                // charged here - that is a real gap on the wire.
                std::fill(chunk.begin() + static_cast<std::ptrdiff_t>(got), chunk.end(),
                          std::byte{0});
                impl_->underruns.fetch_add(1);
            }

            // As in MonitorSink's ALSA backend: the burst has left the queue,
            // so it is written out in full or given up on here. A write that
            // meets an under-run hands over nothing; once recovered, the rest
            // of the burst is written again, so bursts_rendered keeps up with
            // bursts_submitted and a caller draining the one into the other
            // finishes. A device that under-runs on every retry is given up
            // on after kWriteRetries recoveries, the burst counted as
            // finished with all the same.
            std::size_t done = 0;
            int retries = 0;
            while (done < burst_frames && !stop.stop_requested()) {
                const snd_pcm_sframes_t written =
                    snd_pcm_writei(pcm, chunk.data() + done * kCarrierFrameBytes,
                                   static_cast<snd_pcm_uframes_t>(burst_frames - done));
                if (written < 0) {
                    if (snd_pcm_recover(pcm, static_cast<int>(written), /*silent=*/1) < 0) {
                        lost = true;
                        break;
                    }
                    if (++retries > kWriteRetries) {
                        break;
                    }
                    continue;
                }
                done += static_cast<std::size_t>(written);
                handed_over += static_cast<std::uint64_t>(written);
            }
            if (lost) {
                break;
            }
            impl_->rendered_bytes.fetch_add(got);
        }

        if (lost) {
            // The stream has ended with its device, and running() says so as
            // a stop() would have it; see MonitorSink's ALSA backend.
            impl_->running.store(false, std::memory_order_release);
        }
        // drop, not drain: a stop request means stop, and draining would play
        // out a buffer of bursts the caller has already stopped feeding.
        snd_pcm_drop(pcm);
    });

    return {};
}

}  // namespace ac3::audio
