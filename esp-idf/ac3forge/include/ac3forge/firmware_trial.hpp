#pragma once

#include <cstdint>
#include <optional>

// When an image an update wrote has proved itself, and when it has had long
// enough to (planning/esp32-ota.md, "The trial").
//
// The rollback bootloader boots a newly written slot once, marked
// PENDING_VERIFY. The image then has to say it is good -
// esp_ota_mark_app_valid_cancel_rollback() - or any reset brings the previous
// image back. It says so once every condition the board's owner names (for a
// Hearth sink: a network address, the HTTP server, the Sendspin player) has
// held for `hold_ms` without a break, and it gives up -
// esp_ota_mark_app_invalid_rollback_and_reboot() - if that has not happened
// `deadline_ms` after the trial began, as the image booted.
//
// Times are milliseconds on a clock that need not start at zero with the
// image. esp_restart() resets the S3's system timer, and with it
// esp_timer_get_time(), but QEMU's S3 carries the count on through the
// restart, so there an image an update booted on a board that had been up
// for an hour started its trial an hour in, past its deadline. The trial
// counts from the time it was started with, and the arithmetic is modulo
// 2^32, so a clock that wraps does no harm either.
//
// Free of ESP-IDF, like firmware_image.hpp beside it: firmware.cpp calls step()
// once a second from its trial task with esp_timer's milliseconds and whether
// the owner's conditions hold, and does what it answers.
// tests/io/test_firmware_trial.cpp runs the rules on a laptop.

namespace ac3forge {

struct TrialPolicy {
    // How long every condition has to hold without a break.
    std::uint32_t hold_ms = 30'000;
    // How long after the trial began the image has to get there.
    std::uint32_t deadline_ms = 300'000;
};

enum class TrialStep : std::uint8_t { kWait, kAccept, kRollBack };

class Trial {
   public:
    // `started_ms` is the clock's reading as the trial begins.
    Trial(TrialPolicy policy, std::uint32_t started_ms) : policy_(policy), started_ms_(started_ms) {}

    // `now_ms` is the clock's reading now; `healthy` whether every condition
    // holds now. A break restarts the hold: an image whose network comes and
    // goes has not shown it keeps one. Once it has answered kAccept or
    // kRollBack it goes on answering the same.
    [[nodiscard]] TrialStep step(std::uint32_t now_ms, bool healthy) {
        if (decided_) {
            return *decided_;
        }
        if (healthy) {
            if (!healthy_since_) {
                healthy_since_ = now_ms;
            }
            if (static_cast<std::uint32_t>(now_ms - *healthy_since_) >= policy_.hold_ms) {
                decided_ = TrialStep::kAccept;
                return *decided_;
            }
        } else {
            healthy_since_.reset();
        }
        if (elapsed_ms(now_ms) >= policy_.deadline_ms) {
            decided_ = TrialStep::kRollBack;
            return *decided_;
        }
        return TrialStep::kWait;
    }

    // How long the conditions have held without a break, for GET /firmware.
    [[nodiscard]] std::uint32_t healthy_for_ms(std::uint32_t now_ms) const {
        return healthy_since_ ? static_cast<std::uint32_t>(now_ms - *healthy_since_) : 0;
    }

    // How long is left before the deadline, for GET /firmware.
    [[nodiscard]] std::uint32_t remaining_ms(std::uint32_t now_ms) const {
        const std::uint32_t elapsed = elapsed_ms(now_ms);
        return elapsed >= policy_.deadline_ms ? 0 : policy_.deadline_ms - elapsed;
    }

    [[nodiscard]] const TrialPolicy& policy() const { return policy_; }

   private:
    [[nodiscard]] std::uint32_t elapsed_ms(std::uint32_t now_ms) const {
        return static_cast<std::uint32_t>(now_ms - started_ms_);
    }

    TrialPolicy policy_;
    std::uint32_t started_ms_;
    std::optional<std::uint32_t> healthy_since_;
    std::optional<TrialStep> decided_;
};

}  // namespace ac3forge
