#include "session.hpp"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <iterator>
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

// Samples one access unit codes, from its independent substream's own
// numblkscod (§E2.3.1.4): 1, 2, 3 or 6 blocks. What scan() records for the
// first programme only, read here for another.
[[nodiscard]] std::uint32_t unit_samples(std::span<const std::byte> unit) {
    const auto header = io::read_frame_header(unit);
    if (!header || header->kind != io::StreamKind::kEac3) {
        return static_cast<std::uint32_t>(kSamplesPerFrame);
    }
    constexpr std::array<std::uint32_t, 4> kBlocks{1, 2, 3, 6};
    const auto code = static_cast<std::size_t>(std::clamp(header->numblkscod, 0, 3));
    return kBlocks[code] * static_cast<std::uint32_t>(kSamplesPerBlock);
}

}  // namespace

std::expected<Session, std::string> Session::open(const std::string& path,
                                                  const ItemLoader& loader,
                                                  std::optional<int> programme) {
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
    std::string note = std::move(loaded->note);

    // The programme's units, and how long each is: scan() has already read
    // the first programme's lengths; another's are read from its units.
    const io::ScannedProgramme* chosen = nullptr;
    if (programme) {
        const auto found =
            std::ranges::find(session.scanned_.programmes, *programme, &io::ScannedProgramme::substreamid);
        if (found != session.scanned_.programmes.end()) {
            chosen = &*found;
        } else {
            note += fmt::format("{}The stream has no programme {}, so its first one plays.",
                                note.empty() ? "" : " ", *programme);
        }
    }
    std::vector<std::uint32_t> lengths;
    if (chosen != nullptr && chosen != &session.scanned_.programmes.front()) {
        session.units_ = chosen->access_units;
        session.programme_ = chosen->substreamid;
        session.first_programme_ = false;
        session.facts_.channels = static_cast<std::uint16_t>(std::max(chosen->channels, 0));
        lengths.reserve(session.units_.size());
        for (const auto unit : session.units_) {
            lengths.push_back(unit_samples(unit));
        }
    } else {
        session.units_ = session.scanned_.access_units;
        session.programme_ = session.scanned_.programmes.empty()
                                 ? 0
                                 : session.scanned_.programmes.front().substreamid;
        session.facts_.channels =
            static_cast<std::uint16_t>(std::max(session.scanned_.channels, 0));
        lengths = session.scanned_.access_unit_samples;
    }
    if (session.units_.empty()) {
        return std::unexpected(fmt::format("\"{}\" holds no audio.", path));
    }
    session.starts_.assign(1, 0);
    session.starts_.reserve(lengths.size() + 1);
    for (const std::uint32_t length : lengths) {
        session.starts_.push_back(session.starts_.back() + length);
    }
    const std::uint64_t stream_samples = session.starts_.back();

    // The part the item plays, clamped to what the stream holds.
    session.window_start_ = std::min(loaded->skip_samples, stream_samples);
    session.window_end_ = stream_samples;
    if (loaded->play_samples) {
        session.window_end_ = session.window_start_ +
                              std::min(*loaded->play_samples, stream_samples - session.window_start_);
    }
    if (session.window_end_ == session.window_start_) {
        return std::unexpected(
            fmt::format("\"{}\" has nothing left to play once its edit list is applied.", path));
    }
    session.next_frame_ = 0;
    session.skip_until_ = 0;

    const std::uint32_t rate = sample_rate_hz(session.scanned_.sample_rate);
    session.facts_.stream = format_of(session.scanned_.kind);
    session.facts_.sample_rate = rate;
    if (rate != 0) {
        session.facts_.duration =
            std::chrono::milliseconds{static_cast<std::int64_t>(session.total_samples() * 1000 / rate)};
    }
    session.facts_.note = std::move(note);
    return session;
}

void Session::deliver_window(const Target& target, std::span<const std::span<const float>> slots,
                             std::size_t n) {
    // Blocks arrive in stream order - a unit held back for §3.7 comes out
    // late but never out of turn - so a running count says where each sits.
    const std::uint64_t begin = next_frame_;
    next_frame_ += n;
    const std::uint64_t from = std::max({begin, window_start_, skip_until_});
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
}

void Session::report_window(const Target& target, const UnitReport& report) {
    const std::size_t frames = *target.frames - target.unit_start;
    if (frames > 0) {
        (*target.reported)(report, frames);
    }
}

StreamDecoder::UnitFn Session::unit_reports(Target& target) {
    if (target.reported == nullptr || !*target.reported) {
        return {};
    }
    return [&target](const UnitReport& report) { report_window(target, report); };
}

std::uint32_t Session::unit_samples_at(std::uint64_t position) const {
    const std::uint64_t sample = window_start_ + position;
    // The last unit starting at or before `sample`; starts_ begins at 0, so
    // there is always one.
    const auto after = std::upper_bound(starts_.begin(), std::prev(starts_.end()), sample);
    const auto unit = static_cast<std::size_t>(std::distance(starts_.begin(), after)) - 1;
    return static_cast<std::uint32_t>(starts_[unit + 1] - starts_[unit]);
}

void Session::play_whole_units() {
    if (whole_units_) {
        return;
    }
    whole_units_ = true;
    // The unit the first sample played is in starts at or before it; the
    // unit the last one is in ends at or after the end.
    const auto first = std::upper_bound(starts_.begin(), std::prev(starts_.end()), window_start_);
    window_start_ = *std::prev(first);
    const auto last = std::lower_bound(starts_.begin(), starts_.end(), window_end_);
    window_end_ = last == starts_.end() ? starts_.back() : *last;
    if (facts_.sample_rate != 0) {
        facts_.duration = std::chrono::milliseconds{
            static_cast<std::int64_t>(total_samples() * 1000 / facts_.sample_rate)};
    }
}

std::expected<std::size_t, std::string> Session::render(StreamDecoder& decoder,
                                                        const StreamDecoder::BlockFn& deliver,
                                                        std::size_t wanted,
                                                        const ReportFn& reported,
                                                        const SentFn& sent) {
    std::size_t frames = 0;
    // Only the item's own part of the stream is handed on; the rest is
    // decoded for the decoder's sake and dropped. Two pointers captured at
    // most, so each std::function holds its callback without allocating.
    Target target{.deliver = &deliver, .frames = &frames, .reported = &reported};
    const StreamDecoder::BlockFn window = [this, &target](
                                              std::span<const std::span<const float>> slots,
                                              std::size_t n) { deliver_window(target, slots, n); };
    const StreamDecoder::UnitFn units_reported = unit_reports(target);

    const std::size_t units = units_.size();
    while (frames < wanted && next_ < units && next_frame_ < window_end_) {
        target.unit_start = frames;
        if (sent) {
            // Sent when any of it is played: not a priming unit, and not the
            // unit a part-way start decodes first and drops.
            const std::uint64_t played_from = std::max(window_start_, skip_until_);
            if (starts_[next_ + 1] > played_from && starts_[next_] < window_end_) {
                sent(units_[next_], static_cast<std::uint32_t>(starts_[next_ + 1] - starts_[next_]));
            }
        }
        const auto got = decoder.decode(units_[next_], window, units_reported);
        ++next_;
        if (!got) {
            // The unit's samples never arrive, and the decoder has let go of
            // anything it held; count past them, so the frames after land at
            // their own places.
            next_frame_ = starts_[next_];
            return std::unexpected(got.error());
        }
    }
    if (!finished_ && (next_ >= units || next_frame_ >= window_end_)) {
        // Every unit has gone in, or everything the item plays has come out.
        // Either way whatever the decoder still holds is released now - and,
        // past the window, dropped - so a finished session has delivered
        // everything it ever will and leaves the decoder clean.
        target.unit_start = frames;
        decoder.finish(window, units_reported);
        finished_ = true;
    }
    return frames;
}

void Session::start_at(std::size_t unit, StreamDecoder& decoder) {
    decoder.reset();
    finished_ = false;
    const std::size_t target = std::min(unit, units_.size());
    skip_until_ = starts_[target];
    next_ = target > 0 ? target - 1 : 0;
    next_frame_ = starts_[next_];
}

void Session::seek(std::chrono::milliseconds to, StreamDecoder& decoder) {
    const std::int64_t ms = std::max<std::int64_t>(to.count(), 0);
    const std::uint64_t offset = static_cast<std::uint64_t>(ms) * facts_.sample_rate / 1000;
    const std::uint64_t sample = window_start_ + std::min(offset, total_samples());
    // The unit covering `sample`: the last one starting at or before it, or
    // the end when `sample` is the end.
    const auto after = std::upper_bound(starts_.begin(), std::prev(starts_.end()), sample);
    const auto unit = static_cast<std::size_t>(std::distance(starts_.begin(), after)) - 1;
    start_at(sample >= starts_.back() ? units_.size() : unit, decoder);
}

void Session::hand_over(StreamDecoder& current, const StreamDecoder::BlockFn& deliver,
                        const ReportFn& reported) {
    if (finished_) {
        return;
    }
    std::size_t frames = 0;
    Target target{.deliver = &deliver, .frames = &frames, .reported = &reported};
    const StreamDecoder::BlockFn window = [this, &target](
                                              std::span<const std::span<const float>> slots,
                                              std::size_t n) { deliver_window(target, slots, n); };
    current.finish(window, unit_reports(target));
    // Everything before unit next_ has now come out, so the next decoder
    // carries on from there.
    start_at(next_, current);
}

std::uint64_t Session::position_samples() const {
    const std::uint64_t at = std::clamp(std::max(next_frame_, skip_until_), window_start_, window_end_);
    return at - window_start_;
}

}  // namespace ac3::hearth
