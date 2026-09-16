#include "media_inspector.hpp"

#include <algorithm>
#include <utility>

// See media_inspector.hpp.

namespace ac3::hearth {

MediaInspector::MediaInspector(ItemLoader loader, std::size_t cache_size)
    : loader_(std::move(loader)), cache_size_(std::max<std::size_t>(cache_size, 1)) {
    thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

MediaInspector::~MediaInspector() {
    thread_.request_stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void MediaInspector::request(const std::string& path, bool reread) {
    {
        const std::scoped_lock lock(mutex_);
        ++asked_;
        if (pending_) {
            // Replaced before it started: settled, with nothing to hand out.
            ++settled_;
        }
        pending_ = path;
        pending_reread_ = reread;
        if (wanted_ != path) {
            wanted_ = path;
            latest_.reset();
        }
    }
    wake_.notify_one();
    settled_cv_.notify_all();
}

std::optional<MediaInfo> MediaInspector::latest() const {
    const std::scoped_lock lock(mutex_);
    return latest_;
}

void MediaInspector::on_ready(ReadyFn callback) {
    const std::scoped_lock lock(mutex_);
    on_ready_ = std::move(callback);
}

void MediaInspector::sync() {
    std::unique_lock lock(mutex_);
    const std::uint64_t made = asked_;
    settled_cv_.wait(lock, [this, made] { return settled_ >= made; });
}

void MediaInspector::run(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    while (true) {
        wake_.wait(lock, stop, [this] { return pending_.has_value(); });
        if (stop.stop_requested()) {
            return;
        }
        const std::string path = std::move(*pending_);
        const bool reread = pending_reread_;
        pending_.reset();
        pending_reread_ = false;

        std::optional<MediaInfo> described;
        const auto cached = std::ranges::find(cache_, path, &Entry::path);
        if (cached != cache_.end() && !reread) {
            described = cached->info;
        }
        lock.unlock();

        if (!described) {
            auto loaded = loader_(path);
            if (loaded) {
                described = describe_media(path, *loaded);
            } else {
                MediaInfo failed;
                failed.path = path;
                failed.error = std::move(loaded.error());
                described = std::move(failed);
            }
        }

        lock.lock();
        // Newest first, one entry a path.
        std::erase_if(cache_, [&path](const Entry& entry) { return entry.path == path; });
        cache_.push_front(Entry{.path = path, .info = *described});
        while (cache_.size() > cache_size_) {
            cache_.pop_back();
        }
        if (path == wanted_) {
            latest_ = *described;
        }
        ReadyFn callback = on_ready_;
        lock.unlock();

        if (callback) {
            callback(*described);
        }

        lock.lock();
        ++settled_;
        settled_cv_.notify_all();
    }
}

}  // namespace ac3::hearth
