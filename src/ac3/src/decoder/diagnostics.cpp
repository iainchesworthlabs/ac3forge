#include "iclforge/ac3/decoder/diagnostics.hpp"

#include <string_view>

namespace iclforge::ac3 {

std::string_view describe(DiagnosticEvent event) {
    switch (event) {
        case DiagnosticEvent::kCrcMismatch:
            return "the frame's CRC does not check out";
        case DiagnosticEvent::kUnknownEmdfPayload:
            return "an EMDF payload with an unrecognised id was skipped";
    }
    return "unknown diagnostic event";
}

}  // namespace iclforge::ac3
