#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ac3/render/layout.hpp"

// Where each rendered channel comes out: a patch from the renderer's slots to
// the outputs of a device.
//
// The renderer writes one block per slot of an OutputLayout (render.hpp). A
// device has its own outputs in its own order - eight on an HDMI LPCM
// endpoint in WAVE_FORMAT_EXTENSIBLE order, two on a headphone jack, more on
// an interface - and which speaker is plugged into which output is the room's
// business, not the stream's. A patch records it: each rendered channel to one
// output, or to none. An output no channel is patched to is written as
// silence. Two channels on one output are refused, since summing them would be
// a mix and mixing is the renderer's job; that is the rule the ESP32's speaker
// list keeps for its slots, where a list already is a patch.
//
// Fixed-size and allocation-free, so a sink on a board can hold one beside its
// layout. Tested on the host in tests/render/test_routing.cpp.

namespace ac3::render {

class Routing {
   public:
    // A rendered programme's slots.
    static constexpr std::size_t kMaxChannels = OutputLayout::kMaxSlots;
    // A device's outputs. A bound chosen for this type rather than a property
    // of any standard: an HDMI endpoint's eight, a TDM line's sixteen and a
    // multichannel interface's usual count fit, and a patch stays small
    // enough to hold by value.
    static constexpr std::size_t kMaxOutputs = 32;
    static constexpr int kUnassigned = -1;
    // What format() writes at most: "31," for every channel, and the NUL.
    static constexpr std::size_t kTextBytes = (kMaxChannels * 3) + 1;

    // No channels and no outputs.
    Routing() { output_of_.fill(static_cast<std::int8_t>(kUnassigned)); }

    // Channel n to output n, for as many as both have; any other channel is
    // unassigned. std::nullopt for more than kMaxChannels channels or
    // kMaxOutputs outputs.
    [[nodiscard]] static std::optional<Routing> identity(std::size_t channels,
                                                         std::size_t outputs) {
        if (channels > kMaxChannels || outputs > kMaxOutputs) {
            return std::nullopt;
        }
        Routing out;
        out.channels_ = static_cast<std::uint8_t>(channels);
        out.outputs_ = static_cast<std::uint8_t>(outputs);
        for (std::size_t c = 0; c < channels && c < outputs; ++c) {
            out.output_of_[c] = static_cast<std::int8_t>(c);
        }
        return out;
    }

    // One output per channel, kUnassigned for none. std::nullopt for too many
    // channels or outputs, an output out of range, or two channels on one
    // output.
    [[nodiscard]] static std::optional<Routing> from_outputs(std::span<const int> output_of_channel,
                                                             std::size_t outputs) {
        auto out = identity(output_of_channel.size(), outputs);
        if (!out) {
            return std::nullopt;
        }
        out->output_of_.fill(static_cast<std::int8_t>(kUnassigned));
        for (std::size_t c = 0; c < output_of_channel.size(); ++c) {
            if (output_of_channel[c] != kUnassigned && !out->assign(c, output_of_channel[c])) {
                return std::nullopt;
            }
        }
        return out;
    }

    // The text form, for a settings file: one token per channel in channel
    // order, comma-separated, each the output's index counted from 0 or "-"
    // for none - "0,1,2,-,4,5". Surrounding spaces are allowed. std::nullopt
    // for anything from_outputs() would refuse, an empty string, or a token
    // that is neither.
    [[nodiscard]] static std::optional<Routing> parse(std::string_view text, std::size_t outputs) {
        std::array<int, kMaxChannels> indices{};
        std::size_t count = 0;
        std::string_view rest = text;
        for (;;) {
            const std::size_t comma = rest.find(',');
            const std::string_view token =
                trim(comma == std::string_view::npos ? rest : rest.substr(0, comma));
            if (token.empty() || count >= kMaxChannels) {
                return std::nullopt;
            }
            if (token == "-") {
                indices[count++] = kUnassigned;
            } else {
                int value = 0;
                for (const char c : token) {
                    if (c < '0' || c > '9' || value > static_cast<int>(kMaxOutputs)) {
                        return std::nullopt;
                    }
                    value = (value * 10) + (c - '0');
                }
                indices[count++] = value;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            rest = rest.substr(comma + 1);
        }
        return from_outputs(std::span<const int>(indices.data(), count), outputs);
    }

    // The text form of this patch into `out`, NUL-terminated. Returns the
    // length written, or 0 with nothing but the NUL written when it does not
    // fit (kTextBytes always does). A patch of no channels is "".
    std::size_t format(std::span<char> out) const {
        if (out.empty()) {
            return 0;
        }
        std::size_t used = 0;
        for (std::size_t c = 0; c < channels_; ++c) {
            std::array<char, 3> token{};
            std::size_t length = 0;
            const int output = output_of_[c];
            if (output == kUnassigned) {
                token[length++] = '-';
            } else {
                if (output >= 10) {
                    token[length++] = static_cast<char>('0' + (output / 10));
                }
                token[length++] = static_cast<char>('0' + (output % 10));
            }
            const std::size_t needed = length + (c > 0 ? 1 : 0);
            if (used + needed + 1 > out.size()) {
                out[0] = '\0';
                return 0;
            }
            if (c > 0) {
                out[used++] = ',';
            }
            for (std::size_t i = 0; i < length; ++i) {
                out[used++] = token[i];
            }
        }
        out[used] = '\0';
        return used;
    }

    [[nodiscard]] std::size_t channels() const { return channels_; }
    [[nodiscard]] std::size_t outputs() const { return outputs_; }

    // The output `channel` is patched to, or kUnassigned - also for a channel
    // out of range.
    [[nodiscard]] int output_of(std::size_t channel) const {
        return channel < channels_ ? output_of_[channel] : kUnassigned;
    }

    // The channel patched to `output`, or kUnassigned.
    [[nodiscard]] int channel_of(std::size_t output) const {
        for (std::size_t c = 0; c < channels_; ++c) {
            if (output_of_[c] == static_cast<int>(output)) {
                return static_cast<int>(c);
            }
        }
        return kUnassigned;
    }

    // Patches `channel` to `output`, or unpatches it with kUnassigned. False,
    // changing nothing, for a channel or an output out of range, or for an
    // output another channel already has - move that one first, or swap().
    bool assign(std::size_t channel, int output) {
        if (channel >= channels_) {
            return false;
        }
        if (output == kUnassigned) {
            output_of_[channel] = static_cast<std::int8_t>(kUnassigned);
            return true;
        }
        if (output < 0 || output >= static_cast<int>(outputs_)) {
            return false;
        }
        const int holder = channel_of(static_cast<std::size_t>(output));
        if (holder != kUnassigned && holder != static_cast<int>(channel)) {
            return false;
        }
        output_of_[channel] = static_cast<std::int8_t>(output);
        return true;
    }

    // Exchanges two channels' outputs, either of which may be unassigned.
    // False for a channel out of range.
    bool swap(std::size_t a, std::size_t b) {
        if (a >= channels_ || b >= channels_) {
            return false;
        }
        std::swap(output_of_[a], output_of_[b]);
        return true;
    }

    // The outputs a channel is patched to, bit n for output n.
    [[nodiscard]] std::uint32_t patched_outputs() const {
        std::uint32_t mask = 0;
        for (std::size_t c = 0; c < channels_; ++c) {
            if (output_of_[c] != kUnassigned) {
                mask |= std::uint32_t{1} << static_cast<unsigned>(output_of_[c]);
            }
        }
        return mask;
    }

    // The channels patched to nothing, bit n for channel n: rendered audio
    // that will not be heard, which a settings page should say.
    [[nodiscard]] std::uint16_t unpatched_channels() const {
        std::uint16_t mask = 0;
        for (std::size_t c = 0; c < channels_; ++c) {
            if (output_of_[c] == kUnassigned) {
                mask = static_cast<std::uint16_t>(mask | (1U << c));
            }
        }
        return mask;
    }

    // One block onto planar outputs. `rendered` is one span per channel and
    // `out` one per output; the first n samples of each of the first
    // outputs() outputs are OVERWRITTEN - with its channel's samples, or with
    // zeros when it has none or its channel is missing from `rendered` - n
    // being the shortest span in either set.
    void apply(std::span<const std::span<const float>> rendered,
               std::span<const std::span<float>> out) const {
        const std::size_t outputs = std::min<std::size_t>(out.size(), outputs_);
        std::size_t n = shortest(out, outputs);
        for (std::size_t c = 0; c < channels_ && c < rendered.size(); ++c) {
            if (output_of_[c] != kUnassigned) {
                n = std::min(n, rendered[c].size());
            }
        }
        for (std::size_t o = 0; o < outputs; ++o) {
            const int c = channel_of(o);
            if (c == kUnassigned || static_cast<std::size_t>(c) >= rendered.size()) {
                std::fill_n(out[o].data(), n, 0.0F);
            } else {
                std::copy_n(rendered[static_cast<std::size_t>(c)].data(), n, out[o].data());
            }
        }
    }

    // One block into interleaved frames of outputs() samples each, the shape a
    // PCM device takes: frame k's sample for output o at out[k * outputs() + o].
    // Writes min(frames, out.size() / outputs()) frames, each sample from the
    // output's channel or zero, and returns how many.
    std::size_t apply_interleaved(std::span<const std::span<const float>> rendered,
                                  std::span<float> out, std::size_t frames) const {
        if (outputs_ == 0) {
            return 0;
        }
        std::size_t n = std::min(frames, out.size() / outputs_);
        for (std::size_t c = 0; c < channels_ && c < rendered.size(); ++c) {
            if (output_of_[c] != kUnassigned) {
                n = std::min(n, rendered[c].size());
            }
        }
        for (std::size_t o = 0; o < outputs_; ++o) {
            const int c = channel_of(o);
            const bool silent = c == kUnassigned || static_cast<std::size_t>(c) >= rendered.size();
            const float* const src =
                silent ? nullptr : rendered[static_cast<std::size_t>(c)].data();
            for (std::size_t k = 0; k < n; ++k) {
                out[(k * outputs_) + o] = silent ? 0.0F : src[k];
            }
        }
        return n;
    }

    friend bool operator==(const Routing&, const Routing&) = default;

   private:
    static std::string_view trim(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
            s.remove_suffix(1);
        }
        return s;
    }

    static std::size_t shortest(std::span<const std::span<float>> spans, std::size_t count) {
        if (count == 0) {
            return 0;
        }
        std::size_t n = spans[0].size();
        for (std::size_t i = 1; i < count; ++i) {
            n = std::min(n, spans[i].size());
        }
        return n;
    }

    std::array<std::int8_t, kMaxChannels> output_of_{};
    std::uint8_t channels_ = 0;
    std::uint8_t outputs_ = 0;
};

}  // namespace ac3::render
