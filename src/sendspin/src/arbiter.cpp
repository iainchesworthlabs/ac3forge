#include "ac3/sendspin/arbiter.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/messages.hpp"

namespace ac3::sendspin {

namespace {

constexpr Arbiter::Verdict kRejected{.admit = false, .displaced = std::nullopt};
constexpr Arbiter::Verdict kAdmitted{.admit = true, .displaced = std::nullopt};

}  // namespace

Arbiter::Rank Arbiter::rank_of(const std::vector<messages::Activity>& activities) {
    const auto has = [&](messages::Activity activity) {
        return std::find(activities.begin(), activities.end(), activity) != activities.end();
    };
    if (has(messages::Activity::kPlayback)) {
        return Rank::kPlayback;
    }
    return has(messages::Activity::kPairing) ? Rank::kPairing : Rank::kNone;
}

Arbiter::Arbiter(std::optional<crypto::Key32> last_playback) : last_playback_(last_playback) {}

void Arbiter::hold(const Held& connection) {
    admitted_ = connection;
    if (connection.rank == Rank::kPlayback) {
        last_playback_ = connection.server;
    }
}

Arbiter::Verdict Arbiter::activation(Id connection, const crypto::Key32& server, Rank rank, bool first) {
    const std::lock_guard lock(mutex_);
    const Held arriving{.id = connection, .server = server, .rank = rank, .attempt = false};
    // A re-handshake starts the session's activations again, but a connection the client
    // already holds is not a new one to arbitrate.
    const bool held = (admitted_ && admitted_->id == connection) || (beside_ && beside_->id == connection);
    if (first && !held) {
        return incoming(arriving);
    }
    if (admitted_ && admitted_->id == connection) {
        // No arbitration, even when the activities escalate.
        hold({.id = connection, .server = server, .rank = rank, .attempt = admitted_->attempt});
        return kAdmitted;
    }
    if (beside_ && beside_->id == connection) {
        if (rank == Rank::kPairing) {
            return kAdmitted;
        }
        // Having dropped pairing, it is arbitrated against the playback holder as if incoming.
        beside_.reset();
        return incoming(arriving);
    }
    // Not a connection the client holds: one already rejected or displaced.
    return kRejected;
}

Arbiter::Verdict Arbiter::incoming(const Held& connection) {
    if (!admitted_) {
        hold(connection);
        return kAdmitted;
    }
    const Held holder = *admitted_;
    // A pairing attempt in progress is not displaced by incoming playback or pairing, and
    // incoming with nothing declared ranks below it anyway.
    if (holder.attempt) {
        return kRejected;
    }
    if (connection.rank == Rank::kNone && holder.rank == Rank::kNone) {
        const bool arriving_last = last_playback_ && *last_playback_ == connection.server;
        const bool holder_last = last_playback_ && *last_playback_ == holder.server;
        if (!arriving_last || holder_last) {
            return kRejected;
        }
    } else if (holder.rank == Rank::kPlayback && connection.rank == Rank::kPairing) {
        // One pairing connection beside the playback holder; further ones rank below it.
        if (beside_) {
            return kRejected;
        }
        beside_ = connection;
        return kAdmitted;
    } else if (connection.rank < holder.rank) {
        return kRejected;
    }
    hold(connection);
    return {.admit = true, .displaced = holder.id};
}

void Arbiter::attempt(Id connection, bool in_progress) {
    const std::lock_guard lock(mutex_);
    if (admitted_ && admitted_->id == connection) {
        admitted_->attempt = in_progress;
    } else if (beside_ && beside_->id == connection) {
        beside_->attempt = in_progress;
    }
}

void Arbiter::ended(Id connection) {
    const std::lock_guard lock(mutex_);
    if (admitted_ && admitted_->id == connection) {
        // A pairing connection held beside it is now the one the client holds.
        admitted_ = std::exchange(beside_, std::nullopt);
    } else if (beside_ && beside_->id == connection) {
        beside_.reset();
    }
}

std::optional<crypto::Key32> Arbiter::last_playback() const {
    const std::lock_guard lock(mutex_);
    return last_playback_;
}

std::optional<Arbiter::Id> Arbiter::admitted() const {
    const std::lock_guard lock(mutex_);
    return admitted_ ? std::optional<Id>(admitted_->id) : std::nullopt;
}

std::optional<Arbiter::Id> Arbiter::beside() const {
    const std::lock_guard lock(mutex_);
    return beside_ ? std::optional<Id>(beside_->id) : std::nullopt;
}

}  // namespace ac3::sendspin
