#include "ac3_transcoder.hpp"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <iterator>
#include <utility>

#include "ac3/encoder/plan.hpp"
#include "ac3/io/metadata_edit.hpp"

// See ac3_transcoder.hpp.

namespace ac3::hearth {

namespace {

[[nodiscard]] std::optional<SampleRate> ac3_rate(std::uint32_t hz) {
    switch (hz) {
        case 48000: return SampleRate::k48000;
        case 44100: return SampleRate::k44100;
        case 32000: return SampleRate::k32000;
        default: return std::nullopt;
    }
}

// The code whose level is nearest `level`, as `ac3cli transcode` maps E-AC-3's
// finer levels onto AC-3's three.
template <typename Code, std::size_t N>
[[nodiscard]] Code nearest(double level, const std::array<std::pair<Code, double>, N>& codes) {
    const auto best = std::ranges::min_element(codes, [level](const auto& a, const auto& b) {
        return std::abs(a.second - level) < std::abs(b.second - level);
    });
    return best->first;
}

constexpr std::array<std::pair<meta::CentreMixLevel, double>, 3> kCentre{{
    {meta::CentreMixLevel::kMinus3dB, meta::level::kMinus3dB},
    {meta::CentreMixLevel::kMinus4_5dB, meta::level::kMinus4_5dB},
    {meta::CentreMixLevel::kMinus6dB, meta::level::kMinus6dB},
}};
constexpr std::array<std::pair<meta::SurroundMixLevel, double>, 3> kSurround{{
    {meta::SurroundMixLevel::kMinus3dB, meta::level::kMinus3dB},
    {meta::SurroundMixLevel::kMinus6dB, meta::level::kMinus6dB},
    {meta::SurroundMixLevel::kSilent, meta::level::kSilent},
}};

// §5.4.2.8 reserves 0, which a decoder reads as 31.
[[nodiscard]] int valid_dialnorm(int dialnorm) {
    return dialnorm >= 1 && dialnorm <= 31 ? dialnorm : 31;
}

}  // namespace

bool Ac3Transcoder::carries(std::uint32_t sample_rate) {
    return ac3_rate(sample_rate).has_value();
}

Ac3Transcoder::FoldLevels Ac3Transcoder::fold_levels(std::span<const std::byte> unit) {
    FoldLevels out;
    const auto header = io::read_frame_metadata(unit);
    if (!header) {
        return out;
    }
    if (header->cmixlev) {
        out.centre = *header->cmixlev;
    }
    if (header->surmixlev) {
        out.surround = *header->surmixlev;
    }
    if (header->mix) {
        // The fold the stream prefers decides which pair describes it.
        const bool ltrt = header->mix->dmixmod == meta::DownmixMode::kLtRt;
        const auto centre = ltrt ? header->mix->ltrtcmixlev : header->mix->lorocmixlev;
        const auto surround = ltrt ? header->mix->ltrtsurmixlev : header->mix->lorosurmixlev;
        if (centre) {
            out.centre = nearest(meta::coefficient(*centre), kCentre);
        }
        if (surround) {
            out.surround = nearest(meta::coefficient(*surround), kSurround);
        }
    }
    return out;
}

Ac3Transcoder::Ac3Transcoder(std::uint32_t sample_rate, FoldLevels fold)
    : sample_rate_(sample_rate), fold_(fold) {
    spans_.reserve(4);
    frame_spans_.reserve(4);
    said_.reserve(8);
}

void Ac3Transcoder::take(std::span<const std::span<const float>> slots, std::size_t frames,
                         std::size_t record) {
    if (frames == 0) {
        return;
    }
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
        std::vector<float>& queued = queue_[channel];
        const std::span<const float> given =
            channel < slots.size() ? slots[channel].first(std::min(frames, slots[channel].size()))
                                   : std::span<const float>{};
        queued.insert(queued.end(), given.begin(), given.end());
        queued.resize(queued.size() + (frames - given.size()), 0.0F);
        hold_[channel] = queued.back();
    }
    if (spans_.empty() || spans_.back().record != record) {
        spans_.push_back(Span{.record = record, .frames = 0});
    }
    spans_.back().frames += frames;
    taken_ += frames;
}

void Ac3Transcoder::describe_source(const UnitReport& report, std::uint64_t frames,
                                    std::size_t record, DualMonoChoice dual_mono) {
    // Dual mono heard as its second channel is levelled and compressed as
    // that channel is.
    const bool second = report.acmod == Acmod::kDualMono && dual_mono == DualMonoChoice::kSecond;
    Said said{.from = taken_ - std::min(frames, taken_),
              .record = record,
              .dialnorm = valid_dialnorm(second && report.dialnorm2 ? *report.dialnorm2
                                                                    : report.dialnorm),
              .compr = second ? report.compr2 : report.compr,
              .bsmod = std::nullopt};
    // Table 5.5's code 7 is a voice-over below 2/0 and karaoke from 2/0 up;
    // this writes 3/2. A unit that sends no service keeps its item's last.
    if (report.bsmod &&
        (*report.bsmod != 7 || static_cast<int>(report.acmod) >= static_cast<int>(Acmod::k2_0))) {
        said.bsmod = report.bsmod;
    } else if (!said_.empty() && said_.back().record == record) {
        said.bsmod = said_.back().bsmod;
    }
    said_.push_back(said);
}

std::expected<void, std::string> Ac3Transcoder::make_encoder() {
    const auto rate = ac3_rate(sample_rate_);
    if (!rate) {
        return std::unexpected(fmt::format("AC-3 has no {} Hz.", sample_rate_));
    }
    plan::Plan p;
    p.codec = plan::Codec::kAc3;
    p.layout = plan::LayoutId::k51;
    p.sample_rate = *rate;
    p.bitrate_kbps = kBitrateKbps;
    // compre in every frame, so a word can be written into each; the
    // encoder's own is replaced by compr_for()'s.
    p.meta.heavy.emplace();
    p.meta.cmixlev = fold_.centre;
    p.meta.surmixlev = fold_.surround;
    if (const auto bad = plan::validate(p)) {
        return std::unexpected(
            fmt::format("The AC-3 encoder cannot be set up: {}.", plan::describe(*bad)));
    }
    // Several kilobytes of transform state: kept off the stack.
    encoder_ = std::make_unique<FrameEncoder>(plan::ac3_config(p));
    heavy_.emplace(meta::HeavyConfig{}, *rate);
    return {};
}

std::uint8_t Ac3Transcoder::compr_for(std::uint64_t start, int dialnorm,
                                      std::span<const std::span<const float>> channels) {
    // What the frame's gain governs: its own samples, and the last block of
    // the frame before, which a decoder crossfades into it.
    const std::uint64_t reach_from = start - std::min<std::uint64_t>(start, kSamplesPerBlock);
    const std::uint64_t reach_to = start + std::uint64_t{kSamplesPerFrame};
    std::optional<std::uint8_t> said;
    bool unsaid = said_.empty();
    for (std::size_t i = 0; i < said_.size(); ++i) {
        const std::uint64_t to = i + 1 < said_.size() ? said_[i + 1].from : reach_to;
        if (to <= reach_from || said_[i].from >= reach_to) {
            continue;
        }
        if (!said_[i].compr) {
            unsaid = true;
        } else if (!said || meta::compr_gain(*said_[i].compr) < meta::compr_gain(*said)) {
            said = said_[i].compr;
        }
    }
    // The frame's own word keeps the compressor's state running whether or
    // not it is used.
    const double peak = meta::mono_downmix_peak_dbfs(
        std::span<const std::array<float, kSamplesPerBlock>>(history_),
        channels.first(kChannels - 1), Acmod::k3_2, meta::coefficient(fold_.centre),
        meta::coefficient(fold_.surround));
    const std::uint8_t own = heavy_->next(peak, dialnorm);
    if (!said) {
        return own;
    }
    if (unsaid && meta::compr_gain(own) < meta::compr_gain(*said)) {
        return own;
    }
    return *said;
}

std::expected<void, std::string> Ac3Transcoder::encode_frame(std::size_t count,
                                                             const FrameFn& out) {
    if (!encoder_) {
        if (auto made = make_encoder(); !made) {
            return made;
        }
    }
    std::array<std::span<const float>, kChannels> views{};
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
        const std::span<const float> given =
            std::span<const float>(queue_[channel]).subspan(head_, count);
        std::array<float, kSamplesPerFrame>& frame = frame_[channel];
        std::ranges::copy(given, frame.begin());
        std::fill(std::next(frame.begin(), static_cast<std::ptrdiff_t>(count)), frame.end(),
                  given.empty() ? hold_[channel] : given.back());
        views[channel] = frame;
    }

    // Who fills the middle of what the frame decodes to, which runs the
    // encoder's delay behind its samples.
    const std::uint64_t start = consumed_;
    const std::uint64_t middle = start + std::uint64_t{kSamplesPerFrame / 2} - kDelay;
    const auto after = std::ranges::upper_bound(said_, middle, {}, &Said::from);
    const Said said = after == said_.begin() ? (said_.empty() ? Said{} : said_.front())
                                             : *std::prev(after);
    const std::uint8_t compr = compr_for(start, said.dialnorm, views);

    auto encoded = encoder_->encode_frame(views);
    if (!encoded) {
        return std::unexpected(fmt::format("The AC-3 encoder refused a frame: {}.",
                                           ac3::describe(encoded.error())));
    }
    const auto edited = io::edit_frame_metadata(
        *encoded,
        io::MetadataEdit{.dialnorm = said.dialnorm, .compr = compr, .bsmod = said.bsmod});
    if (!edited) {
        return std::unexpected(
            fmt::format("The source's metadata could not be written into a frame: {}.",
                        io::describe(edited.error())));
    }
    for (std::size_t channel = 0; channel + 1 < kChannels; ++channel) {
        std::ranges::copy(std::span<const float>(frame_[channel]).last(kSamplesPerBlock),
                          history_[channel].begin());
    }

    // Whose the frame's samples were.
    frame_spans_.clear();
    std::uint64_t left = count;
    while (left > 0 && !spans_.empty()) {
        Span& oldest = spans_.front();
        const std::uint64_t part = std::min(left, oldest.frames);
        frame_spans_.push_back(Span{.record = oldest.record, .frames = part});
        oldest.frames -= part;
        left -= part;
        if (oldest.frames == 0) {
            spans_.erase(spans_.begin());
        }
    }
    head_ += count;
    consumed_ += count;
    encoded_ += kSamplesPerFrame;
    // What the next frame's gain reaches back to, and nothing before it, is
    // still wanted - and the newest report always, for what follows.
    const std::uint64_t next_reach =
        consumed_ - std::min<std::uint64_t>(consumed_, kSamplesPerBlock);
    while (said_.size() > 1 && said_[1].from <= next_reach) {
        said_.erase(said_.begin());
    }
    out(*encoded, frame_spans_);
    return {};
}

void Ac3Transcoder::compact() {
    if (head_ == 0 || head_ < queue_[0].size() / 2) {
        return;
    }
    for (std::vector<float>& queued : queue_) {
        queued.erase(queued.begin(), std::next(queued.begin(), static_cast<std::ptrdiff_t>(head_)));
    }
    head_ = 0;
}

std::expected<void, std::string> Ac3Transcoder::encode_ready(const FrameFn& out) {
    while (buffered() >= kSamplesPerFrame) {
        if (auto done = encode_frame(kSamplesPerFrame, out); !done) {
            return done;
        }
    }
    compact();
    return {};
}

std::expected<void, std::string> Ac3Transcoder::finish(const FrameFn& out) {
    std::expected<void, std::string> result;
    // A sample taken has come out once the frames encoded reach kDelay past
    // it.
    while (taken_ != 0 && encoded_ < taken_ + kDelay) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffered(), kSamplesPerFrame));
        result = encode_frame(count, out);
        if (!result) {
            break;
        }
    }
    reset();
    return result;
}

void Ac3Transcoder::reset() {
    encoder_.reset();
    heavy_.reset();
    for (std::vector<float>& queued : queue_) {
        queued.clear();
    }
    head_ = 0;
    spans_.clear();
    frame_spans_.clear();
    said_.clear();
    taken_ = 0;
    consumed_ = 0;
    encoded_ = 0;
    hold_.fill(0.0F);
    for (auto& channel : history_) {
        channel.fill(0.0F);
    }
}

}  // namespace ac3::hearth
