#include "session.hpp"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <numeric>
#include <span>
#include <utility>

#include "ac3/core/tables.hpp"
#include "ac3/render/layout.hpp"

// See session.hpp.

namespace ac3::hearth {

namespace {

// Passthrough's question about the stream, which is a different question
// from what the bytes are: an AC-3 core carrying E-AC-3 dependents only
// reaches a sink whole as E-AC-3.
[[nodiscard]] audio::BitstreamFormat format_of(io::StreamKind kind) {
    return kind == io::StreamKind::kAc3 ? audio::BitstreamFormat::kAc3
                                        : audio::BitstreamFormat::kEac3;
}

}  // namespace

std::expected<Session, std::string> Session::open(const std::string& path,
                                                  const ItemLoader& loader) {
    if (!loader) {
        return std::unexpected(std::string{"Nothing is set up to read items."});
    }
    auto loaded = loader(path);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }

    Session session;
    session.bytes_ = std::move(loaded->bytes);
    auto scanned = io::scan(session.bytes_);
    if (!scanned) {
        return std::unexpected(fmt::format(
            "\"{}\" is not an AC-3 or E-AC-3 stream this player can read.", path));
    }
    if (scanned->access_units.empty()) {
        return std::unexpected(fmt::format("\"{}\" holds no audio.", path));
    }
    session.scanned_ = std::move(*scanned);
    const std::uint64_t stream_samples = std::accumulate(
        session.scanned_.access_unit_samples.begin(), session.scanned_.access_unit_samples.end(),
        std::uint64_t{0});

    // The part the item plays, clamped to what the stream holds.
    session.window_start_ = std::min(loaded->skip_samples, stream_samples);
    session.window_end_ = stream_samples;
    if (loaded->play_samples) {
        session.window_end_ =
            session.window_start_ + std::min(*loaded->play_samples, stream_samples - session.window_start_);
    }
    if (session.window_end_ == session.window_start_) {
        return std::unexpected(
            fmt::format("\"{}\" has nothing left to play once its edit list is applied.", path));
    }

    const std::uint32_t rate = sample_rate_hz(session.scanned_.sample_rate);
    session.facts_.stream = format_of(session.scanned_.kind);
    session.facts_.sample_rate = rate;
    session.facts_.channels = static_cast<std::uint16_t>(std::max(session.scanned_.channels, 0));
    if (rate != 0) {
        session.facts_.duration =
            std::chrono::milliseconds{static_cast<std::int64_t>(session.total_samples() * 1000 / rate)};
    }
    session.facts_.note = std::move(loaded->note);
    return session;
}

std::uint64_t Session::unit_start(std::size_t unit) const {
    const std::size_t upto = std::min(unit, scanned_.access_unit_samples.size());
    return std::accumulate(scanned_.access_unit_samples.begin(),
                           scanned_.access_unit_samples.begin() + static_cast<std::ptrdiff_t>(upto),
                           std::uint64_t{0});
}

std::expected<std::size_t, std::string> Session::render(StreamDecoder& decoder,
                                                        const StreamDecoder::BlockFn& deliver,
                                                        std::size_t wanted) {
    std::size_t frames = 0;
    // Only the item's own part of the stream is handed on; the rest is
    // decoded for the decoder's sake and dropped here. Blocks arrive in stream
    // order - a unit held back for §3.7 comes out late but never out of turn -
    // so a running count says where each one sits. Two pointers captured, so
    // the std::function holds it without allocating.
    struct Target {
        const StreamDecoder::BlockFn* deliver;
        std::size_t* frames;
    } target{&deliver, &frames};
    const StreamDecoder::BlockFn trimmed = [this, &target](
                                               std::span<const std::span<const float>> slots,
                                               std::size_t n) {
        const std::uint64_t begin = next_frame_;
        next_frame_ += n;
        const std::uint64_t from = std::max(begin, window_start_);
        const std::uint64_t to = std::min(begin + n, window_end_);
        if (from >= to) {
            return;
        }
        const auto offset = static_cast<std::size_t>(from - begin);
        const auto count = static_cast<std::size_t>(to - from);
        *target.frames += count;
        if (offset == 0 && count == n) {
            (*target.deliver)(slots, n);
            return;
        }
        std::array<std::span<const float>, render::OutputLayout::kMaxSlots> views{};
        const std::size_t width = std::min(slots.size(), views.size());
        for (std::size_t slot = 0; slot < width; ++slot) {
            views[slot] = slots[slot].subspan(offset, count);
        }
        (*target.deliver)(std::span<const std::span<const float>>(views.data(), width), count);
    };

    const std::size_t units = scanned_.access_units.size();
    while (frames < wanted && next_ < units && next_frame_ < window_end_) {
        const auto got = decoder.decode(scanned_.access_units[next_], trimmed);
        ++next_;
        if (!got) {
            // The unit's samples never arrive; count past them, so the frames
            // after it still land at their own places in the window.
            next_frame_ = unit_start(next_);
            return std::unexpected(got.error());
        }
    }
    if (!finished_ && (next_ >= units || next_frame_ >= window_end_)) {
        // Every unit has gone in, or everything the item plays has come out.
        // Either way whatever the decoder still holds is released now - and,
        // past the window, dropped - so a finished session has delivered
        // everything it ever will and leaves the decoder clean.
        decoder.finish(trimmed);
        finished_ = true;
    }
    return frames;
}

void Session::seek(std::chrono::milliseconds to, StreamDecoder& decoder) {
    decoder.reset();
    finished_ = false;
    const std::int64_t ms = std::max<std::int64_t>(to.count(), 0);
    const std::uint64_t offset = static_cast<std::uint64_t>(ms) * facts_.sample_rate / 1000;
    const std::uint64_t sample = window_start_ + std::min(offset, total_samples());
    const auto unit = io::access_unit_at_sample(scanned_, sample);
    next_ = unit.value_or(scanned_.access_units.size());
    next_frame_ = unit_start(next_);
}

std::uint64_t Session::position_samples() const {
    const std::uint64_t at = std::clamp(next_frame_, window_start_, window_end_);
    return at - window_start_;
}

}  // namespace ac3::hearth
