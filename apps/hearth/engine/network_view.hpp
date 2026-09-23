#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/ac3forge_player.hpp"

// Pure view-building for the Network page (planning/hearth-reference-player.md,
// A6: discovery and pairing - the first slice; see network_sinks.hpp's own
// comment for what is not here yet and why). Turning what NetworkSinks knows
// about a discovered Sendspin player into the strings the page reads is kept
// apart from NetworkSinks itself so it can be tested the way output_decision.hpp
// is - hand-built facts, no socket, no clock - while network_sinks.cpp, the
// harder-to-test layer that calls this from what ServerHost and discovery
// actually report, stays thin.
//
// A6's second slice (a Hearth sink's own speaker and decoder settings pages,
// planning/hearth-reference-player.md#a6-network-outputs-in-the-application)
// reads SinkFacts' ac3forge_support/ac3forge_state directly rather than through
// a new to_map()-style function here: unlike SinkRow/SinkDetail's plain display
// strings, the settings pages need editable numeric fields QML binds to, and
// HearthController's own decoder_settings_to_map()/from_map() (hearth_
// controller.cpp) already sets the precedent of doing that conversion in the
// Qt controller itself, not a separate hand-tested view layer - network_
// controller.cpp follows the same pattern for the same reason. ac3forge_player.hpp
// is a lightweight, dependency-free header (no Qt, no ac3::render), the same
// reason network_sinks.hpp already includes sendspin headers directly.
//
// Wording follows planning/hearth-sendspin-extension.md's own terms: "roles",
// "takes", "outputs", "latency" and "clock" are its words for client/hello's
// roles, the codecs and data types a sink accepts, its output slot count and
// width, its required lead time, and whether its time filter has converged.

namespace ac3::hearth {

enum class SinkKind : std::uint8_t {
    // Offers `_ac3forge_player@v1`: takes the bitstream itself, rendered on
    // the sink to its own layout.
    kHearthSink,
    // `player@v1` only: takes stereo PCM, FLAC or Opus decoded here.
    kStandardPlayer,
    // ac3hearth-testsink, on this computer or another (DeviceInfo::product_name
    // says so - there is no role of its own).
    kTestSink,
};

[[nodiscard]] std::string_view describe(SinkKind kind);

enum class PairState : std::uint8_t {
    // Never paired with this computer, or the record has been forgotten.
    kNotPaired,
    // Paired: the connection matched the stored long-term PSK.
    kPaired,
};

// What NetworkSinks knows about one discovered player, gathered from mDNS and,
// once dialled, `client/hello` and `_ac3forge_player@v1_support`/
// `player@v1_support`. A field the sink has not told this run about yet is
// left at its default rather than guessed.
struct SinkFacts {
    std::string id{};
    std::string name{};
    SinkKind kind = SinkKind::kStandardPlayer;
    PairState pair_state = PairState::kNotPaired;
    // "192.168.1.52" or "hearth-s3-kitchen.local" - whichever mDNS answered.
    std::string address{};
    std::uint16_t port = 0;
    // mDNS TXT `path`, e.g. "/sendspin".
    std::string path{};
    // DeviceInfo::product_name, e.g. "ESP32-S3"; empty when the sink has not
    // said, or has not been dialled yet.
    std::string hardware{};
    // DeviceInfo::software_version, e.g. "hearth_sink 0.1.0" - the settings
    // pages' own "only on the sink" panel; empty on the same terms as
    // hardware.
    std::string firmware{};
    // client/hello's own roles, in its own order (e.g.
    // {"_ac3forge_player@v1", "player@v1"}); empty before the sink has said.
    std::vector<std::string> roles{};
    // `_ac3forge_player@v1_support.data_types`, or empty for a standard
    // player - {"ac3", "eac3"} becomes "AC-3 and E-AC-3" in that order.
    std::vector<std::string> data_types{};
    // Codecs from whichever support object the sink offers, most preferred
    // first (`supported_formats`' own order).
    std::vector<std::string> codecs{};
    // `_ac3forge_player@v1_support.outputs`, when the role is offered.
    std::optional<std::uint32_t> output_slots{};
    std::optional<std::uint32_t> output_bit_depth{};
    // `required_lead_time_ms` from `client/state`, once the sink has sent
    // one - not before, so the panel says "not reported yet" rather than 0.
    std::optional<std::uint32_t> required_lead_time_ms{};
    // Whether this run's clock exchange has converged (messaging.md, Clock
    // Synchronization) - always false before pairing, since an unpaired
    // connection never activates a playback role to converge one.
    bool clock_converged = false;
    // Set only when pair_state is kPaired: the date the pairing record was
    // made (PairingRecordView::paired_on).
    std::string paired_on{};

    // --- a Hearth sink's own settings pages -----------------------------
    // The sink's own support object (client/hello's `_ac3forge_player@v1_
    // support`), when it offers the role: layout grammar, management ranges
    // (trim/delay/crossover, whether it takes routing or identify) and which
    // of the 11 decoder keys it accepts - what the settings pages gate their
    // controls on.
    std::optional<sendspin::ac3forge::Support> ac3forge_support{};
    // The sink's own most recently reported client/state object: settings_
    // revision/settings_error (whether intended_settings below has actually
    // reached it), its decoder report, levels and counters - the "what the
    // sink reports" panel. Absent before the sink has sent one.
    std::optional<sendspin::ac3forge::State> ac3forge_state{};
    // This app's own record of the last settings command it successfully
    // sent this sink - NOT a read-back (ac3forge_player.hpp's own comment:
    // the command is set-only). The settings pages show this, not
    // ac3forge_state, as each control's "current" value.
    std::optional<sendspin::ac3forge::Settings> intended_settings{};
    // The output slot this app last told the sink to sound the identify tone
    // on, or nothing - this app's own intent again, for the same reason as
    // intended_settings: there is no "identify state" on the wire to read
    // back either.
    std::optional<std::int32_t> identify_slot{};
};

// One row of NetworkSinkList.qml's `sinks` model.
struct SinkRow {
    std::string id{};
    std::string name{};
    // Two letters for the list's glyph tile ("HS", "SP", "TS").
    std::string icon{};
    std::string subtitle{};
    // "notPaired" | "paired" - QML's own switch key, never translated, so a
    // page's `if` never has to spell an English string back.
    std::string badge{};
    // "not paired" | "paired" - the design's own words, for the badge
    // chip's own label.
    std::string badge_text{};
};

[[nodiscard]] SinkRow to_row(const SinkFacts& facts);

// The right-hand "THIS SINK" info panel's rows (NetworkSinkInfo.qml), each
// already the display string the page shows - QML formats nothing itself, the
// way HearthController's own decoder-settings map does not either.
struct SinkDetail {
    std::string id{};
    std::string name{};
    std::string badge{};
    std::string kind_text{};
    // "192.168.1.52:8928 · /sendspin".
    std::string address{};
    std::string roles_text{};
    std::string takes_text{};
    std::string outputs_text{};
    std::string latency_text{};
    std::string clock_text{};
    std::string paired_on_text{};
};

[[nodiscard]] SinkDetail to_detail(const SinkFacts& facts);

}  // namespace ac3::hearth
