#include "ac3forge.h"

#include "esphome/core/log.h"

#include <algorithm>

namespace esphome {
namespace ac3forge {

static const char *const TAG = "ac3forge";

void Ac3ForgeComponent::setup() {
  // Reserved once, here, rather than grown during playback: the accumulator
  // takes storage the caller owns precisely so that framing never allocates,
  // and a component that allocated it lazily on the first frame would give that
  // property away for nothing.
  storage_.resize(buffer_size_);
  accumulator_ = std::make_unique<ac3::io::AccessUnitAccumulator>(storage_);
}

void Ac3ForgeComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "ac3forge:");
  ESP_LOGCONFIG(TAG, "  Framing buffer: %u bytes", static_cast<unsigned>(buffer_size_));
}

std::size_t Ac3ForgeComponent::feed(std::span<const std::byte> bytes) {
  if (accumulator_ == nullptr) {
    return 0;
  }
  auto destination = accumulator_->writable();
  const auto taken = std::min(destination.size(), bytes.size());
  if (taken == 0) {
    return 0;
  }
  std::copy_n(bytes.begin(), taken, destination.begin());
  accumulator_->commit(taken);
  return taken;
}

void Ac3ForgeComponent::finish() {
  if (accumulator_ != nullptr) {
    accumulator_->finish();
  }
}

const std::vector<std::vector<float>> *Ac3ForgeComponent::decode() {
  if (accumulator_ == nullptr || failed_) {
    return nullptr;
  }

  const auto unit = accumulator_->next();
  using Status = ac3::io::AccessUnitAccumulator::Status;
  switch (unit.status) {
    case Status::kUnit:
      break;
    case Status::kNeedMoreInput:
    case Status::kEndOfStream:
      return nullptr;
    case Status::kBufferTooSmall:
      // Not recoverable by feeding more, so say which knob moves it rather
      // than looping silently.
      ESP_LOGE(TAG, "framing buffer of %u bytes is too small for this stream - raise buffer_size",
               static_cast<unsigned>(buffer_size_));
      failed_ = true;
      return nullptr;
    case Status::kError:
      ESP_LOGE(TAG, "stream is not readable (scan error %d)",
               static_cast<int>(accumulator_->error()));
      failed_ = true;
      return nullptr;
  }

  auto decoded = decoder_.decode_frame(unit.bytes);
  if (!decoded) {
    ESP_LOGE(TAG, "decode failed (%d)", static_cast<int>(decoded.error()));
    failed_ = true;
    return nullptr;
  }
  channels_ = std::move(decoded->channels);
  ++frames_;
  return &channels_;
}

}  // namespace ac3forge
}  // namespace esphome
