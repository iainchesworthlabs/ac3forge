#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// A loopback-only HTTP endpoint, opt-in and off by default - Settings >
// Diagnostics' "Save"/"Copy"/"View live..." are the discoverable ways to get
// a report out; this is the fourth, for a script or a second terminal, not a
// button anyone sees. GET /diagnostics returns exactly what `report` returns
// at the moment of the request - the same fully-scrubbed text
// render_report() already produces for Save/Copy/the live view, never a
// lesser version of it: diagnostics_report.cpp's own second scrub pass runs
// over the whole rendered text, which a lone ring line never gets, so
// nothing here reads DiagnosticLog's ring directly.
//
// httplib stays out of this header, the same boundary sink_firmware.hpp
// already keeps, so a caller needs nothing more than this file -
// ac3sendspin_httplib is linked PRIVATE on hearth_engine.

namespace iclforge::hearth {

class DiagnosticsHttpServer {
public:
    // `report` is called fresh for every request; never cached here.
    explicit DiagnosticsHttpServer(std::function<std::string()> report);
    ~DiagnosticsHttpServer();
    DiagnosticsHttpServer(const DiagnosticsHttpServer&) = delete;
    DiagnosticsHttpServer& operator=(const DiagnosticsHttpServer&) = delete;

    // Binds 127.0.0.1:`port` and starts serving; `port` == 0 takes whichever
    // free port the OS hands back (a unit test's own way to avoid a fixed
    // one - read it back from port() once this returns). False if the bind
    // failed (a stale process already holding the port, most likely) and
    // nothing is started. Route registration lives in the constructor, not
    // here, so start() after a stop() - or after a failed start() - is safe
    // to call again.
    [[nodiscard]] bool start(std::uint16_t port);
    // No-op if not started. The destructor calls this too.
    void stop();
    // The bound port once start() has succeeded, else 0.
    [[nodiscard]] std::uint16_t port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iclforge::hearth
