#pragma once

#include <memory>

#include "audio_devices.hpp"
#include "default_device.hpp"
#include "foreground.hpp"
#include "session_monitor.hpp"
#include "virtual_device.hpp"

// The one place the application asks for the machine it is running on.
//
// Exactly one platform/<os>/services.cpp defines these, chosen by CMake the
// way src/audio/src/backend/<os>/ is chosen for the library
// (docs/platforms/raspberry-pi.md, "Why there's no Raspberry Pi-specific
// code"). Nothing above this header names an operating system, and no
// #ifdef selects between them: a platform is a directory.
//
// Each returns a working object on every platform, never null. Where a
// platform cannot do something - Wayland cannot say what is full-screen,
// macOS has no silent device to install - the object returned says so
// through its own support()/state() rather than being absent, so the UI can
// explain the gap instead of the code having to test for a hole.
//
// tests/crucible/platform_services_stub.cpp provides the same five for a
// build with no platform half at all, which is what lets the engine's tests
// run on a Linux CI leg that has no audio hardware.

namespace ac3::crucible {

// Render endpoints, sinks and taps: the production set for this platform.
[[nodiscard]] std::shared_ptr<AudioDevices> platform_audio_devices();

// Who is playing sound.
[[nodiscard]] std::shared_ptr<SessionMonitor> platform_session_monitor();

// What is full-screen in front.
[[nodiscard]] std::shared_ptr<Foreground> platform_foreground();

// The system default output, and moving it.
[[nodiscard]] std::shared_ptr<DefaultDevice> platform_default_device();

// The silent device applications play into.
[[nodiscard]] std::shared_ptr<VirtualDevice> platform_virtual_device();

}  // namespace ac3::crucible
