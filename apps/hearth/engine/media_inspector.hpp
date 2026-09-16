#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "media_info.hpp"
#include "session.hpp"

// Media information off the engine's thread (media_info.hpp): a thread of its
// own loads and describes the items asked about, one at a time, and keeps the
// last few descriptions.
//
// Only the newest request waits. Asking for another item while one is being
// read lets that one finish and replaces any request not yet started - what a
// page that follows the user's pick through a queue wants. A path described
// before is served from the cache, unless the request asks for it to be read
// again.

namespace ac3::hearth {

class MediaInspector {
public:
    using ReadyFn = std::function<void(const MediaInfo&)>;

    // `cache_size` descriptions are kept, at least one.
    explicit MediaInspector(ItemLoader loader, std::size_t cache_size = 8);
    ~MediaInspector();
    MediaInspector(const MediaInspector&) = delete;
    MediaInspector& operator=(const MediaInspector&) = delete;
    MediaInspector(MediaInspector&&) = delete;
    MediaInspector& operator=(MediaInspector&&) = delete;

    // Asks for the item at `path`. `reread` ignores what the cache holds for
    // it, for a file that may have changed.
    void request(const std::string& path, bool reread = false);

    // The description of the newest path asked for, once it is ready.
    [[nodiscard]] std::optional<MediaInfo> latest() const;

    // Called on the inspector's thread with each description it hands out,
    // one from the cache included - including one for a path that has since
    // stopped being the newest asked for. The callback must not call sync().
    void on_ready(ReadyFn callback);

    // Waits until every request made before the call has been answered, or
    // replaced by a later one.
    void sync();

private:
    struct Entry {
        std::string path;
        MediaInfo info;
    };

    void run(const std::stop_token& stop);

    ItemLoader loader_;
    std::size_t cache_size_;

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable settled_cv_;
    std::optional<std::string> pending_;
    bool pending_reread_ = false;
    // The newest path asked for, and its description once there is one.
    std::string wanted_;
    std::optional<MediaInfo> latest_;
    // Requests made, and requests answered or replaced.
    std::uint64_t asked_ = 0;
    std::uint64_t settled_ = 0;
    // Most recently described first.
    std::list<Entry> cache_;
    ReadyFn on_ready_;

    // Last, so it starts once everything above exists and stops before any
    // of it goes.
    std::jthread thread_;
};

}  // namespace ac3::hearth
