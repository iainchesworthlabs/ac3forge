#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "ac3/audio/monitor.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/routing.hpp"

// A PCM output at the device's own width, with each rendered channel placed
// where a routing patch says.
//
// MonitorSink opens a stream as wide as the CALLER asks and leaves the device
// to the platform: a 5.1 programme on an 8-channel HDMI endpoint becomes a
// six-channel shared-mode stream that Windows spreads over eight outputs
// however its mixer sees fit, PipeWire and ALSA likewise, and Core Audio
// refuses outright - a HAL device has no engine layer to do the reconciling
// (src/audio/src/backend/macos/monitor.cpp's own header says why). None of
// that is what a player wants. Which speaker a rendered channel comes out of
// is the room's business, and the renderer already has the type that records
// it: ac3::render::Routing.
//
// So this opens the stream at the DEVICE's channel count and writes each
// rendered channel to the output the patch names, with silence in the outputs
// no channel is patched to. The platform is then never asked to widen
// anything, Core Audio's exact-width requirement is met by construction
// rather than worked around, and a patch with two channels swapped comes out
// of the swapped speakers - which is what A2's exit asks for.
//
// The default patch is built from the device's speaker mask where the backend
// reports one, so a 5.1 programme lands on a 7.1 device's front, centre, LFE
// and surround outputs and leaves its rear pair silent, rather than counting
// outputs off from zero and hoping the orders agree. See speaker_routing().
//
// Everything else - the queue, the position, flush, pause - is MonitorSink's,
// forwarded unchanged. Per-output trim and delay are deliberately NOT applied
// here: they are ac3::render::TrimDelay's, applied to the rendered channels
// before they are submitted, so one pass covers every output whatever it is
// plugged into.

namespace ac3::audio {

// How wide to open for `device`: its own channel count where the backend can
// say, else `rendered_channels`. RenderDeviceInfo::channels is 0 for "cannot
// say" rather than "no channels", and the caller's own width is then the only
// number there is.
[[nodiscard]] std::uint16_t output_width(const RenderDeviceInfo& device,
                                          std::uint16_t rendered_channels);

// The patch to start from: each of the layout's slots to the output carrying
// the same speaker, from the device's channel mask (speakers.hpp's
// locations_of gives the mask's speakers in the order an interleaved stream
// carries them). Left unpatched - rendered audio that will not be heard,
// which Routing::unpatched_channels() reports and a settings page should
// say - are a slot whose speaker the device has not got, an empty slot, and
// a slot placed by angle rather than named by a speaker, which has nothing
// to match against and is the caller's own patch to make.
//
// With no mask - a backend that cannot say - the standard arrangement for the
// device's width is assumed instead (speakers.hpp's default_speakers, which
// RenderDeviceInfo::speakers names as the most a caller can assume from the
// width alone), and the same matching runs against that. Counting outputs off
// from zero would be wrong, not merely uninformed: a rendered programme's
// slots are in the coded channel order - L C R Ls Rs, with the LFE feed last -
// while a device's outputs are in WAVEFORMATEXTENSIBLE's, so on a 5.1 device
// the identity would send the centre to the right speaker and the LFE to a
// surround. Only a width no single arrangement fits (ten channels is 5.1.4 or
// 7.1.2, and nothing says which) falls back to the identity, there being
// nothing left to match against.
[[nodiscard]] render::Routing speaker_routing(const render::OutputLayout& layout,
                                               std::uint32_t speakers, std::uint16_t outputs);

// What start() settled on, which a settings page or a log should show: a
// caller asks for a device and a layout, and this says what it got.
struct PcmOutputInfo {
    std::string device_id;
    std::string device_name;
    // The stream's width, which is the device's own where it says.
    std::uint16_t outputs = 0;
    // Whether `outputs` came from the device or from the layout's slot count
    // because the backend could not say.
    bool from_device = false;
    // The device's speaker mask, 0 where it does not say (speakers.hpp).
    std::uint32_t speakers = 0;
    std::uint32_t sample_rate = 0;
};

class PcmOutput {
public:
    PcmOutput();
    ~PcmOutput();
    PcmOutput(const PcmOutput&) = delete;
    PcmOutput& operator=(const PcmOutput&) = delete;

    // Opens `device_id` at the device's own channel count and `sample_rate`,
    // ready to take blocks of `layout`'s slots, patched by
    // speaker_routing(). `low_latency` is MonitorSink's.
    //
    // An empty `device_id` selects the endpoint the enumeration marks
    // default, by its id, rather than leaving "default" to the platform: the
    // width and the speaker mask everything else is built from come from that
    // record, so the same endpoint has to be the one opened. The two are the
    // same thing on Windows, Core Audio and PipeWire; on ALSA the enumeration
    // prefers a digital output, while the name "default" is whatever the
    // user's own configuration routes it to - pass that name explicitly to
    // get it. Where nothing could be enumerated at all, the platform's own
    // default is opened and the layout's width is used.
    //
    // kDeviceNotFound if the endpoint is not there, or is wider than a patch
    // can name (ac3::render::Routing::kMaxOutputs) - such a device cannot be
    // driven this way at all, and opening it narrower would hand the widening
    // back to the platform mixer this class exists to avoid. kComFailure for
    // a layout with no slots, there being no stream to open for it.
    [[nodiscard]] std::expected<PcmOutputInfo, MonitorError> start(
        const std::string& device_id, std::uint32_t sample_rate,
        const render::OutputLayout& layout, bool low_latency = false);

    void stop();
    [[nodiscard]] bool running() const;
    [[nodiscard]] const PcmOutputInfo& info() const;

    // Replaces the patch, between blocks. False, changing nothing, for a
    // patch whose output count is not the open stream's - a patch built for
    // another device.
    bool set_routing(const render::Routing& routing);
    [[nodiscard]] const render::Routing& routing() const;

    // Queues `frames` frames of `rendered` - one span per rendered channel,
    // each at least `frames` long - placed by the patch. False, having queued
    // nothing, when the queue is full (the caller is ahead of real time and
    // should wait), when nothing is open, or when a patched channel is
    // shorter than `frames`. A channel `rendered` does not carry at all is
    // silence, the same as an unpatched one.
    bool submit(std::span<const std::span<const float>> rendered, std::size_t frames);
    [[nodiscard]] bool can_submit() const;

    // MonitorSink's own, forwarded; see monitor.hpp for what each promises.
    [[nodiscard]] std::optional<MonitorPosition> position() const;
    void flush();
    [[nodiscard]] std::expected<void, MonitorError> pause();
    [[nodiscard]] std::expected<void, MonitorError> resume();
    [[nodiscard]] bool paused() const;
    [[nodiscard]] MonitorStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::audio
