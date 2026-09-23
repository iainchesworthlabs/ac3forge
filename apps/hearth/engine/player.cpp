#include "player.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>

// See player.hpp. The transport decides and this carries it out; every
// judgement about WHAT should happen is transport.cpp's, and this file is
// about doing it without losing or duplicating a frame.

namespace ac3::hearth {

namespace {

// The pending ring's first size: more blocks than one pump at the default
// budget plus one access unit's worth, so it rarely has to grow.
constexpr std::size_t kInitialPendingBlocks = 32;

// A note, and what the transport said about it, if anything.
[[nodiscard]] std::string said(std::string_view what, std::string_view why) {
    return why.empty() ? std::string{what} : fmt::format("{}: {}", what, why);
}

[[nodiscard]] std::string_view stream_name(audio::BitstreamFormat format) {
    return format == audio::BitstreamFormat::kAc3 ? "AC-3" : "E-AC-3";
}

[[nodiscard]] std::string_view describe(iec61937::WrapError error) {
    switch (error) {
        case iec61937::WrapError::kNotAFrame: return "it is not a whole frame";
        case iec61937::WrapError::kFrameTooLarge: return "it is too large for a burst";
    }
    return "it could not be packed";
}

// What a transcode decodes onto: 5.1, whose slots are in the order the AC-3
// encoder takes its channels.
[[nodiscard]] const render::OutputLayout& transcode_layout() {
    static const render::OutputLayout layout = render::OutputLayout::named("5.1").value();
    return layout;
}

}  // namespace

Player::Player(std::unique_ptr<PcmSink> sink, ItemLoader loader, const render::OutputLayout& layout,
               const DecoderSettings& settings, DiagnosticLog* diagnostics)
    : Player(PlayerOutputs{.pcm = std::move(sink), .bitstream = {}, .group = {}, .choose = {}},
             std::move(loader), layout, settings, diagnostics) {}

Player::Player(PlayerOutputs outputs, ItemLoader loader, const render::OutputLayout& layout,
               const DecoderSettings& settings, DiagnosticLog* diagnostics)
    : sink_(std::move(outputs.pcm)),
      bitstream_(std::move(outputs.bitstream)),
      group_(std::move(outputs.group)),
      choose_(std::move(outputs.choose)),
      loader_(std::move(loader)),
      layout_(layout),
      settings_(settings),
      diagnostics_(diagnostics) {}

bool Player::output_open() const {
    if (bitstreaming()) {
        return bitstream_ && bitstream_->is_open();
    }
    if (mode_ == OutputMode::kNetworkGroup) {
        return group_ && group_->is_open();
    }
    return sink_ && sink_->is_open();
}

std::optional<audio::MonitorPosition> Player::output_position() const {
    if (bitstreaming()) {
        return bitstream_ ? bitstream_->position() : std::nullopt;
    }
    if (mode_ == OutputMode::kNetworkGroup) {
        return group_ ? group_->position() : std::nullopt;
    }
    return sink_ ? sink_->position() : std::nullopt;
}

bool Player::pause_output() {
    if (bitstreaming()) {
        return bitstream_->pause();
    }
    if (mode_ == OutputMode::kNetworkGroup) {
        return group_->pause();
    }
    return sink_->pause();
}

bool Player::resume_output() {
    if (bitstreaming()) {
        return bitstream_->resume();
    }
    if (mode_ == OutputMode::kNetworkGroup) {
        return group_->resume();
    }
    return sink_->resume();
}

void Player::flush_output() {
    if (bitstreaming()) {
        bitstream_->flush();
    } else if (mode_ == OutputMode::kNetworkGroup) {
        group_->flush();
    } else {
        sink_->flush();
    }
}

std::uint64_t Player::heard_frames() const {
    const auto device = output_position();
    if (!device) {
        return 0;
    }
    return device->frames_played > device->latency_frames
               ? device->frames_played - device->latency_frames
               : 0;
}

std::uint64_t Player::timeline_end() const {
    const std::uint64_t queued = submitted_since_open_ + pending_frames_;
    if (transcoder_) {
        // A sample the encoder has taken is heard its delay after its place.
        return queued + transcoder_->buffered() + Ac3Transcoder::kDelay;
    }
    return queued + (bitstreaming() ? packed_frames_ : 0);
}

std::uint64_t Player::link_start() const {
    return transcoder_ ? Ac3Transcoder::kDelay : 0;
}

std::uint64_t Player::decoded_end() const {
    if (!session_ || segments_.empty()) {
        return 0;
    }
    // Every unit the item plays is sent whole, so a place in its stream is
    // the same place on the link, less anything that was not sent after all.
    // Counting the frames the decode delivers would lose step at the first
    // unit that did not decode.
    const Segment& segment = segments_.back();
    const std::uint64_t at = session_->position_samples();
    const std::uint64_t end =
        segment.output_start + (at > segment.item_start ? at - segment.item_start : 0);
    return end > segment.unsent ? end - segment.unsent : 0;
}

HeldOutput Player::held_output() const {
    if (!output_open()) {
        return {};
    }
    const OpenOutputFormat& open = transport_.open_format();
    return HeldOutput{.mode = mode_,
                      .endpoint_id = choice_.endpoint_id,
                      .sample_rate = open.sample_rate,
                      .stream = open.stream};
}

OutputChoice Player::decide(const Session& session) const {
    if (!choose_) {
        return OutputChoice{.mode = OutputMode::kLocalPcm,
                            .endpoint_id = {},
                            .endpoint_name = {},
                            .reason = "Decoding here, to the output this player was given."};
    }
    // A receiver decodes a stream's first programme, and a stream cannot be
    // sent to it without the others; another programme is decoded here.
    ItemFacts facts = session.facts();
    const bool other_programme = !session.first_programme();
    if (other_programme) {
        facts.stream = std::nullopt;
    }
    OutputChoice choice = choose_(facts, held_output());
    if (other_programme && choice.mode == OutputMode::kLocalPcm) {
        choice.reason += fmt::format(
            " Programme {} is chosen, and a receiver plays only a stream's first.",
            session.programme());
    }
    return choice;
}

std::string Player::join_blocked(const OutputChoice& next, std::string_view title) const {
    if (next.endpoint_id != choice_.endpoint_id) {
        return fmt::format("\"{}\" plays on \"{}\", so the output reopens there - there is a gap.",
                           title, next.endpoint_name);
    }
    if (next.group_name != choice_.group_name) {
        // endpoint_id alone cannot see this: a network group carries none
        // (refollow()'s own comment says why), so two different groups
        // would otherwise compare equal here and the next item would join
        // the WRONG one silently open.
        return fmt::format("\"{}\" plays to the group \"{}\", so the output reopens there - "
                           "there is a gap.",
                           title, next.group_name);
    }
    if (transcoder_ && prepared_ &&
        Ac3Transcoder::fold_levels(prepared_->first_unit()) != transcoder_->fold()) {
        return fmt::format("\"{}\" folds to stereo at other levels, which an AC-3 encoder sets "
                           "once, so the output reopens - there is a gap.",
                           title);
    }
    if (mode_ != OutputMode::kBitstream || packed_frames_ == 0 || !prepared_) {
        return {};
    }
    // The packer holds part of a burst. It is made whole only by units that
    // add up to the six blocks a burst period is, and an E-AC-3 stream's
    // units are all the same length, so the next item's first says whether
    // they can.
    constexpr auto kBlock = static_cast<std::uint64_t>(kSamplesPerBlock);
    constexpr auto kBurstBlocks = static_cast<std::uint64_t>(kBlocksPerFrame);
    const std::uint64_t pending = packed_frames_ / kBlock;
    const std::uint64_t next_blocks = prepared_->unit_samples_at(0) / kBlock;
    if (next_blocks != 0 && pending < kBurstBlocks && (kBurstBlocks - pending) % next_blocks == 0) {
        return {};
    }
    return fmt::format(
        "\"{}\" has units of another length, which cannot finish the burst the item before left "
        "open, so the output reopens - there is a gap.",
        title);
}

std::string_view Player::settings_note() const {
    if (transcoder_) {
        return "The receiver decodes what is transcoded for it with its own settings, and the "
               "meters show what is sent, so these are not used.";
    }
    if (!bitstreaming()) {
        return {};
    }
    return "The receiver decodes the bitstream with its own settings, so these reach only the "
           "meters here.";
}

void Player::reset_packer() {
    packer_.reset();
    packed_frames_ = 0;
    packed_spans_.clear();
    group_payload_.clear();
    group_burst_start_frame_ = 0;
}

void Player::send_unit(std::span<const std::byte> unit, std::uint32_t samples) {
    if (history_.empty() || segments_.empty()) {
        return;
    }
    if (mode_ == OutputMode::kNetworkGroup) {
        send_unit_to_group(unit);
        return;
    }
    const std::size_t record = history_.size() - 1;

    // AC-3 is a burst a frame. E-AC-3's units are packed until they make six
    // blocks, which for a stream of shorter frames spans several units - and
    // at a join, units of both items.
    std::expected<std::optional<std::vector<std::byte>>, iec61937::WrapError> packed;
    if (transport_.open_format().stream == audio::BitstreamFormat::kEac3) {
        if (!packer_) {
            packer_.emplace();
        }
        packed = packer_->push(unit);
    } else {
        auto wrapped = iec61937::wrap_frame(unit);
        if (wrapped) {
            packed = std::optional<std::vector<std::byte>>{std::move(*wrapped)};
        } else {
            packed = std::unexpected(wrapped.error());
        }
    }
    if (!packed) {
        // Not sent, and a packer that refused a unit has let go of what it
        // held, so none of that is on its way either. Everything after it
        // on the link comes that much sooner.
        std::uint64_t lost = samples;
        if (packed.error() == iec61937::WrapError::kFrameTooLarge) {
            lost += packed_frames_;
            packed_frames_ = 0;
            packed_spans_.clear();
        }
        segments_.back().unsent += lost;
        note_unit_error(fmt::format("a unit could not be sent over IEC 61937, as {}",
                                    describe(packed.error())));
        return;
    }
    packed_frames_ += samples;
    if (packed_spans_.empty() || packed_spans_.back().record != record) {
        packed_spans_.push_back(Span{.record = record, .frames = 0});
    }
    packed_spans_.back().frames += samples;
    if (!packed->has_value()) {
        return;
    }
    Pending& block = push_block();
    block.samples.clear();
    block.burst = std::move(**packed);
    block.spans.assign(packed_spans_.begin(), packed_spans_.end());
    block.frames = static_cast<std::size_t>(packed_frames_);
    block.record = record;
    pending_frames_ += block.frames;
    packed_frames_ = 0;
    packed_spans_.clear();
}

void Player::send_unit_to_group(std::span<const std::byte> unit) {
    // The programme frame the burst this unit joins will start at, latched
    // the first time group_payload_ is empty - the PCM this unit's own
    // frames belong to has not been queued yet (take_block() for this same
    // unit runs after send_unit(), inside the decoder.decode() call
    // session.cpp's render() makes right after calling `sent` - session.cpp
    // itself is the source for that order, not assumed here), so
    // submitted_since_open_ + pending_frames_ is exactly "everything before
    // this unit" at this point.
    if (group_payload_.empty()) {
        group_burst_start_frame_ = static_cast<std::int64_t>(submitted_since_open_ + pending_frames_);
    }
    group_payload_.insert(group_payload_.end(), unit.begin(), unit.end());

    // The same packer AC-3/E-AC-3 bitstreaming uses, purely for its "is a
    // burst whole yet" state machine and its Pc/Pd - group_payload_ above,
    // not this wrapped output, is what becomes the burst's payload
    // (network_group_sink.hpp's own comment on submit_burst() says why: a
    // group's members are not S/PDIF, so there is nothing here to
    // word-swizzle or zero-pad).
    std::expected<std::optional<std::vector<std::byte>>, iec61937::WrapError> packed;
    if (transport_.open_format().stream == audio::BitstreamFormat::kEac3) {
        if (!packer_) {
            packer_.emplace();
        }
        packed = packer_->push(unit);
    } else {
        auto wrapped = iec61937::wrap_frame(unit);
        if (wrapped) {
            packed = std::optional<std::vector<std::byte>>{std::move(*wrapped)};
        } else {
            packed = std::unexpected(wrapped.error());
        }
    }
    if (!packed) {
        // Not sent to the group; what had accumulated toward it goes with
        // it. Unlike send_unit()'s own bitstream case, nothing here adjusts
        // segments_.back().unsent - that only feeds decoded_end(), which
        // bitstreaming() gates and a network group never reads (this
        // player's PCM timeline, which a group's own members-with-PCM play
        // from, is untouched by a burst failing to pack).
        group_payload_.clear();
        note_unit_error(fmt::format("a unit could not be sent to the group, as {}",
                                    describe(packed.error())));
        return;
    }
    if (!packed->has_value()) {
        return;  // E-AC-3: more units still wanted before this burst is whole
    }
    // Pc and Pd, read back from the wrap rather than recomputed: the
    // preamble's four words are emitted plainly (iec61937.hpp's own header
    // comment - only the FRAME bytes after them are word-swizzled for the
    // S/PDIF carrier), so bytes 4..7 are exactly Pc and Pd, little-endian,
    // regardless of AC-3's single-unit wrap_frame() or E-AC-3's accumulating
    // packer_ above.
    const std::vector<std::byte>& wrapped = **packed;
    if (wrapped.size() < 8) {
        group_payload_.clear();
        return;  // cannot happen for a real wrap_frame()/Eac3BurstPacker output
    }
    const auto byte_at = [&wrapped](std::size_t i) { return std::to_integer<unsigned>(wrapped[i]); };
    const auto pc = static_cast<std::uint16_t>(byte_at(4) | (byte_at(5) << 8U));
    const auto pd = static_cast<std::uint16_t>(byte_at(6) | (byte_at(7) << 8U));
    pending_group_bursts_.push_back(PendingGroupBurst{
        .pc = pc, .pd = pd, .payload = std::move(group_payload_), .frame = group_burst_start_frame_});
    group_payload_.clear();
}

void Player::encode_transcoded(bool last) {
    if (!transcoder_ || transcode_error_) {
        return;
    }
    const Ac3Transcoder::FrameFn queue = [this](std::span<const std::byte> frame,
                                                std::span<const Ac3Transcoder::Span> spans) {
        std::uint64_t samples = 0;
        for (const Ac3Transcoder::Span& span : spans) {
            samples += span.frames;
        }
        auto wrapped = iec61937::wrap_frame(frame);
        if (!wrapped) {
            // The frame's samples are not on the link, and what follows them
            // is that much sooner.
            if (!segments_.empty()) {
                segments_.back().unsent += samples;
            }
            note_unit_error(fmt::format("a transcoded frame could not be sent over IEC 61937, as {}",
                                        describe(wrapped.error())));
            return;
        }
        Pending& block = push_block();
        block.samples.clear();
        block.burst = std::move(*wrapped);
        block.spans.clear();
        for (const Ac3Transcoder::Span& span : spans) {
            block.spans.push_back(Span{.record = span.record, .frames = span.frames});
        }
        // A frame is a whole burst period on the link, padding and all.
        block.frames = static_cast<std::size_t>(kSamplesPerFrame);
        block.record = spans.empty() ? history_.size() - 1 : spans.back().record;
        pending_frames_ += block.frames;
    };
    const auto encoded = last ? transcoder_->finish(queue) : transcoder_->encode_ready(queue);
    if (!encoded) {
        note(fmt::format("the transcode failed: {}", encoded.error()));
        // Nothing more can be sent. pump() stops playback once the decode
        // that got here has returned; at the end, what was sent plays out.
        if (!last) {
            transcode_error_ = encoded.error();
        }
    }
}

void Player::note(std::string_view line) const {
    if (diagnostics_ != nullptr) {
        diagnostics_->note(line);
    }
}

void Player::note_withheld(std::size_t index, std::string_view line) const {
    if (diagnostics_ == nullptr) {
        return;
    }
    Secrets secrets;
    if (index < queue_.size()) {
        withhold_path(secrets, queue_.items()[index].path);
    }
    diagnostics_->note(scrub(std::string{line}, secrets));
}

void Player::note_item(std::size_t index, std::string_view title, std::string_view what) const {
    if (diagnostics_ == nullptr) {
        return;
    }
    const bool queued = index < queue_.size();
    note_withheld(index,
                  fmt::format("{} {}", describe_item(queued ? index : Queue::kNone, title), what));
}

std::string_view Player::title_of(std::size_t index) const {
    return index < queue_.size() ? std::string_view{queue_.items()[index].title} : std::string_view{};
}

void Player::note_started(std::size_t item, bool joined) const {
    if (diagnostics_ == nullptr || !session_) {
        return;
    }
    const ItemFacts& facts = session_->facts();
    const std::string_view stream =
        !facts.stream                                      ? "an unknown stream"
        : *facts.stream == audio::BitstreamFormat::kAc3 ? "AC-3"
                                                          : "E-AC-3";
    const std::uint64_t ms =
        facts.sample_rate == 0 ? 0 : session_->total_samples() * 1000 / facts.sample_rate;
    std::string what = fmt::format("{}: {}, {} Hz, {} channels, {}.{:03} s",
                                   joined ? "joined the open output" : "started", stream,
                                   facts.sample_rate, facts.channels, ms / 1000, ms % 1000);
    if (session_->programme() != 0) {
        what += fmt::format(", programme {}", session_->programme());
    }
    if (!facts.note.empty()) {
        what += "; ";
        what += facts.note;
    }
    note_item(item, title_of(item), what);
}

void Player::note_unit_error(const std::string& reason) {
    if (diagnostics_ == nullptr || history_.empty()) {
        return;
    }
    const std::size_t record = history_.size() - 1;
    if (record == unit_error_record_) {
        ++unit_errors_more_;
        return;
    }
    settle_unit_errors();
    unit_error_record_ = record;
    const PlayedItem& played = history_[record];
    note_item(played.queue_index, played.title,
              fmt::format("has a unit that could not be decoded: {}", reason));
}

void Player::settle_unit_errors() {
    if (unit_errors_more_ != 0 && unit_error_record_ < history_.size()) {
        const PlayedItem& played = history_[unit_error_record_];
        note_item(played.queue_index, played.title,
                  fmt::format("had {} more units that could not be decoded", unit_errors_more_));
    }
    unit_error_record_ = Queue::kNone;
    unit_errors_more_ = 0;
}

void Player::set_decoder_settings(const DecoderSettings& settings) {
    if (settings == settings_) {
        return;
    }
    if (settings.programme != settings_.programme) {
        // A prepared session holds the old programme's units.
        drop_prepared();
    }
    settings_ = settings;
    if (!session_ || !decoder_) {
        // Nothing is being decoded: the next item to start builds its decoder
        // with these.
        decoder_.reset();
        return;
    }
    if (decoder_fits(decoder_rate_, transcoder_.has_value())) {
        // A transcode's decode keeps its own settings, which few of these
        // reach.
        return;
    }
    const StreamDecoder::BlockFn deliver =
        [this](std::span<const std::span<const float>> rendered, std::size_t n) {
            take_block(rendered, n);
        };
    const Session::ReportFn reported = [this](const UnitReport& report, std::size_t frames) {
        take_report(report, frames);
    };
    session_->hand_over(*decoder_, deliver, reported);
    build_decoder(decoder_rate_, transcoder_.has_value());
}

void Player::build_decoder(std::uint32_t rate, bool transcode) {
    if (transcode) {
        decoder_.emplace(transcode_layout(), rate, transcode_settings(settings_),
                         Substreams::kIndependent);
    } else {
        decoder_.emplace(layout_, rate, settings_);
        // Not for a transcode: its output goes to Ac3Transcoder, not to
        // speakers, so the crossover, the trim/delay and the identify tone a
        // room's setup asks for do not apply to it - see take_block()'s own
        // guard.
        decoder_->set_crossover_hz(crossover_hz_);
        reconfigure_trim_delay(rate);
        reconfigure_identify(rate);
    }
    decoder_rate_ = rate;
}

void Player::reconfigure_trim_delay(std::uint32_t rate) {
    if (trim_delay_rate_ == rate) {
        return;
    }
    const std::size_t slots = layout_.slots();
    const std::size_t max_delay_samples = render::TrimDelay::samples_for_ms(kMaxDelayMs, rate);
    trim_delay_storage_.assign(render::TrimDelay::storage_floats(slots, max_delay_samples), 0.0F);
    const bool configured = trim_delay_.configure(trim_delay_storage_, slots, max_delay_samples);
    static_cast<void>(configured);  // slots <= kMaxSlots == kMaxOutputs; storage sized to fit
    for (std::size_t slot = 0; slot < slots; ++slot) {
        trim_delay_.set_trim_db(slot, trim_db_[slot]);
        trim_delay_.set_delay(slot, render::TrimDelay::samples_for_ms(delay_ms_[slot], rate));
    }
    trim_delay_rate_ = rate;
}

bool Player::set_trim_db(std::size_t slot, double db) {
    if (slot >= layout_.slots() ||
        !(db >= render::TrimDelay::kMinTrimDb && db <= render::TrimDelay::kMaxTrimDb)) {
        return false;
    }
    trim_db_[slot] = db;
    if (trim_delay_rate_ != 0) {
        trim_delay_.set_trim_db(slot, db);
    }
    return true;
}

bool Player::set_delay_ms(std::size_t slot, double ms) {
    if (slot >= layout_.slots() || !std::isfinite(ms) || ms < 0.0 || ms > kMaxDelayMs) {
        return false;
    }
    delay_ms_[slot] = ms;
    if (trim_delay_rate_ != 0) {
        trim_delay_.set_delay(slot, render::TrimDelay::samples_for_ms(ms, trim_delay_rate_));
    }
    return true;
}

double Player::trim_db(std::size_t slot) const {
    return slot < layout_.slots() ? trim_db_[slot] : 0.0;
}

double Player::delay_ms(std::size_t slot) const {
    return slot < layout_.slots() ? delay_ms_[slot] : 0.0;
}

bool Player::set_crossover_hz(double hz) {
    // set_crossover_hz()'s own check is a static range, the same whatever
    // layout or sample rate it is asked against, so validating here needs no
    // decoder to ask - only decoder_->set_crossover_hz(), when there is a
    // decoder, actually reaches into the renderer to apply it.
    if (!(hz >= render::LayoutRenderer::kMinCrossoverHz &&
          hz <= render::LayoutRenderer::kMaxCrossoverHz)) {
        return false;
    }
    crossover_hz_ = hz;
    if (decoder_) {
        decoder_->set_crossover_hz(hz);  // already validated above; cannot fail
    }
    return true;
}

bool Player::set_layout(const render::OutputLayout& layout) {
    if (layout.slots() == 0) {
        return false;
    }
    if (layout.text() == layout_.text()) {
        return true;  // already this layout; not a reopen
    }
    // Read where a playing item has got to BEFORE anything below touches
    // decoder_rate_: position() reads decoder_rate_ itself as its own "is
    // there anything to report" guard, so asking after the reset always
    // reads back zero - the reopen would silently restart the item from
    // its beginning instead of resuming it.
    const bool was_open = output_open() && session_.has_value();
    const std::size_t item = was_open ? queue_.current_index() : Queue::kNone;
    const PlayPosition at = was_open ? position() : PlayPosition{};
    const bool paused = transport_.state() == TransportState::kPaused;

    layout_ = layout;
    // A slot index means a different speaker under a different layout, so
    // last layout's trim/delay would silently land on the wrong one carried
    // over as-is - reset, the way an AVR's own speaker-configuration screen
    // does when the speaker count changes. crossover_hz_ is not per-slot and
    // is left alone; build_decoder() reapplies it to whatever is small under
    // the new layout, if anything is.
    trim_db_.fill(0.0);
    delay_ms_.fill(0.0);
    trim_delay_rate_ = 0;  // reconfigure_trim_delay() rebuilds at the new width, next used
    decoder_.reset();      // built against the old layout_; decoder_fits() cannot see that
    decoder_rate_ = 0;
    if (!was_open) {
        return true;  // nothing playing through this layout yet
    }
    if (item == Queue::kNone) {
        close_output();
        return true;
    }
    // Reopen at the new width, continuing this item from where it had got
    // to - refollow()'s own shape, for a different reason.
    note(fmt::format("layout changed: {} ({} slots) - the output reopens, there is a gap",
                     layout_.text(), layout_.slots()));
    after_drain_.reset();  // a pending drain-then-X is superseded by this reopen
    close_output();
    session_.reset();
    seek_on_start_ = SeekOnStart{.item = item, .to = at.heard};
    const OpenFailure failure = open_output_for(item, nullptr);
    if (failure != OpenFailure::kNone) {
        open_failed(item, failure, nullptr);
        return true;  // the layout still changed; the item just could not resume
    }
    if (paused && !pause_output()) {
        note("the output would not pause");
    }
    return true;
}

void Player::reconfigure_identify(std::uint32_t rate) {
    if (identify_rate_ == rate) {
        return;
    }
    // A fresh generator: IdentifyTone takes its sample rate at construction,
    // with no setter, since it bakes the rate into the low band's filter
    // coefficients. reset()'s own comment on determinism does not matter
    // here - the generator only ever runs from wherever identify_start()
    // last reset it, not from a rate change mid-session.
    identify_tone_ = render::IdentifyTone(rate);
    identify_tone_.set_level_db(identify_level_db_);  // already validated; cannot fail
    identify_rate_ = rate;
}

bool Player::identify_start(std::size_t slot) {
    if (slot >= layout_.slots()) {
        return false;
    }
    identify_tone_.reset();
    identify_slot_ = slot;
    return true;
}

void Player::identify_stop() {
    identify_slot_ = Queue::kNone;
}

bool Player::set_identify_level_db(double db) {
    if (!(db >= render::IdentifyTone::kMinLevelDb && db <= render::IdentifyTone::kMaxLevelDb)) {
        return false;
    }
    identify_level_db_ = db;
    if (identify_rate_ != 0) {
        identify_tone_.set_level_db(db);
    }
    return true;
}

bool Player::set_volume_db(double db) {
    // A static range, the same whatever is playing - no decoder to ask, the
    // way set_crossover_hz()'s own check needs none either.
    if (!(db >= kMinVolumeDb && db <= kMaxVolumeDb)) {
        return false;
    }
    volume_db_ = db;
    volume_gain_ = static_cast<float>(std::pow(10.0, db / 20.0));
    return true;
}

bool Player::decoder_fits(std::uint32_t rate, bool transcode) const {
    if (!decoder_ || decoder_rate_ != rate) {
        return false;
    }
    // Only a transcode's decoder takes the independent substream alone, and
    // it is built on the transcode's layout.
    return transcode ? decoder_->substreams() == Substreams::kIndependent &&
                           decoder_->settings() == transcode_settings(settings_)
                     : decoder_->substreams() == Substreams::kAll &&
                           decoder_->settings() == settings_;
}

void Player::after_edit() {
    // An index names a place in the list, so anything keyed by one is
    // re-read: the transport moved the queue's current item to the one a
    // waiting reopen is for, and the queue kept "current" on that item
    // through the edit.
    if (after_drain_ && after_drain_->action == TransportAction::kReopenForItem) {
        after_drain_->item = queue_.current_index();
        if (after_drain_->item == Queue::kNone) {
            after_drain_->action = TransportAction::kStopOutput;
        }
    }
    drop_prepared();
}

void Player::remap_history(const std::function<std::size_t(std::size_t)>& moved) {
    for (PlayedItem& played : history_) {
        if (played.queue_index != Queue::kNone) {
            played.queue_index = moved(played.queue_index);
        }
    }
    // A seek kept for an item's next start follows the item, and goes with it.
    if (seek_on_start_) {
        seek_on_start_->item = moved(seek_on_start_->item);
        if (seek_on_start_->item == Queue::kNone) {
            seek_on_start_.reset();
        }
    }
    // So does the next item that would not open, which playback stops at.
    if (failed_next_ != Queue::kNone) {
        failed_next_ = moved(failed_next_);
    }
}

void Player::add(QueueItem item) {
    queue_.add(std::move(item));
    after_edit();
}

void Player::insert(std::size_t index, QueueItem item) {
    const std::size_t at = std::min(index, queue_.size());
    queue_.insert(at, std::move(item));
    remap_history([at](std::size_t i) { return i >= at ? i + 1 : i; });
    after_edit();
}

void Player::remove(std::size_t index) {
    if (index >= queue_.size()) {
        return;
    }
    const bool current_changed = queue_.remove(index);
    remap_history([index](std::size_t i) {
        return i == index ? Queue::kNone : (i > index ? i - 1 : i);
    });
    after_edit();
    if (current_changed && transport_.state() != TransportState::kStopped) {
        // The item playing, or the one a reopen was waiting to start, has
        // gone: carry on with whatever is current now.
        perform(transport_.current_item_removed(), nullptr);
    }
}

bool Player::move(std::size_t from, std::size_t to) {
    if (!queue_.move(from, to)) {
        return false;
    }
    remap_history([from, to](std::size_t i) {
        if (i == from) {
            return to;
        }
        if (from < to && i > from && i <= to) {
            return i - 1;
        }
        if (to < from && i >= to && i < from) {
            return i + 1;
        }
        return i;
    });
    after_edit();
    return true;
}

void Player::clear() {
    const bool had_items = !queue_.empty();
    queue_.clear();
    remap_history([](std::size_t) { return Queue::kNone; });
    after_edit();
    if (had_items && transport_.state() != TransportState::kStopped) {
        perform(transport_.current_item_removed(), nullptr);
    }
}

TransportOutcome Player::play_item(std::size_t index) {
    if (index >= queue_.size()) {
        return TransportOutcome{.state = transport_.state(),
                                .action = TransportAction::kNone,
                                .item = Queue::kNone,
                                .seek_to = std::chrono::milliseconds{0},
                                .note = "That item is no longer in the queue."};
    }
    // Whatever was playing stops where it is; the chosen item starts from
    // its beginning, on an output opened for it.
    select(index);
    return play();
}

bool Player::select(std::size_t index) {
    if (index >= queue_.size()) {
        return false;
    }
    perform(transport_.stop(), nullptr);
    queue_.set_current(index);
    seek_on_start_.reset();
    return true;
}

bool Player::meters(MeterSnapshot& latest) {
    if (!meters_ || !output_position()) {
        return false;
    }
    return meters_->release(heard_frames(), latest);
}

bool Player::unit_report(UnitReport& latest) {
    if (!output_position()) {
        return false;
    }
    return reports_.release(heard_frames(), latest);
}

void Player::take_report(const UnitReport& report, std::size_t frames) {
    // take_block() drops a block that has no item to belong to; so is its
    // unit's report.
    if (history_.empty()) {
        return;
    }
    // The unit's frames are the last ones decoded, so it starts being heard
    // that far back from where the decode has got to: the end of the queue
    // for a PCM output or a transcode, and the session's place on the link
    // for a bitstream, whose units are packed before they are decoded.
    const std::uint64_t end = transcoder_     ? timeline_end()
                              : bitstreaming() ? decoded_end()
                                               : submitted_since_open_ + pending_frames_;
    reports_.add(report, end > frames ? end - frames : 0);
    if (transcoder_) {
        // What the unit says goes into the frames its samples complete. The
        // decoder that made them says which dual mono channel they are.
        transcoder_->describe_source(report, frames, history_.size() - 1,
                                     decoder_ ? decoder_->settings().dual_mono
                                              : settings_.dual_mono);
        encode_transcoded(false);
    }
}

PlayPosition Player::position() const {
    PlayPosition out;
    if (segments_.empty() || decoder_rate_ == 0) {
        return out;
    }
    const std::uint64_t heard = heard_frames();
    // The latest segment the clock has reached, or the first.
    const Segment* segment = &segments_.front();
    for (const Segment& candidate : segments_) {
        if (candidate.output_start <= heard) {
            segment = &candidate;
        }
    }
    if (segment->record >= history_.size()) {
        return out;
    }
    const PlayedItem& played = history_[segment->record];
    // Frames of the item that were never sent are passed over, not heard.
    const std::uint64_t into =
        heard > segment->output_start ? heard - segment->output_start + segment->unsent : 0;
    const std::uint64_t at = std::min(segment->item_start + into, played.expected_frames);
    out.item = played.queue_index;
    out.heard = std::chrono::milliseconds{static_cast<std::int64_t>(at * 1000 / decoder_rate_)};
    out.duration = std::chrono::milliseconds{
        static_cast<std::int64_t>(played.expected_frames * 1000 / decoder_rate_)};
    return out;
}

TransportOutcome Player::play() {
    TransportOutcome outcome = transport_.play();
    perform(outcome, nullptr);
    return outcome;
}

TransportOutcome Player::pause() {
    TransportOutcome outcome = transport_.pause();
    perform(outcome, nullptr);
    return outcome;
}

TransportOutcome Player::stop() {
    TransportOutcome outcome = transport_.stop();
    perform(outcome, nullptr);
    return outcome;
}

TransportOutcome Player::next() {
    TransportOutcome outcome = transport_.next();
    perform(outcome, nullptr);
    return outcome;
}

TransportOutcome Player::previous() {
    TransportOutcome outcome = transport_.previous();
    perform(outcome, nullptr);
    return outcome;
}

TransportOutcome Player::seek(std::chrono::milliseconds to) {
    TransportOutcome outcome = transport_.seek(to);
    perform(outcome, nullptr);
    return outcome;
}

void Player::perform(const TransportOutcome& outcome, PumpReport* report) {
    if (report != nullptr && !outcome.note.empty()) {
        report->note = outcome.note;
    }
    switch (outcome.action) {
        case TransportAction::kNone:
        case TransportAction::kJoinItem:  // item_ended() swaps the session in itself
            break;
        case TransportAction::kStartItem:
        case TransportAction::kReopenForItem: {
            // A command, or a reopen whose old audio has already been heard:
            // either way the output starts afresh for this item.
            after_drain_.reset();
            close_output();
            open_failed(outcome.item, open_output_for(outcome.item, report), report);
            break;
        }
        case TransportAction::kPauseOutput:
            if (output_open() && !pause_output()) {
                note("the output would not pause");
            }
            break;
        case TransportAction::kResumeOutput:
            if (output_open() && !resume_output()) {
                note("the output would not resume");
            }
            break;
        case TransportAction::kStopOutput:
            // A seek kept for the next start survives a stop: stopping and
            // then choosing where to start is the ordinary way to use one.
            after_drain_.reset();
            close_output();
            session_.reset();
            drop_prepared();
            // Tried again when playback next gets there.
            failed_next_ = Queue::kNone;
            failed_next_why_.clear();
            if (report != nullptr) {
                report->stopped = true;
            }
            break;
        case TransportAction::kSeekItem:
            if (session_ && decoder_) {
                session_->seek(outcome.seek_to, *decoder_);
                // What was decoded and submitted for the old position must
                // not be heard after the new one; the sink's counts restart
                // with the flush, and so do ours. Units packed toward a
                // burst, and samples a transcode has taken, belong to the old
                // position too.
                clear_pending();
                reset_packer();
                if (transcoder_) {
                    transcoder_->reset();
                }
                // An item that was being heard out has more to decode now.
                drain_target_.reset();
                tail_waiting_ = false;
                if (output_open()) {
                    flush_output();
                }
                submitted_since_open_ = 0;
                if (!history_.empty()) {
                    segments_.assign(1, Segment{.record = history_.size() - 1,
                                                .output_start = link_start(),
                                                .item_start = session_->position_samples()});
                }
                if (meters_) {
                    meters_->restart_timeline();
                }
                reports_.clear();
            } else if (outcome.item != Queue::kNone) {
                seek_on_start_ = SeekOnStart{.item = outcome.item, .to = outcome.seek_to};
            }
            break;
    }
}

void Player::open_failed(std::size_t item, OpenFailure failure, PumpReport* report) {
    if (failure == OpenFailure::kItem && item < queue_.size()) {
        // The item could not be played. It is marked so the transport skips
        // it from now on, and the failure policy says what now: playback
        // moves past it - which ends, since a queue of nothing playable has
        // no next item - or stops at it.
        mark_unplayable(item, last_error_);
        if (transport_.on_failure() == FailurePolicy::kStop) {
            note_item(item, title_of(item), "stopped playback, as an item that fails is set to");
        }
        perform(transport_.item_failed(item), report);
    } else if (failure == OpenFailure::kOutput) {
        // The device would not open. Nothing in the queue is at fault, so
        // playback stops and the reason is kept.
        perform(transport_.stop(), report);
        if (report != nullptr) {
            report->note = last_error_;
        }
    }
}

std::string Player::refollow() {
    // The next item is decided again when its turn comes.
    prepared_choice_.reset();
    // Only an item being played through an open output: one playing out its
    // last units has nothing left to move, and a stopped player decides
    // when it next starts.
    if (after_drain_ || !session_ || !output_open()) {
        return {};
    }
    if (session_->finished()) {
        // Its tail finishes where it is - but a seek back would play more of
        // it, and pump() asks again then.
        refollow_pending_ = true;
        return {};
    }
    const std::size_t item = queue_.current_index();
    if (item == Queue::kNone) {
        return {};
    }
    const PlayPosition at = position();
    if (at.item != item) {
        // The item before, joined to this one, is still being heard: moving
        // now would cut its end. pump() asks again once the join is heard.
        refollow_pending_ = true;
        return {};
    }
    refollow_pending_ = false;
    const OutputChoice choice = decide(*session_);
    // group_name too: a network group carries no endpoint_id
    // (output_decision.cpp's own choose_output() leaves it empty, a group
    // being no endpoint of this machine), so endpoint_id alone cannot tell
    // one group the user has switched to from another already open.
    if (choice.mode == mode_ && choice.endpoint_id == choice_.endpoint_id &&
        choice.group_name == choice_.group_name) {
        // Still right; the reason may read differently now.
        choice_ = choice;
        return {};
    }
    const bool paused = transport_.state() == TransportState::kPaused;
    note(fmt::format("output changed: {}", choice.reason));
    close_output();
    session_.reset();
    seek_on_start_ = SeekOnStart{.item = item, .to = at.heard};
    const OpenFailure failure = open_output_for(item, nullptr);
    if (failure != OpenFailure::kNone) {
        const std::string why = last_error_;
        open_failed(item, failure, nullptr);
        // Whatever the failure policy started in its place waits, paused, as
        // this item was.
        if (paused && transport_.state() == TransportState::kPlaying) {
            perform(transport_.pause(), nullptr);
        }
        return fmt::format("The output changed, and the item could not follow: {}", why);
    }
    if (paused && !pause_output()) {
        note("the output would not pause");
    }
    return fmt::format("The output changed: {}", choice_.reason);
}

bool Player::start_session(std::size_t item) {
    // Whatever plays next has no tail to wait for yet.
    tail_waiting_ = false;
    if (item >= queue_.size()) {
        drop_prepared();
        last_error_ = "That item is no longer in the queue.";
        return false;
    }
    // The prepared session is only this item's if the queue has not been
    // edited under it since: an index names a place in the list, and the
    // path is what says the same file is still there.
    const bool prepared_here = prepared_ && prepared_index_ == item &&
                               prepared_path_ == queue_.items()[item].path;
    if (prepared_here) {
        session_ = std::move(prepared_);
        drop_prepared();
        return true;
    }
    drop_prepared();
    auto opened = Session::open(queue_.items()[item].path, loader_, settings_.programme);
    if (!opened) {
        last_error_ = std::move(opened.error());
        return false;
    }
    queue_.set_facts(item, opened->facts());
    session_ = std::move(*opened);
    return true;
}

void Player::apply_seek_on_start(std::size_t item) {
    // A seek made on this item lands before its first unit is decoded; one
    // made on any other item is stale now that this one is starting.
    if (seek_on_start_ && seek_on_start_->item == item && session_ && decoder_) {
        session_->seek(seek_on_start_->to, *decoder_);
    }
    seek_on_start_.reset();
}

Player::OpenFailure Player::open_output_for(std::size_t item, PumpReport* report) {
    if (!start_session(item)) {
        note_item(item, title_of(item), fmt::format("cannot be played: {}", last_error_));
        if (report != nullptr) {
            report->note = last_error_;
        }
        return OpenFailure::kItem;
    }
    choice_ = decide(*session_);
    if (choose_) {
        note(fmt::format("output chosen: {}", choice_.reason));
    }
    return open_chosen(item, report);
}

Player::OpenFailure Player::open_chosen(std::size_t item, PumpReport* report) {
    const auto refuse = [&](OpenFailure failure, std::string why, std::string_view what) {
        last_error_ = std::move(why);
        note_item(item, title_of(item), fmt::format("{}: {}", what, last_error_));
        session_.reset();
        if (report != nullptr) {
            report->note = last_error_;
        }
        return failure;
    };
    const ItemFacts facts = session_->facts();
    const std::uint32_t rate = facts.sample_rate;
    switch (choice_.mode) {
        case OutputMode::kLocalPcm:
            if (!sink_) {
                return refuse(OpenFailure::kOutput, "This player has no local output.",
                              "could not start");
            }
            break;
        case OutputMode::kBitstream:
        case OutputMode::kBitstreamAsAc3:
            if (!bitstream_) {
                return refuse(OpenFailure::kOutput, "This player has no passthrough output.",
                              "could not start");
            }
            if (!facts.stream) {
                return refuse(OpenFailure::kItem, "It carries nothing IEC 61937 can wrap.",
                              "cannot be played");
            }
            if (choice_.mode == OutputMode::kBitstream) {
                // Before anything is decoded, so the decode and the bursts
                // cover the same units. A transcode can cut its decode, so
                // it plays exactly the item's part.
                session_->play_whole_units();
            } else if (!Ac3Transcoder::carries(rate)) {
                // The decision's to avoid; the item decodes as it is.
                return refuse(OpenFailure::kOutput,
                              fmt::format("AC-3 has no {} Hz, so this cannot be transcoded to it.",
                                          rate),
                              "could not start");
            }
            break;
        case OutputMode::kNone:
            // The outputs, not the item, are in the way - no output at all,
            // or one that follow=off will not fall back from - so playback
            // stops with the reason rather than marking the queue unplayable
            // item by item.
            return refuse(OpenFailure::kOutput, choice_.reason, "could not start");
        case OutputMode::kNetworkGroup:
            if (!group_) {
                return refuse(OpenFailure::kOutput, "This player has no network group output.",
                              "could not start");
            }
            if (facts.stream) {
                // Only whole units make a whole burst, the same reason
                // kBitstream needs this below - a group offers bursts
                // whenever the item has any, alongside the PCM every member
                // can take, so this is unconditional on the mode rather than
                // gated on whether any member actually wants bursts (the
                // group itself routes each form to the members that do).
                session_->play_whole_units();
            }
            break;
    }

    const bool transcode = choice_.mode == OutputMode::kBitstreamAsAc3;
    const bool bitstream = choice_.mode == OutputMode::kBitstream || transcode;
    if (!decoder_fits(rate, transcode)) {
        build_decoder(rate, transcode);
    } else {
        decoder_->reset();
    }
    // What the link carries: the item's own stream, or the transcode's AC-3.
    const std::optional<audio::BitstreamFormat> link =
        transcode ? std::optional{audio::BitstreamFormat::kAc3} : facts.stream;
    std::expected<OpenOutputFormat, std::string> opened;
    if (bitstream) {
        opened = bitstream_->open(BitstreamSink::Format{
            .format = *link, .sample_rate = rate, .endpoint_id = choice_.endpoint_id});
    } else if (choice_.mode == OutputMode::kNetworkGroup) {
        opened = group_->open(choice_.group_name, NetworkGroupSink::Format{.sample_rate = rate,
                                                                           .layout = layout_,
                                                                           .stream = facts.stream});
    } else {
        opened = sink_->open(
            PcmSink::Format{.sample_rate = rate, .layout = layout_, .endpoint_id = choice_.endpoint_id});
    }
    if (!opened) {
        return refuse(OpenFailure::kOutput, opened.error(),
                      "could not start: the output would not open");
    }
    OpenOutputFormat format = *opened;
    format.mode = choice_.mode;
    if (bitstream) {
        format.stream = link;
    }
    mode_ = choice_.mode;
    ++opens_;
    if (transcode) {
        note(fmt::format("output opened: {} ({} transcoded), {} Hz (open {})",
                         describe(format.mode), stream_name(*facts.stream), format.sample_rate,
                         opens_));
    } else if (bitstream) {
        note(fmt::format("output opened: {} ({}), {} Hz (open {})", describe(format.mode),
                         stream_name(*facts.stream), format.sample_rate, opens_));
    } else {
        note(fmt::format("output opened: {}, {} Hz, {} channels (open {})", describe(format.mode),
                         format.sample_rate, format.channels, opens_));
    }
    submitted_since_open_ = 0;
    reset_packer();
    // close_output() let go of any transcoder before this open. Its fold
    // levels are the item's, and hold for the link.
    if (transcode) {
        transcoder_.emplace(rate, Ac3Transcoder::fold_levels(session_->first_unit()));
    }
    transport_.set_open_format(format);
    clear_pending();
    // A transcode meters what it sends, on the transcode's layout.
    if (!meters_ || meters_->sample_rate() != rate || meters_transcoding_ != transcode) {
        meters_.emplace(transcode ? transcode_layout() : layout_, rate);
        meters_transcoding_ = transcode;
    } else {
        meters_->restart_timeline();
    }
    metered_record_ = Queue::kNone;
    reports_.clear();
    apply_seek_on_start(item);
    history_.push_back(PlayedItem{.queue_index = item,
                                  .title = queue_.items()[item].title,
                                  .first_frame = 0,
                                  .frames = 0,
                                  .expected_frames = session_->total_samples(),
                                  .output_opens = opens_});
    segments_.assign(1, Segment{.record = history_.size() - 1,
                                .output_start = link_start(),
                                .item_start = session_->position_samples()});
    note_started(item, false);
    if (report != nullptr) {
        report->item_started = true;
        report->output_reopened = opens_ > 1;
        if (!session_->facts().note.empty()) {
            report->note = session_->facts().note;
        }
    }
    return OpenFailure::kNone;
}

void Player::close_output() {
    settle_unit_errors();
    // A lost output is closed too: its sink stopped by itself, and closing it
    // releases whatever it still holds.
    if (output_open() || output_lost()) {
        if (bitstreaming()) {
            bitstream_->close();
        } else if (mode_ == OutputMode::kNetworkGroup) {
            group_->close();
        } else {
            sink_->close();
        }
        note("output closed");
    }
    mode_ = OutputMode::kNone;
    refollow_pending_ = false;
    transport_.clear_open_format();
    clear_pending();
    reset_packer();
    transcoder_.reset();
    transcode_error_.reset();
    drain_target_.reset();
    submitted_since_open_ = 0;
    segments_.clear();
    if (meters_) {
        meters_->restart_timeline();
    }
    reports_.clear();
}

Player::Pending& Player::push_block() {
    if (pending_count_ == pending_.size()) {
        // Full: turn the ring so its oldest block comes first, then grow it
        // at the end. The blocks keep their buffers through both.
        std::rotate(pending_.begin(),
                    std::next(pending_.begin(), static_cast<std::ptrdiff_t>(pending_head_)),
                    pending_.end());
        pending_head_ = 0;
        pending_.resize(pending_.empty() ? kInitialPendingBlocks : pending_.size() * 2);
    }
    Pending& block = pending_[(pending_head_ + pending_count_) % pending_.size()];
    ++pending_count_;
    // Anything new to hear moves the end a play-out waits for.
    drain_target_.reset();
    return block;
}

void Player::clear_pending() {
    pending_head_ = 0;
    pending_count_ = 0;
    pending_frames_ = 0;
    group_pcm_offset_ = 0;
    pending_group_bursts_.clear();
}

void Player::take_block(std::span<const std::span<const float>> rendered, std::size_t n) {
    if (n == 0 || history_.empty()) {
        return;
    }
    const std::size_t slots = layout_.slots();
    const std::size_t record = history_.size() - 1;
    // Where the block's end will be heard: after everything submitted and
    // queued ahead of it - for a transcode, the samples it has taken too - or
    // for a bitstream, whose bursts carry the audio, at the session's place
    // on the link.
    const std::uint64_t heard_at = transcoder_     ? timeline_end() + n
                                   : bitstreaming() ? decoded_end()
                                                    : submitted_since_open_ + pending_frames_ + n;
    if (meters_) {
        // Metered as it is queued, stamped with where it will be heard. An
        // item's programme measurements start with its first block; after an
        // open, the meters have started again already.
        if (record != metered_record_) {
            meters_->restart_programme();
            metered_record_ = record;
        }
        meters_->meter(rendered, n, heard_at);
    }
    if (transcoder_) {
        // Encoded once the unit's report has said what goes with it.
        transcoder_->take(rendered, n, record);
        return;
    }
    if (bitstreaming()) {
        return;
    }
    Pending& block = push_block();
    block.frames = n;
    block.record = record;
    // A reused buffer is as large as the largest block it has held, so this
    // only allocates while the ring is new.
    block.samples.resize(slots * n);
    for (std::size_t slot = 0; slot < slots; ++slot) {
        const auto out = std::next(block.samples.begin(), static_cast<std::ptrdiff_t>(slot * n));
        if (slot < rendered.size()) {
            std::copy_n(rendered[slot].begin(), n, out);
        } else {
            std::fill_n(out, n, 0.0F);
        }
    }
    // In place, on this player's own copy - never on `rendered`, which is
    // the decoder's reused buffer and, for a bitstream or a transcode, has
    // already returned above without reaching here.
    std::array<std::span<float>, render::OutputLayout::kMaxSlots> out_spans{};
    for (std::size_t slot = 0; slot < slots; ++slot) {
        out_spans[slot] = std::span<float>(block.samples).subspan(slot * n, n);
    }
    const std::span<std::span<float>> slot_view(out_spans.data(), slots);
    if (identify_slot_ != Queue::kNone) {
        // Overwrites everything the copy loop above just wrote: the tone on
        // identify_slot_, silence on the rest - render::IdentifyTone::
        // fill()'s own behaviour. Ahead of trim_delay_, which does not run
        // while identifying - this player's own header comment says why.
        const render::IdentifyTone::Band band =
            layout_.slot(identify_slot_).kind == render::Speaker::Kind::kLfe
                ? render::IdentifyTone::Band::kLow
                : render::IdentifyTone::Band::kFull;
        identify_tone_.fill(slot_view, identify_slot_, band);
    } else if (trim_delay_rate_ != 0) {
        // TrimDelay costs nothing per output at 0 dB with no delay, so this
        // is unconditional rather than gated on active().
        trim_delay_.process(slot_view);
    }
    if (volume_gain_ != 1.0F) {
        // The master volume, last: after render and after the per-speaker
        // trim/delay above, the same way and for the same reason - unity
        // costs nothing per block, so this is gated on the gain rather than
        // on active().
        for (float& sample : block.samples) {
            sample *= volume_gain_;
        }
    }
    pending_frames_ += n;
}

void Player::fill(std::size_t frames) {
    if (!session_ || !decoder_ || history_.empty()) {
        return;
    }
    // Built once per call, capturing one pointer, so the std::functions hold
    // them without allocating.
    const StreamDecoder::BlockFn deliver =
        [this](std::span<const std::span<const float>> rendered, std::size_t n) {
            take_block(rendered, n);
        };
    const Session::ReportFn reported = [this](const UnitReport& report, std::size_t count) {
        take_report(report, count);
    };
    // A bitstream output, or a network group (both PCM and bursts, from a
    // group's own members), is sent each unit as it is decoded.
    const Session::SentFn sent =
        mode_ == OutputMode::kBitstream || mode_ == OutputMode::kNetworkGroup
            ? Session::SentFn{[this](std::span<const std::byte> unit, std::uint32_t samples) {
                  send_unit(unit, samples);
              }}
            : Session::SentFn{};
    while (pending_frames_ < frames && !session_->finished() && !transcode_error_) {
        const auto got =
            session_->render(*decoder_, deliver, frames - pending_frames_, reported, sent);
        if (!got) {
            // One undecodable unit: say so and carry on with the next. The
            // session has already stepped past it.
            last_error_ = got.error();
            note_unit_error(last_error_);
        }
        // Frames a unit completed without a report of its own.
        encode_transcoded(false);
    }
}

std::size_t Player::drain(std::size_t budget) {
    if (mode_ == OutputMode::kNetworkGroup) {
        return drain_group(budget);
    }
    const std::size_t slots = layout_.slots();
    std::array<std::span<const float>, render::OutputLayout::kMaxSlots> views{};
    std::size_t submitted = 0;
    while (pending_count_ != 0 && submitted < budget) {
        const Pending& block = pending_[pending_head_];
        if (bitstreaming()) {
            if (!bitstream_->submit(block.burst)) {
                break;
            }
            // Each item's units in the burst count towards it now they are
            // on their way, and not before: a burst dropped by a seek, a
            // reopen or a stop, or units that never made a whole burst, were
            // not played. A transcode's samples are heard its delay later.
            std::uint64_t offset = 0;
            for (const Span& span : block.spans) {
                if (span.record < history_.size()) {
                    PlayedItem& played = history_[span.record];
                    if (played.frames == 0) {
                        played.first_frame = submitted_since_open_ + link_start() + offset;
                    }
                    played.frames += span.frames;
                }
                offset += span.frames;
            }
        } else {
            for (std::size_t slot = 0; slot < slots; ++slot) {
                views[slot] = std::span<const float>(block.samples).subspan(slot * block.frames,
                                                                             block.frames);
            }
            if (!sink_->submit(std::span<const std::span<const float>>(views.data(), slots),
                               block.frames)) {
                break;
            }
            if (block.record < history_.size()) {
                PlayedItem& played = history_[block.record];
                if (played.frames == 0) {
                    played.first_frame = submitted_since_open_;
                }
                played.frames += block.frames;
            }
        }
        submitted += block.frames;
        submitted_since_open_ += block.frames;
        pending_frames_ -= block.frames;
        pending_head_ = (pending_head_ + 1) % pending_.size();
        --pending_count_;
    }
    return submitted;
}

std::size_t Player::drain_group(std::size_t budget) {
    // Bursts first: an unrelated channel to the same group, paced by its own
    // backpressure, not this budget - see PendingGroupBurst's own comment on
    // why they are not counted here or in pending_frames_.
    while (!pending_group_bursts_.empty()) {
        const PendingGroupBurst& burst = pending_group_bursts_.front();
        if (!group_->submit_burst(burst.pc, burst.pd, burst.payload, burst.frame)) {
            break;
        }
        pending_group_bursts_.pop_front();
    }

    const std::size_t slots = layout_.slots();
    std::array<std::span<const float>, render::OutputLayout::kMaxSlots> views{};
    std::size_t submitted = 0;
    while (pending_count_ != 0 && submitted < budget) {
        const Pending& block = pending_[pending_head_];
        // Group::push() can take part of what is offered - unlike
        // sink_/bitstream_'s all-or-nothing submit(), group_pcm_offset_
        // tracks how far into this block's own frames the group has already
        // taken, so a partial take leaves the rest at the ring's head for
        // the next drain() rather than being re-offered or dropped.
        const std::size_t offered = block.frames - group_pcm_offset_;
        for (std::size_t slot = 0; slot < slots; ++slot) {
            views[slot] = std::span<const float>(block.samples)
                              .subspan((slot * block.frames) + group_pcm_offset_, offered);
        }
        const std::size_t taken =
            group_->submit_pcm(std::span<const std::span<const float>>(views.data(), slots), offered);
        if (taken == 0) {
            break;
        }
        if (block.record < history_.size()) {
            PlayedItem& played = history_[block.record];
            if (played.frames == 0) {
                played.first_frame = submitted_since_open_;
            }
            played.frames += taken;
        }
        submitted += taken;
        submitted_since_open_ += taken;
        pending_frames_ -= taken;
        group_pcm_offset_ += taken;
        if (group_pcm_offset_ < block.frames) {
            break;  // the rest of this block waits for the next drain()
        }
        group_pcm_offset_ = 0;
        pending_head_ = (pending_head_ + 1) % pending_.size();
        --pending_count_;
    }
    return submitted;
}

bool Player::played_out() {
    if (pending_count_ != 0 || !pending_group_bursts_.empty()) {
        return false;
    }
    // A bitstream's last units short of a burst are never sent: a burst
    // is six blocks or nothing. A network group's own pending bursts are
    // the same idea, checked above rather than here: pending_count_ alone
    // does not see them (PendingGroupBurst's own comment says why they are
    // not pending_'s own entries), and unlike a bitstream's leftover
    // partial burst they are already whole and only waiting on room, so
    // they belong with pending_count_'s "still have something to submit"
    // check, not this position-based one.
    const auto position = output_position();
    if (!position) {
        // Closed, or a sink with no clock to wait on.
        return true;
    }
    if (!drain_target_) {
        // Taken once, when the last block has gone in: the device-clock
        // frame by which the last frame submitted will have been heard -
        // everything the clock has run through, everything still held, and
        // the output path's own delay. Not "as many frames played as were
        // submitted": the clock also runs through the silence an underrun
        // inserts, so that test would close the output early by however much
        // silence there had been. The two counts behind the reading are taken
        // without a lock, so it can be short by up to one device period
        // (ac3/audio/playback_counter.hpp).
        drain_target_ =
            position->frames_played + position->frames_queued + position->latency_frames;
    }
    return position->frames_played >= *drain_target_;
}

void Player::drop_prepared() {
    prepared_.reset();
    prepared_index_ = Queue::kNone;
    prepared_choice_.reset();
}

std::size_t Player::prepare_next(PumpReport& report) {
    // Open the next item before asking the transport, so its join decision
    // sees the item's real rate rather than "not probed yet", which would
    // force a reopen on every item the player had not read ahead of time.
    // An item that will not open is marked, which takes it out of
    // next_index()'s answer, and the one after it is tried - so a run of
    // unreadable items between two good ones still ends in a join. Each pass
    // marks one more item, so this ends.
    //
    // When an item that fails is to stop playback, the first that will not
    // open ends the search, and playback stops at it once the current item
    // has been heard. Until the transport hears of it, it is remembered and
    // not marked, so it keeps its place in the order the queue plays in: an
    // item put before it meanwhile plays first, and it is tried again when
    // its turn comes. Under a policy changed to passing over, it is.
    if (failed_next_ != Queue::kNone) {
        if (transport_.on_failure() == FailurePolicy::kStop && failed_next_stands()) {
            return failed_next_;
        }
        failed_next_ = Queue::kNone;
        failed_next_why_.clear();
    }
    for (;;) {
        const std::size_t next = queue_.next_index(transport_.repeat());
        if (next == Queue::kNone) {
            return Queue::kNone;
        }
        const std::string path = queue_.items()[next].path;
        if (prepared_ && prepared_index_ == next && prepared_path_ == path) {
            return Queue::kNone;
        }
        drop_prepared();
        auto opened = Session::open(path, loader_, settings_.programme);
        if (opened) {
            queue_.set_facts(next, opened->facts());
            prepared_ = std::move(*opened);
            prepared_index_ = next;
            prepared_path_ = path;
            return Queue::kNone;
        }
        std::string why = std::move(opened.error());
        note_item(next, title_of(next), fmt::format("cannot be played: {}", why));
        report.note = why;
        if (transport_.on_failure() == FailurePolicy::kStop) {
            failed_next_ = next;
            failed_next_why_ = std::move(why);
            return next;
        }
        mark_unplayable(next, std::move(why));
    }
}

void Player::mark_unplayable(std::size_t item, std::string why) {
    if (item >= queue_.size()) {
        return;
    }
    ItemFacts facts = queue_.items()[item].facts;
    facts.unplayable_because = std::move(why);
    queue_.set_facts(item, std::move(facts));
}

bool Player::failed_next_stands() const {
    // Nothing that can be played lies between the current item and it, in
    // the order the queue plays in.
    const std::size_t size = queue_.size();
    const std::size_t current = queue_.current_index();
    if (failed_next_ >= size || current >= size) {
        return false;
    }
    for (std::size_t step = 1; step <= size; ++step) {
        if (current + step >= size && !transport_.repeat()) {
            return false;
        }
        const std::size_t at = (current + step) % size;
        if (at == failed_next_) {
            return true;
        }
        if (queue_.items()[at].playable()) {
            return false;
        }
    }
    return false;
}

const OutputChoice& Player::prepared_decision() {
    // How the next item would be played, asked once for the session: the
    // decision can enumerate the machine's outputs.
    if (!prepared_choice_) {
        prepared_choice_ = decide(*prepared_);
    }
    return *prepared_choice_;
}

bool Player::next_joins(PumpReport& report) {
    if (prepare_next(report) != Queue::kNone || !prepared_) {
        return false;
    }
    // Once the item is waiting for its tail, with nothing left to hand over
    // and nothing held by the device, it is over but for the output path's
    // delay: what came next would follow silence, and on a timeline that had
    // not counted it. At the item's end itself an empty device is only an
    // underrun - the player fell behind it - and the next item, queued all
    // along, joins as it always has.
    if (tail_waiting_ && pending_count_ == 0) {
        const auto device = output_position();
        if (device && device->frames_queued == 0) {
            return false;
        }
    }
    const OutputChoice& next = prepared_decision();
    return transport_.would_join(next.mode) && join_blocked(next, title_of(prepared_index_)).empty();
}

void Player::item_ended(PumpReport& report, bool heard) {
    const std::size_t failed = prepare_next(report);

    // How the next item would be played, so that it joins only an output
    // already playing it that way: a bitstream does not join a decoded
    // output, and an item that would be bitstreamed is not decoded into one.
    // The transport rules on the mode and the stream; what it cannot see -
    // the endpoint, and whether the packer can make whole bursts of the next
    // item's units - turns a join it would allow into a reopen here.
    std::optional<OutputChoice> next_choice;
    if (failed == Queue::kNone && prepared_) {
        next_choice = prepared_decision();
    }
    if (failed != Queue::kNone) {
        // Marked now the transport is to hear of it, which says why.
        mark_unplayable(failed, std::move(failed_next_why_));
    }
    failed_next_ = Queue::kNone;
    failed_next_why_.clear();
    TransportOutcome outcome =
        failed == Queue::kNone
            ? transport_.item_finished(next_choice ? std::optional<OutputMode>{next_choice->mode}
                                                   : std::nullopt)
            : transport_.item_failed(failed);
    if (outcome.action == TransportAction::kJoinItem && next_choice) {
        std::string blocked = join_blocked(*next_choice, title_of(outcome.item));
        if (!blocked.empty()) {
            outcome.action = TransportAction::kReopenForItem;
            outcome.note = std::move(blocked);
        }
    }
    if (outcome.action == TransportAction::kJoinItem && heard) {
        // Nothing else stands in the way, but the output has played
        // everything out: the item would follow silence, not the item before.
        outcome.action = TransportAction::kReopenForItem;
        outcome.note = fmt::format(
            "\"{}\" came after the output had played out, so it reopens - there is a gap.",
            title_of(outcome.item));
    }
    if (!outcome.note.empty()) {
        report.note = outcome.note;
    }
    switch (outcome.action) {
        case TransportAction::kJoinItem: {
            // The next item follows into the open output. The decoder was
            // reset by the finished session's last render(); the output, and
            // everything already queued for it, carries on.
            const std::uint32_t rate = transport_.open_format().sample_rate;
            const bool transcode = transcoder_.has_value();
            if (!start_session(outcome.item)) {
                // Prepared above, so this is a queue edited in between and
                // an item that no longer opens. Marked, and the transport is
                // asked again from it - each pass marks one more item, so
                // this ends, in a join, a reopen or a stop.
                note_item(outcome.item, title_of(outcome.item),
                          fmt::format("cannot be played: {}", last_error_));
                mark_unplayable(outcome.item, last_error_);
                session_.reset();
                report.note = last_error_;
                if (transport_.on_failure() == FailurePolicy::kStop) {
                    play_out_then(transport_.item_failed(outcome.item), report, heard);
                    return;
                }
                item_ended(report, heard);
                return;
            }
            if (!decoder_fits(rate, transcode)) {
                build_decoder(rate, transcode);
            }
            if (next_choice) {
                choice_ = *next_choice;
            }
            if (mode_ == OutputMode::kBitstream) {
                session_->play_whole_units();
            }
            apply_seek_on_start(outcome.item);
            settle_unit_errors();
            history_.push_back(PlayedItem{.queue_index = outcome.item,
                                          .title = queue_.items()[outcome.item].title,
                                          .first_frame = 0,
                                          .frames = 0,
                                          .expected_frames = session_->total_samples(),
                                          .output_opens = opens_});
            // The new item's first frame goes in behind everything the last
            // one still has queued, or packed toward a burst.
            segments_.push_back(Segment{.record = history_.size() - 1,
                                        .output_start = timeline_end(),
                                        .item_start = session_->position_samples()});
            note_started(outcome.item, true);
            report.item_started = true;
            if (!session_->facts().note.empty()) {
                report.note = session_->facts().note;
            }
            break;
        }
        case TransportAction::kReopenForItem:
        case TransportAction::kStopOutput:
            play_out_then(outcome, report, heard);
            break;
        default:
            session_.reset();
            break;
    }
}

void Player::play_out_then(const TransportOutcome& outcome, PumpReport& report, bool heard) {
    if (!outcome.note.empty()) {
        report.note = outcome.note;
    }
    // A transcode's last frame, padded, and the samples its encoder still
    // holds go out ahead of the wait - unless they already went out, and were
    // heard, before the transport was asked.
    encode_transcoded(true);
    // A stop's note can say why an item cannot be played, which can quote
    // its path: the item it is about is the outcome's.
    if (outcome.action == TransportAction::kReopenForItem) {
        note_item(outcome.item, title_of(outcome.item),
                  said("is next, once the output has played out and reopened", outcome.note));
    } else {
        note_withheld(outcome.item,
                      said("playback ends once the output has played out", outcome.note));
    }
    after_drain_ = outcome;
    if (!heard) {
        drain_target_.reset();
    }
    session_.reset();
}

PumpReport Player::pump(std::size_t budget) {
    PumpReport report;
    if (stop_for_lost_output(report)) {
        return report;
    }
    if (after_drain_) {
        report.frames_submitted += drain(budget);
        // A reopen waits out a pause too: the next item starts on resume.
        const bool held = after_drain_->action == TransportAction::kReopenForItem &&
                          transport_.state() == TransportState::kPaused;
        if (!held && played_out()) {
            const TransportOutcome outcome = *after_drain_;
            after_drain_.reset();
            perform(outcome, &report);
        }
        return report;
    }
    if (transport_.state() != TransportState::kPlaying || !session_) {
        return report;
    }
    // An output change that came while a join was still being heard, once
    // the clock has reached the item it is about - or while a tail was being
    // heard, once a seek has given the item more to play.
    if (refollow_pending_ && !session_->finished() &&
        position().item == queue_.current_index()) {
        std::string moved = refollow();
        if (!moved.empty()) {
            report.note = std::move(moved);
        }
        if (!session_ || transport_.state() != TransportState::kPlaying) {
            return report;
        }
    }
    fill(budget);
    if (stop_for_transcode(report)) {
        return report;
    }
    report.frames_submitted += drain(budget);
    if (session_ && session_->finished()) {
        // Decoded to its end. What follows gapless joins now, behind the
        // item's tail. Anything else waits until the tail has been heard,
        // with the transport still playing the item - a pause or a seek in
        // its last moment is still the item's, and an item added meanwhile
        // can still join.
        const bool joins = next_joins(report);
        if (!joins) {
            encode_transcoded(true);
            report.frames_submitted +=
                drain(budget > report.frames_submitted ? budget - report.frames_submitted : 0);
            if (!played_out()) {
                tail_waiting_ = true;
                return report;
            }
        }
        item_ended(report, !joins);
        if (session_ && report.item_started) {
            // A join: the next item's first blocks go in behind the last
            // item's tail straight away, so the sink never waits on a gap
            // the output does not have.
            fill(budget);
            if (stop_for_transcode(report)) {
                return report;
            }
            report.frames_submitted += drain(budget > report.frames_submitted
                                                 ? budget - report.frames_submitted
                                                 : 0);
        }
    }
    return report;
}

bool Player::stop_for_transcode(PumpReport& report) {
    if (!transcode_error_) {
        return false;
    }
    // The encoder refused a frame: nothing after it can be sent, and the
    // output, not the item, is at fault.
    last_error_ = fmt::format("The transcode to AC-3 failed: {}", *transcode_error_);
    perform(transport_.stop(), &report);
    report.note = last_error_;
    return true;
}

bool Player::stop_for_lost_output(PumpReport& report) {
    if (!output_lost()) {
        return false;
    }
    // The sink closed itself: its device was unplugged, switched off or
    // taken by the system. Nothing submitted to it will be heard, and its
    // clock has stopped, so a play-out waiting on that clock would wait for
    // ever and the blocks waiting for room would never go in. Playback stops
    // instead, and says why. The transport can be stopped already - the
    // queue ran out and the output was playing its last - so the stop is
    // carried out whatever the transport answers. Either way the output is
    // closed, which is what releases whatever the sink still holds.
    last_error_ = choice_.endpoint_name.empty()
                      ? std::string{"Playback stopped: the output device went away."}
                      : fmt::format("Playback stopped: the output device \"{}\" went away.",
                                    choice_.endpoint_name);
    note("output lost: its device went away");
    TransportOutcome outcome = transport_.stop();
    outcome.action = TransportAction::kStopOutput;
    perform(outcome, &report);
    report.note = last_error_;
    return true;
}

}  // namespace ac3::hearth
