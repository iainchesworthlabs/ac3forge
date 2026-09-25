// What this board is, as against what this image is.
//
// A Hearth sink is flashed once and then lives on a shelf: which network it
// joins, what it is called on that network, how wide its DAC's slots are and
// whether a second I2S line is wired are all properties of the BOARD, and
// changing any of them by rebuilding and reflashing is the thing B2 exists to
// stop (planning/hearth-reference-player.md).
//
// So they live in NVS, in one namespace, read once at boot and written when
// something changes them. Kconfig still supplies every default, which is what
// keeps CI working: a QEMU run boots with an empty NVS partition, finds
// nothing, and uses the build's own values exactly as it did before any of
// this existed.
//
// Strings are fixed-size and NUL-terminated rather than std::string: this is
// read on the main task before the decoder has allocated anything, and a
// settings read that fragments the heap it is about to hand over would be a
// poor trade for the convenience.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace player {

// The longest a name may be, mDNS's own limit on an instance name's label
// (RFC 6763 §4.1.1 is 63 octets; this is the shorter thing a person types).
inline constexpr std::size_t kMaxNameBytes = 32;
// WPA2's own limits: 32 octets of SSID, 63 of passphrase.
inline constexpr std::size_t kMaxSsidBytes = 32;
inline constexpr std::size_t kMaxPasswordBytes = 63;

struct Settings {
    // What the board calls itself: the mDNS instance name, the name a server
    // shows in a list of sinks. Defaults to "hearth-xxxxxx" from the low three
    // bytes of the board's MAC (board_mac()), so two boards out of the same box
    // differ.
    std::array<char, kMaxNameBytes + 1> name{};
    // The network to join. Empty SSID means "none stored" - the board has
    // never been told, and waits for Improv (B2) to tell it.
    std::array<char, kMaxSsidBytes + 1> ssid{};
    std::array<char, kMaxPasswordBytes + 1> password{};
    // The DAC wiring: how wide a slot the bus carries, and whether the second
    // I2S line is connected to anything. Both are what the board was built
    // with until someone says otherwise.
    int slot_bits = 32;
    bool second_line = false;
};

// Read at boot, before anything else uses them. Initialises NVS if it has to
// (network_up() used to be the only thing that did). Never fails in a way a
// caller can do anything about: a partition that cannot be opened leaves every
// field at its Kconfig default and says so on the console.
void settings_load();

// What was loaded, with whatever has been stored since.
[[nodiscard]] const Settings& settings();

// The board's MAC address, which its default name and the address it gives a
// Sendspin server are both made from: the WiFi station's where the chip has a
// radio, and the chip's own base MAC where it has none - the ESP32-P4, whose
// WiFi is an ESP32-C6 across SDIO, so the address is the P4's and not that of
// the radio it borrows. False, with `mac` unspecified, if ESP-IDF cannot read
// one at all.
[[nodiscard]] bool board_mac(std::span<std::uint8_t, 6> mac);

// Store one field and keep it in `settings()`. Each returns false if the value
// is not one this board can hold - a name longer than kMaxNameBytes, a slot
// width that is not 16 or 32 - or if NVS refused the write, and says which on
// the console. A refused write leaves the setting as it was.
[[nodiscard]] bool settings_set_name(std::string_view name);
[[nodiscard]] bool settings_set_network(std::string_view ssid, std::string_view password);
[[nodiscard]] bool settings_set_slot_bits(int bits);
[[nodiscard]] bool settings_set_second_line(bool wired);

// Everything back to the image's own defaults, for a board being handed on or
// a board whose stored network no longer exists. The name goes back to the one
// derived from the MAC.
[[nodiscard]] bool settings_forget();

}  // namespace player
