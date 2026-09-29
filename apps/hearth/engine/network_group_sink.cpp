#include "network_group_sink.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "ac3/sendspin/session_driver.hpp"

// The NetworkGroupSink over a real ac3::sendspin::Group: everything here is a
// translation between the two interfaces, as passthrough_sink.cpp and
// device_sink.cpp are for their own sinks. What differs is that a group is
// resolved by name at open() rather than held fixed from construction (see
// network_group_sink.hpp's own comment on GroupResolver), and that pushing
// PCM is a partial-take call (Group::push() can take fewer frames than
// offered) rather than the all-or-nothing bool PcmSink::submit() is -
// Player's own drain_group() (player.cpp) is what retries the remainder.

namespace ac3::hearth {

namespace {

namespace m = sendspin::messages;

// Full-scale: the least lossy choice for the group's own declared bit depth
// - Group::push() rescales down for a member at a lower depth itself
// (server_host.cpp's rescaled()), so this sink never needs to know what any
// member actually is.
constexpr std::int32_t kBitDepth = 32;

[[nodiscard]] std::int32_t to_sample(float value) {
    const double scaled = std::clamp(static_cast<double>(value), -1.0, 1.0) * 2147483647.0;
    return static_cast<std::int32_t>(std::lround(scaled));
}

// The role's data type for a stream's bursts: every AC-4 link is one AC-4
// stream to the role, which drops the link along with the sync words.
[[nodiscard]] sendspin::ac3forge::DataType data_type_of(audio::BitstreamFormat format) {
    switch (format) {
        case audio::BitstreamFormat::kAc3:
            return sendspin::ac3forge::DataType::kAc3;
        case audio::BitstreamFormat::kEac3:
            return sendspin::ac3forge::DataType::kEac3;
        case audio::BitstreamFormat::kAc4:
        case audio::BitstreamFormat::kAc4Hbr4:
        case audio::BitstreamFormat::kAc4Hbr16:
            break;
    }
    return sendspin::ac3forge::DataType::kAc4;
}

class GroupSink final : public NetworkGroupSink {
public:
    explicit GroupSink(GroupResolver resolve) : resolve_(std::move(resolve)) {}

    ~GroupSink() override { close(); }

    GroupSink(const GroupSink&) = delete;
    GroupSink& operator=(const GroupSink&) = delete;

    std::expected<OpenOutputFormat, std::string> open(const std::string& group_name,
                                                       const Format& format) override {
        close();
        std::shared_ptr<sendspin::Group> group = resolve_ ? resolve_(group_name) : nullptr;
        if (!group) {
            return std::unexpected(fmt::format("The group \"{}\" is not available.", group_name));
        }
        const auto channels = static_cast<std::int32_t>(format.layout.slots());
        const m::AudioFormat pcm{.codec = m::Codec::kPcm,
                                 .channels = channels,
                                 .sample_rate = static_cast<std::int32_t>(format.sample_rate),
                                 .bit_depth = kBitDepth};
        std::optional<sendspin::ac3forge::StreamStart> bursts;
        if (format.stream) {
            bursts = sendspin::ac3forge::StreamStart{
                .data_type = data_type_of(*format.stream),
                .sample_rate = static_cast<std::int32_t>(format.sample_rate)};
        }
        if (!group->start({.pcm = pcm, .bursts = bursts, .buffered = true})) {
            return std::unexpected(fmt::format("The group \"{}\" would not start.", group_name));
        }
        group_ = std::move(group);
        channels_ = static_cast<std::size_t>(channels);
        sample_rate_ = format.sample_rate;
        taken_ = 0;
        flushed_ = 0;
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = static_cast<std::uint16_t>(channels),
                                .mode = OutputMode::kNetworkGroup,
                                .stream = format.stream};
    }

    void close() override {
        if (group_) {
            group_->stop();
        }
        group_.reset();
        channels_ = 0;
        taken_ = 0;
        flushed_ = 0;
    }

    [[nodiscard]] bool is_open() const override { return group_ != nullptr; }

    std::size_t submit_pcm(std::span<const std::span<const float>> slots, std::size_t frames) override {
        if (!group_) {
            return 0;
        }
        interleaved_.resize(frames * channels_);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (std::size_t channel = 0; channel < channels_; ++channel) {
                const float sample = channel < slots.size() ? slots[channel][frame] : 0.0F;
                interleaved_[(frame * channels_) + channel] = to_sample(sample);
            }
        }
        const std::size_t taken = group_->push(interleaved_);
        taken_ += taken;
        return taken;
    }

    bool submit_burst(std::uint16_t pc, std::uint16_t pd, std::span<const std::byte> payload,
                      std::int64_t frame, std::int64_t frames) override {
        if (!group_) {
            return false;
        }
        // Group::Burst::payload is std::span<const std::uint8_t> - sendspin's
        // own byte type, not forge's std::byte this interface otherwise
        // matches (network_group_sink.hpp's own submit_burst() comment).
        // Both alias unsigned char, so reinterpreting the span is well-defined.
        const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                                   payload.size());
        return group_->push_burst(
            {.pc = pc, .pd = pd, .payload = bytes, .frame = frame, .frames = frames});
    }

    [[nodiscard]] std::optional<audio::MonitorPosition> position() const override {
        if (!group_) {
            return std::nullopt;
        }
        // Played is what the group's timeline has run through - the frame
        // every member is playing now - and never more than has been taken.
        std::uint64_t played = 0;
        if (const std::optional<std::int64_t> start = group_->start_time()) {
            const std::int64_t elapsed_us = clock_.now_us() - *start;
            if (elapsed_us > 0) {
                const auto elapsed =
                    static_cast<std::uint64_t>((static_cast<double>(elapsed_us) * static_cast<double>(sample_rate_)) / 1e6);
                // The timeline runs on through a flush; what came before it is
                // not this count's.
                played = elapsed > flushed_ ? std::min(elapsed - flushed_, taken_) : 0;
            }
        }
        return audio::MonitorPosition{.frames_played = played, .frames_queued = taken_ - played, .latency_frames = 0};
    }

    void flush() override {
        // The group's own frame count goes on from where it was: the next
        // frame taken is frame flushed_ of its timeline.
        flushed_ += taken_;
        taken_ = 0;
    }

    bool pause() override { return true; }
    bool resume() override { return true; }

private:
    GroupResolver resolve_;
    std::shared_ptr<sendspin::Group> group_;
    std::size_t channels_ = 0;
    std::uint32_t sample_rate_ = 0;
    std::uint64_t taken_ = 0;
    // Frames taken before the last flush() since open().
    std::uint64_t flushed_ = 0;
    // The clock a group's timeline is on (ServerHost's own).
    sendspin::SteadyClock clock_;
    // Reused across submit_pcm() calls so steady playback allocates nothing
    // once it has grown to the largest block it has seen, the same reason
    // Player::Pending's own buffers are reused (player.hpp).
    std::vector<std::int32_t> interleaved_;
};

}  // namespace

std::unique_ptr<NetworkGroupSink> make_group_sink(GroupResolver resolve) {
    return std::make_unique<GroupSink>(std::move(resolve));
}

}  // namespace ac3::hearth
