// Improv Wi-Fi over the console. See provision.hpp.

#include "provision.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include <unistd.h>

#include "ac3forge/improv.hpp"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "network.hpp"
#include "settings.hpp"

namespace player {
namespace {

namespace improv = ac3forge::improv;

// The console is shared with everything the player prints, which is what the
// specification's serial transport is: a client picks its packets out of
// whatever else the device is saying. Written in one fwrite so a packet does
// not interleave with a printf from another task.
//
// Then synced as well as flushed. On the S3's USB-Serial-JTAG console (the
// board shapes' sdkconfig.hw), fflush only hands the bytes to the peripheral,
// which sends its buffer to the host at a newline, and a packet carries none:
// the answer to a client's request sat there until the board next printed a
// line, which on an idle board, or after cannot_connect, was never.
//
// These bytes are not text, and the console must not treat them as text.
// ESP-IDF converts line endings both ways by default: a CR goes out before
// every LF, and a CR that arrives becomes an LF. Either one rewrites a length
// byte, a string or a checksum inside a packet, which then fails its checksum
// at the other end - a board could not be given a network named
// "MyHomeNetwork", thirteen characters, because the length byte in front of
// it is a CR; and an answer carrying a ten-character name, whose length byte
// is an LF, left broken. sdkconfig.defaults asks for LF in both
// directions, which is no conversion at all, and the comment there has the
// measurements; tools/checks/run_improv_qemu.sh holds the board to it.
void send(std::span<const std::uint8_t> packet) {
    if (packet.empty()) {
        return;
    }
    (void)std::fwrite(packet.data(), 1, packet.size(), stdout);
    (void)std::fflush(stdout);
    (void)fsync(fileno(stdout));
}

void send_state(improv::State state) {
    std::array<std::uint8_t, improv::kMaxPacket> out{};
    send(std::span<const std::uint8_t>(out.data(), improv::write_state(state, out)));
}

void send_error(improv::Error error) {
    std::array<std::uint8_t, improv::kMaxPacket> out{};
    send(std::span<const std::uint8_t>(out.data(), improv::write_error(error, out)));
}

void send_result(improv::Command command, std::span<const std::string_view> strings) {
    std::array<std::uint8_t, improv::kMaxPacket> out{};
    send(std::span<const std::uint8_t>(out.data(), improv::write_result(command, strings, out)));
}

// Provisioned once there is a network to join, whether it came from Improv or
// from the build: a client that asks should be told what is true, not what
// this task happens to have done.
[[nodiscard]] improv::State current_state() {
    if (network_ready()) {
        return improv::State::provisioned;
    }
    return settings().ssid[0] != '\0' ? improv::State::provisioning : improv::State::ready;
}

// "http://192.168.1.45/" - where the board's own page is, which is what a
// client sends the user to after provisioning. Empty while there is no
// address, which the specification allows.
[[nodiscard]] std::string page_url() {
    const std::string address = network_address();
    if (address.empty()) {
        return {};
    }
    return "http://" + address + "/";
}

void answer(const improv::Rpc& rpc) {
    switch (rpc.command) {
        case improv::Command::wifi_settings: {
            if (rpc.ssid.empty()) {
                send_error(improv::Error::invalid_packet);
                return;
            }
            send_state(improv::State::provisioning);
            if (!settings_set_network(rpc.ssid, rpc.password)) {
                send_error(improv::Error::invalid_packet);
                return;
            }
            // The credentials are stored before the association is tried, so a
            // board that is reset mid-attempt comes back with them. A board
            // whose last attempt failed - a mistyped passphrase, a network
            // that has gone - tries these now, and a client that got
            // cannot_connect can send another pair straight away.
            if (!network_up()) {
                send_error(improv::Error::cannot_connect);
                send_state(improv::State::ready);
                return;
            }
            send_error(improv::Error::none);
            const std::string url = page_url();
            const std::array<std::string_view, 1> strings{url};
            send_result(improv::Command::wifi_settings, strings);
            send_state(improv::State::provisioned);
            return;
        }
        case improv::Command::current_state: {
            send_error(improv::Error::none);
            send_state(current_state());
            // A provisioned device answers the state request with its URL too,
            // so a client that connects to a board already on a network can
            // still offer the link.
            if (current_state() == improv::State::provisioned) {
                const std::string url = page_url();
                const std::array<std::string_view, 1> strings{url};
                send_result(improv::Command::current_state, strings);
            }
            return;
        }
        case improv::Command::device_info: {
            const esp_app_desc_t* app = esp_app_get_description();
            esp_chip_info_t chip{};
            esp_chip_info(&chip);
            const char* family = chip.model == CHIP_ESP32S3   ? "ESP32-S3"
                                 : chip.model == CHIP_ESP32C6 ? "ESP32-C6"
                                 : chip.model == CHIP_ESP32C3 ? "ESP32-C3"
                                                              : "ESP32";
            const std::array<std::string_view, 4> strings{
                "AC3Forge Hearth sink",
                app != nullptr ? app->version : "unknown",
                family,
                settings().name.data(),
            };
            send_error(improv::Error::none);
            send_result(improv::Command::device_info, strings);
            return;
        }
        case improv::Command::device_name: {
            send_error(improv::Error::none);
            const std::array<std::string_view, 1> strings{settings().name.data()};
            send_result(improv::Command::device_name, strings);
            return;
        }
        case improv::Command::scan:
            // A scan means bringing the radio up to listen, which on a board
            // that is already playing would interrupt what it is doing. A
            // client that gets no networks asks the user to type the name,
            // which is the one thing this always supports.
            send_error(improv::Error::none);
            send_result(improv::Command::scan, {});
            return;
        case improv::Command::hostname:
        case improv::Command::network_state:
        default:
            send_error(improv::Error::unknown_command);
            return;
    }
}

// Written by app_main, when the task starts and again if the Sendspin player
// starts after a network joined over Improv, and read by the task.
std::atomic<ConsoleCommands> g_commands{nullptr};

[[noreturn]] void improv_task(void*) {
    improv::Reader reader;
    std::size_t dropped = 0;
    // Text typed on the console, a line at a time. An Improv packet's binary
    // bytes are not text, so one arriving empties the line.
    std::array<char, 64> line{};
    std::size_t length = 0;
    // The console's own state is announced once at start, so a client that
    // opens the port mid-run knows where it stands without asking.
    send_state(current_state());
    while (true) {
        const int byte = std::fgetc(stdin);
        if (byte == EOF) {
            // No driver is installed on the console, so a read with nothing
            // waiting returns EOF and sets the stream's error flag rather
            // than blocking. 20 ms is far inside a serial client's patience
            // and costs nothing measurable.
            std::clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (const auto rpc = reader.feed(static_cast<std::uint8_t>(byte))) {
            answer(*rpc);
        } else if (reader.dropped() != dropped) {
            dropped = reader.dropped();
            send_error(improv::Error::invalid_packet);
        }
        const ConsoleCommands commands = g_commands;
        if (commands == nullptr) {
            continue;
        }
        if (byte == '\n' || byte == '\r') {
            if (length > 0 && !commands(std::string_view(line.data(), length))) {
                std::printf("console: commands are pair reset, pair cancel, pair forget, pair token and sendspin\n");
            }
            length = 0;
        } else if (byte >= 0x20 && byte < 0x7F && length < line.size()) {
            line[length++] = static_cast<char>(byte);
        } else {
            length = 0;
        }
    }
}

}  // namespace

void provisioning_start(ConsoleCommands commands) {
    // 4 KB: the task parses packets into its own fixed buffers and writes
    // through stdio, and holds nothing else.
    static TaskHandle_t task = nullptr;
    if (task != nullptr) {
        // Already listening, since boot, on a board that had no network then.
        // Its Sendspin player has started since, and the console takes the
        // player's commands from here on.
        if (commands != nullptr && g_commands.exchange(commands) != commands) {
            std::printf("console: listening for commands (pair reset, pair cancel, pair forget, "
                        "pair token, sendspin)\n");
        }
        return;
    }
    g_commands = commands;
    if (network_ready() && commands == nullptr) {
        // Already on a network, so there is nothing for a client to hand this
        // board that it does not have - and the 4 KB is worth more to the
        // decoder. See provision.hpp.
        std::printf("improv: already on a network, so not listening\n");
        return;
    }
    if (xTaskCreate(&improv_task, "improv", 4096, nullptr, 2, &task) != pdPASS) {
        std::printf("warning: no room for the Improv task; the board cannot be provisioned over "
                    "serial\n");
        task = nullptr;
        return;
    }
    if (network_ready()) {
        std::printf("console: listening for commands (pair reset, pair cancel, pair forget, pair token, "
                    "sendspin)\n");
    } else {
        std::printf("improv: listening on the console for Wi-Fi credentials\n");
    }
}

}  // namespace player
