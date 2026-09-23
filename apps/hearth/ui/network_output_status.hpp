#pragma once

#include <map>
#include <memory>
#include <string>

namespace ac3::sendspin {
class Group;
}  // namespace ac3::sendspin

// What NetworkController knows about each group, shared with HearthController
// without either holding a reference to the other's QObject
// (network_controller.hpp's own comment explains why they do not: two
// independent QML singletons, on the same poll rhythm for the same reason,
// with no natural owner to hand a reference between them). Both are
// QML_SINGLETON, instantiated by the QML engine itself with no constructor
// arguments to inject one through, so this is the third, shared piece of
// state the coupling needs instead - a Meyers singleton, not a QObject,
// deliberately outside either controller's own ownership.
//
// Single-threaded: both controllers' poll() timers and every Q_INVOKABLE run
// on the one Qt GUI thread these singletons live on (QTimer callbacks and
// QML-invoked methods both do), so nothing here needs a lock the way
// NetworkSinks's own cross-thread status() does.
//
// NetworkController's poll() replaces the whole table each tick
// (set_groups()), so a group that disappears (deleted, or the sink lost)
// drops out within one tick rather than being read stale. HearthController's
// own poll() reads it to keep OutputPreferences::group_ready current for
// whichever group is pinned as the output (hearth_controller.cpp's own
// selectOutputGroup()/poll()), and the resolver Player's own NetworkGroupSink
// is built with (engine_thread.cpp) reads group() at each open() - always
// whatever NetworkController last published, never a value captured once.

namespace ac3::hearth::ui {

class NetworkOutputStatus {
public:
    static NetworkOutputStatus& instance() {
        static NetworkOutputStatus status;
        return status;
    }

    struct Entry {
        // At least one member exists and is connected right now
        // (network_view.hpp's own GroupMemberFacts::connected) - see this
        // class's own header comment for why this does not require every
        // member: a group with one of two sinks briefly disconnected can
        // still usefully play to the other, and Group::push()/push_burst()
        // already route to whichever members are actually there.
        bool ready = false;
        std::shared_ptr<sendspin::Group> group{};
    };

    // Called by NetworkController's poll() with its own status().groups,
    // keyed by GroupFacts::id (the id a caller resolves by - group_id(),
    // never a display name: NetworkSinks::group()'s own comment says why).
    void set_groups(std::map<std::string, Entry> groups) { groups_ = std::move(groups); }

    // False, and null, for an id nothing has published - deleted, or never
    // created, or NetworkController has not started yet at all.
    [[nodiscard]] bool ready(const std::string& group_id) const {
        const auto found = groups_.find(group_id);
        return found != groups_.end() && found->second.ready;
    }
    [[nodiscard]] std::shared_ptr<sendspin::Group> group(const std::string& group_id) const {
        const auto found = groups_.find(group_id);
        return found != groups_.end() ? found->second.group : nullptr;
    }

private:
    NetworkOutputStatus() = default;

    std::map<std::string, Entry> groups_;
};

}  // namespace ac3::hearth::ui
