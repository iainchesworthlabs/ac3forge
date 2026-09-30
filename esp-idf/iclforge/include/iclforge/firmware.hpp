#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ac3forge/firmware_trial.hpp"

// Updates over the network (planning/esp32-ota.md): the routes Control
// forwards here, flash mode, the upload into the slot that is not running,
// the trial of an image an update wrote, and the report of all of it.
//
//   GET  /firmware           what each slot holds, the trial, an upload's
//                            progress, how the last update ended, and the
//                            partition table as the board has it, as JSON
//                            (firmware_status.hpp)
//   PUT  /firmware           body: an application image. Enters flash mode,
//                            writes the other slot, checks it and restarts
//                            into it
//   PUT  /firmware/mode      body: flash, or normal (a restart into the image
//                            that runs now)
//   PUT  /firmware/rollback  the other slot's image boots next; restarts
//   POST /restart            restarts into the image that runs now
//
// The owner supplies what only it knows through FirmwareHooks: how to stop
// everything that plays, what the trial waits for, the names the board
// answers to, and where its network comes from. Everything that needs no
// ESP-IDF is in firmware_image.hpp, firmware_trial.hpp and firmware_status.hpp
// beside this, tested on the host.

struct httpd_req;

namespace ac3forge {

struct FirmwareHooks {
    // Flash mode's teardown (planning/esp32-ota.md, "Flash mode"): end any
    // play, tell servers the board is going and stop the Sendspin player,
    // close the sink, withdraw the mDNS service. Run once, from the task an
    // upload or PUT /firmware/mode runs on; returns when it is done. Nothing
    // is brought back up afterwards: every way out of flash mode is a restart.
    std::function<void()> enter_flash_mode;
    // The trial's conditions, each named in words a person reads, with
    // whether it holds now ("a network address", true). Called about once a
    // second from a timer while an image is on trial; keep it quick.
    std::function<std::vector<std::pair<std::string, bool>>()> trial_conditions;
    // The names the board answers to, for the Host check on the firmware
    // PUTs: its mDNS host name, without ".local".
    std::function<std::vector<std::string>()> host_names;
    // Where the board's network comes from: "stored", "built-in", "wired" or
    // "none" (FirmwareStatus::network).
    std::function<const char*()> network_source;
    // Before a restart this makes on a request - an update, a rollback, a
    // restart, leaving flash mode - and before the trial gives up: the last
    // chance to tell servers the board is going. Not called for the restart a
    // panic makes.
    std::function<void()> before_restart;
};

struct FirmwareConfig {
    TrialPolicy trial;
    // Flash mode with no upload in progress for this long restarts the board
    // into the image it runs.
    std::uint32_t flash_mode_idle_ms = 600'000;
    // The upload task's stack, from internal RAM: a task that writes flash
    // may not have its stack where the cache is needed to reach it.
    std::size_t task_stack_bytes = 8192;
    // The receive buffer, from internal RAM.
    std::size_t buffer_bytes = 4096;
    // The longest an upload may take, from its first byte to its last: a
    // client that sends a byte every few seconds never stalls for long enough
    // to be dropped, and would otherwise keep the board busy - refusing
    // restarts, rollbacks and leaving flash mode - until its power was cut.
    std::uint32_t upload_deadline_ms = 600'000;
    // CI's switches: an image that never reports healthy, and one that panics
    // as it starts a trial, for the rollback tests under QEMU. Never set in a
    // board's build.
    bool test_unhealthy = false;
    bool test_panic_at_trial = false;
};

class Firmware {
   public:
    Firmware();
    ~Firmware();
    Firmware(const Firmware&) = delete;
    Firmware& operator=(const Firmware&) = delete;

    // At boot, before Control starts. Reads the slots and how the last update
    // ended, starts the trial when the running image is on one, and checks
    // the other slot's image in the background. False, having said why, when
    // it cannot allocate what it needs; the routes then answer that the board
    // takes no updates.
    [[nodiscard]] bool start(FirmwareHooks hooks, FirmwareConfig config = {});

    // Whether the board is in flash mode: Control refuses a play and every
    // setting then.
    [[nodiscard]] bool flash_mode() const;

    // A restart its owner asks for, outside the routes, made as a route's
    // restart is: `why` on the console ("firmware: restarting <why>"), and
    // before_restart first. An image still on trial goes back at this
    // restart, and the image that runs next records `why` as the reason.
    [[noreturn]] void restart(const char* why);

    // Control's routes. Each answers the request itself.
    int on_status(httpd_req* req);
    int on_upload(httpd_req* req);
    int on_mode(httpd_req* req);
    int on_rollback(httpd_req* req);
    int on_restart(httpd_req* req);
    // The last crash's core dump (O4): sent as it lies in flash, and erased.
    int on_coredump(httpd_req* req);
    int on_coredump_erase(httpd_req* req);

    struct Impl;

   private:
    Impl* impl_ = nullptr;
};

}  // namespace ac3forge
