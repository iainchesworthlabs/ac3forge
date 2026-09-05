// Asks every render endpoint the library can see whether it will take an
// AC-3 or E-AC-3 bitstream, which is roadmap DR9's per-backend question and,
// since Crucible on Linux cannot use ALSA, the question that decides whether
// Crucible has anywhere to send its output there
// (docs/crucible/promotion.md, "ALSA or PipeWire").
//
// enumerate_render_devices() is the same call the output stage makes, so a
// "yes" here is the "yes" it would get. On PipeWire that answer comes from
// the sink's iec958.codecs - the session manager's reading of the display's
// EDID - and never from a connect alone: the adapter accepts an IEC 958
// stream on a headphone jack as readily as on a receiver, and the first run
// of this tool on the Raspberry Pi said YES to the jack. With a device id and
// a file, it then streams that file's elementary stream to the device as
// IEC 61937 bursts, looping, for as long as asked - which is what lets the
// receiver's own display say what it locked to. That display is the only
// oracle for "did the bitstream arrive intact"; nothing on this side can see
// it.
//
// Not a CMake target: built by hand on the machine with the session, against
// a PipeWire build of the library in build-pw/. One command, wrapped here; as
// run on the Raspberry Pi:
//
//   g++ -std=c++23 -O1 -o /tmp/ptprobe tools/checks/passthrough_probe.cpp
//       -Isrc/audio/include -Isrc/forge/include -Ibuild-pw/src/forge/generated
//       $(pkg-config --cflags libpipewire-0.3) -DAC3FORGE_STATIC_DEFINE
//       build-pw/src/audio/libac3audio.a build-pw/src/forge/libac3forge_static.a
//       build-pw/vcpkg_installed/arm64-linux/lib/libfmt.a
//       $(pkg-config --libs libpipewire-0.3) -lpthread
//
//   passthrough_probe                         list endpoints and what they accept
//   passthrough_probe <id> <file.ec3|.ac3> [seconds]   bitstream it, looping (default 25 s)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "ac3/audio/passthrough.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/iec61937/iec61937.hpp"

namespace {

void submit_paced(ac3::audio::PassthroughSink& sink, std::span<const std::byte> burst) {
    // submit() returns false when the sink is ahead of real time; the
    // caller waits, exactly as the sink documents.
    while (!sink.submit(burst)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

}  // namespace

int main(int argc, char** argv) {
    const auto devices = ac3::audio::enumerate_render_devices(48000);
    if (!devices) {
        std::printf("enumerate_render_devices refused: %s\n",
                    std::string(ac3::audio::describe(devices.error())).c_str());
        return 1;
    }
    std::printf("%zu render endpoint(s)\n", devices->size());
    for (const auto& d : *devices) {
        std::printf("  %s%-48s ac3=%-3s eac3=%-3s exclusive-pcm=%-3s  \"%s\"\n",
                    d.is_default ? "*" : " ", d.id.c_str(),
                    d.supports_ac3_passthrough ? "YES" : "no",
                    d.supports_eac3_passthrough ? "YES" : "no",
                    d.supports_exclusive_pcm ? "yes" : "no", d.name.c_str());
    }
    if (argc < 3) {
        return 0;
    }

    const std::string id = argv[1];
    const std::string path = argv[2];
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 25;

    std::ifstream in(path, std::ios::binary);
    std::vector<std::byte> stream;
    for (std::istreambuf_iterator<char> it(in), end; it != end; ++it) {
        stream.push_back(static_cast<std::byte>(*it));
    }
    if (stream.empty()) {
        std::printf("cannot read %s\n", path.c_str());
        return 2;
    }
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        std::printf("%s holds no syncframe\n", path.c_str());
        return 2;
    }
    const bool eac3 = *bsid > 8;

    // Whole access units for E-AC-3 (an independent substream's frame plus
    // any dependents), which is the granularity the burst packer wants;
    // single syncframes for AC-3, each of which is its own burst.
    const auto units = eac3 ? ac3::split_access_units(stream) : ac3::split_frames(stream);
    if (!units || units->empty()) {
        std::printf("could not split %s into frames\n", path.c_str());
        return 2;
    }
    std::printf("\n%s: %s, %zu %s; bitstreaming to %s for %d s ...\n", path.c_str(),
                eac3 ? "E-AC-3" : "AC-3", units->size(), eac3 ? "access units" : "syncframes",
                id.c_str(), seconds);

    ac3::audio::PassthroughSink sink;
    const auto started = sink.start(id, 48000,
                                    eac3 ? ac3::audio::BitstreamFormat::kEac3
                                         : ac3::audio::BitstreamFormat::kAc3);
    if (!started) {
        std::printf("start refused: %s\n", std::string(ac3::audio::describe(started.error())).c_str());
        return 3;
    }

    ac3::iec61937::Eac3BurstPacker packer;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::size_t bursts = 0;
    std::size_t loops = 0;
    while (std::chrono::steady_clock::now() < until) {
        for (const auto& unit : *units) {
            if (std::chrono::steady_clock::now() >= until) {
                break;
            }
            if (eac3) {
                const auto burst = packer.push(unit);
                if (!burst) {
                    std::printf("packer refused an access unit\n");
                    sink.stop();
                    return 4;
                }
                if (*burst) {
                    submit_paced(sink, **burst);
                    ++bursts;
                }
            } else {
                const auto burst = ac3::iec61937::wrap_frame(unit);
                if (!burst) {
                    std::printf("wrap_frame refused a syncframe\n");
                    sink.stop();
                    return 4;
                }
                submit_paced(sink, *burst);
                ++bursts;
            }
        }
        ++loops;
    }
    const auto stats = sink.stats();
    sink.stop();
    std::printf("done: %zu bursts over %zu loop(s); sink reports %llu submitted, %llu rendered, "
                "%llu underruns\n",
                bursts, loops, static_cast<unsigned long long>(stats.bursts_submitted),
                static_cast<unsigned long long>(stats.bursts_rendered),
                static_cast<unsigned long long>(stats.underruns));
    return 0;
}
