#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/messages.hpp"

// Admission between the servers connected to one client (connection.md, Multiple servers
// (server-initiated)): which connection the client holds, which it rejects, and which one an
// incoming connection displaces.
//
// A client holds one admitted connection, ranked by the highest activity it declares: playback
// above pairing above none. A connection is provisional until its first admissible
// server/activate, whose rank is compared with the holder's: equal or higher is admitted and
// displaces the holder, lower is rejected. Three exceptions: a pairing attempt in progress is
// not displaced by incoming playback or pairing; when neither declares anything, the incoming
// one is admitted only if it comes from the last-playback server and the holder does not; and
// one incoming pairing connection is held beside a playback holder, then arbitrated against it
// as if incoming once it drops pairing. Later activations change a connection's rank without
// arbitration.
//
// The arbiter only decides. Its owner makes the sessions act on the verdicts - a rejected
// session refuses its activation, a displaced one is told to leave with
// PlayerSession::displace() - and persists last_playback(). It has a lock of its own, so the
// sessions' listeners may call it from any thread.

namespace ac3::sendspin {

class Arbiter {
   public:
    using Id = std::uint64_t;

    enum class Rank : std::uint8_t { kNone, kPairing, kPlayback };
    [[nodiscard]] static Rank rank_of(const std::vector<messages::Activity>& activities);

    struct Verdict {
        bool admit = false;
        // The connection this admission displaces, which the owner tells to leave.
        std::optional<Id> displaced;
    };

    explicit Arbiter(std::optional<crypto::Key32> last_playback = std::nullopt);

    // An admissible server/activate on `connection` from `server`, declaring `rank`; `first`
    // when it is the connection's first.
    [[nodiscard]] Verdict activation(Id connection, const crypto::Key32& server, Rank rank, bool first);
    // A pairing attempt began or ended on `connection`.
    void attempt(Id connection, bool in_progress);
    // `connection` closed.
    void ended(Id connection);
    // The client's operator forgot its pairing with `server`: it is not the last-playback server
    // any more, if it was, so it no longer takes the client from a holder that declares nothing.
    void forget(const crypto::Key32& server);

    // The server that most recently held the admitted connection while declaring playback.
    [[nodiscard]] std::optional<crypto::Key32> last_playback() const;
    [[nodiscard]] std::optional<Id> admitted() const;
    // The pairing connection held beside a playback holder.
    [[nodiscard]] std::optional<Id> beside() const;

   private:
    struct Held {
        Id id = 0;
        crypto::Key32 server{};
        Rank rank = Rank::kNone;
        bool attempt = false;
    };

    [[nodiscard]] Verdict incoming(const Held& connection);
    void hold(const Held& connection);

    mutable std::mutex mutex_;
    std::optional<Held> admitted_;
    std::optional<Held> beside_;
    std::optional<crypto::Key32> last_playback_;
};

}  // namespace ac3::sendspin
