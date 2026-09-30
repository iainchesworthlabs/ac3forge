#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>

// The syntax trace both directions share: the AC-4 decoder (src/ac4dec) emits
// one record per syntax element it reads, and the AC-4 encoder one per element
// it writes, in the same shape, so that the two, and the Python reference
// parser (tools/references/ac4_syntax.py), can be compared record for record.
// Header-only; it sits with the inspector because both libraries link it.
//
// A syntax element is one entry with a bit count in a syntax table of either
// part (ETSI TS 103 190-1 V1.4.1, TS 103 190-2 V1.3.1). Its record carries the
// bit offset where it starts within its substream, how many bits it took, and
// its value:
//
//   - a fixed-width field: its width and its value as an unsigned integer;
//   - variable_bits(n) (Part 1 clause 4.2.2): one record for the whole
//     element, with the total bits and the decoded value;
//   - a Huffman codeword: its length and the index of the codeword in its
//     codebook, before any cb_off is subtracted;
//   - quad_sign_bits and pair_sign_bits: one record for the group, with as
//     many bits as there were nonzero lines and the bits as an integer;
//   - ext_code: one record for the escape, with its total length and the
//     decoded magnitude;
//   - a field whose width the stream sets and whose bits the syntax does not
//     interpret (add_data, extensions_bits, drc2_bits): one record of its
//     width, valued at its last 64 bits, split into 65535-bit records when
//     longer.
//
// byte_align, fill_bits and fill_area are not recorded. docs/verification.md
// states the whole contract.

namespace ac4 {

struct SyntaxRecord {
    int substream = 0;             // index into the frame's substream_index_table
    std::uint32_t bit_offset = 0;  // from the first bit of that substream
    std::uint16_t bits = 0;
    std::uint64_t value = 0;
    std::string_view name;         // the element's name in the syntax table
};

// Where a configuration sends its trace: ac4::DecoderConfig::syntax and
// ac4::EncoderConfig::trace. It owns a copy of the callable, so a lambda
// written in place lives as long as the configuration, and the decoder or
// encoder built from it keeps a copy of its own. What the callable refers to
// (the vector a lambda captures by reference, say) must still outlive the
// decoder or encoder, as with any callable. Empty, the default, traces
// nothing, at the cost of one branch per syntax element.
using SyntaxTrace = std::function<void(const SyntaxRecord&)>;

// A non-owning reference to any callable taking a const SyntaxRecord&, in the
// shape of ac3::BlockSink: what the readers and writers inside the libraries
// hold while they run. It refers to a named callable only; a temporary would be
// gone before the first record, so binding one does not compile.
class SyntaxSink {
   public:
    SyntaxSink() noexcept = default;

    template <typename F>
        requires std::is_lvalue_reference_v<F> && std::invocable<F, const SyntaxRecord&> &&
                     (!std::same_as<std::remove_cvref_t<F>, SyntaxSink>)
    // NOLINTNEXTLINE(google-explicit-constructor): the call site is the point
    SyntaxSink(F&& f) noexcept
        : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          call_([](void* object, const SyntaxRecord& record) {
              (*static_cast<std::remove_reference_t<F>*>(object))(record);
          }) {}

    // A temporary callable: name it first, or hand it to a SyntaxTrace, which
    // keeps a copy.
    template <typename F>
        requires(!std::is_lvalue_reference_v<F>) &&
                    (!std::same_as<std::remove_cvref_t<F>, SyntaxSink>)
    SyntaxSink(F&&) = delete;

    explicit operator bool() const noexcept { return call_ != nullptr; }
    void operator()(const SyntaxRecord& record) const { call_(object_, record); }

   private:
    void* object_ = nullptr;
    void (*call_)(void*, const SyntaxRecord&) = nullptr;
};

// The reference a reader or writer holds for `trace` while it runs: null when
// `trace` is empty, so that nothing calls an empty std::function. It takes a
// SyntaxTrace itself, never one converted from another callable, which would
// be a temporary.
template <typename T>
    requires std::same_as<T, SyntaxTrace>
[[nodiscard]] SyntaxSink sink_of(const T& trace) noexcept {
    return trace ? SyntaxSink(trace) : SyntaxSink{};
}

}  // namespace ac4
