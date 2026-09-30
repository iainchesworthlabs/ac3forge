#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "ac3forge/log_ring.hpp"

// The console's recent output, for GET /log (planning/esp32-ota.md, O4): a
// board updated over its network usually has no cable on it, and its console
// is where it says what went wrong.

namespace ac3forge {

// Starts keeping the console's last `bytes` bytes (CONFIG_AC3FORGE_LOG_BYTES),
// in PSRAM where the board has it and in internal RAM otherwise. Call it first
// thing in app_main, so that the boot's own lines are kept. False, keeping
// nothing, when `bytes` is 0 or the memory is not there.
bool log_start(std::size_t bytes);

// What the ring holds from byte `from` on, at most `limit` bytes of it
// (LogRing::read); nothing when log_start kept no ring.
std::optional<LogRing::Read> log_read(std::uint64_t from, std::size_t limit);

// While one lives, what this task writes to the console stays out of the ring:
// for what the console alone may show, such as the Sendspin pairing token,
// which pairs a server with no code. Whoever reads the console is holding the
// board; whoever reads GET /log need only be on its network.
class ConsoleOnly {
   public:
    ConsoleOnly();
    ~ConsoleOnly();
    ConsoleOnly(const ConsoleOnly&) = delete;
    ConsoleOnly& operator=(const ConsoleOnly&) = delete;
};

}  // namespace ac3forge
