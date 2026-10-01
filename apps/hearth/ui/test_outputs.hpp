#pragma once

#include <functional>
#include <memory>

#include "iclforge/audio/passthrough.hpp"
#include "output_selector.hpp"
#include "pcm_sink.hpp"

// The outputs HearthController::start() and refreshOutputDevices() use in
// place of this machine's own, set only through
// HearthController::set_test_outputs() - the Qt Quick suites'
// qml_test_main.cpp, never the shipped window. The same seam shape
// CrucibleController::set_test_services() gives the Crucible suites, for the
// same reason: a headless runner has no audio device to play into (no
// PipeWire daemon on a CI container, and a developer's own machine should
// not start sounding test tones), so a suite that wants to see the engine
// actually play - position advancing, meters moving, the output picker
// moving playback to another endpoint - hands the controller a fake device
// with a clock of its own, the way tests/hearth/test_engine.cpp's
// ClockedDevice stands in for one under iclforge-tests.
//
// Kept out of hearth_controller.hpp (which forward-declares it) for the
// reason that header gives for every iclforge::hearth type: pcm_sink.hpp reaches
// iclforge::render::OutputLayout, whose slots() collides with Qt's `slots` macro.

namespace iclforge::hearth::ui {

struct TestOutputs {
    // Builds the engine's PCM sink - called once, from start().
    std::function<std::unique_ptr<iclforge::hearth::PcmSink>()> make_pcm;
    // The render endpoints the engine's output decision reads
    // (EngineOutputs::endpoints), in place of device_endpoints().
    iclforge::hearth::EndpointSource endpoints;
    // What refreshOutputDevices() lists, in place of
    // iclforge::audio::enumerate_render_devices().
    std::function<decltype(iclforge::audio::enumerate_render_devices())()> enumerate;
};

}  // namespace iclforge::hearth::ui
