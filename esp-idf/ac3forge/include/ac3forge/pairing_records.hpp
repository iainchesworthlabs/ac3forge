#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// A Sendspin player's pairing records in the order it last used them, and the
// names of the servers they are with (pairing.md, Pairing Records). The board's
// store (sendspin_store.hpp) keeps both in NVS; this is the part that decides
// and encodes, free of ESP-IDF, so that tests/io/test_pairing_records.cpp
// checks it on the host.
//
// ORDER. The records run from the least to the most recently used: a pairing
// puts its record last, and so does a connection one authenticates that the
// board admits (touch()).
// At the capacity a new pairing evicts the least recently used record that no
// open connection rests on. The specification leaves the choice to the client
// bar that one rule, and a board holds fewer connections than records, so
// there is always one to evict. Before this order the first record was the
// oldest pairing, which could be the server that plays to the board every day,
// or one an open connection was using.
//
// NAMES. What a server's hello called it, so that a list of the records says
// something a person can match to a server; a record alone holds a key. A
// board keeps the names in NVS only and reads them when someone asks for the
// list: its internal RAM is too short while it streams to hold them for
// nothing (hearth_sink's README, "When a sample plays", has the figures).
//
// BLOBS. The records blob is what the board has written since Hearth B3: 64
// bytes a record, the server's key and then the long-term PSK, least recently
// used first - so a firmware from before the order reads the same records,
// with the first as the one it evicts. The names blob is 80 bytes a name: the
// server's key, then the name, NUL-padded.

namespace ac3forge {

// A server's key or a PSK: ac3::sendspin::crypto::Key32, which this header
// names for itself so as to need nothing from src/sendspin.
using PairingKey = std::array<std::uint8_t, 32>;

// Zeroes a key in a way the compiler keeps.
inline void clear_key(PairingKey& key) {
    volatile std::uint8_t* const bytes = key.data();
    for (std::size_t i = 0; i < key.size(); ++i) {
        bytes[i] = 0;
    }
}

template <std::size_t Capacity>
class PairingRecords {
   public:
    static_assert(Capacity > 0);

    struct Record {
        PairingKey server_key{};
        PairingKey psk{};
        // Used since the board started, by a pairing or a connection. Never
        // stored.
        bool seen = false;
    };

    static constexpr std::size_t kCapacity = Capacity;
    static constexpr std::size_t kRecordBytes = 64;
    static constexpr std::size_t kBlobBytes = Capacity * kRecordBytes;

    [[nodiscard]] std::size_t size() const { return count_; }
    // The least recently used first.
    [[nodiscard]] const Record& operator[](std::size_t i) const { return records_[i]; }

    [[nodiscard]] std::optional<std::size_t> find(const PairingKey& server_key) const {
        for (std::size_t i = 0; i < count_; ++i) {
            if (records_[i].server_key == server_key) {
                return i;
            }
        }
        return std::nullopt;
    }

    // Stores `psk` for `server_key` as the most recently used record,
    // replacing any it had. At the capacity the least recently used record
    // whose server `in_use` does not name goes, and its server's key is
    // returned. Were every record in use - which a board that holds fewer
    // connections than records never meets - the least recently used goes
    // anyway: a pairing never fails for want of room (pairing.md).
    std::optional<PairingKey> add(const PairingKey& server_key, const PairingKey& psk,
                                  std::span<const PairingKey> in_use) {
        if (const std::optional<std::size_t> at = find(server_key)) {
            erase_at(*at);
        }
        std::optional<PairingKey> evicted;
        if (count_ == Capacity) {
            std::size_t victim = 0;
            while (victim < count_ &&
                   std::find(in_use.begin(), in_use.end(), records_[victim].server_key) != in_use.end()) {
                ++victim;
            }
            if (victim == count_) {
                victim = 0;
            }
            evicted = records_[victim].server_key;
            erase_at(victim);
        }
        records_[count_] = Record{.server_key = server_key, .psk = psk, .seen = true};
        ++count_;
        return evicted;
    }

    // A connection `server_key`'s record authenticated but the board did not
    // admit: the server is there, and nothing moves - a server refused again
    // and again would otherwise rewrite the records each time.
    void mark_seen(const PairingKey& server_key) {
        if (const std::optional<std::size_t> at = find(server_key)) {
            records_[*at].seen = true;
        }
    }

    // A connection that `server_key`'s record authenticated, and the board
    // admitted: the record is the most recently used now, and seen. True when
    // that moved it, which is when a store has something to write.
    bool touch(const PairingKey& server_key) {
        const std::optional<std::size_t> at = find(server_key);
        if (!at) {
            return false;
        }
        records_[*at].seen = true;
        if (*at + 1 == count_) {
            return false;
        }
        Record used = records_[*at];
        std::copy(records_.begin() + static_cast<std::ptrdiff_t>(*at + 1),
                  records_.begin() + static_cast<std::ptrdiff_t>(count_),
                  records_.begin() + static_cast<std::ptrdiff_t>(*at));
        records_[count_ - 1] = used;
        clear_key(used.psk);
        return true;
    }

    // True when there was a record for `server_key`; its PSK is wiped.
    bool remove(const PairingKey& server_key) {
        const std::optional<std::size_t> at = find(server_key);
        if (!at) {
            return false;
        }
        erase_at(*at);
        return true;
    }

    void clear() {
        for (std::size_t i = 0; i < count_; ++i) {
            clear_key(records_[i].psk);
        }
        count_ = 0;
    }

    // The records blob, into `out`, which holds kBlobBytes; the bytes written.
    std::size_t encode(std::span<std::uint8_t> out) const {
        const std::size_t n = std::min(count_, out.size() / kRecordBytes);
        for (std::size_t i = 0; i < n; ++i) {
            const auto at = out.begin() + static_cast<std::ptrdiff_t>(i * kRecordBytes);
            std::copy(records_[i].server_key.begin(), records_[i].server_key.end(), at);
            std::copy(records_[i].psk.begin(), records_[i].psk.end(), at + 32);
        }
        return n * kRecordBytes;
    }

    // The records a blob holds, in place of these, none of them seen. A partial
    // record at the end, and any past the capacity, are left out.
    void decode(std::span<const std::uint8_t> blob) {
        clear();
        const std::size_t n = std::min(blob.size() / kRecordBytes, Capacity);
        for (std::size_t i = 0; i < n; ++i) {
            const auto at = blob.begin() + static_cast<std::ptrdiff_t>(i * kRecordBytes);
            std::copy_n(at, 32, records_[i].server_key.begin());
            std::copy_n(at + 32, 32, records_[i].psk.begin());
            records_[i].seen = false;
        }
        count_ = n;
    }

   private:
    void erase_at(std::size_t i) {
        clear_key(records_[i].psk);
        std::copy(records_.begin() + static_cast<std::ptrdiff_t>(i + 1),
                  records_.begin() + static_cast<std::ptrdiff_t>(count_),
                  records_.begin() + static_cast<std::ptrdiff_t>(i));
        --count_;
        records_[count_] = Record{};
    }

    std::array<Record, Capacity> records_{};
    std::size_t count_ = 0;
};

// A stored name's room, the NUL included: Music Assistant's own
// ("Music Assistant (d5369777-music-assistant)") is 42 characters.
inline constexpr std::size_t kServerNameBytes = 48;

// `name` in at most out.size() bytes, cut between characters, and without what
// a console or a page would show as nothing or as a replacement character:
// control characters, and bytes that are not part of a whole UTF-8 character.
// Returns how many bytes it wrote.
inline std::size_t fit_server_name(std::string_view name, std::span<char> out) {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < name.size()) {
        const auto lead = static_cast<unsigned char>(name[i]);
        const std::size_t width = lead >= 0x20 && lead < 0x7F ? 1
                                  : (lead & 0xE0U) == 0xC0  ? 2
                                  : (lead & 0xF0U) == 0xE0  ? 3
                                  : (lead & 0xF8U) == 0xF0  ? 4
                                                            : 0;
        bool whole = width > 0 && i + width <= name.size();
        for (std::size_t k = 1; whole && k < width; ++k) {
            whole = (static_cast<unsigned char>(name[i + k]) & 0xC0U) == 0x80;
        }
        if (!whole) {
            ++i;
            continue;
        }
        if (n + width > out.size()) {
            break;
        }
        for (std::size_t k = 0; k < width; ++k) {
            out[n++] = name[i + k];
        }
        i += width;
    }
    return n;
}

template <std::size_t Capacity>
class ServerNames {
   public:
    static constexpr std::size_t kEntryBytes = 32 + kServerNameBytes;
    static constexpr std::size_t kBlobBytes = Capacity * kEntryBytes;

    [[nodiscard]] std::size_t size() const { return count_; }

    // `server_key`'s name, or empty.
    [[nodiscard]] std::string_view find(const PairingKey& server_key) const {
        for (std::size_t i = 0; i < count_; ++i) {
            if (entries_[i].server_key == server_key) {
                return {entries_[i].name.data()};
            }
        }
        return {};
    }

    // Names `server_key` `name`, fitted to kServerNameBytes - 1 bytes; an empty
    // name forgets the one it had. True when anything changed. With every
    // entry taken the first goes, which a caller that keeps() the names of the
    // records it holds before setting one never meets.
    bool set(const PairingKey& server_key, std::string_view name) {
        std::array<char, kServerNameBytes> fitted{};
        fit_server_name(name, std::span<char>(fitted).first(kServerNameBytes - 1));
        std::size_t at = 0;
        while (at < count_ && entries_[at].server_key != server_key) {
            ++at;
        }
        if (fitted[0] == '\0') {
            if (at == count_) {
                return false;
            }
            erase_at(at);
            return true;
        }
        if (at < count_) {
            if (entries_[at].name == fitted) {
                return false;
            }
            entries_[at].name = fitted;
            return true;
        }
        if (count_ == Capacity) {
            erase_at(0);
        }
        entries_[count_] = Entry{.server_key = server_key, .name = fitted};
        ++count_;
        return true;
    }

    // Keeps the names of the servers `holds(key)` is true for. True when any
    // went.
    template <class Holds>
    bool keep(Holds&& holds) {
        bool changed = false;
        for (std::size_t i = count_; i-- > 0;) {
            if (!holds(entries_[i].server_key)) {
                erase_at(i);
                changed = true;
            }
        }
        return changed;
    }

    // The names blob, into `out`, which holds kBlobBytes; the bytes written.
    std::size_t encode(std::span<std::uint8_t> out) const {
        const std::size_t n = std::min(count_, out.size() / kEntryBytes);
        for (std::size_t i = 0; i < n; ++i) {
            const auto at = out.begin() + static_cast<std::ptrdiff_t>(i * kEntryBytes);
            std::copy(entries_[i].server_key.begin(), entries_[i].server_key.end(), at);
            std::transform(entries_[i].name.begin(), entries_[i].name.end(), at + 32,
                           [](char c) { return static_cast<std::uint8_t>(c); });
        }
        return n * kEntryBytes;
    }

    // The names a blob holds, in place of these. A partial entry at the end,
    // and any past the capacity, are left out, and each name is fitted again,
    // so a blob another firmware wrote reads as names or not at all.
    void decode(std::span<const std::uint8_t> blob) {
        count_ = 0;
        const std::size_t n = std::min(blob.size() / kEntryBytes, Capacity);
        for (std::size_t i = 0; i < n; ++i) {
            const auto at = blob.begin() + static_cast<std::ptrdiff_t>(i * kEntryBytes);
            PairingKey key{};
            std::copy_n(at, 32, key.begin());
            std::array<char, kServerNameBytes> raw{};
            std::transform(at + 32, at + static_cast<std::ptrdiff_t>(kEntryBytes), raw.begin(),
                           [](std::uint8_t b) { return static_cast<char>(b); });
            raw.back() = '\0';
            (void)set(key, std::string_view(raw.data()));
        }
    }

   private:
    struct Entry {
        PairingKey server_key{};
        std::array<char, kServerNameBytes> name{};
    };

    void erase_at(std::size_t i) {
        std::copy(entries_.begin() + static_cast<std::ptrdiff_t>(i + 1),
                  entries_.begin() + static_cast<std::ptrdiff_t>(count_),
                  entries_.begin() + static_cast<std::ptrdiff_t>(i));
        --count_;
        entries_[count_] = Entry{};
    }

    std::array<Entry, Capacity> entries_{};
    std::size_t count_ = 0;
};

}  // namespace ac3forge
