#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/io/stream_accumulator.hpp"

// The plumbing, not a player.
//
// This owns the two things any ESPHome integration of ac3forge needs and that
// are awkward to own from a media_player: a decoder, and the streaming framer
// that turns arriving bytes into whole access units. Another component feeds it
// bytes and takes PCM back; nothing here touches an output device, because
// which device is not this component's business.
//
// See __init__.py for what is deliberately absent.

namespace esphome {
namespace ac3forge {

class Ac3ForgeComponent : public Component {
 public:
  explicit Ac3ForgeComponent(std::size_t buffer_size) : buffer_size_(buffer_size) {}

  void setup() override;
  void dump_config() override;
  // After the sinks it might feed, before nothing in particular - it has no
  // hardware to bring up, only memory to reserve.
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Hand over however many bytes have arrived. Returns how many were taken:
  // less than offered means the framer's buffer is full and decode() has to be
  // drained before the rest will fit.
  std::size_t feed(std::span<const std::byte> bytes);

  // No more bytes are coming. Without this the last access unit never comes
  // out - a unit's end is found by reading the start of the next one.
  void finish();

  // The next decoded frame, or nullptr when more input is needed. The returned
  // channels stay valid until the next call.
  //
  // Planar, in the decoder's own coded order, exactly as DecodedFrame::channels
  // holds it - the caller decides what to do about layout, because a component
  // that folded to stereo here would be imposing a choice on every consumer.
  const std::vector<std::vector<float>> *decode();

  // Set when decode() returns nullptr for a reason that is not "feed me".
  bool failed() const { return failed_; }

  std::uint64_t frames_decoded() const { return frames_; }

 protected:
  std::size_t buffer_size_;
  std::vector<std::byte> storage_;
  std::unique_ptr<ac3::io::AccessUnitAccumulator> accumulator_;
  ac3::FrameDecoder decoder_;
  std::vector<std::vector<float>> channels_;
  std::uint64_t frames_{0};
  bool failed_{false};
};

}  // namespace ac3forge
}  // namespace esphome
