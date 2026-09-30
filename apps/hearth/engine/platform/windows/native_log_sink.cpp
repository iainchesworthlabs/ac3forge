#include "native_log_sink.hpp"

#include <string>
#include <string_view>

#include "diagnostic_log.hpp"

// Last: windows.h's own macros (small, among others) would break the
// headers above - sink_firmware.cpp's own comment on <httplib.h> carries the
// identical rule.
#include <windows.h>

namespace iclforge::hearth {

void install_native_log_sink() {
    process_diagnostics().add_observer([](std::string_view line) {
        // OutputDebugStringA wants a null-terminated buffer; string_view
        // gives no guarantee of one, and every note is already short
        // (DiagnosticLog::kMaxLine), so the copy costs nothing worth
        // avoiding.
        OutputDebugStringA(std::string(line).c_str());
    });
}

}  // namespace iclforge::hearth
