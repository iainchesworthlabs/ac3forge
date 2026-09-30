# Quick start

Choose the path that matches what you want to run:

| Goal | Instructions |
|---|---|
| Install the `ac3cli` and `ac3gui` applications | [Install Forge](forge/index.md#installing) |
| Build the library, CLI, and tests from source | Continue below |
| Install or build an ESP32 network player | [Sink firmware](hearth/sink-firmware.md) for a published image, or [an ESP32-S3 sink](hearth/sink-esp32-s3.md) or [an ESP32-C6 sink](hearth/sink-esp32-c6.md) to build one |
| Use another platform or interface | [Choose a platform](platforms/index.md) |

## Build from source

This path builds the library, CLI, and tests on Windows or Linux. See
[Building from source](building.md) for macOS, all presets and options, and troubleshooting.

### Prerequisites

| | Version | Notes |
|---|---|---|
| A compiler | MSVC (VS 2026), clang-cl 22, GCC 16, or Clang 22 | C++23. One preset per compiler. |
| CMake | ≥ 3.28 | |
| Ninja | any recent | The presets hard-code the Ninja generator. |
| [vcpkg](https://github.com/microsoft/vcpkg) | any recent, with `VCPKG_ROOT` set | The presets read it. Supplies fmt, Catch2 and the libraries of the default Hearth build (plus Boost/Tracy only for the opt-in `adm`/`profiling` features — see [building.md](building.md)). |
| Qt | 6.5+ prebuilt for the GUI; 6.8+ for Hearth's window and Crucible, which are skipped without it | Not needed for the library and CLI. The Windows x64 presets build the GUI, so they need it or `-DAC3FORGE_BUILD_GUI=OFF`. Never from vcpkg — see [building.md](building.md). |

### Configure, build, and test

=== "Windows"

    From any shell — a Developer PowerShell is not required, the presets chainload the MSVC
    environment themselves:

    ```bash
    cmake --preset config-windows-msvc-debug
    cmake --build --preset build-windows-msvc-debug
    ctest --preset test-windows-msvc-debug
    ```

    Swap `msvc` for `llvm` to build with clang-cl instead.

=== "Linux"

    ```bash
    export VCPKG_ROOT=/path/to/vcpkg
    cmake --preset config-linux-gcc-debug
    cmake --build --preset build-linux-gcc-debug
    ctest --preset test-linux-gcc-debug
    ```

    Swap `gcc` for `llvm` to build with Clang. The GUI defaults off on Linux; enable it with
    `-DAC3FORGE_BUILD_GUI=ON`.

The vcpkg toolchain supplies fmt, Catch2 and Hearth's libraries, the preset selects the compiler,
and `ctest` runs the test suite and compiled examples. The tools are written to
`build/<preset>/bin/`. [Building from source](building.md) covers release presets, workflow
presets, and compiler-detection errors.

## Encode and decode from the command line

`ac3cli` reads the format from the stream, so one `decode` command takes AC-3, E-AC-3 or AC-4.
With a 48 kHz stereo `in.wav`:

```bash
ac3cli encode in.wav out.ac3 448 couple
ac3cli ac4-encode in.wav out.ac4 192
ac3cli probe out.ac4
ac3cli decode out.ac4 out.wav
```

`ac3cli help` lists every command and `ac3cli help <command>` gives one command's options; the
[CLI reference](forge/cli/index.md) has the grammars.

## Call the library from C++

Two headers and about a dozen lines to encode a frame. This is excerpted from
[`examples/encode_ac3.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/encode_ac3.cpp),
with the error handling elided:

```cpp
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"

// Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history state.
auto encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
    .bitrate_kbps = 448,
    .acmod = ac3::Acmod::k3_2,  // L, C, R, SL, SR
    .lfe = true,
});

// Table 5.8 order, LFE last, exactly kSamplesPerFrame (1536) samples each.
std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
// encode_frame takes a span of spans, so the views must outlive the call.
const std::vector<std::span<const float>> views{pcm.begin(), pcm.end()};

for (int frame = 0; frame < 31; ++frame) {
    fill_with_audio(pcm, frame, 48000.0);
    if (const auto encoded = encoder->encode_frame(views)) {
        write(stream, *encoded);  // one complete syncframe
    }
}
```

## Next steps

- [Library](library/index.md) — C++ API, modules, and package integration.
- [CLI reference](forge/cli/index.md) — encode, decode, inspect, and validate from a shell.
- [GUI guide](forge/gui/index.md) — file and live audio, objects, metadata, and quality checks.
- [Crucible](crucible/index.md) — capture and position desktop applications.
- [Hearth](hearth/index.md) — ESP32 network playback and desktop-player status.
