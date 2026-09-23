#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ac3/sendspin/discovery.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "network_view.hpp"
#include "pairing_store.hpp"

// Discovery and pairing (planning/hearth-reference-player.md, A6's first
// slice - see planning/hearth-reference-player.md#a6-network-outputs-in-the-application
// for the rest of A6, not built here). Owns this computer's Sendspin server
// identity, browses `_sendspin._tcp`, and pairs on request; the window polls
// status() the way HearthController polls Engine::status() (A5's own reason:
// no on_change() callback, so nothing here has to cross onto the Qt thread by
// itself).
//
// What A6 asks for and is NOT here, and why:
//   * Groups, a sink's own settings pages, and reported levels all need a
//     live stream to a sink - this class never starts one (ServerHost::Group
//     is untouched), so there is nothing yet to hang them on. Tracked as
//     follow-up work once this slice lands.
//   * "A sink in use elsewhere, with an explicit takeover action"
//     (network-in-use.png) needs the sink to say who else holds it, or at
//     least that it is held. Nothing in ac3::sendspin reports this: pairing
//     is not exclusive (a sink may hold long-term PSKs for several servers
//     at once), only activated PLAYBACK is, and that is arbitrated on the
//     SINK, not the server - ServerHost never sees a rival's connection, and
//     even discards the wire signal that comes closest
//     (messages::GoodbyeReason::kAnotherServer, read and dropped in
//     ServerHost's own on_goodbye()). Given the design of dial()/pair(): a
//     freshly-paired or reconnecting long-term-PSK client is given playback
//     as soon as it connects (ServerHost's own class comment says so) - so
//     with today's library, reconnecting to a sink this computer has paired
//     with does not risk a surprise takeover, it simply IS one, every time,
//     unconditionally. That is the right behaviour for "this is now one of
///    Hearth's own sinks" (and it is why rescan()/on_found() dials
//     everything found rather than waiting to be asked - a paired sink's row
//     is only ever as reliable as Hearth's own hold on it), but it leaves no
//     safe way to first ask "is someone else already using this" the way the
//     design's artboard shows. That needs new library-level work
//     (ServerHostEvents gaining a goodbye reason, and something in Group or
//     ServerHost that connects without claiming playback), which belongs in
//     src/sendspin, not here - filed as a follow-up rather than guessed at.

namespace ac3::hearth {

struct NetworkStatus {
    std::uint64_t generation = 0;
    std::vector<SinkFacts> sinks{};
    std::string selected_id{};
    // Set when the selected sink's pairing attempt just ended without
    // pairing (pairing_messages::AbortReason, in words); cleared by the next
    // select_sink() or a fresh attempt.
    std::string pairing_error{};
};

class NetworkSinks final : private sendspin::discovery::BrowseListener, private sendspin::ServerHostEvents {
   public:
    // `store` outlives this: it is both the key ring ServerHost pairs
    // through and, via PairingRecordView::paired_on, where a paired sink's
    // own "Paired on" text comes from. `identity` is this run's server
    // identity; keeping it stable across runs (so a paired sink's stored
    // server_id still matches, connection.md's E8) is the caller's job once
    // it has somewhere durable to keep the private key, which this slice
    // does not yet (see the Settings page's own QSettings-backed store).
    NetworkSinks(sendspin::noise::KeyPair identity, std::string name, PairingStore& store);
    ~NetworkSinks() override;
    NetworkSinks(const NetworkSinks&) = delete;
    NetworkSinks& operator=(const NetworkSinks&) = delete;
    NetworkSinks(NetworkSinks&&) = delete;
    NetworkSinks& operator=(NetworkSinks&&) = delete;

    // True when the host started (mDNS and the listening socket are both
    // platform calls that can fail - a machine with no usable interface, or
    // the port already taken). False leaves every other method a no-op and
    // status() an empty list.
    [[nodiscard]] bool started() const { return host_ != nullptr; }

    // Asks mDNS again now rather than at its next scheduled query
    // (discovery::Browser::refresh()) - NetworkSinkList.qml's "Look again".
    void rescan();

    // The Network page's own action on a row: for a sink not yet paired,
    // starts a dynamic-code pairing attempt once it is connected (now, or as
    // soon as on_client() next hears from it - see select_sink()'s own
    // comment); for one already paired, only remembers it as selected, since
    // it is dialled already (on_found() dials everything discovered).
    void select_sink(const std::string& id);
    // The digits from the code boxes (NetworkPairing.qml), once all of them
    // are filled in.
    void submit_pairing_code(const std::string& id, const std::string& code);
    void cancel_pairing(const std::string& id);
    // Drops the pairing record: the sink has to be paired again, with a new
    // code (ServerHost::unpair(), which also asks the store to forget it).
    void forget_pairing(const std::string& id);

    [[nodiscard]] NetworkStatus status() const;

    // discovery::BrowseListener and ServerHostEvents - public, rather than
    // the more usual private override, so a test can drive this class with a
    // synthetic Service or ClientView directly instead of standing up a real
    // mDNS multicast group or WebSocket to prove the reaction to one (the
    // same reason a fake sink in tests/hearth/test_engine.cpp stands in for
    // a device). Nothing outside a test calls these directly; discovery and
    // ServerHost reach them through the base class references this
    // constructor hands them, never through this name.
    void on_found(const sendspin::discovery::Service& service) override;
    void on_lost(const std::string& instance) override;
    void on_client(const sendspin::ClientView& client) override;
    void on_client_gone(const std::string& client_id) override;
    void on_pairing_code_wanted(const std::string& client_id) override;
    void on_paired(const std::string& client_id) override;
    void on_pairing_ended(const std::string& client_id,
                          std::optional<sendspin::pairing_messages::AbortReason> reason) override;
    void on_log(std::string_view line) override;

   private:
    struct Entry {
        sendspin::discovery::Service service{};
        // Set once on_client() has matched this instance to a connection
        // (by the URL on_found() dialled it at).
        std::optional<sendspin::ClientView> client{};
        std::string client_id{};
        // select_sink() asked to pair before hello arrived; on_client()
        // starts the attempt the moment it can and clears this.
        bool pairing_requested = false;
    };

    // Rebuilds facts_ from `entry` and republishes; called with mutex_ held.
    [[nodiscard]] SinkFacts facts_locked(const std::string& instance, const Entry& entry) const;
    void publish_locked();

    PairingStore& store_;
    mutable std::mutex mutex_;
    std::unique_ptr<sendspin::ServerHost> host_;
    std::unique_ptr<sendspin::discovery::Browser> browser_;

    // Keyed by the mDNS instance name - the one identifier a row has before
    // any connection exists, and the id this class hands to the page.
    std::map<std::string, Entry> sinks_;
    // The instance a dial's URL belongs to, so on_client() (which knows only
    // the URL it dialled and the client_id the handshake gave) can find its
    // way back to the row on_found() made.
    std::map<std::string, std::string> instance_by_url_;
    // The instance a client_id belongs to, once known - on_client_gone(),
    // on_paired() and on_pairing_ended() name only the client_id.
    std::map<std::string, std::string> instance_by_client_id_;

    std::string selected_id_;
    std::string pairing_error_;
    std::uint64_t generation_ = 0;
};

}  // namespace ac3::hearth
