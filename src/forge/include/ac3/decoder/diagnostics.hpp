#pragma once

#include <cstdint>
#include <string_view>

#include "ac3/export.hpp"

// A consumer-facing diagnostic sink: a callback hook for the recoverable,
// informational decode events a caller otherwise has no way to hear about -
// the CRC on this frame did not check out, or an EMDF container carried a
// payload id this decoder does not interpret and moved past. Neither of
// those stops the decode (see DecodeError for what does), so today they
// leave nothing behind: a CRC failure is only visible in the returned error
// (and, with DecoderConfig::concealment set, not even that - the same frame
// comes back as a SUCCESSFUL result), and a skipped EMDF payload id was never
// reported anywhere at all.
//
// A plain function pointer plus an opaque context, not std::function -
// matching DecoderConfig::trace/syntax's own pointer convention immediately
// above this field, this costs one null check and no allocation on a path
// that has to run from the minimum-footprint decoder profile
// (AC3FORGE_MINIMAL_DECODER: no exceptions, no RTTI) and from a real-time
// caller. Distinct from Tracy (ac3/internal/profiling.hpp): that answers
// "how fast", built only when AC3FORGE_ENABLE_TRACY is on and absent from
// the minimal profile entirely; this answers "what happened", always
// compiled in, and aimed at production rather than development.

namespace ac3 {

enum class DiagnosticEvent : std::uint8_t {
    // §6.2 (AC-3 crc1/crc2)/§E2.3.1.2 (E-AC-3 crc2): the CRC did not check
    // out. Reported at the moment the check fails, before concealment (if
    // any) has a chance to turn the call into a successful return - this is
    // the only signal a caller not polling DecodedFrame::concealed/
    // DecodedSubstream::concealed on every call gets.
    kCrcMismatch,
    // §H.2.2: the EMDF container carried a payload id this decoder does not
    // interpret (anything but OAMD or JOC). EMDF is designed so this never
    // fails the surrounding frame - a decoder that does not understand a
    // payload reads the rest of the frame exactly as it would without it -
    // which is exactly why nothing else here ever reported it.
    kUnknownEmdfPayload,
};

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(DiagnosticEvent event);

// What happened. Which fields beyond `event` are meaningful depends on it
// alone - unused ones stay at their default.
struct Diagnostic {
    DiagnosticEvent event;
    // kUnknownEmdfPayload only: the §H.2.2.2.2 payload id (5 bits) that was
    // skipped.
    std::uint8_t emdf_payload_id = 0;
};

// Called synchronously on the thread that called decode_frame/
// decode_substream/decode_access_unit, before that call returns - so a
// callback that logs or counts sees events in the same order the frames
// arrived, and with the same lifetime guarantees as the call itself.
// `context` is DecoderConfig::diagnostics_context, passed through unchanged.
using DiagnosticSink = void (*)(const Diagnostic& diagnostic, void* context);

}  // namespace ac3
