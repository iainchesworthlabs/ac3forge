#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ac3/sendspin/discovery.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_host.hpp"
#include "network_view.hpp"
#include "pairing_store.hpp"

// Discovery, pairing and groups (planning/hearth-reference-player.md, A6 -
// see planning/hearth-reference-player.md#a6-network-outputs-in-the-application
// for the rest of A6, not built here). Owns this computer's Sendspin server
// identity, browses `_sendspin._tcp`, pairs on request, and makes the groups
// a person builds on the Network page; the window polls status() the way
// HearthController polls Engine::status() (A5's own reason: no on_change()
// callback, so nothing here has to cross onto the Qt thread by itself).
//
// A group here is membership and volume/mute only - actually streaming a
// programme to one (issue #874's own exit) is Player's job once it grows a
// network-group output seam; this class only ever calls
// ac3::sendspin::Group::add()/remove()/set_group_volume()/set_member_volume()
// and the like, never start()/push()/push_burst(). ac3::sendspin::Group keeps
// no member list of its own to read back, so groups_ (below) is this class's
// own record of which of ITS sinks belong to which group, kept in sink-id
// terms (this class's own mDNS-instance-name ids) rather than client_id:
// a sink's client_id is stable across a reconnect (it comes from the
// device's own long-term Noise key, confirmed against PairingRecordView's
// own use of it above), but a sink can still vanish from sinks_ entirely
// while disconnected (on_client_gone() erases the whole row) - membership
// itself must outlive that, or a sink dropping off Wi-Fi for a few seconds
// would silently evict it from every group it was in.
//
// What A6 still asks for and is NOT here, and why:
//   * A sink's own settings pages and reported levels are issues #875/#876's
//     own slices, not this one.
//   * "A sink in use elsewhere, with an explicit takeover action"
//     (network-in-use.png), in full, is still not buildable (issue #876):
//     pairing is not exclusive (a sink may hold long-term PSKs for several
//     servers at once), only activated PLAYBACK is, and that is arbitrated on
//     the SINK, not the server - ServerHost never sees a rival's connection
//     directly. Given the design of dial()/pair(): a freshly-paired or
//     reconnecting long-term-PSK client is given playback as soon as it
//     connects (ServerHost's own class comment says so) - so with today's
//     library, reconnecting to a sink this computer has paired with does not
//     risk a surprise takeover, it simply IS one, every time, unconditionally.
//     That is the right behaviour for "this is now one of Hearth's own sinks"
//     (and it is why rescan()/on_found() dials everything found rather than
//     waiting to be asked), but it leaves no safe way to first ask "is
//     someone else already using this" BEFORE dialling, the way the design's
//     artboard shows - that would need something in Group or ServerHost that
//     connects without claiming playback, which is a genuine library-level
//     question (does the wire protocol even have room for "ask without
//     claiming"?) worth raising with the Sendspin project itself, not solved
//     here.
//     What IS here, since issue #876: ServerHostEvents::on_client_goodbye()
//     now carries the one wire signal that comes close,
//     messages::GoodbyeReason, which ServerHost used to read and discard
//     (src/sendspin/src/server_host.cpp's on_goodbye()) - kAnotherServer when
//     a connection of equal or higher rank displaces this one, kConcurrentAttempt
//     when this one's own activation collided with another server's pairing
//     attempt already under way on the sink. That is necessarily AFTER THE
//     FACT, not a pre-connect peek: on_client_goodbye() below turns it into a
//     notice (SinkFacts::notice) a row keeps showing for a while, not a
//     warning shown before this computer would take the sink over.

namespace ac3::hearth {

struct NetworkStatus {
    std::uint64_t generation = 0;
    std::vector<SinkFacts> sinks{};
    std::string selected_id{};
    // Set when the selected sink's pairing attempt just ended without
    // pairing (pairing_messages::AbortReason, in words); cleared by the next
    // select_sink() or a fresh attempt.
    std::string pairing_error{};
    std::vector<GroupFacts> groups{};
    // Mutually exclusive with selected_id: selecting a sink clears this, and
    // selecting a group clears selected_id.
    std::string selected_group_id{};
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

    // Makes a new, empty group (ServerHost::make_group()) and selects it;
    // empty string if the host never started. Not persisted across a run -
    // see this file's own header comment on what A6 still needs.
    std::string create_group(const std::string& name);
    // Bookkeeping only: ac3::sendspin::Group has no concept of its own
    // display name on the wire, so renaming never touches the library.
    void rename_group(const std::string& group_id, const std::string& name);
    // Ends the group's own programme if one was running and forgets it.
    void delete_group(const std::string& group_id);
    void select_group(const std::string& group_id);
    // A no-op if the sink is not currently connected: Group::add() takes a
    // client_id, which only exists once a sink has said hello.
    void add_group_member(const std::string& group_id, const std::string& sink_id);
    void remove_group_member(const std::string& group_id, const std::string& sink_id);
    void set_group_volume(const std::string& group_id, std::int32_t volume);
    void set_group_muted(const std::string& group_id, bool muted);
    void set_member_volume(const std::string& group_id, const std::string& sink_id, std::int32_t volume);
    void set_member_muted(const std::string& group_id, const std::string& sink_id, bool muted);

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
    // kAnotherServer (another server took playback here) or kConcurrentAttempt
    // (another server's pairing attempt was already under way) becomes the
    // row's notice (see this file's own header comment); any other reason is
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
        // Set once on_client() has matched this instance to a connection
        // (by the URL on_found() dialled it at).
        std::optional<sendspin::ClientView> client{};
        std::string client_id{};
        // select_sink() asked to pair before hello arrived; on_client()
        // starts the attempt the moment it can and clears this.
        bool pairing_requested = false;
    };

    struct GroupEntry {
        std::string name{};
        std::shared_ptr<sendspin::Group> group{};
        // This class's own sink ids, in the order added - see this file's
        // own header comment on why membership is kept here rather than
        // read back from Group, and in sink-id rather than client_id terms.
        std::vector<std::string> member_sink_ids{};
    };

    // Rebuilds facts_ from `entry` and republishes; called with mutex_ held.
    [[nodiscard]] SinkFacts facts_locked(const std::string& instance, const Entry& entry) const;
    [[nodiscard]] GroupFacts group_facts_locked(const std::string& group_id, const GroupEntry& entry) const;
    // The member's current client_id, or empty if the sink is not known, or
    // known but not currently connected.
    [[nodiscard]] std::string member_client_id_locked(const std::string& sink_id) const;
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
    // An instance's current notice (on_client_goodbye()), kept independently
    // of Entry so it survives the row's own removal in on_client_gone() and
    // still shows once the instance is found again - cleared by on_client()
    // (a fresh connection supersedes it) or on_lost() (nothing left to show it
    // on).
    std::map<std::string, std::string> notice_by_instance_;

    std::string selected_id_;
    std::string pairing_error_;
    std::uint64_t generation_ = 0;

    // Keyed by ac3::sendspin::Group::id() - already unique per host, so
    // there is no need for a second id scheme on top of it.
    std::map<std::string, GroupEntry> groups_;
    std::string selected_group_id_;
};

}  // namespace ac3::hearth
