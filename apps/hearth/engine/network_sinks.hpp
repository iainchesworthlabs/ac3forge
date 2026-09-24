#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/discovery.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "network_view.hpp"
#include "pairing_store.hpp"

// Discovery, connection, pairing and groups (planning/hearth-reference-player.md,
// A6). Owns this computer's Sendspin server identity, browses `_sendspin._tcp`,
// keeps a connection to each sink it can, pairs on request, and makes the
// groups a person builds on the Network page; the window polls status() and
// calls tick() the way HearthController polls Engine::status() (A5's own
// reason: no on_change() callback, so nothing here has to cross onto the Qt
// thread by itself).
//
// A row is a sink mDNS lists, keyed by its instance name, and it stays while
// mDNS lists it or a connection to it is live - not only while this computer
// happens to be connected. What a sink last said about itself (its hello and
// support objects) is kept with the row, so a sink whose connection ended
// still reads as what it is.
//
// Connections. Every sink found is dialled. What it gets once it says hello is
// ServerHost's decision: a paired sink gets playback (so reconnecting to a
// sink this computer has paired with is meant to hold it - "this is now one of
// Hearth's own sinks"); any other waits with no activities. A sink another
// server holds refuses that wait (connection.md, Multiple servers: an
// activation with nothing declared ranks below any holder, bar the
// last-playback exception) with client/goodbye concurrent_attempt, and a
// connection displaced by another server's later activation ends with
// another_server: either way the row is marked held elsewhere and is NOT
// dialled again by itself - dialling a paired sink would take it back, and an
// unpaired one would only be refused again - until the person asks: pair it,
// take it back (connect_sink()), or look again (rescan()). Any other failure -
// the dial, the handshake, a connection that drops - is dialled again after a
// back-off (1, 2, 4, 8, 15, then every 30 seconds), driven by tick().
//
// Pairing is asked for explicitly (pair_sink()), never by selecting a row. On a
// connection that is waiting it starts at once (ServerHost::pair()); without one
// - the usual case for a sink another server holds - it dials to pair
// (ServerHost::dial_to_pair()), so the connection's first activation is the
// pairing, which the sink admits beside or over the other server rather than
// refusing. Only the dynamic six-digit code is offered: it is what the page
// takes, and what a Hearth sink shows on its own page and console.
//
// A group is membership and volume/mute only here - streaming a programme to
// one is Player's job (network_group_sink.hpp): this class only ever calls
// ac3::sendspin::Group::add()/remove()/set_group_volume()/set_member_volume()
// and the like, never start()/push()/push_burst(). ac3::sendspin::Group keeps
// no member list of its own to read back, so groups_ (below) is this class's
// own record of which of ITS sinks belong to which group, in sink-id terms
// (this class's mDNS-instance ids). A member only needs this computer to have
// heard its client_id once - a sink that has said hello, now or earlier in this
// run: Group::add() takes a client that is not connected, and starts it at the
// next audio pushed once it is (server_host.cpp's try_start()), so a sink that
// drops off Wi-Fi for a few seconds stays in every group it was in.
//
// A sink's own settings pages (issue #875, push_sink_settings()/
// push_sink_identify() below) need a live connection offering
// _ac3forge_player@v1: ServerHost::ac3forge_command() resolves by client_id
// alone (server_host.cpp), and ClientView carries ac3forge_support/
// ac3forge_state as soon as such a client connects, neither Group-gated.
//
// What A6 still does not have: a warning BEFORE this computer takes a paired
// sink that another server is playing to (network-in-use.png). The wire has no
// way to ask "is someone else using this" without claiming it (issue #876):
// only the displaced side hears of a displacement. What IS here is the after-
// the-fact half - held_elsewhere, from the goodbye reasons ServerHost passes on
// (ServerHostEvents::on_client_goodbye()).

namespace ac3::hearth {

struct NetworkStatus {
    std::uint64_t generation = 0;
    std::vector<SinkFacts> sinks{};
    std::string selected_id{};
    // Set when the selected sink's pairing attempt just ended without
    // pairing (pairing_messages::AbortReason, in words), or could not start;
    // cleared by the next select_sink() or a fresh attempt.
    std::string pairing_error{};
    std::vector<GroupFacts> groups{};
    // Mutually exclusive with selected_id: selecting a sink clears this, and
    // selecting a group clears selected_id.
    std::string selected_group_id{};
};

class NetworkSinks final : private sendspin::discovery::BrowseListener, private sendspin::ServerHostEvents {
   public:
    using Clock = std::chrono::steady_clock;

    // `store` outlives this: it is both the key ring ServerHost pairs
    // through and, via PairingRecordView::paired_on, where a paired sink's
    // own "Paired on" text comes from. `identity` is this computer's server
    // identity, which has to be the same on every start for a pairing to
    // outlive the process (server_identity.hpp says why).
    // `request_firewall_exception` reaches the mDNS browser this starts
    // (sendspin::discovery::mdns::Options::request_firewall_exception, whose
    // own comment says why) - true, the default, for a real window that
    // needs other machines' replies to actually arrive; a test driving
    // on_found()/on_lost() synthetically has no use for them and passes
    // false so it is never asked to relaunch elevated for a rule it could
    // not finish adding anyway.
    NetworkSinks(sendspin::noise::KeyPair identity, std::string name, PairingStore& store,
                 bool request_firewall_exception = true);
    // Explicit, not defaulted: stops host_/browser_'s own background
    // threads before any other member they call back into (sinks_ and the
    // rest) is torn down - see the .cpp for why that order matters.
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

    // Dials what is due: the sinks whose back-off has run out. The owner calls
    // it often - the window on its status poll - since nothing else here has
    // a clock of its own to wake up on.
    void tick(Clock::time_point now = Clock::now());

    // NetworkSinkList.qml's "Look again": asks mDNS again now rather than at
    // its next scheduled query (discovery::Browser::refresh()), and dials at
    // once every sink that is not connected - bar a paired sink another
    // server holds, which only connect_sink() takes back.
    void rescan();

    // The Network page's own selection. Nothing else: pairing is pair_sink().
    void select_sink(const std::string& id);
    // Pairs sink `id` by a dynamic code: at once on a connection that is
    // waiting, else by dialling to pair (this file's own header comment). The
    // sink then shows six digits, which submit_pairing_code() takes.
    void pair_sink(const std::string& id);
    // The digits from the code boxes (NetworkPairing.qml), once all of them
    // are filled in.
    void submit_pairing_code(const std::string& id, const std::string& code);
    void cancel_pairing(const std::string& id);
    // Drops the pairing record: the sink has to be paired again, with a new
    // code (ServerHost::unpair(), which also asks the store to forget it; the
    // store alone for a sink that is not connected).
    void forget_pairing(const std::string& id);
    // Connects to sink `id` now: a paired sink another server holds is taken
    // back (its playback activation displaces the holder), and any other that
    // is not connected is dialled without waiting for its back-off.
    void connect_sink(const std::string& id);

    // Sends `settings` to sink `id` as a complete replacement - Settings
    // "replaces the sink's settings whole" (ac3forge_player.hpp's own
    // comment), so this is never a sparse patch: NetworkController reads
    // status()'s own intended_settings first and merges a page edit onto it
    // before calling this, the same "whole struct, apply what changed"
    // shape HearthController::setDecoderSettings() already uses locally.
    // `settings.revision` is overwritten with this sink's own next number -
    // the caller does not choose it. False, nothing sent, for a sink that is
    // not connected or does not offer _ac3forge_player@v1; true updates
    // status()'s intended_settings to `settings` (with the assigned
    // revision) so the page shows it as "current" at once, optimistically -
    // there is no read-back to confirm it with (see SinkFacts::
    // intended_settings's own comment). Whether the sink actually applied it
    // shows up later, separately, in status()'s ac3forge_state.
    bool push_sink_settings(const std::string& id, sendspin::ac3forge::Settings settings);
    // Starts the identify tone on `output`, moving it there if another
    // output was already sounding it, or stops it with std::nullopt - same
    // connectedness and return-value terms as push_sink_settings(). Tracked
    // optimistically the same way, in status()'s identify_slot, since the
    // wire has no "identify state" to read back either.
    bool push_sink_identify(const std::string& id, std::optional<sendspin::ac3forge::Identify> identify);

    // Makes a new, empty group (ServerHost::make_group()) and selects it;
    // empty string if the host never started. Not persisted across a run.
    std::string create_group(const std::string& name);
    // Bookkeeping only: ac3::sendspin::Group has no concept of its own
    // display name on the wire, so renaming never touches the library.
    void rename_group(const std::string& group_id, const std::string& name);
    // Ends the group's own programme if one was running and forgets it.
    void delete_group(const std::string& group_id);
    void select_group(const std::string& group_id);
    // A no-op for a sink that has never said hello this run: Group::add()
    // takes a client_id, which only a hello gives. A sink that has, but is not
    // connected now, joins and plays once it is (this file's header comment).
    void add_group_member(const std::string& group_id, const std::string& sink_id);
    void remove_group_member(const std::string& group_id, const std::string& sink_id);
    void set_group_volume(const std::string& group_id, std::int32_t volume);
    void set_group_muted(const std::string& group_id, bool muted);
    void set_member_volume(const std::string& group_id, const std::string& sink_id, std::int32_t volume);
    void set_member_muted(const std::string& group_id, const std::string& sink_id, bool muted);

    [[nodiscard]] NetworkStatus status() const;

    // The real Group behind `group_id` (a GroupFacts::id status() lists),
    // for the app's own network output seam to resolve and stream to
    // (issue #874's own exit) - this class's other methods only ever call
    // add()/remove()/set_group_volume() and the like on it (this file's own
    // header comment says why), never start()/push()/push_burst(), which is
    // Player's job once handed the group itself. Null for an id this class
    // does not know. By id, not by this class's own bookkeeping name
    // (GroupEntry::name): rename_group() is bookkeeping only, with no
    // uniqueness check, so a name cannot resolve one group unambiguously the
    // way ac3::sendspin::Group::id() (unique per host) does.
    [[nodiscard]] std::shared_ptr<sendspin::Group> group(const std::string& group_id) const;

    // The host's own trail (ServerHostEvents::on_log()) since the last call,
    // oldest first: what was dialled, what each sink was given, and why a
    // connection ended. At most kLogLines are kept between calls; older ones
    // are dropped.
    static constexpr std::size_t kLogLines = 200;
    [[nodiscard]] std::vector<std::string> take_log();

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
    void on_dial_failed(const std::string& url, bool answered) override;
    // kAnotherServer (another server took playback here) or kConcurrentAttempt
    // (the sink refused this computer's activation for another server's
    // connection, or another server's pairing attempt) marks the row held
    // elsewhere (see this file's own header comment); any other reason is
    // left to on_client() and on_client_gone(), which already cover it.
    void on_client_goodbye(const std::string& client_id, sendspin::messages::GoodbyeReason reason) override;
    void on_pairing_code_wanted(const std::string& client_id) override;
    void on_paired(const std::string& client_id) override;
    void on_pairing_ended(const std::string& client_id,
                          std::optional<sendspin::pairing_messages::AbortReason> reason) override;
    void on_log(std::string_view line) override;

   private:
    struct Entry {
        sendspin::discovery::Service service{};
        // Whether mDNS lists the sink now. A row mDNS has let go of stays
        // while a connection to it is live, and goes when that ends.
        bool listed = true;
        // The live connection, once it has said hello.
        std::optional<sendspin::ClientView> client{};
        // What the sink last said about itself: client while connected, kept
        // after the connection ends.
        std::optional<sendspin::ClientView> known{};
        std::string client_id{};

        SinkLink link = SinkLink::kIdle;
        std::uint32_t failed_dials = 0;
        // SinkFacts::dial_failed.
        bool dial_failed = false;
        // When the next dial is due, while link is kRetrying.
        Clock::time_point next_dial{};
        // When the live connection said hello: a connection that lasted a
        // while clears the back-off when it ends, one that did not adds to it.
        Clock::time_point connected_at{};
        bool held_elsewhere = false;

        // pair_sink() asked; cleared once the attempt runs, or is cancelled.
        bool pairing_requested = false;
        // A code was entered and the attempt has not yet asked for another:
        // asked again, the code did not match (on_client()).
        bool code_entered = false;

        // This app's own intent for this sink - see SinkFacts::
        // intended_settings/identify_slot's own comments. Neither is reset
        // when the sink's connection drops and reconnects (on_client() keeps
        // the same Entry, keyed by mDNS instance, not by client_id), so a
        // brief reconnect does not forget what was last pushed.
        std::optional<sendspin::ac3forge::Settings> intended_settings{};
        std::int64_t next_settings_revision = 1;
        std::optional<std::int32_t> identify_slot{};
    };

    struct GroupEntry {
        std::string name{};
        std::shared_ptr<sendspin::Group> group{};
        // This class's own sink ids, in the order added - see this file's
        // own header comment on why membership is kept here rather than
        // read back from Group, and in sink-id rather than client_id terms.
        std::vector<std::string> member_sink_ids{};
    };

    // A dial to make once mutex_ is released: to pair, or not.
    struct Dial {
        std::string url;
        bool to_pair = false;
    };

    [[nodiscard]] SinkFacts facts_locked(const std::string& instance, const Entry& entry) const;
    [[nodiscard]] GroupFacts group_facts_locked(const std::string& group_id, const GroupEntry& entry) const;
    // The member's client_id, or empty if the sink is not known, or has not
    // said hello this run.
    [[nodiscard]] std::string member_client_id_locked(const std::string& sink_id) const;
    // Marks `entry` dialling and says what to dial, or nothing when it has no
    // URL; called with mutex_ held.
    [[nodiscard]] std::optional<Dial> start_dial_locked(Entry& entry);
    // After a failed dial or a connection that ended: the next dial after the
    // back-off, or none for a row held elsewhere.
    void schedule_retry_locked(Entry& entry, Clock::time_point now);
    // Removes a row mDNS no longer lists and nothing is connected to, with
    // its indices; true when it did.
    bool forget_if_unlisted_locked(std::map<std::string, Entry>::iterator it);
    void dial(const std::vector<Dial>& dials);
    void publish_locked();

    PairingStore& store_;
    mutable std::mutex mutex_;
    std::unique_ptr<sendspin::ServerHost> host_;
    std::unique_ptr<sendspin::discovery::Browser> browser_;

    // Keyed by the mDNS instance name - the one identifier a row has before
    // any connection exists, and the id this class hands to the page.
    std::map<std::string, Entry> sinks_;
    // The instance a dial's URL belongs to, so on_client() (which knows only
    // the URL it dialled and the client_id the handshake gave) and
    // on_dial_failed() can find their way back to the row on_found() made.
    std::map<std::string, std::string> instance_by_url_;
    // The instance a client_id belongs to, once known - on_client_gone(),
    // on_paired() and on_pairing_ended() name only the client_id.
    std::map<std::string, std::string> instance_by_client_id_;

    std::string selected_id_;
    std::string pairing_error_;
    std::uint64_t generation_ = 0;
    std::vector<std::string> log_;

    // Keyed by ac3::sendspin::Group::id() - already unique per host, so
    // there is no need for a second id scheme on top of it.
    std::map<std::string, GroupEntry> groups_;
    std::string selected_group_id_;
};

}  // namespace ac3::hearth
