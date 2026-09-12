#pragma once

#include <cstdint>
#include <span>

// Where decoded audio goes, as a seam CMake resolves rather than a flag the
// code branches on - the same rule the library follows for its own platform
// choices (tools/checks/check_platform_macros.ps1, and the arch/ and profile/
// directories under src/internal/).
//
// Four implementations, one chosen per build:
//
//   sink/i2s/      the real one. Standard I2S, two slots, 32-bit by default.
//   sink/tdm/      multi-channel on one data line, up to sixteen slots.
//   sink/capture/  converts exactly as the two above do and checks the result;
//                  what CI runs.
//   sink/null/     counts what it is given and returns.
//
// The last two stand in for a peripheral qemu-system-xtensa does not have:
// with a real sink the first write blocks on a DMA that never drains and the
// job times out. Neither is a stub that skips the work - each is a SINK,
// called exactly as often and with exactly the same audio, so the player and
// its timing arithmetic are still exercised. What they cannot do is prove a DAC
// makes a noise, and nothing running without hardware can.
//
// Selected in Kconfig (main/Kconfig.projbuild), read by main/CMakeLists.txt.
// Not by C++: the player does not know which sink it has, which is the point.
//
// --- WHY THE INTERFACE IS PLANAR FLOAT --------------------------------------
//
// Because interleaving and sample format are the SINK's business, and the sinks
// do both differently.
//
// Standard I2S carries two slots. Anything wider out of an ESP32-S3 means TDM
// (driver/i2s_tdm.h): the S3's I2S packs up to 16 slots onto one data line, so
// 8 channels needs three pins - BCLK, WS and DATA - rather than four data lines,
// and 8 slots of 32 bits at 48 kHz is a 12.3 MHz bit clock, well inside what it
// will do. The DAC has to speak TDM; a PCM3168A does, a SigmaDSP does, the
// common stereo breakouts (MAX98357A, PCM5102) do not.
//
// That sink wants 24-bit samples in 32-bit slots, as many as the bus has. The
// stereo one wants two slots of 16 or 32 bits. If this interface carried
// interleaved int16_t - as it did for about an hour - adding the TDM sink would
// have meant changing the seam, and changing a seam is how the implementations
// behind it drift apart. Handing over planar float and letting each sink
// convert costs one pass over the samples (ac3forge/interleave.hpp, shared by
// all of them) and settles the question.
//
// --- WHAT A WRITE IS ---------------------------------------------------------
//
// One BLOCK: one span per output slot of the player's layout, each
// ac3::kSamplesPerBlock (256) samples or fewer, six times a frame. A block
// rather than a frame because that is what the decoder's block form hands
// over and what keeps a sixteen-slot layout's storage at 16 KB rather than
// 96 KB - see esp-idf/ac3forge/include/ac3forge/player.hpp.
//
// --- NOTES THE TDM SINK WAS WRITTEN FROM ------------------------------------
//
// Still untested against a TDM DAC, so still worth keeping in one place.
//
//   * Slots are FIXED WIDTH. A 5.1 layout on an 8-slot bus has to write zeros
//     into slots 6 and 7 every block, not leave them; whatever was in the DMA
//     buffer last time is what the DAC will otherwise clock out, which is
//     digital noise on two channels nobody is watching.
//
//   * 24-bit in 32-bit slots is the usual arrangement, so the conversion is to
//     int32_t, not int16_t. Sixteen slots of 32 bits at 48 kHz is a 24.6 MHz
//     bit clock, inside what the S3 will do; whether the DAC follows is its
//     datasheet's business.
//
//   * The driver caps a DMA descriptor at 4,092 bytes and quietly shortens one
//     that asks for more. The two I2S sinks size their descriptors from the
//     bus width (sink/sink_common.hpp) so the configured depth survives.
//
//   * i2s_channel_write COPIES into the driver's own descriptors, so the
//     buffer handed to it does NOT need MALLOC_CAP_DMA. That requirement
//     belongs to the zero-copy paths (i2s_channel_preload_data, or taking the
//     DMA buffer directly). Reaching for heap_caps_malloc here would put an
//     allocation into a decode path this profile exists to keep free of them -
//     a static array in .bss is internal SRAM already, and with PSRAM off there
//     is nothing else it could be.
//
//   * The interleave is unlikely to be worth vectorising, and PIE is a trap
//     worth naming. Roughly 12,000 stores per 32 ms frame at 8 channels is
//     under 1% of a 240 MHz core, and the S3's PIE does no floating-point
//     ARITHMETIC at all (docs/platforms/esp32.md) - so it could only help the
//     integer shuffle after conversion, which is not where the time goes. The
//     decode is the cost, and it is still unmeasured on hardware. Measure
//     before optimising this.

namespace player {

// `channels` is how many slots the sink will be given per write - the
// player's layout. Returns false if it cannot do that many - which is not a
// failure of the caller, just a limit of this sink, and the caller should stop
// and say so.
[[nodiscard]] bool sink_open(std::uint32_t sample_rate, int channels);

// How many slots the bus has, once open: a layout with no more than this many
// can replace the one the sink was opened with (the TDM sink pads what a
// narrower layout leaves), a wider one cannot.
[[nodiscard]] int sink_slots();

// A play is beginning, and the next write is its first block. Called with no
// write in progress: the player that made the last play has stopped, and the
// next has not started. From here on sink_report() is about this play alone;
// sink_frames_written() goes on counting from sink_open.
//
// For the two sinks with a DAC it restarts the model of the DMA queue
// (ac3forge/dac_queue_model.hpp), and the play's first block is exempt from
// its underrun count, as the first block after sink_open always was: the
// queue has been draining since the last play ended, and the time between two
// plays is not a gap in either of them. sink/capture/ checks the new play's
// samples from zero, and sink/null/ has nothing to restart.
void sink_begin_play();

// One block: one span per slot, each up to ac3::kSamplesPerBlock samples,
// nominally in [-1, 1). Blocks until the sink has taken it, which for I2S is
// the back-pressure that paces the whole player at real time.
void sink_write(std::span<const std::span<const float>> channels);

// For the log line, so a run says which sink produced its numbers.
[[nodiscard]] const char* sink_name();

// Blocks accepted since sink_open, across every play. The null sink's reason
// for existing: it gives CI something to gate on that the real sink cannot
// report.
[[nodiscard]] std::uint64_t sink_frames_written();

// What the sink has to say about the play so far, as key=value lines: at the
// end of a play, and with each progress line.
//
// The two sinks with a DAC say whether the samples reached it in time, from
// the queue model in ac3forge/dac_queue_model.hpp; what the DAC then did with
// them is not something this side of the wire can report. sink/capture/ is
// where this earns its place - it runs the same conversion the real sinks run
// and then checks the result, so CI has something to gate on that is about
// the AUDIO rather than about the loop having turned over. sink/null/ says
// nothing.
void sink_report();

}  // namespace player
