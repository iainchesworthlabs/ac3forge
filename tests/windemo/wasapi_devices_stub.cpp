#include <memory>

#include "audio_devices.hpp"

// ac3tests compiles the demo's tap pool and output stage on every platform
// (tests/CMakeLists.txt); both fall back to wasapi_devices() when handed no
// devices, a path the tests never take. This definition satisfies the
// linker where the Windows platform directory is not built, and answers
// with nothing if it is ever reached.

namespace ac3::windemo {

namespace {

class NoDevices final : public AudioDevices {
public:
    std::vector<DeviceFacts> render_devices(std::uint32_t) override { return {}; }
    std::unique_ptr<BurstSink> burst_sink() override { return nullptr; }
    std::unique_ptr<PcmSink> pcm_sink() override { return nullptr; }
    std::unique_ptr<ObjectSink> object_sink() override { return nullptr; }
    std::unique_ptr<TapSource> tap() override { return nullptr; }
};

}  // namespace

std::shared_ptr<AudioDevices> wasapi_devices() {
    return std::make_shared<NoDevices>();
}

}  // namespace ac3::windemo
