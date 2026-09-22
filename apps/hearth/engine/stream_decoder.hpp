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
#include "ac3/decoder/output.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/render.hpp"
#include "ac3/render/serving.hpp"
#include "decoder_settings.hpp"

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
//   * One programme: the first unit's. Which programme that is, is the
//     session's choice of units (Session::open), not this decoder's.
//
// Everything else a listener can choose comes from DecoderSettings: the
// library configuration decoder_setup() makes of it, and dual mono's choice
// of programme, applied here to each unit that codes 1+1 before it is placed.
//
// After a unit's blocks, what the unit said about itself goes to an optional
// second callback as a UnitReport: its service, dialnorm, compr and dynrng
// words, the fold levels in force, whether it was concealed, and its objects.
// The report belongs to the blocks the call delivered, so a unit held back
// for transient pre-noise processing is reported by the call that releases
// it, and the last one by finish().
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

// What one access unit said about itself, as decoded.
struct UnitReport {
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    // Substreams the unit assembled from: 1 for AC-3, the independent and
    // its dependents for E-AC-3.
    int substreams = 1;
    // The locations it decoded to; empty for dual mono.
    eac3::chanmap::Layout layout{};
    // §5.4.2.2's service, when the unit sends one (E-AC-3 in its
    // informational metadata only).
    std::optional<int> bsmod = std::nullopt;
    int dialnorm = 31;
    std::optional<int> dialnorm2 = std::nullopt;
    std::optional<std::uint8_t> compr = std::nullopt;
    std::optional<std::uint8_t> compr2 = std::nullopt;  // AC-3 1+1
    // The effective dynrng word of each of the unit's `blocks` blocks.
    std::array<std::uint8_t, kBlocksPerFrame> dynrng{};
    int blocks = kBlocksPerFrame;
    // AC-3: the blocks in which any channel used the short transform.
    std::optional<int> short_blocks = std::nullopt;
    // The fold levels the unit's own metadata gives, defaults included.
    MixLevels levels{};
    // How the decoder concealed the unit, when it did (§7.10).
    std::optional<Concealment> concealed = std::nullopt;
    // The program its object metadata describes, with every update block's
    // positions, when it carried any.
    std::optional<oba::DecodedProgram> objects = std::nullopt;
};

// Which of an access unit's substreams are decoded.
enum class Substreams : std::uint8_t {
    // All of them: the programme as a listener hears it.
    kAll,
    // The first syncframe alone - the independent substream, or an AC-3
    // core. For a 7.1 stream that is the 5.1 its own encoder made, which is
    // what an encoder of a narrower format wants rather than a fold of the
    // extra channels.
    kIndependent,
};

class StreamDecoder {
public:
    // One rendered block: a span per slot of the output layout, each
    // `frames` long, valid for the duration of the call.
    using BlockFn = std::function<void(std::span<const std::span<const float>> slots,
                                       std::size_t frames)>;
    // A unit's report, valid for the duration of the call.
    using UnitFn = std::function<void(const UnitReport& report)>;

    // `layout` is what the output renders onto; `sample_rate` is the
    // stream's own, which only the renderer's small-speaker crossover uses.
    StreamDecoder(const render::OutputLayout& layout, std::uint32_t sample_rate,
                  const DecoderSettings& settings = {}, Substreams substreams = Substreams::kAll);

    // Decodes `unit` and hands each of its rendered blocks to `deliver`
    // during the call, then the unit's report to `reported`. A unit held back
    // for transient pre-noise processing delivers nothing now; its blocks and
    // report arrive during the call that releases it. Returns the frames this
    // call delivered, or a sentence saying why the unit could not be decoded -
    // after which the decoders are reset, so the next unit starts clean rather
    // than inheriting a broken state.
    [[nodiscard]] std::expected<std::size_t, std::string> decode(std::span<const std::byte> unit,
                                                                 const BlockFn& deliver,
                                                                 const UnitFn& reported = {});

    // End of stream: releases and delivers whatever is still held back, and
    // reports it. Returns the frames delivered. Leaves the decoder ready for a
    // new stream.
    std::size_t finish(const BlockFn& deliver, const UnitFn& reported = {});

    // Forgets the stream: decoders, programme, beds and the renderer's state.
    // What a seek needs before the first unit at its new position.
    void reset();

    // The small-speaker crossover's corner (render.hpp's LayoutRenderer).
    // False, changing nothing, for a frequency set_crossover_hz() itself
    // refuses. A player applies this once per StreamDecoder - a new one is
    // built on every rate change (decoder_rate_) - rather than this taking
    // it in its constructor, so the one setting that changes at runtime does
    // not grow every call site that only ever passes the default.
    bool set_crossover_hz(double hz) { return renderer_.set_crossover_hz(hz); }
    [[nodiscard]] double crossover_hz() const { return renderer_.crossover_hz(); }

    // How many samples the renderer holds the bed's LFE back for while it
    // places objects - render.hpp's LayoutRenderer::object_lag(), which
    // DecoderSettings::joc_domain drives (decoder_setup()). Exposed so a test
    // can tell the renderer picked up the setting's domain, construction and
    // reset() (a seek) both, without decoding a whole stream to hear it.
    [[nodiscard]] std::size_t object_lag() const { return renderer_.object_lag(); }

    [[nodiscard]] const render::Serving& serving() const { return serving_; }
    [[nodiscard]] const render::OutputLayout& layout() const { return layout_; }
    [[nodiscard]] const DecoderSettings& settings() const { return settings_; }
    [[nodiscard]] std::uint32_t sample_rate() const { return sample_rate_; }
    [[nodiscard]] Substreams substreams() const { return substreams_; }

private:
    // What a unit's headers say about how to place it, read before it is
    // decoded: its bed, and whether it codes dual mono.
    struct UnitBed {
        eac3::chanmap::Layout layout{};
        bool dual_mono = false;
    };

    void place(const PcmBlock& block, const BlockFn& deliver);
    std::size_t render_flushed(std::span<DecodedSubstream> substreams, const BlockFn& deliver,
                               const UnitFn& reported);

    render::OutputLayout layout_;
    std::uint32_t sample_rate_;
    DecoderSettings settings_;
    Substreams substreams_;
    render::Serving serving_;
    DecoderConfig config_;
    render::LayoutRenderer renderer_;
    std::optional<FrameDecoder> ac3_decoder_;
    std::optional<Eac3Decoder> eac3_decoder_;
    std::optional<int> programme_;
    std::deque<UnitBed> beds_;
    std::optional<eac3::chanmap::Layout> renderer_bed_;
    // Whether the unit being placed codes dual mono.
    bool dual_mono_ = false;
    std::array<std::array<float, kSamplesPerBlock>, render::OutputLayout::kMaxSlots> block_{};
    std::size_t delivered_ = 0;
    // Filled for each unit and handed out by reference, keeping its storage.
    UnitReport report_{};
};

}  // namespace ac3::hearth
