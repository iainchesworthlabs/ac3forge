#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The fakes qml_test_main.cpp's TestServices hands the real controllers: a
// room of fake render endpoints with a clock of their own (for
// HearthController::set_test_outputs()), and an in-process Hearth test sink
// (apps/hearth/testsink) announced to NetworkController's own NetworkSinks as
// if mDNS had found it.
//
// Qt-free, and free of every ac3::render/ac3::hearth header too: those reach
// ac3::render::OutputLayout, whose slots() collides with the `slots` macro
// qml_test_main.cpp's Qt headers define (hearth_controller.cpp's own #undef
// comment). test_room.cpp is where the real headers are; this file only
// forward-declares what it passes through.

namespace ac3::hearth {
class NetworkSinks;
}
namespace ac3::hearth::ui {
struct TestOutputs;
}

namespace ac3::hearth::uitest {

struct FakeEndpoint {
    std::string id;
    std::string name;
    bool is_default = false;
    std::uint16_t channels = 2;
    // 0 for "the standard arrangement for the width" (default_speakers()).
    std::uint32_t speakers = 0;
    bool passthrough = false;
};

// What the fake device has been asked to do so far - read by a suite to
// prove the engine really played into it, not just that a property changed.
struct RoomReading {
    bool open = false;
    bool paused = false;
    std::string endpoint;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint32_t opens = 0;
    std::uint64_t frames_heard = 0;
    // The largest |sample| submitted since reset_peak(), across every slot -
    // non-zero means decoded audio, not silence, reached the device.
    double peak = 0.0;
};

class FakeRoom;

[[nodiscard]] std::shared_ptr<FakeRoom> make_room(std::vector<FakeEndpoint> endpoints);
// The TestOutputs over `room` - one PCM sink whose open() plays into
// whichever endpoint the engine's output decision names.
[[nodiscard]] std::shared_ptr<ac3::hearth::ui::TestOutputs> outputs_for(const std::shared_ptr<FakeRoom>& room);
[[nodiscard]] RoomReading read(const FakeRoom& room);
void reset_peak(FakeRoom& room);
// The device clock's rate against real time (1.0 = real time).
void set_speed(FakeRoom& room, double speed);

// An in-process apps/hearth/testsink Sink on 127.0.0.1, dynamic six-digit
// codes, no mDNS advertisement, its state and output directories under
// `directory`.
class TestSinkHost;

[[nodiscard]] std::shared_ptr<TestSinkHost> start_test_sink(const std::string& name, const std::string& directory,
                                                            std::string* error);
// Hands `host` to `sinks` as a found `_sendspin._tcp` service (instance
// `host`'s name), which dials it.
void announce(ac3::hearth::NetworkSinks& sinks, const TestSinkHost& host);
// The last pairing code the sink printed, digits only; empty if none yet.
[[nodiscard]] std::string pairing_code(const TestSinkHost& host);
[[nodiscard]] std::vector<std::string> log_lines(const TestSinkHost& host);
[[nodiscard]] std::uint32_t connections(const TestSinkHost& host);

}  // namespace ac3::hearth::uitest
