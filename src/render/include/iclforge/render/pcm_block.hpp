#pragma once

#include <concepts>
#include <memory>
#include <span>
#include <type_traits>

#include "iclforge/objects/oamd.hpp"

namespace iclforge::render {

// One block of a decoded programme's PCM, as the *_by_block forms hand it
// over: kSamplesPerBlock samples of every output slot, in the same slot order
// the *_into forms write - the rendered layout's for an access unit, coded
// order for an AC-3 frame and for dual mono - after the output stage has run,
// so the samples are exactly what the *_into form would have written into a
// caller's spans, delivered a block at a time. The spans view the decoder's
// own storage and are valid for the duration of the call that receives them,
// never past it.
struct PcmBlock {
    int index = 0;   // 0 .. blocks-1, in order
    int blocks = 0;  // six for AC-3; numblkscod's count for an E-AC-3 unit
    std::span<const std::span<const float>> channels;
    // The programme's reconstructed objects for the same block, when the unit
    // carried an object layer and the decoder reconstructed it: one span per
    // JOC output, kSamplesPerBlock samples each, the block of
    // DecodedAccessUnit::object_audio `channels` is the block of. Parallel to
    // `object_indices`, whose entries mean what DecodedAccessUnit::object_indices'
    // do, and described by `object_metadata` (DecodedAccessUnit::object_metadata,
    // the same optional's contents, or null when it is unset). All three are
    // empty for an AC-3 frame, for a bed-only decode
    // (DecoderConfig::skip_object_reconstruction) and for a unit with no object
    // layer, and view the decoder's own storage for the duration of the call
    // exactly as `channels` does. What they are for: a sink placing objects on
    // loudspeakers gets everything it needs a block at a time, with nothing
    // copied - the value form's object_audio is a frame of copies per object.
    std::span<const std::span<const float>> objects;
    std::span<const int> object_indices;
    const oba::DecodedProgram* object_metadata = nullptr;
};

// A caller's receiver for PcmBlocks. A non-owning reference to any callable,
// so handing one to a decoder costs no allocation and a sink that fills a DMA
// ring needs one block of storage per slot where the *_into forms need a
// frame - twelve kilobytes rather than seventy-three for a 7.1.4 programme,
// on a part with 280 KB free. The callable must outlive the decode call it
// is handed to; for a lambda written in the call itself that is automatic.
class BlockSink {
   public:
    template <typename F>
        requires std::invocable<F&, const PcmBlock&> &&
                 (!std::same_as<std::remove_cvref_t<F>, BlockSink>)
    // NOLINTNEXTLINE(google-explicit-constructor): the call site is the point
    BlockSink(F&& f) noexcept
        : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          call_([](void* object, const PcmBlock& block) {
              (*static_cast<std::remove_reference_t<F>*>(object))(block);
          }) {}

    void operator()(const PcmBlock& block) const { call_(object_, block); }

   private:
    void* object_;
    void (*call_)(void*, const PcmBlock&);
};

}  // namespace iclforge::render
