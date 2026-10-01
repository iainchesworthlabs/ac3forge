#pragma once

// A live tap for DiagnosticLog's notes onto whatever native, always-on debug
// channel the platform offers - Windows' OutputDebugString, which DebugView
// or an attached debugger already shows with the app doing nothing else to
// help. Exactly one platform/<os>/native_log_sink.cpp defines
// install_native_log_sink() below (CMakeLists.txt picks it, the same way
// apps/crucible/engine/platform_services.hpp's own comment states the rule
// for platform_session_monitor()) - no #ifdef selects between them, and the
// non-Windows implementation is a real, visibly-empty function rather than a
// hidden branch: nobody has asked for a native log sink on macOS or Linux,
// so this isn't inventing one.
//
// Call once, for the process's whole life - apps/hearth/ui/main.cpp does,
// not HearthController's constructor, so iclforge-tests and the Qt Quick test
// binary never register it and never interact with their own repeated
// construct/destroy cycles (DiagnosticLog::add_observer() has no remove).

namespace iclforge::hearth {

void install_native_log_sink();

}  // namespace iclforge::hearth
