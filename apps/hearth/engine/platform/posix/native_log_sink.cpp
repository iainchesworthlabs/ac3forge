#include "native_log_sink.hpp"

// The visible, deliberate absence native_log_sink.hpp's own comment
// promises, not a silently-skipped branch: nobody has asked for a native
// log sink on macOS or Linux, so this installs none rather than inventing
// one.

namespace ac3::hearth {

void install_native_log_sink() {}

}  // namespace ac3::hearth
