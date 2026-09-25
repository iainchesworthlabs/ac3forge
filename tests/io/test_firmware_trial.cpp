// When an image an update wrote accepts itself or gives up, tested on the
// host - see ac3forge/firmware_trial.hpp's own header comment.

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "ac3forge/firmware_trial.hpp"

using ac3forge::Trial;
using ac3forge::TrialPolicy;
using ac3forge::TrialStep;

namespace {

TrialPolicy policy() {
    TrialPolicy p;
    p.hold_ms = 30'000;
    p.deadline_ms = 300'000;
    return p;
}

}  // namespace

TEST_CASE("an image healthy from the start is accepted once the hold has passed", "[io][firmware_trial]") {
    Trial trial(policy(), 0);
    CHECK(trial.step(5'000, true) == TrialStep::kWait);
    CHECK(trial.step(20'000, true) == TrialStep::kWait);
    CHECK(trial.healthy_for_ms(20'000) == 15'000);
    CHECK(trial.step(34'999, true) == TrialStep::kWait);
    CHECK(trial.step(35'000, true) == TrialStep::kAccept);
}

TEST_CASE("a break in the conditions starts the hold again", "[io][firmware_trial]") {
    // A network that comes and goes has not shown the image keeps one.
    Trial trial(policy(), 0);
    CHECK(trial.step(10'000, true) == TrialStep::kWait);
    CHECK(trial.step(35'000, false) == TrialStep::kWait);
    CHECK(trial.healthy_for_ms(35'000) == 0);
    CHECK(trial.step(36'000, true) == TrialStep::kWait);
    CHECK(trial.step(65'999, true) == TrialStep::kWait);
    CHECK(trial.step(66'000, true) == TrialStep::kAccept);
}

TEST_CASE("an image that never becomes healthy is rolled back at the deadline", "[io][firmware_trial]") {
    Trial trial(policy(), 0);
    CHECK(trial.step(299'999, false) == TrialStep::kWait);
    CHECK(trial.remaining_ms(299'999) == 1);
    CHECK(trial.step(300'000, false) == TrialStep::kRollBack);
    CHECK(trial.remaining_ms(300'001) == 0);
}

TEST_CASE("a hold that has not finished by the deadline is a rollback", "[io][firmware_trial]") {
    // The network came up four and a half minutes in: 20 s held at the deadline.
    Trial trial(policy(), 0);
    CHECK(trial.step(280'000, true) == TrialStep::kWait);
    CHECK(trial.step(300'000, true) == TrialStep::kRollBack);
}

TEST_CASE("a hold that finishes exactly at the deadline is accepted", "[io][firmware_trial]") {
    Trial trial(policy(), 0);
    CHECK(trial.step(270'000, true) == TrialStep::kWait);
    CHECK(trial.step(300'000, true) == TrialStep::kAccept);
}

TEST_CASE("once decided, the trial answers the same whatever follows", "[io][firmware_trial]") {
    Trial accepted(policy(), 0);
    CHECK(accepted.step(0, true) == TrialStep::kWait);
    CHECK(accepted.step(30'000, true) == TrialStep::kAccept);
    CHECK(accepted.step(31'000, false) == TrialStep::kAccept);
    CHECK(accepted.step(400'000, false) == TrialStep::kAccept);

    Trial rolled_back(policy(), 0);
    CHECK(rolled_back.step(300'000, false) == TrialStep::kRollBack);
    CHECK(rolled_back.step(301'000, true) == TrialStep::kRollBack);
}

TEST_CASE("the deadline counts from the trial's start, not from the clock's zero", "[io][firmware_trial]") {
    // QEMU's S3 carries esp_timer on through esp_restart(): the image an
    // update boots on a board that had been up for an hour starts its trial
    // an hour in, and must still have its five minutes.
    constexpr std::uint32_t kHour = 3'600'000;
    Trial trial(policy(), kHour);
    CHECK(trial.remaining_ms(kHour) == 300'000);
    CHECK(trial.step(kHour + 1'000, false) == TrialStep::kWait);
    CHECK(trial.step(kHour + 5'000, true) == TrialStep::kWait);
    CHECK(trial.remaining_ms(kHour + 5'000) == 295'000);
    CHECK(trial.step(kHour + 35'000, true) == TrialStep::kAccept);

    Trial never(policy(), kHour);
    CHECK(never.step(kHour + 299'999, false) == TrialStep::kWait);
    CHECK(never.step(kHour + 300'000, false) == TrialStep::kRollBack);
}

TEST_CASE("a clock that wraps during the trial does no harm", "[io][firmware_trial]") {
    // esp_timer's milliseconds, cut to 32 bits, wrap after 49.7 days.
    constexpr std::uint32_t kStart = 0xFFFF'FFFFU - 10'000U;  // 10 s before the wrap
    Trial trial(policy(), kStart);
    CHECK(trial.step(kStart + 5'000U, true) == TrialStep::kWait);
    const std::uint32_t after_wrap = kStart + 35'000U;  // wraps to 24'999
    CHECK(after_wrap == 24'999);
    CHECK(trial.healthy_for_ms(after_wrap) == 30'000);
    CHECK(trial.remaining_ms(after_wrap) == 265'000);
    CHECK(trial.step(after_wrap, true) == TrialStep::kAccept);

    Trial never(policy(), kStart);
    CHECK(never.step(kStart + 299'999U, false) == TrialStep::kWait);
    CHECK(never.step(kStart + 300'000U, false) == TrialStep::kRollBack);
}
