#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

// The sliding-window bit reservoir behind E-AC-3's average-rate (ABR) mode.
//
// VBR fixes quality and lets each frame cost whatever the content asks for;
// CBR fixes the frame size and lets quality follow. Neither delivers what a
// streaming ladder rung or a DVB mux actually contracts for, which is an
// AVERAGE rate over time with the per-frame size still free to move. This is
// the accounting that makes that deliverable: a window of consecutive frames
// shares one pooled budget, so a cheap frame's leftover words are still there
// for the frame after it, and a frame that overspends is paid for by the
// frames it slides past.
//
// Deliberately just the accounting - it hands out a word allowance and takes
// back what was actually spent. Which SNR offset fits that allowance is
// snr_search.hpp's monotone search, exactly as it is for CBR; ABR is that
// same search against a budget that moves.
//
// Internal to src/forge/src/encoder/ for the same reason snr_search.hpp is:
// plumbing between the encoder's own translation units, not library surface.

namespace ac3::internal {

class BitReservoir {
   public:
    // target_words: the per-frame word count the average is held to -
    // frame_words(sample_rate, abr.target_kbps). window_frames: how many
    // consecutive frames share one pooled budget; 1 makes every frame stand
    // on its own (the degenerate, CBR-sized case) and is legal.
    //
    // The window starts out recording every past frame as having spent
    // exactly its share, so the very first frame's allowance is one frame's
    // target rather than the whole window's budget. Without that a stream's
    // opening frame would be free to spend a second of bits on 32 ms of
    // audio, which is not what "average 192 kbit/s" is understood to allow.
    BitReservoir(std::uint32_t target_words, std::uint32_t window_frames)
        : target_words_(target_words),
          window_(window_frames > 1 ? window_frames - 1 : 0, target_words),
          spent_(static_cast<std::uint64_t>(window_.size()) * target_words) {}

    // Words this frame may spend: the window's whole budget less what the
    // frames still inside the window already spent. Floored at zero - a
    // frame that overspent its share leaves later frames with nothing to
    // draw on, not with a negative allowance to somehow honour.
    [[nodiscard]] std::uint32_t allowance() const {
        const std::uint64_t budget =
            static_cast<std::uint64_t>(window_.size() + 1) * target_words_;
        if (spent_ >= budget) {
            return 0;
        }
        return static_cast<std::uint32_t>(
            std::min<std::uint64_t>(budget - spent_, 0xFFFFFFFFULL));
    }

    // Records what the frame ACTUALLY cost - including any padding a
    // min_kbps floor forced, and any overshoot the allowance could not stop
    // (a frame is never smaller than the syntax it must carry) - and slides
    // the window on. Honest accounting is the point: an overspend that is
    // not recorded is an average the stream quietly misses.
    void commit(std::uint32_t words) {
        if (window_.empty()) {
            return;
        }
        spent_ -= window_[next_];
        window_[next_] = words;
        spent_ += words;
        next_ = (next_ + 1) % window_.size();
    }

    [[nodiscard]] std::uint32_t target_words() const { return target_words_; }
    [[nodiscard]] std::size_t window_frames() const { return window_.size() + 1; }

   private:
    std::uint32_t target_words_;
    // The window_frames - 1 frames BEFORE this one; this frame is the last
    // slot and is what allowance() is being asked about.
    std::vector<std::uint32_t> window_;
    std::uint64_t spent_ = 0;
    std::size_t next_ = 0;
};

// The rate control ABR actually runs: the window above, plus the composite
// SNR offset it steers.
//
// The offset is the point. Sizing each frame by searching for the largest
// offset that fits a budget - CBR's rate control, and what a "budget from a
// reservoir" design reduces to - spends the whole budget every frame by
// construction, so the frame size stops following the content and ABR
// becomes CBR with extra steps. That was measured, not assumed: with the
// offset capped at a fixed quality the reservoir banked words it could never
// spend, and scored 1.75 dB BELOW plain CBR at the same average rate on
// material with real dynamics.
//
// So the offset is HELD across frames and nudged instead. At a fixed offset
// a quiet frame is genuinely cheaper than a loud one - that is the whole of
// VBR - and an integral controller moves the offset until the average of
// those varying costs is the rate that was asked for. The window's allowance
// stays on as a hard ceiling underneath, so no window can overrun its budget
// however the controller is behaving.
class AbrController {
   public:
    AbrController(std::uint32_t target_words, std::uint32_t window_frames)
        : reservoir_(target_words, window_frames) {}

    [[nodiscard]] std::uint32_t allowance() const { return reservoir_.allowance(); }
    [[nodiscard]] std::uint32_t target_words() const { return reservoir_.target_words(); }

    // The composite offset to encode this frame at, or nullopt before any
    // frame has been encoded: there is nothing to steer from yet, so the
    // caller runs the same budget-fitting search CBR does and reports what it
    // found through observe_search below.
    [[nodiscard]] std::optional<int> offset() const {
        if (!operating_) {
            return std::nullopt;
        }
        return std::clamp(static_cast<int>(std::lround(*operating_)), 0, kMaxComposite);
    }

    // A search against a real budget settled on `composite` - the first
    // frame's seed, or a later frame whose allowance capped what the
    // controller asked for. Either way it is an offset now KNOWN to fit, so
    // the operating point never stays above it.
    void observe_search(int composite) {
        const auto found = static_cast<double>(composite);
        operating_ = operating_ ? std::min(*operating_, found) : found;
    }

    // Records the frame's real size and steers the offset by how far that
    // size landed from one frame's share. Integral, not proportional: the
    // offset keeps moving while the stream is off its target rate and stops
    // moving when it is on it, which is what makes the average a delivered
    // figure rather than a direction of travel.
    void commit(std::uint32_t words) {
        reservoir_.commit(words);
        if (!operating_) {
            return;
        }
        const auto target = static_cast<double>(reservoir_.target_words());
        *operating_ += kGain * (target - static_cast<double>(words)) / target;
        *operating_ = std::clamp(*operating_, 0.0, static_cast<double>(kMaxComposite));
    }

   private:
    // (csnroffst << 4) | fsnroffst, the search space snr_search.hpp walks.
    static constexpr int kMaxComposite = 1023;
    // Composite units per whole frame of over- or under-spend. The offset
    // scale is not linear in bit cost - about 100 units is a 1.5x change in
    // rate on real material - so a frame that costs double its share pulls
    // the offset down by roughly a sixth of that, which settles within a few
    // frames without ringing. Large enough to track a scene change, small
    // enough that one atypical frame does not re-rate the stream around it.
    static constexpr double kGain = 16.0;
    BitReservoir reservoir_;
    // Unset until the first frame has been encoded; see offset().
    std::optional<double> operating_;
};

}  // namespace ac3::internal
