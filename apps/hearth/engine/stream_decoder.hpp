#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/render.hpp"
#include "ac3/render/serving.hpp"

// Access units in, rendered blocks out (planning/hearth-reference-player.md,
// A3: "a session per item: ... decoder, renderer ...").
//
// One AC-3 syncframe or one E-AC-3 access unit at a time, decoded and placed
// onto the output layout a 256-frame block at a time, with nothing copied
// that the decoders' block form does not already hand over. The shape is the
// one apps/hearth/testsink/burst_output.cpp settled on for the same job, and
// for the same reasons:
//
//   * A unit that is exactly one AC-3 syncframe goes to the AC-3 decoder, and
//     anything else - an AC-3 core carrying E-AC-3 dependents included - to
//     the E-AC-3 decoder, which a fold also applies to. Deciding from the
//     first frame's bsid alone sends a legacy-core stream down the wrong path.
//   * The block form hands samples over before it returns the unit's layout,
//     so each unit's bed is read from its headers first and queued; a unit's
//     first block takes the oldest bed not yet placed. That keeps a unit held
//     back for transient pre-noise processing (§3.7) matched to its own bed
//     when it is finally released, one call later.
//   * One programme: the first unit's.
//
// What the test sink never needed and a player does is the end of a stream.
// A unit still held back when the stream ends is released by
// Eac3Decoder::flush() as raw substreams rather than an assembled unit, so
// finish() assembles them and runs them through a §7.8 output stage of its
// own before rendering - without that, the last frame of every stream that
// used the tool is silently lost (which `ac3cli monitor` does today). The
// stage is a fresh one, so the two parts of the output stage that carry state
// between frames - the Lt/Rt phase shift's filter tail and RF mode's
// protection gain - restart for that one frame; everything else, dialnorm
// included, is exactly what the decoder would have applied.

namespace ac3::hearth {

class StreamDecoder {
public:
    // One rendered block: a span per slot of the output layout, each
    // `frames` long, valid for the duration of the call.
    using BlockFn = std::function<void(std::span<const std::span<const float>> slots,
                                       std::size_t frames)>;

    // `layout` is what the output renders onto; `sample_rate` is the
    // stream's own, which only the renderer's small-speaker crossover uses.
    StreamDecoder(const render::OutputLayout& layout, std::uint32_t sample_rate);

    // Decodes `unit` and hands each of its rendered blocks to `deliver`
    // during the call. A unit held back for transient pre-noise processing
    // delivers nothing now; its blocks arrive during the call that releases
    // it. Returns the frames this call delivered, or a sentence saying why
    // the unit could not be decoded - after which the decoders are reset, so
    // the next unit starts clean rather than inheriting a broken state.
    [[nodiscard]] std::expected<std::size_t, std::string> decode(std::span<const std::byte> unit,
                                                                 const BlockFn& deliver);

    // End of stream: releases and delivers whatever is still held back.
    // Returns the frames delivered. Leaves the decoder ready for a new
    // stream.
    std::size_t finish(const BlockFn& deliver);

    // Forgets the stream: decoders, programme, beds and the renderer's state.
    // What a seek needs before the first unit at its new position.
    void reset();

    [[nodiscard]] const render::Serving& serving() const { return serving_; }
    [[nodiscard]] const render::OutputLayout& layout() const { return layout_; }

private:
    void place(const PcmBlock& block, const BlockFn& deliver);
    std::size_t render_flushed(std::span<DecodedSubstream> substreams, const BlockFn& deliver);

    render::OutputLayout layout_;
    std::uint32_t sample_rate_;
    render::Serving serving_;
    DecoderConfig config_;
    render::LayoutRenderer renderer_;
    std::optional<FrameDecoder> ac3_decoder_;
    std::optional<Eac3Decoder> eac3_decoder_;
    std::optional<int> programme_;
    std::deque<eac3::chanmap::Layout> beds_;
    std::optional<eac3::chanmap::Layout> renderer_bed_;
    std::array<std::array<float, kSamplesPerBlock>, render::OutputLayout::kMaxSlots> block_{};
    std::size_t delivered_ = 0;
};

}  // namespace ac3::hearth
