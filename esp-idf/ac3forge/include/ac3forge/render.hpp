#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/spatial/spatial.hpp"

#include "ac3forge/layout.hpp"

// From what the decoder rendered to what the speakers want, one 256-sample
// block at a time.
//
// The decoder hands over a PcmBlock: the coded channels of the programme in
// Table E2.5 order and, when the stream carries an object layer and the
// decoder reconstructed it, the objects beside them with the metadata that
// places them. This turns that into one block per output slot of an
// OutputLayout, two ways:
//
//   THE BED, when there are no objects to place (an AC-3 or plain E-AC-3
//   stream, or a player that chose not to reconstruct). Every coded channel is
//   a source at its Table E2.5 direction, panned onto the layout's speakers by
//   ac3::spatial::pan_direction - which for a channel whose location the layout
//   has is unit gain to that one slot, exactly, and for one it lacks (a 7.1
//   stream's rear surrounds on a 5.1 room) is the pairwise spread the panner
//   gives. The LFE goes to the LFE slots and nowhere else.
//
//   THE OBJECTS, when a unit carries them and the player asked for them. Each
//   object's own reconstructed audio is summed into the speakers by its OAMD
//   position, at its own gain - the render apps/baremetal/probe.cpp's
//   eac3_atmos_render row performs and ac3cli's `qc objects=` meters, in the
//   same arithmetic and the same order, so a level measured here agrees with
//   the probe's reference to the digit. The bed's other channels are NOT added
//   on top: for a JOC programme the bed IS the objects' 5.1 fold, and adding it
//   would render everything twice. The bed's LFE passes through, because it is
//   not an object.
//
// The gains are trigonometry in double, refreshed when the coded layout
// changes (set_bed) and once per unit for the objects (set_objects, on the
// unit's first block); the per-sample work is float multiply-adds on the FPU,
// which is where an ESP32-S3 spends its time well. Nothing here allocates
// except describe_objects' own vector of descriptions, once per unit.
//
// Free of ESP-IDF, and tested on the host in tests/io/test_layout.cpp: the
// geometry has its own tests under tests/spatial/, so what is checked here is
// the indexing between coded channels, objects and slots - the part where a
// swapped subscript is silent.
//
// Bass management. A slot marked ":small" (OutputLayout::listed()) cannot
// reproduce the bottom two octaves, so its bass is redirected to the LFE
// feed rather than sent there - the same thing every AVR's speaker-size
// setting does. A matched high-pass/low-pass pair per small slot, same
// corner frequency and Q (Butterworth, 1/sqrt(2)) on both sides, since a
// mismatched pair would leave a dip or a peak in the room's response around
// the crossover. Only ever reached through render(): fold() never returns a
// target for a layout with an LFE feed, and a small speaker is rejected
// unless the layout has one, so a layout with any small speaker can never
// reach render_folded() - nothing to branch on there.
//
// CrossoverBiquad, below, is float throughout - state, coefficients, the
// arithmetic itself - deliberately NOT ac3::dsp::Biquad (ac3/dsp/biquad.hpp),
// the second-order section bundle C's explicit-LFE-assignment feature
// already has: that one accumulates in double even though its interface is
// float, which is free on the desktop-class hardware its one caller runs
// on and expensive here. Every double operation on the S3's PIE is a
// software call, and Direct Form II Transposed is nine of them a sample (5
// multiplies, 4 adds); the example's own level meter once cost twice the
// decode it was measuring by squaring every sample in double
// (docs/platforms/esp32.md), which is the same trap at a larger scale. A
// small duplicated type, rather than templating the shared one onto a
// caller with different precision needs, matches the coefficient
// functions' own reasoning below.

namespace ac3forge {

class LayoutRenderer {
   public:
    using Location = ac3::eac3::chanmap::Location;
    static constexpr std::size_t kMaxSlots = OutputLayout::kMaxSlots;
    // JOC carries at most sixteen objects (TS 103 420); a rendered programme
    // has at most sixteen slots (§E3.8.2).
    static constexpr std::size_t kMaxObjects = 16;
    static constexpr std::size_t kMaxCoded = 16;
    // The standard AVR bass-management crossover - not §7.8's LFE handling
    // (ac3::OutputConfig::mix_lfe) or bundle C's ~120 Hz LFE-channel
    // low-pass (ac3::dsp::LfeLowpass), both different questions.
    static constexpr double kDefaultCrossoverHz = 80.0;

    // sample_rate_hz and crossover_hz only matter when `layout` has a
    // ":small" speaker (OutputLayout::has_small()); with none, small_slots_
    // stays empty, crossover_hp_/crossover_lp_ are never resized, and this
    // costs three empty vector headers rather than filter state for slots
    // nothing uses - the ESP32-S3 WiFi shape's internal heap has been seen
    // with under 1 KB free during a 7.1.4 play (planning/esp32-714-realtime.md),
    // and a fixed per-instance cost paid by every layout regardless of
    // whether it is small-aware would eat straight into that. sample_rate_hz
    // defaults to this player's only rate (stream_player.cpp's kSampleRate).
    explicit LayoutRenderer(const OutputLayout& layout, std::uint32_t sample_rate_hz = 48000,
                            double crossover_hz = kDefaultCrossoverHz)
        : layout_(layout) {
        int lfe_slot = -1;
        int any_lfe_slot = -1;
        for (std::size_t slot = 0; slot < layout_.slots(); ++slot) {
            const Speaker& speaker = layout_.slot(slot);
            if (speaker.kind == Speaker::Kind::kSpeaker) {
                target_directions_[targets_] = speaker.direction;
                target_slots_[targets_] = slot;
                ++targets_;
                if (speaker.small) {
                    small_slots_.push_back(slot);
                }
            } else if (speaker.kind == Speaker::Kind::kLfe) {
                if (any_lfe_slot < 0) {
                    any_lfe_slot = static_cast<int>(slot);
                }
                // The same feed set_bed()'s own LFE routing prefers: never
                // the one named LFE2 while another LFE feed exists.
                if (lfe_slot < 0 && speaker.location != Location::kLfe2) {
                    lfe_slot = static_cast<int>(slot);
                }
            }
        }
        has_small_ = !small_slots_.empty();
        lfe_slot_ = lfe_slot >= 0 ? lfe_slot : any_lfe_slot;
        if (has_small_) {
            crossover_hp_.resize(small_slots_.size());
            crossover_lp_.resize(small_slots_.size());
            for (std::size_t i = 0; i < small_slots_.size(); ++i) {
                configure_highpass(crossover_hp_[i], crossover_hz, sample_rate_hz);
                configure_lowpass(crossover_lp_[i], crossover_hz, sample_rate_hz);
            }
        }
    }

    [[nodiscard]] const OutputLayout& layout() const { return layout_; }

    // The coded layout of the units about to arrive: the channels a PcmBlock
    // will carry, in its order. Recomputes every bed gain. Call when it
    // changes, which for a stream is once.
    void set_bed(const ac3::eac3::chanmap::Layout& coded) {
        coded_ = coded;
        bed_channels_ = std::min(static_cast<std::size_t>(coded.count), kMaxCoded);
        for (auto& row : bed_gains_) {
            row.fill(0.0F);
        }
        // Where the coded surrounds sit depends on the coded layout's own
        // company, exactly as the output layout's do - see OutputLayout.
        const bool has_rears = coded.index_of(Location::kLrs) >= 0;
        const bool has_side_discrete = coded.index_of(Location::kLsd) >= 0;
        const bool layout_has_lfe2 = layout_.index_of(Location::kLfe2) >= 0;
        std::array<double, kMaxSlots> gains{};
        for (std::size_t c = 0; c < bed_channels_; ++c) {
            const Location location = coded[static_cast<int>(c)];
            if (location == Location::kLfe || location == Location::kLfe2) {
                // LFE to the LFE feeds. A second LFE goes to a second feed when
                // the room has one, and joins the first otherwise; the first
                // never lands on a slot named LFE2.
                for (std::size_t slot = 0; slot < layout_.slots(); ++slot) {
                    const Speaker& speaker = layout_.slot(slot);
                    if (speaker.kind != Speaker::Kind::kLfe) {
                        continue;
                    }
                    const bool slot_is_lfe2 = speaker.location == Location::kLfe2;
                    const bool wanted = location == Location::kLfe2
                                            ? (layout_has_lfe2 ? slot_is_lfe2 : !slot_is_lfe2)
                                            : !slot_is_lfe2;
                    if (wanted) {
                        bed_gains_[c][slot] = 1.0F;
                    }
                }
                continue;
            }
            const int exact = layout_.index_of(location);
            if (exact >= 0 && layout_.slot(static_cast<std::size_t>(exact)).kind ==
                                  Speaker::Kind::kSpeaker) {
                bed_gains_[c][static_cast<std::size_t>(exact)] = 1.0F;
                continue;
            }
            if (targets_ == 0) {
                continue;
            }
            const auto direction =
                ac3::spatial::direction_of(location, has_rears, has_side_discrete);
            ac3::spatial::pan_direction(
                direction, std::span<const ac3::spatial::Direction>(target_directions_.data(), targets_),
                std::span<double>(gains.data(), targets_));
            for (std::size_t t = 0; t < targets_; ++t) {
                bed_gains_[c][target_slots_[t]] = static_cast<float>(gains[t]);
            }
        }
    }

    // The objects of the unit about to be rendered, as describe_objects sees
    // them: position, gain and whether active. Only the first kMaxObjects are
    // placed.
    void set_objects(std::span<const ac3::oba::DisplayObject> objects) {
        object_count_ = std::min(objects.size(), kMaxObjects);
        std::array<double, kMaxSlots> gains{};
        for (std::size_t i = 0; i < object_count_; ++i) {
            object_gains_[i].fill(0.0F);
            if (!objects[i].active || targets_ == 0) {
                continue;
            }
            const auto direction = ac3::spatial::position_direction(
                objects[i].position.x, objects[i].position.y, objects[i].position.z);
            ac3::spatial::pan_direction(
                direction, std::span<const ac3::spatial::Direction>(target_directions_.data(), targets_),
                std::span<double>(gains.data(), targets_));
            const double linear = std::pow(10.0, objects[i].gain_db / 20.0);
            for (std::size_t t = 0; t < targets_; ++t) {
                // Double until here, float from here: the probe's arithmetic.
                object_gains_[i][target_slots_[t]] = static_cast<float>(gains[t] * linear);
            }
        }
    }

    // The same, from the metadata a PcmBlock carries. `audio_count` is how
    // many object signals the block has (PcmBlock::objects.size()); the
    // description and the audio are parallel, so the shorter wins.
    void set_objects(const ac3::oba::DecodedProgram* metadata, std::size_t audio_count) {
        if (metadata == nullptr || audio_count == 0) {
            object_count_ = 0;
            return;
        }
        const std::vector<ac3::oba::DisplayObject> described = ac3::oba::describe_objects(*metadata);
        const std::size_t count = std::min(described.size(), audio_count);
        set_objects(std::span<const ac3::oba::DisplayObject>(described.data(), count));
    }

    [[nodiscard]] std::size_t object_count() const { return object_count_; }
    [[nodiscard]] std::size_t bed_channels() const { return bed_channels_; }
    [[nodiscard]] float bed_gain(std::size_t coded, std::size_t slot) const {
        return bed_gains_[coded][slot];
    }
    [[nodiscard]] float object_gain(std::size_t object, std::size_t slot) const {
        return object_gains_[object][slot];
    }

    // The slots the bed reaches, bit n for slot n: every slot a coded channel
    // has a gain into, or with `lfe_only` the ones the bed's LFE reaches -
    // which is all of the bed render() plays while it places objects. Also
    // sets the LFE feed's bit when bass management would reach it: a small
    // slot the bed itself feeds has its low end redirected to the LFE feed by
    // render() regardless of `lfe_only` or whether that call places objects
    // instead of the bed - see render.hpp's header comment on the crossover.
    [[nodiscard]] std::uint16_t bed_slots(bool lfe_only = false) const {
        std::uint16_t mask = 0;
        for (std::size_t c = 0; c < bed_channels_; ++c) {
            const Location location = coded_[static_cast<int>(c)];
            if (lfe_only && location != Location::kLfe && location != Location::kLfe2) {
                continue;
            }
            for (std::size_t slot = 0; slot < layout_.slots(); ++slot) {
                if (bed_gains_[c][slot] != 0.0F) {
                    mask = static_cast<std::uint16_t>(mask | (1U << slot));
                }
            }
        }
        if (has_small_ && lfe_slot_ >= 0 && bed_feeds_a_small_slot()) {
            mask = static_cast<std::uint16_t>(mask | (1U << static_cast<unsigned>(lfe_slot_)));
        }
        return mask;
    }

    // The slots the current objects reach (set_objects), the same way.
    [[nodiscard]] std::uint16_t object_slots() const {
        std::uint16_t mask = 0;
        for (std::size_t i = 0; i < object_count_; ++i) {
            for (std::size_t slot = 0; slot < layout_.slots(); ++slot) {
                if (object_gains_[i][slot] > 0.0F) {
                    mask = static_cast<std::uint16_t>(mask | (1U << slot));
                }
            }
        }
        return mask;
    }

    // One block. `out` is one span per slot of the layout (out.size() ==
    // layout().slots()), each at least as long as the block; the first
    // block-length samples of every slot are OVERWRITTEN - an empty slot with
    // zeros, so a bus reused from the last block never replays it. `objects`
    // says whether to place the objects the block carries (when it carries
    // none, the bed is placed whatever this says); `gain` is applied to
    // everything, 1.0 being free.
    void render(const ac3::PcmBlock& block, bool objects, float gain,
                std::span<const std::span<float>> out) {
        const std::size_t slots = std::min(out.size(), layout_.slots());
        const std::size_t n = block_length(block, out);
        for (std::size_t slot = 0; slot < slots; ++slot) {
            std::fill_n(out[slot].data(), n, 0.0F);
        }
        const bool place_objects = objects && object_count_ > 0 && !block.objects.empty();
        if (place_objects) {
            const std::size_t count = std::min(object_count_, block.objects.size());
            for (std::size_t i = 0; i < count; ++i) {
                const std::span<const float> audio = block.objects[i];
                if (audio.size() < n) {
                    continue;
                }
                for (std::size_t t = 0; t < targets_; ++t) {
                    const std::size_t slot = target_slots_[t];
                    if (slot >= slots) {
                        continue;
                    }
                    const float g = object_gains_[i][slot];
                    if (g <= 0.0F) {
                        continue;
                    }
                    float* const dst = out[slot].data();
                    for (std::size_t k = 0; k < n; ++k) {
                        dst[k] += g * audio[k];
                    }
                }
            }
            // The bed's LFE, and only that, through its own gains.
            for (std::size_t c = 0; c < bed_channels_ && c < block.channels.size(); ++c) {
                const Location location = coded_[static_cast<int>(c)];
                if (location != Location::kLfe && location != Location::kLfe2) {
                    continue;
                }
                add_channel(block.channels[c], c, n, slots, out);
            }
        } else {
            for (std::size_t c = 0; c < bed_channels_ && c < block.channels.size(); ++c) {
                add_channel(block.channels[c], c, n, slots, out);
            }
        }
        if (has_small_) {
            apply_crossover(n, slots, out);
        }
        if (gain != 1.0F) {
            for (std::size_t slot = 0; slot < slots; ++slot) {
                float* const dst = out[slot].data();
                for (std::size_t k = 0; k < n; ++k) {
                    dst[k] *= gain;
                }
            }
        }
    }

    // Drops the crossover filters' delay-line state (not their
    // coefficients), for reuse across streams - the same reasoning
    // ac3::OutputStage::reset() has for its own Lt/Rt phase-shift history.
    // A no-op when nothing is small.
    void reset() {
        for (CrossoverBiquad& hp : crossover_hp_) {
            hp.reset();
        }
        for (CrossoverBiquad& lp : crossover_lp_) {
            lp.reset();
        }
    }

    // A block the decoder's own output stage already folded (kLoRo, kLtRt,
    // kMono): channel j goes to the j-th full-bandwidth slot, or by name when
    // the slots have names - L to the slot named L, R to R - so a list that
    // wires a stereo DAC as "R,L" still plays the right way round. Empty and
    // LFE slots are written as zeros.
    void render_folded(const ac3::PcmBlock& block, float gain,
                       std::span<const std::span<float>> out) const {
        const std::size_t slots = std::min(out.size(), layout_.slots());
        const std::size_t n = block_length(block, out);
        for (std::size_t slot = 0; slot < slots; ++slot) {
            std::fill_n(out[slot].data(), n, 0.0F);
        }
        std::size_t next_speaker = 0;
        for (std::size_t ch = 0; ch < block.channels.size(); ++ch) {
            int slot = -1;
            if (block.channels.size() == 2) {
                slot = layout_.index_of(ch == 0 ? Location::kLeft : Location::kRight);
            }
            if (slot < 0) {
                while (next_speaker < slots &&
                       layout_.slot(next_speaker).kind != Speaker::Kind::kSpeaker) {
                    ++next_speaker;
                }
                if (next_speaker >= slots) {
                    break;
                }
                slot = static_cast<int>(next_speaker++);
            }
            const std::span<const float> src = block.channels[ch];
            float* const dst = out[static_cast<std::size_t>(slot)].data();
            const std::size_t m = std::min(n, src.size());
            if (gain == 1.0F) {
                std::copy_n(src.data(), m, dst);
            } else {
                for (std::size_t k = 0; k < m; ++k) {
                    dst[k] = src[k] * gain;
                }
            }
        }
    }

   private:
    // A second-order IIR section, float throughout - see the header comment
    // on why this duplicates ac3::dsp::Biquad's shape rather than reusing
    // it. Direct Form II Transposed: two state variables, no separate
    // input/output delay lines to keep in sync, the same structure
    // ac3::dsp::Biquad uses.
    struct CrossoverBiquad {
        float b0 = 1.0F, b1 = 0.0F, b2 = 0.0F, a1 = 0.0F, a2 = 0.0F;
        float z1 = 0.0F, z2 = 0.0F;

        float process(float x) {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        void reset() {
            z1 = 0.0F;
            z2 = 0.0F;
        }
    };

    // Butterworth Q for a single second-order section - the standard
    // maximally-flat choice, and what gives a matched low-pass/high-pass
    // pair a flat combined response through the crossover.
    static constexpr double kCrossoverQ = 0.70710678118654752;  // 1/sqrt(2)

    // RBJ Audio EQ Cookbook low-pass/high-pass biquad (public-domain DSP,
    // not sourced from any particular codebase), computed in double - this
    // runs once per stream at construction, not per sample - and narrowed to
    // the float coefficients CrossoverBiquad::process() actually uses.
    static void configure_lowpass(CrossoverBiquad& biquad, double corner_hz,
                                  double sample_rate_hz) {
        const double omega = 2.0 * std::numbers::pi * corner_hz / sample_rate_hz;
        const double cos_omega = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * kCrossoverQ);
        const double a0 = 1.0 + alpha;
        const double b0 = (1.0 - cos_omega) / 2.0;
        biquad.b0 = static_cast<float>(b0 / a0);
        biquad.b1 = static_cast<float>((1.0 - cos_omega) / a0);
        biquad.b2 = biquad.b0;
        biquad.a1 = static_cast<float>((-2.0 * cos_omega) / a0);
        biquad.a2 = static_cast<float>((1.0 - alpha) / a0);
    }

    static void configure_highpass(CrossoverBiquad& biquad, double corner_hz,
                                   double sample_rate_hz) {
        const double omega = 2.0 * std::numbers::pi * corner_hz / sample_rate_hz;
        const double cos_omega = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * kCrossoverQ);
        const double a0 = 1.0 + alpha;
        const double b0 = (1.0 + cos_omega) / 2.0;
        biquad.b0 = static_cast<float>(b0 / a0);
        biquad.b1 = static_cast<float>(-(1.0 + cos_omega) / a0);
        biquad.b2 = biquad.b0;
        biquad.a1 = static_cast<float>((-2.0 * cos_omega) / a0);
        biquad.a2 = static_cast<float>((1.0 - alpha) / a0);
    }

    // Whether the bed's own gains reach any small slot at all - the
    // condition bed_slots() redirects into the LFE bit, since that is what
    // the crossover has something to act on. Independent of any `lfe_only`
    // filtering bed_slots() itself applies to the mask it returns.
    [[nodiscard]] bool bed_feeds_a_small_slot() const {
        for (std::size_t c = 0; c < bed_channels_; ++c) {
            for (const std::size_t slot : small_slots_) {
                if (bed_gains_[c][slot] != 0.0F) {
                    return true;
                }
            }
        }
        return false;
    }

    // High-passes every small slot in place and sums its low-passed
    // complement into the primary LFE slot - see the header comment. Called
    // only when has_small_; a no-op if the layout somehow has no LFE slot at
    // all (OutputLayout::listed() already refuses that combination, so this
    // is a defensive bound rather than a real case).
    void apply_crossover(std::size_t n, std::size_t slots, std::span<const std::span<float>> out) {
        if (lfe_slot_ < 0 || static_cast<std::size_t>(lfe_slot_) >= slots) {
            return;
        }
        float* const lfe = out[static_cast<std::size_t>(lfe_slot_)].data();
        for (std::size_t i = 0; i < small_slots_.size(); ++i) {
            const std::size_t slot = small_slots_[i];
            if (slot >= slots) {
                continue;
            }
            float* const dst = out[slot].data();
            CrossoverBiquad& hp = crossover_hp_[i];
            CrossoverBiquad& lp = crossover_lp_[i];
            for (std::size_t k = 0; k < n; ++k) {
                const float x = dst[k];
                dst[k] = hp.process(x);
                lfe[k] += lp.process(x);
            }
        }
    }

    static std::size_t block_length(const ac3::PcmBlock& block,
                                    std::span<const std::span<float>> out) {
        std::size_t n = block.channels.empty() ? 0 : block.channels.front().size();
        if (n == 0 && !block.objects.empty()) {
            n = block.objects.front().size();
        }
        for (const auto& slot : out) {
            n = std::min(n, slot.size());
        }
        return n;
    }

    void add_channel(std::span<const float> src, std::size_t c, std::size_t n, std::size_t slots,
                     std::span<const std::span<float>> out) const {
        if (src.size() < n) {
            return;
        }
        for (std::size_t slot = 0; slot < slots; ++slot) {
            const float g = bed_gains_[c][slot];
            if (g == 0.0F) {
                continue;
            }
            float* const dst = out[slot].data();
            if (g == 1.0F) {
                for (std::size_t k = 0; k < n; ++k) {
                    dst[k] += src[k];
                }
            } else {
                for (std::size_t k = 0; k < n; ++k) {
                    dst[k] += g * src[k];
                }
            }
        }
    }

    OutputLayout layout_;
    std::array<ac3::spatial::Direction, kMaxSlots> target_directions_{};
    std::array<std::size_t, kMaxSlots> target_slots_{};
    std::size_t targets_ = 0;
    ac3::eac3::chanmap::Layout coded_{};
    std::size_t bed_channels_ = 0;
    std::array<std::array<float, kMaxSlots>, kMaxCoded> bed_gains_{};
    std::array<std::array<float, kMaxSlots>, kMaxObjects> object_gains_{};
    std::size_t object_count_ = 0;
    // Bass management: which slots are small, the LFE slot their bass is
    // redirected to, and each small slot's own filter pair - small_slots_[i]
    // owns crossover_hp_[i]/crossover_lp_[i]. All three stay empty, and
    // has_small_ false, when the layout has no small speaker: three vector
    // headers rather than sixteen slots' worth of filter state nothing
    // uses - see the constructor's comment on why that distinction matters
    // on this target.
    bool has_small_ = false;
    int lfe_slot_ = -1;
    std::vector<std::size_t> small_slots_;
    std::vector<CrossoverBiquad> crossover_hp_;
    std::vector<CrossoverBiquad> crossover_lp_;
};

}  // namespace ac3forge
