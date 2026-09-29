// ac3hearth-render: plays one item through Hearth's engine into a WAV file
// (planning/ac4.md, phase I2: "the gain scripts through the engine").
//
// The item goes through what the window plays it with - a Player with a
// queue of one, the Session that reads the item and the StreamDecoder that
// decodes it onto the layout - into a sink that plays whatever it is given at
// once and keeps every sample, and the WAV holds what that sink was given.
// The settings are DecoderSettings' fields (decoder_settings.hpp), so
// tools/checks/gain_ac4_decode.py --engine holds what the engine puts out to
// ETSI TS 103 190-1's formulas as it holds ac3cli decode's.
//
// Never installed or packaged.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ac3/render/layout.hpp"
#include "decoder_settings.hpp"
#include "pcm_sink.hpp"
#include "player.hpp"
#include "queue.hpp"
#include "session.hpp"

namespace {

namespace fs = std::filesystem;
using ac3::hearth::DecoderSettings;
using ac3::hearth::LoadedItem;
using ac3::hearth::OpenOutputFormat;
using ac3::hearth::OutputMode;
using ac3::hearth::PcmSink;

constexpr std::string_view kUsage = R"(usage: ac3hearth-render INPUT OUTPUT.wav [setting=value]...

Plays INPUT, an .ac3, .ec3 or .ac4 elementary stream, through Hearth's engine:
the player, session and stream decoder the window plays an item with. What
the engine puts out goes to OUTPUT.wav, 32-bit float, a channel for each slot
of the layout in the layout's order.

  layout=TEXT                 the speaker layout, as the Speakers page takes it
                              (2.0, 5.1, L,R,C,LFE,Ls,Rs ...); by default the
                              item's own channels in the order ac3cli decode
                              writes them: 1.0, 2.0, L,R,C,Ls,Rs or
                              L,R,C,LFE,Ls,Rs
  downmix=loro|ltrt           the fold a two-speaker layout takes (loro)
  lfe=on|off                  the LFE in a fold (off for AC-3 and E-AC-3, on
                              for AC-4)
  concealment=stop|repeat-fade|mute

AC-4, from the stream as coded:
  normalise=on|off            dialogue normalisation to the output level (off)
  output-level=DBFS           the output level, -31 to 0 (-31)
  drc=auto|home-theatre|flat-panel-tv|portable-speakers|portable-headphones|off
                              the DRC decoder mode (off)
  dialogue-enhancement=DB     0 to 12 (0)
  dialogue-level=DB           the dialogue against the rest (0)
  audio-description=on|off    (off)
  associated-level=DB         the audio description's level, 0 or less (0)
  presentation=ID             the presentation with this presentation_id
  presentation-index=N        the presentation at this place, from 0
  preferred-downmix=on|off    fold by the stream's preferred method (off)
  language=TAG                the listener's language, BCP 47
)";

// A device that plays whatever it is given at once, keeping every sample.
class CaptureSink final : public PcmSink {
   public:
    struct Log {
        std::vector<std::vector<float>> slots;
        std::uint32_t sample_rate = 0;
        std::uint64_t played = 0;
        bool open = false;
    };

    explicit CaptureSink(std::shared_ptr<Log> log) : log_(std::move(log)) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        log_->open = true;
        log_->played = 0;
        log_->sample_rate = format.sample_rate;
        log_->slots.resize(format.layout.slots());
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = static_cast<std::uint16_t>(format.layout.slots()),
                                .mode = OutputMode::kLocalPcm};
    }
    void close() override { log_->open = false; }
    [[nodiscard]] bool is_open() const override { return log_->open; }
    bool submit(std::span<const std::span<const float>> slots, std::size_t frames) override {
        for (std::size_t s = 0; s < slots.size() && s < log_->slots.size(); ++s) {
            log_->slots[s].insert(log_->slots[s].end(), slots[s].begin(),
                                  slots[s].begin() + static_cast<std::ptrdiff_t>(frames));
        }
        log_->played += frames;
        return true;
    }
    [[nodiscard]] std::optional<ac3::audio::MonitorPosition> position() const override {
        if (!log_->open) {
            return std::nullopt;
        }
        return ac3::audio::MonitorPosition{
            .frames_played = log_->played, .frames_queued = 0, .latency_frames = 0};
    }
    void flush() override {
        for (std::vector<float>& slot : log_->slots) {
            slot.clear();
        }
        log_->played = 0;
    }
    bool pause() override { return true; }
    bool resume() override { return true; }

   private:
    std::shared_ptr<Log> log_;
};

std::expected<LoadedItem, std::string> load(const std::string& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) {
        return std::unexpected("could not open " + path);
    }
    const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    LoadedItem item;
    item.bytes.resize(chars.size());
    std::ranges::transform(chars, item.bytes.begin(),
                           [](char c) { return static_cast<std::byte>(c); });
    return item;
}

std::optional<double> number(std::string_view text) {
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> switched(std::string_view text) {
    if (text == "on") {
        return true;
    }
    if (text == "off") {
        return false;
    }
    return std::nullopt;
}

std::optional<ac4::DrcMode> drc_mode(std::string_view text) {
    if (text == "auto") {
        return ac4::DrcMode::kDefault;
    }
    if (text == "home-theatre") {
        return ac4::DrcMode::kHomeTheatre;
    }
    if (text == "flat-panel-tv") {
        return ac4::DrcMode::kFlatPanelTv;
    }
    if (text == "portable-speakers") {
        return ac4::DrcMode::kPortableSpeakers;
    }
    if (text == "portable-headphones") {
        return ac4::DrcMode::kPortableHeadphones;
    }
    if (text == "off") {
        return ac4::DrcMode::kOff;
    }
    return std::nullopt;
}

// One setting=value onto `settings` or `layout`; false where it is not one.
bool apply(std::string_view key, std::string_view value, DecoderSettings& settings,
           std::optional<std::string>& layout) {
    ac3::hearth::Ac4Settings& ac4 = settings.ac4;
    const auto set_number = [&value](double& field) {
        const std::optional<double> parsed = number(value);
        if (parsed) {
            field = *parsed;
        }
        return parsed.has_value();
    };
    const auto set_switch = [&value](bool& field) {
        const std::optional<bool> parsed = switched(value);
        if (parsed) {
            field = *parsed;
        }
        return parsed.has_value();
    };
    const auto set_int = [&value](std::optional<int>& field) {
        int parsed = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size() || parsed < 0) {
            return false;
        }
        field = parsed;
        return true;
    };
    if (key == "layout") {
        layout = std::string{value};
        return true;
    }
    if (key == "downmix") {
        if (value != "loro" && value != "ltrt") {
            return false;
        }
        settings.stereo_fold =
            value == "ltrt" ? ac3::DownmixTarget::kLtRt : ac3::DownmixTarget::kLoRo;
        return true;
    }
    if (key == "lfe") {
        const std::optional<bool> parsed = switched(value);
        settings.mix_lfe = parsed;
        return parsed.has_value();
    }
    if (key == "concealment") {
        if (value == "stop") {
            settings.concealment = ac3::ConcealmentPolicy::kNone;
        } else if (value == "repeat-fade") {
            settings.concealment = ac3::ConcealmentPolicy::kRepeatFade;
        } else if (value == "mute") {
            settings.concealment = ac3::ConcealmentPolicy::kMute;
        } else {
            return false;
        }
        return true;
    }
    if (key == "normalise") {
        return set_switch(ac4.normalise);
    }
    if (key == "output-level") {
        return set_number(ac4.output_level_dbfs);
    }
    if (key == "drc") {
        const std::optional<ac4::DrcMode> mode = drc_mode(value);
        if (mode) {
            ac4.drc = *mode;
        }
        return mode.has_value();
    }
    if (key == "dialogue-enhancement") {
        return set_number(ac4.dialogue_enhancement_db);
    }
    if (key == "dialogue-level") {
        return set_number(ac4.dialogue_db);
    }
    if (key == "audio-description") {
        return set_switch(ac4.audio_description);
    }
    if (key == "associated-level") {
        return set_number(ac4.associated_db);
    }
    if (key == "presentation") {
        return set_int(ac4.presentation_id);
    }
    if (key == "presentation-index") {
        return set_int(ac4.presentation_index);
    }
    if (key == "preferred-downmix") {
        return set_switch(ac4.preferred_downmix);
    }
    if (key == "language") {
        ac4.language = std::string{value};
        return true;
    }
    return false;
}

// The item's own channels in the order ac3cli decode writes them.
std::optional<std::string> own_layout(std::uint16_t channels) {
    switch (channels) {
        case 1:
            return "1.0";
        case 2:
            return "2.0";
        case 5:
            return "L,R,C,Ls,Rs";
        case 6:
            return "L,R,C,LFE,Ls,Rs";
        default:
            return std::nullopt;
    }
}

void put_u16(std::ofstream& out, std::uint16_t value) {
    const std::array<char, 2> bytes = {static_cast<char>(value & 0xFFU),
                                       static_cast<char>((value >> 8U) & 0xFFU)};
    out.write(bytes.data(), bytes.size());
}

void put_u32(std::ofstream& out, std::uint32_t value) {
    const std::array<char, 4> bytes = {
        static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
        static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>((value >> 24U) & 0xFFU)};
    out.write(bytes.data(), bytes.size());
}

// An IEEE float WAV of `slots`, interleaved, little-endian.
bool write_wav(const std::string& path, const std::vector<std::vector<float>>& slots,
               std::uint32_t rate) {
    const auto channels = static_cast<std::uint16_t>(slots.size());
    const std::size_t frames = slots.empty() ? 0 : slots.front().size();
    const std::uint64_t data_bytes = static_cast<std::uint64_t>(frames) * channels * 4U;
    if (data_bytes > 0xFFFFFFFFULL - 36U) {
        return false;
    }
    std::ofstream out(fs::path(path), std::ios::binary);
    if (!out) {
        return false;
    }
    out.write("RIFF", 4);
    put_u32(out, static_cast<std::uint32_t>(36U + data_bytes));
    out.write("WAVEfmt ", 8);
    put_u32(out, 16);
    put_u16(out, 3);  // WAVE_FORMAT_IEEE_FLOAT
    put_u16(out, channels);
    put_u32(out, rate);
    put_u32(out, rate * channels * 4U);
    put_u16(out, static_cast<std::uint16_t>(channels * 4U));
    put_u16(out, 32);
    out.write("data", 4);
    put_u32(out, static_cast<std::uint32_t>(data_bytes));
    std::vector<char> frame(static_cast<std::size_t>(channels) * 4U);
    for (std::size_t n = 0; n < frames; ++n) {
        for (std::size_t c = 0; c < channels; ++c) {
            const float sample = n < slots[c].size() ? slots[c][n] : 0.0F;
            std::memcpy(frame.data() + (c * 4U), &sample, 4U);
        }
        out.write(frame.data(), static_cast<std::streamsize>(frame.size()));
    }
    return out.good();
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() < 2 || args.front() == "--help" || args.front() == "-h") {
        std::cerr << kUsage;
        return args.size() == 1 && (args.front() == "--help" || args.front() == "-h") ? 0 : 2;
    }
    const std::string input{args[0]};
    const std::string output{args[1]};
    // The stream as coded: no normalisation and no compression.
    DecoderSettings settings;
    settings.ac4.normalise = false;
    settings.ac4.drc = ac4::DrcMode::kOff;
    std::optional<std::string> layout_text;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::size_t equals = args[i].find('=');
        if (equals == std::string_view::npos ||
            !apply(args[i].substr(0, equals), args[i].substr(equals + 1), settings, layout_text)) {
            std::cerr << "ac3hearth-render: \"" << args[i] << "\" is not a setting it takes\n\n"
                      << kUsage;
            return 2;
        }
    }
    const ac3::hearth::ItemLoader loader = load;
    if (!layout_text) {
        auto session = ac3::hearth::Session::open(input, loader, std::nullopt,
                                                  ac3::hearth::presentation_choice(settings));
        if (!session) {
            std::cerr << "ac3hearth-render: " << session.error() << "\n";
            return 1;
        }
        layout_text = own_layout(session->facts().channels);
        if (!layout_text) {
            std::cerr << "ac3hearth-render: the item has " << session->facts().channels
                      << " channels; name a layout= for them\n";
            return 2;
        }
    }
    const std::optional<ac3::render::OutputLayout> layout =
        ac3::render::OutputLayout::parse(*layout_text);
    if (!layout) {
        std::cerr << "ac3hearth-render: \"" << *layout_text << "\" is not a speaker layout\n";
        return 2;
    }

    auto log = std::make_shared<CaptureSink::Log>();
    ac3::hearth::Player player(std::make_unique<CaptureSink>(log), loader, *layout, settings);
    player.add(ac3::hearth::QueueItem{.path = input, .title = input, .facts = {}});
    player.play();
    int status = 0;
    std::string last_note;
    for (std::uint64_t pumps = 0; player.active(); ++pumps) {
        const ac3::hearth::PumpReport report = player.pump();
        if (!report.note.empty() && report.note != last_note) {
            std::cerr << "ac3hearth-render: " << report.note << "\n";
            last_note = report.note;
        }
        // Every pump moves a block while an item plays; this many and more
        // means the player has stopped moving.
        if (pumps > (std::uint64_t{1} << 26U)) {
            std::cerr << "ac3hearth-render: the player never finished\n";
            return 1;
        }
    }
    if (!player.last_error().empty()) {
        std::cerr << "ac3hearth-render: " << player.last_error() << "\n";
        status = 1;
    }
    if (player.history().size() != 1) {
        std::cerr << "ac3hearth-render: the item did not play\n";
        return 1;
    }
    const ac3::hearth::PlayedItem& played = player.history().front();
    if (played.frames != played.expected_frames) {
        std::cerr << "ac3hearth-render: " << played.frames << " frames put out, "
                  << played.expected_frames << " coded\n";
        status = 1;
    }
    if (!write_wav(output, log->slots, log->sample_rate)) {
        std::cerr << "ac3hearth-render: could not write " << output << "\n";
        return 1;
    }
    std::cout << output << ": " << played.frames << " frames, " << log->slots.size()
              << " channels (" << *layout_text << ") at " << log->sample_rate << " Hz\n";
    return status;
}
