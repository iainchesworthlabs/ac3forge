#include "play_meters.hpp"

#include <algorithm>
#include <utility>

#include "ac3/core/tables.hpp"

// See play_meters.hpp.

namespace ac3::hearth {

namespace {

// BS.1770's short-term window. Once a new item's meter has been fed this
// long, its windows hold all that the meter of the item before could offer.
constexpr std::uint64_t kShortTermSeconds = 3;

// The loudness meter's rate code. A rate the stream codes and the table does
// not name falls back to 48 kHz, whose K-weighting is the reference design.
[[nodiscard]] SampleRate rate_code(std::uint32_t rate) {
    for (const SampleRate code : {SampleRate::k48000, SampleRate::k44100, SampleRate::k32000}) {
        if (sample_rate_hz(code) == rate) {
            return code;
        }
    }
    return SampleRate::k48000;
}

}  // namespace

PlayMeters::PlayMeters(const render::OutputLayout& layout, std::uint32_t sample_rate,
                       std::size_t interval)
    : rate_(sample_rate),
      interval_(std::max<std::size_t>(interval, 1)),
      slots_(layout.slots()),
      // The acmod only names the first channel; nothing here reads a name or
      // a direction from the meter, since the output's slots have their own.
      levels_(Acmod::k1_0, false, sample_rate, static_cast<int>(std::max<std::size_t>(slots_, 1))) {
    for (std::size_t slot = 0; slot < slots_; ++slot) {
        const auto& location = layout.slot(slot).location;
        if (location && loudness_layout_.count < eac3::chanmap::kMaxChannels) {
            loudness_slots_.push_back(slot);
            loudness_layout_.items[static_cast<std::size_t>(loudness_layout_.count)] = *location;
            ++loudness_layout_.count;
        }
    }
    loudness_views_.resize(loudness_slots_.size());
    if (!loudness_slots_.empty()) {
        loudness_.emplace(make_loudness());
    }
}

meta::LoudnessMeter PlayMeters::make_loudness() const {
    return meta::LoudnessMeter{rate_code(rate_), loudness_layout_};
}

void PlayMeters::meter(std::span<const std::span<const float>> slots, std::size_t frames,
                       std::uint64_t output_end) {
    if (frames == 0) {
        return;
    }
    levels_.process(slots);
    if (loudness_) {
        for (std::size_t i = 0; i < loudness_slots_.size(); ++i) {
            const std::size_t slot = loudness_slots_[i];
            loudness_views_[i] = slot < slots.size() ? slots[slot].first(std::min(frames, slots[slot].size()))
                                                     : std::span<const float>{};
        }
        loudness_->push(loudness_views_);
        programme_frames_ += frames;
        since_programme_read_ += frames;
        if (outgoing_) {
            outgoing_->push(loudness_views_);
            if (programme_frames_ >= kShortTermSeconds * rate_) {
                outgoing_.reset();
            }
        }
    }
    since_snapshot_ += frames;
    while (since_snapshot_ >= interval_) {
        since_snapshot_ -= interval_;
        // The block is short against the interval, so its end stands for
        // where the interval ended.
        take_snapshot(output_end);
    }
}

void PlayMeters::take_snapshot(std::uint64_t output_frame) {
    if (count_ == ring_.size()) {
        // Full: turn the ring so its oldest entry comes first, then grow it.
        std::rotate(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(head_), ring_.end());
        head_ = 0;
        ring_.resize(ring_.empty() ? 16 : ring_.size() * 2);
    }
    MeterSnapshot& snapshot = ring_[(head_ + count_) % ring_.size()];
    ++count_;
    snapshot.output_frame = output_frame;
    const auto levels = levels_.levels();
    snapshot.levels.assign(levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(
                                                                std::min(levels.size(), slots_)));
    if (loudness_) {
        // The windows come from whichever meter has been fed the longer run
        // of what was just played.
        const meta::LoudnessMeter& recent = outgoing_ ? *outgoing_ : *loudness_;
        snapshot.momentary_lkfs = recent.momentary_lkfs();
        snapshot.short_term_lkfs = recent.short_term_lkfs();
        if (since_programme_read_ >= rate_) {
            since_programme_read_ = 0;
            integrated_lkfs_ = loudness_->integrated_lkfs();
            loudness_range_ = loudness_->loudness_range();
        }
        snapshot.integrated_lkfs = integrated_lkfs_;
        snapshot.loudness_range = loudness_range_;
        snapshot.true_peak_dbtp = loudness_->true_peak_dbtp();
    } else {
        snapshot.momentary_lkfs.reset();
        snapshot.short_term_lkfs.reset();
        snapshot.integrated_lkfs.reset();
        snapshot.loudness_range.reset();
        snapshot.true_peak_dbtp.reset();
    }
}

void PlayMeters::restart_programme() {
    // A meter fed nothing yet has nothing to start again.
    if (!loudness_ || programme_frames_ == 0) {
        return;
    }
    // A meter still bridging an earlier join has been fed since before it,
    // longer than this item's, so it stays and this item's simply goes.
    if (!outgoing_) {
        outgoing_.emplace(std::move(*loudness_));
    }
    loudness_.emplace(make_loudness());
    programme_frames_ = 0;
    since_programme_read_ = 0;
    integrated_lkfs_.reset();
    loudness_range_.reset();
}

void PlayMeters::restart_timeline() {
    head_ = 0;
    count_ = 0;
    since_snapshot_ = 0;
    levels_.reset();
    outgoing_.reset();
    if (loudness_ && programme_frames_ != 0) {
        loudness_.emplace(make_loudness());
    }
    programme_frames_ = 0;
    since_programme_read_ = 0;
    integrated_lkfs_.reset();
    loudness_range_.reset();
}

bool PlayMeters::release(std::uint64_t heard, MeterSnapshot& latest) {
    const MeterSnapshot* newest = nullptr;
    while (count_ != 0 && ring_[head_].output_frame <= heard) {
        newest = &ring_[head_];
        head_ = (head_ + 1) % ring_.size();
        --count_;
    }
    if (newest == nullptr) {
        return false;
    }
    // Copied member by member so `latest` keeps its vector's capacity.
    latest.output_frame = newest->output_frame;
    latest.levels.assign(newest->levels.begin(), newest->levels.end());
    latest.momentary_lkfs = newest->momentary_lkfs;
    latest.short_term_lkfs = newest->short_term_lkfs;
    latest.integrated_lkfs = newest->integrated_lkfs;
    latest.loudness_range = newest->loudness_range;
    latest.true_peak_dbtp = newest->true_peak_dbtp;
    return true;
}

}  // namespace ac3::hearth
