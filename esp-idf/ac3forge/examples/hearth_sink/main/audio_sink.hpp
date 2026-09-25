#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "ac3forge/playout.hpp"

// Where decoded audio goes, as a seam CMake resolves rather than a flag the
// code branches on - the same rule the library follows for its own platform
// choices (tools/checks/check_platform_macros.ps1, and the arch/ and profile/
// directories under src/internal/).
//
// Four implementations, one chosen per build:
//
//   sink/i2s/      the real one for a part whose I2S TDM frame is 128 bits
//                  (the ESP32-S3, the ESP32-C6). Standard I2S or TDM,
//                  reconfiguring its own mode and slot count to whatever a
//                  layout needs (up to a hardware ceiling -
//                  ac3forge/sink_plan.hpp), across one or two of the part's
//                  I2S lines.
//   sink/i2s_wide/ the same arithmetic for a part whose one controller
//                  reaches the product's full channel target alone, at a
//                  wider frame (512 bits on the ESP32-P4) - never a second
//                  line, see that directory's own top-of-file comment.
//   sink/capture/  converts exactly as the real ones do and checks the
//                  result; what CI runs.
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
// Standard I2S carries one or two slots. Anything wider out of an ESP32-S3
// means TDM (driver/i2s_tdm.h): one data line packs at most 128 bits a frame -
// 4 slots at 32 bits - and the real sink reconfigures between the two modes
// itself as the layout asks for more or fewer channels (ac3forge/sink_plan.hpp),
// rather than a build picking one mode and staying there. The DAC has to
// speak whichever it gets; a PCM3168A speaks TDM, a SigmaDSP does on its
// serial inputs, the common stereo breakouts (MAX98357A, PCM5102) do not.
//
// TDM mode wants 24-bit samples in 32-bit slots, as many as the bus has.
// Standard mode wants one or two slots of 16 or 32 bits. If this interface
// carried interleaved int16_t - as it did for about an hour - reconfiguring
// between them would have meant changing the seam, and changing a seam is how
// the implementations behind it drift apart. Handing over planar float and
// letting the sink convert costs one pass over the samples
// (ac3forge/interleave.hpp) and settles the question.
//
// --- WHAT A WRITE IS ---------------------------------------------------------
//
// One BLOCK: one span per output slot of the player's layout, each
// ac3::kSamplesPerBlock (256) samples or fewer, six times a frame. A block
// rather than a frame because that is what the decoder's block form hands
// over and what keeps a sixteen-slot layout's storage at 16 KB rather than
// 96 KB - see esp-idf/ac3forge/include/ac3forge/player.hpp.
//
// --- NOTES THE REAL SINK'S TDM MODE WAS WRITTEN FROM -------------------------
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
//     ARITHMETIC at all (docs/platforms/bare-metal/esp32-s3.md) - so it could only help the
//     integer shuffle after conversion, which is not where the time goes. The
//     decode is the cost, and it is still unmeasured on hardware. Measure
//     before optimising this.

namespace player {

// `channels` is how many slots the sink will be given per write - the
// player's layout. Returns false if it cannot do that many - which is not a
// failure of the caller, just a limit of this sink, and the caller should stop
// and say so.
//
// Callable more than once: the real sink reconfigures its mode and slot count
// to whatever `channels` needs (ac3forge/sink_plan.hpp), between plays, so a
// layout change over the control surface never needs a rebuild. Not safe to
// call while a play is in progress - see hearth_sink.cpp's begin_play.
[[nodiscard]] bool sink_open(std::uint32_t sample_rate, int channels);

// The most slots this sink could ever be asked to carry - its hardware
// ceiling, not whatever it happens to be open for right now. A layout with no
// more than this many is accepted and reconfigures the sink at the next play
// if it differs from today's; a wider one is refused up front.
//
// It moves with the slot width below: an I2S line carries 128 bits a frame,
// so the same two lines reach sixteen slots at 16 bits and eight at 32.
[[nodiscard]] int sink_slots();

// The most slots this sink could carry at any setting it takes - at either
// slot width, with a second line where the part has a controller for one.
// sink_slots() is the ceiling for the settings in force; this is what a player
// sizes its buffers for once, so that a later setting never outgrows them.
[[nodiscard]] int sink_max_slots();

// The slot width sink_max_slots()'s own figure is reached at - 16 or 32 - for
// GET /hardware to report the two together rather than a bare count a reader
// cannot place (ac3forge::HardwareFacts::sink_max_slots_bits). 0 for a sink
// whose ceiling is not a function of slot width at all (capture, null): the
// report then falls back to the plain count, no qualifier guessed.
[[nodiscard]] int sink_max_slots_bit_width();

// Whether this sink can have a second line at all: only the I2S sink, and only
// on a part with a second I2S controller. On an ESP32-C6, which has one, a
// stored setting saying a second line is wired is not acted on, and the
// control surface does not offer to store one.
[[nodiscard]] bool sink_second_line_possible();

// The slot width in force, in bits, and setting it.
//
// Which width a board wants is a property of the DACs it is wired to, not of
// the image: the sink starts at the Kconfig default and takes 16 or 32 here.
// Refused (false, with a line saying why) for any other number. Not safe to
// call while a play is in progress, for the reason sink_open is not: a width
// change closes the lines so the next sink_open builds them again, and
// sink_slots() reports the new ceiling from the moment it returns - so a
// caller that keeps a layout open against the old one must re-check it.
//
// The sinks with no hardware behind them (capture, null) report the width
// they were built for and refuse to change it: nothing about their output
// depends on it that a rebuild does not already decide.
[[nodiscard]] int sink_slot_bits();
[[nodiscard]] bool sink_set_slot_bits(int bits);

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

// One block of exactly ac3::kSamplesPerBlock samples a slot, as sink_write,
// for a player that times its output (the Sendspin player,
// ac3forge/playout.hpp): when the block's first sample leaves the audio port,
// by the sink's own clock, whether the block was written too late to play
// whole, and whether the queue ran dry before it. The i2s sink reads that
// from its DMA ring's end-of-frame interrupts. The capture sink has no ring,
// so it says nothing unless CONFIG_AC3FORGE_EXAMPLE_CAPTURE_PACED asks it to
// pace itself as one would and report its times, which under QEMU are the
// emulator's; the null sink always does. Nothing when the sink is not open.
[[nodiscard]] std::optional<ac3forge::PlayoutWrite> sink_write_timed(std::span<const std::span<const float>> channels);

// For the log line, so a run says which sink produced its numbers.
[[nodiscard]] const char* sink_name();

// Blocks accepted since sink_open, across every play. The null sink's reason
// for existing: it gives CI something to gate on that the real sink cannot
// report.
[[nodiscard]] std::uint64_t sink_frames_written();

// Flash mode (planning/esp32-ota.md): every line closed and its DMA buffers
// given back, so nothing reaches the DACs until the board restarts. Not safe to
// call while a play is in progress, for the reason sink_open is not. A later
// sink_open would open the lines again, though nothing calls one after this:
// every way out of flash mode is a restart. The sinks with no hardware behind
// them have nothing to close.
void sink_close();

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
