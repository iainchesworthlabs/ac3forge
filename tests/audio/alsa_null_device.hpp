#pragma once

#include <alsa/asoundlib.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

// Software ALSA devices for the ALSA backend's tests, so its success paths run
// on a machine with no sound card at all.
//
// alsa-lib resolves every PCM name through its configuration, and that
// configuration can define devices made of nothing but software plugins. The
// built-in `null` plugin is the useful one: it opens, negotiates any format,
// rate and width it is offered, accepts every write and delivers silence on
// every read, without a card, a kernel driver or a sound server behind it. A
// handful of the other plugins then shape it into the device a case needs:
//
//   * `file` with an `infile` replays a file into a capture stream, which is
//     how a case feeds a capture known samples (or an IEC 61937 carrier) and
//     checks what arrives; for playback, pointed at a path that cannot be
//     created (the file is opened lazily, so the PCM opens and then every
//     flush fails with EIO, which snd_pcm_recover cannot mend) it is a
//     device that goes away;
//   * `route` fixes the slave at S16_LE, which leaves S32_LE as the first
//     format this backend's preference order finds the client side offering;
//   * `mulaw`/`lfloat` over an S16_LE slave offer only MU_LAW/float, so no
//     format this backend reads, or no S16_LE carrier, is on offer;
//   * `multi` over null is a device that drops into XRUN as soon as its start
//     threshold starts it, so every buffer's worth ends in -EPIPE and a
//     snd_pcm_recover - the under-run path, deterministically; with one bound
//     channel it is also the one-channel device;
//   * `hw` naming a card that does not exist fails to open with ENOENT.
//
// Not used, and deliberately: `share` would pin a device at S16_LE and under-run
// after every write (the xrun/recover path), but alsa-lib 1.2.11's share
// plugin writes to freed memory in snd_pcm_close and crashes outright on a
// capture open, so a case built on it would test alsa-lib's heap instead.
//
// Nothing here fakes a SOUND CARD, and nothing can from userspace: the
// card/device walk (snd_card_next) looks for /dev/snd/controlC<N> directly, so
// enumerate_render_devices() stays empty and the default capture entry stays
// the only one. What is reachable is every path that takes an ALSA device NAME.
//
// Two ways in. In-process, AlsaConfigScope points ALSA_CONFIG_PATH at a file
// for its lifetime - alsa-lib re-reads the variable on every snd_config_update
// (which every snd_pcm_open makes), so setting it before the first open is
// enough and restoring it afterwards leaves the next case where it found it.
// For ac3cli run as a subprocess, env_prefix() is the same assignment spelled
// for the front of a POSIX shell command.

namespace ac3test::alsa_null {

namespace fs = std::filesystem;

// The stock configuration's own path: the top of what alsa-lib would load with
// no ALSA_CONFIG_PATH at all, so every name it defines ("plug:", "hw:", the
// iec958/hdmi templates) still resolves beside the devices defined here.
inline std::string stock_config() {
    return (fs::path{snd_config_topdir()} / "alsa.conf").string();
}

// The one playback device that fails for good: the file plugin's output
// cannot be created, so its first flush fails with EIO. Nothing is ever
// written anywhere - in particular not to /dev/full, which a test running as
// root must never be pointed at by a writer that removes what it failed to
// fill (RecordingSink does, for an empty take).
inline constexpr std::string_view kFailingPlayback =
    "pcm.full { type file slave.pcm \"null\" file \"/dev/null/unwritable\" format raw }\n";

// The null devices every configuration below starts from.
inline constexpr std::string_view kNullDevices =
    "pcm.!default { type null }\n"
    "pcm.null { type null }\n";

// Named software devices for the in-process backend cases. Every one is built
// from `null` and none needs a card; see the header comment for what each
// plugin contributes.
inline std::string named_devices(const fs::path& infile_float, const fs::path& infile_s32) {
    std::string text{kNullDevices};
    text += kFailingPlayback;
    text += "pcm.s32 { type route slave { pcm \"null\" format S16_LE channels 2 } "
            "ttable.0.0 1 ttable.1.1 1 }\n";
    text += "pcm.mulawonly { type mulaw slave { pcm \"null\" format S16_LE } }\n";
    text += "pcm.floatonly { type lfloat slave { pcm \"null\" format S16_LE } }\n";
    text += "pcm.mono { type multi slaves.a { pcm \"null\" channels 1 } "
            "bindings.0 { slave a channel 0 } }\n";
    text += "pcm.stereo_xrun { type multi slaves.a { pcm \"null\" channels 2 } "
            "bindings.0 { slave a channel 0 } bindings.1 { slave a channel 1 } }\n";
    text += "pcm.nocard { type hw card 31 }\n";
    text += "pcm.replay_float { type file slave.pcm \"null\" file \"/dev/null\" infile \"" +
            infile_float.string() + "\" format raw }\n";
    text += "pcm.replay_s32 { type file slave { pcm \"s32\" } file \"/dev/null\" infile \"" +
            infile_s32.string() + "\" format raw }\n";
    // The iec958/hdmi templates the stock configuration defines per card,
    // replaced by argument-taking devices that resolve to `null` whatever the
    // arguments: the four AES channel-status bytes passthrough_device_name()
    // appends then go through alsa-lib's real argument parsing.
    for (const char* name : {"iec958", "hdmi"}) {
        text += std::string{"pcm.!"} + name +
                " { @args [ CARD DEV AES0 AES1 AES2 AES3 ] "
                "@args.CARD { type string default \"0\" } "
                "@args.DEV { type integer default 0 } "
                "@args.AES0 { type integer default 4 } "
                "@args.AES1 { type integer default 130 } "
                "@args.AES2 { type integer default 0 } "
                "@args.AES3 { type integer default 2 } "
                "type null }\n";
    }
    return text;
}

// A directory of this test process's own under the shared scratch root.
inline fs::path scratch_dir(std::string_view leaf) {
#ifdef _WIN32
    const auto pid = std::to_string(_getpid());
#else
    const auto pid = std::to_string(getpid());
#endif
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / (std::string{leaf} + "_" + pid);
    fs::create_directories(dir);
    return dir;
}

// Writes `body` to `path` as an alsa-lib configuration file.
inline fs::path write_config(const fs::path& path, std::string_view body) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    REQUIRE(out.is_open());
    out << body;
    out.close();
    REQUIRE(out.good());
    return path;
}

// ALSA_CONFIG_PATH's value for a configuration file: the stock one first, so
// `body` adds to it and overrides it ("pcm.!default") rather than replacing it.
inline std::string config_path_value(const fs::path& config) {
    return stock_config() + ":" + config.string();
}

// "ALSA_CONFIG_PATH='...' " - for the front of a subprocess's command line.
inline std::string env_prefix(const fs::path& config) {
    return "ALSA_CONFIG_PATH='" + config_path_value(config) + "' ";
}

// Points this process's alsa-lib at `config` until destroyed.
class AlsaConfigScope {
public:
    explicit AlsaConfigScope(const fs::path& config) {
        if (const char* previous = std::getenv("ALSA_CONFIG_PATH"); previous != nullptr) {
            previous_ = previous;
        }
        REQUIRE(::setenv("ALSA_CONFIG_PATH", config_path_value(config).c_str(), 1) == 0);
    }
    ~AlsaConfigScope() {
        if (previous_) {
            ::setenv("ALSA_CONFIG_PATH", previous_->c_str(), 1);
        } else {
            ::unsetenv("ALSA_CONFIG_PATH");
        }
    }
    AlsaConfigScope(const AlsaConfigScope&) = delete;
    AlsaConfigScope& operator=(const AlsaConfigScope&) = delete;

private:
    std::optional<std::string> previous_;
};

}  // namespace ac3test::alsa_null
