#include "session.hpp"

#include <algorithm>
#include <fmt/format.h>
#include <numeric>
#include <utility>

#include "ac3/core/tables.hpp"

// See session.hpp.

namespace ac3::hearth {

namespace {

// Passthrough's question about the stream, which is a different question
// from what the bytes are: an AC-3 core carrying E-AC-3 dependents only
// reaches a sink whole as E-AC-3.
[[nodiscard]] audio::BitstreamFormat format_of(io::StreamKind kind) {
    return kind == io::StreamKind::kAc3 ? audio::BitstreamFormat::kAc3
                                        : audio::BitstreamFormat::kEac3;
}

}  // namespace

std::expected<Session, std::string> Session::open(const std::string& path,
                                                  const ItemLoader& loader) {
    if (!loader) {
        return std::unexpected(std::string{"Nothing is set up to read items."});
    }
    auto bytes = loader(path);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }

    Session session;
    session.bytes_ = std::move(*bytes);
    auto scanned = io::scan(session.bytes_);
    if (!scanned) {
        return std::unexpected(fmt::format(
            "\"{}\" is not an AC-3 or E-AC-3 stream this player can read.", path));
    }
    if (scanned->access_units.empty()) {
        return std::unexpected(fmt::format("\"{}\" holds no audio.", path));
    }
    session.scanned_ = std::move(*scanned);
    session.total_samples_ = std::accumulate(session.scanned_.access_unit_samples.begin(),
                                             session.scanned_.access_unit_samples.end(),
                                             std::uint64_t{0});

    const std::uint32_t rate = sample_rate_hz(session.scanned_.sample_rate);
    session.facts_.stream = format_of(session.scanned_.kind);
    session.facts_.sample_rate = rate;
    session.facts_.channels = static_cast<std::uint16_t>(std::max(session.scanned_.channels, 0));
    if (rate != 0) {
        session.facts_.duration =
            std::chrono::milliseconds{static_cast<std::int64_t>(session.total_samples_ * 1000 / rate)};
    }
    return session;
}

std::expected<std::size_t, std::string> Session::render(StreamDecoder& decoder,
                                                        const StreamDecoder::BlockFn& deliver,
                                                        std::size_t wanted) {
    std::size_t frames = 0;
    const std::size_t units = scanned_.access_units.size();
    while (frames < wanted && next_ < units) {
        const auto got = decoder.decode(scanned_.access_units[next_], deliver);
        ++next_;
        if (!got) {
            return std::unexpected(got.error());
        }
        frames += *got;
    }
    if (next_ >= units && !finished_) {
        // The last unit has gone in; whatever the decoder is still holding
        // for transient pre-noise processing comes out now, so a finished
        // session has delivered everything it ever will.
        frames += decoder.finish(deliver);
        finished_ = true;
    }
    return frames;
}

void Session::seek(std::chrono::milliseconds to, StreamDecoder& decoder) {
    decoder.reset();
    finished_ = false;
    const std::int64_t ms = std::max<std::int64_t>(to.count(), 0);
    const std::uint64_t sample = static_cast<std::uint64_t>(ms) * facts_.sample_rate / 1000;
    const auto unit = io::access_unit_at_sample(scanned_, sample);
    next_ = unit.value_or(scanned_.access_units.size());
}

std::uint64_t Session::position_samples() const {
    const std::size_t upto = std::min(next_, scanned_.access_unit_samples.size());
    return std::accumulate(scanned_.access_unit_samples.begin(),
                           scanned_.access_unit_samples.begin() + static_cast<std::ptrdiff_t>(upto),
                           std::uint64_t{0});
}

}  // namespace ac3::hearth
