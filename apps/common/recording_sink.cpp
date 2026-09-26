#include "recording_sink.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include "ac3/core/tables.hpp"

namespace {

// The user-facing strings match EncoderController::writeOutput's for the
// same failures, so a streamed take and a whole-buffer one report a broken
// disk in the same words.
constexpr const char* kCannotOpen = "Could not open the output file for writing.";
constexpr const char* kCannotWrap = "Could not wrap the stream into IEC 61937 bursts.";
constexpr const char* kNothingEncoded = "Nothing was encoded.";
constexpr const char* kNoAc4Matroska =
    "Matroska registers no codec ID for AC-4: record it raw, as MPEG-TS, as an IEC 61937 "
    "carrier or as fragmented MP4.";
constexpr const char* kNotAc4SyncFrame = "An AC-4 frame was not a whole sync frame.";
constexpr const char* kNoAc4Burst =
    "No IEC 61937-14 burst type carries AC-4 frames at this rate.";

// An AC-4 sync frame's raw_ac4_frame (ETSI TS 103 190-2 Annex G.3.1): past
// the sync word and frame_size, 16 bits or 0xFFFF and 24 more, and before the
// crc_word a 0xAC41 frame ends with. Empty where `sync_frame` is not whole.
std::span<const std::byte> raw_ac4_frame(std::span<const std::byte> sync_frame) {
    if (sync_frame.size() < 4) {
        return {};
    }
    const auto byte = [&](std::size_t i) { return std::to_integer<std::size_t>(sync_frame[i]); };
    const bool crc = byte(1) == 0x41;
    std::size_t head = 4;
    std::size_t size = (byte(2) << 8) | byte(3);
    if (size == 0xFFFF) {
        if (sync_frame.size() < 7) {
            return {};
        }
        size = (byte(4) << 16) | (byte(5) << 8) | byte(6);
        head = 7;
    }
    if (head + size + (crc ? 2 : 0) > sync_frame.size()) {
        return {};
    }
    return sync_frame.subspan(head, size);
}

const char* write_failed_for(RecordingSink::Container container) {
    switch (container) {
        case RecordingSink::Container::kMatroska:
            return "Writing the Matroska file failed.";
        case RecordingSink::Container::kMpegts:
            return "Writing the MPEG-TS file failed.";
        case RecordingSink::Container::kSpdif:
            return "Writing the WAV carrier failed.";
        case RecordingSink::Container::kFmp4:
            return "Writing the fragmented MP4 folder failed.";
        case RecordingSink::Container::kElementary:
            break;
    }
    return "Writing the stream failed.";
}

}  // namespace

std::string RecordingSink::open(const std::string& path, const Config& config) {
    config_ = config;
    path_ = path;
    frames_ = 0;
    // Whether anything at all - a file, a folder, a device node, a symlink,
    // dangling or not - was at `path` before this take. Asked of the link
    // itself (symlink_status), so a symlink counts as there whatever it
    // points at. close() cleans up after an empty take only when it was not:
    // what the user already had is theirs, and "nothing was encoded" is no
    // licence to delete it.
    std::error_code probe;
    created_ = !std::filesystem::exists(std::filesystem::symlink_status(path, probe));

    if (config.ac4.has_value() && config.container == Container::kMatroska) {
        return kNoAc4Matroska;
    }

    if (config.container == Container::kSpdif) {
        if (config.ac4.has_value()) {
            // IEC 61937-14's link, as the caller chose it for the stream's
            // rate: ac3cli's own run_spdif makes the same choice for a
            // finished stream.
            if (config.ac4->carrier_rate_hz == 0) {
                return kNoAc4Burst;
            }
            if (!wav_.open(path, config.ac4->carrier_rate_hz, config.ac4->carrier_channels)) {
                return kCannotOpen;
            }
            ac4_packer_.emplace(config.ac4->burst_type);
            open_ = true;
            return {};
        }
        // The carrier runs at 4x the content rate for E-AC-3 - see
        // ac3cli's own run_spdif (apps/cli/main.cpp) for the citation.
        const auto carrier_rate =
            config.eac3 ? config.sample_rate * 4 : config.sample_rate;
        if (!wav_.open(path, carrier_rate, 2)) {
            return kCannotOpen;
        }
        packer_ = {};
        open_ = true;
        return {};
    }

    if (config.container == Container::kFmp4) {
        // A folder, not a file - and nothing is written into it yet: the
        // fragmenter's track needs a bitstream scan, so it waits for the
        // first frame (see Fmp4FolderWriter). Creating the folder here still
        // means an unwritable destination refuses the take before capture
        // starts, which is what open()'s contract above promises. An AC-4
        // take's track is its caller's description instead.
        std::optional<Fmp4FolderWriter::Track> described;
        if (config.ac4.has_value()) {
            described = config.ac4->fmp4;
        }
        if (auto problem = fmp4_.open(path, config.fmp4_window_segments, std::move(described));
            !problem.empty()) {
            return problem;
        }
        open_ = true;
        return {};
    }

    if (config.container == Container::kMatroska) {
        auto writer = matroska::Writer::create(matroska::AudioTrack{
            .codec_id = std::string{config.eac3 ? matroska::kCodecEac3 : matroska::kCodecAc3},
            .sample_rate = config.sample_rate,
            .channels = config.channels,
            .samples_per_frame = ac3::kSamplesPerFrame});
        if (!writer.has_value()) {
            return std::string{matroska::describe(writer.error())};
        }
        matroska_.emplace(std::move(*writer));
    } else if (config.container == Container::kMpegts) {
        // An AC-4 track's PMT says no more than its codec (the presentation
        // detail lives in the table of contents), as 'ac3cli ts' writes it.
        const bool ac4 = config.ac4.has_value();
        auto writer = mpegts::Writer::create(mpegts::AudioTrack{
            .codec = ac4           ? mpegts::AudioCodec::kAc4
                     : config.eac3 ? mpegts::AudioCodec::kEac3
                                   : mpegts::AudioCodec::kAc3,
            .sample_rate = config.sample_rate,
            .channels = ac4 ? 2 : config.channels,
            .samples_per_frame = ac4 ? config.ac4->samples_per_frame
                                     : static_cast<std::uint32_t>(ac3::kSamplesPerFrame)});
        if (!writer.has_value()) {
            return std::string{mpegts::describe(writer.error())};
        }
        mpegts_.emplace(std::move(*writer));
    }

    file_.open(path, std::ios::binary);
    if (!file_) {
        return kCannotOpen;
    }
    if (matroska_.has_value() && !write_file(matroska_->header())) {
        return write_failed_for(config_.container);
    }
    open_ = true;
    return {};
}

std::string RecordingSink::push(std::span<const std::byte> frame, bool sync) {
    switch (config_.container) {
        case Container::kElementary:
            if (!write_file(frame)) {
                return write_failed_for(config_.container);
            }
            break;
        case Container::kMatroska: {
            auto closed = matroska_->push(frame);
            if (!closed.has_value()) {
                return std::string{matroska::describe(closed.error())};
            }
            if (!closed->empty() && !write_file(*closed)) {
                return write_failed_for(config_.container);
            }
            break;
        }
        case Container::kMpegts: {
            auto packets = mpegts_->push(frame);
            if (!packets.has_value()) {
                return std::string{mpegts::describe(packets.error())};
            }
            if (!write_file(*packets)) {
                return write_failed_for(config_.container);
            }
            break;
        }
        case Container::kFmp4: {
            // An AC-4 sample is the raw frame alone (TS 103 190-2 Annex E.4).
            std::span<const std::byte> sample = frame;
            if (config_.ac4.has_value()) {
                sample = raw_ac4_frame(frame);
                if (sample.empty()) {
                    return kNotAc4SyncFrame;
                }
            }
            if (const auto problem = fmp4_.push(sample, sync); !problem.empty()) {
                return problem;
            }
            break;
        }
        case Container::kSpdif: {
            if (ac4_packer_.has_value()) {
                auto burst = ac4_packer_->push(frame);
                if (!burst.has_value()) {
                    return kCannotWrap;
                }
                if (!wav_.write(*burst)) {
                    return write_failed_for(config_.container);
                }
            } else if (config_.eac3) {
                auto burst = packer_.push(frame);
                if (!burst.has_value()) {
                    return kCannotWrap;
                }
                if (burst->has_value() && !wav_.write(**burst)) {
                    return write_failed_for(config_.container);
                }
            } else {
                auto burst = ac3::iec61937::wrap_frame(frame);
                if (!burst.has_value()) {
                    return kCannotWrap;
                }
                if (!wav_.write(*burst)) {
                    return write_failed_for(config_.container);
                }
            }
            // One frame is 32 ms, so this patches the carrier's header
            // about once a second - WavPcm16StreamWriter::flush_header's
            // own crash-worst-case rationale.
            if (frames_ % 32 == 31) {
                wav_.flush_header();
            }
            break;
        }
    }
    ++frames_;
    return {};
}

std::string RecordingSink::close() {
    if (!open_) {
        return {};
    }
    open_ = false;
    if (frames_ == 0) {
        // The whole-buffer path never created a file for an empty take;
        // matching that means removing the one open() already created. kFmp4
        // never wrote anything into its folder (start_fmp4 waits for a first
        // frame that never came), so removing the folder is the same
        // gesture - and remove(), not remove_all(), so a folder the user
        // pointed at that already had something in it is left alone.
        //
        // Only what open() itself created, though, and only if it is still
        // the plain file (or, for kFmp4, folder) it made. A path that already
        // held something - a user's file, a symlink, a device node such as
        // /dev/null - is left where it was: open() truncated a file there,
        // as any take to an existing path does, but removing it would
        // delete something this sink never owned (and, run as root, a
        // device node the whole machine uses).
        wav_.close();
        file_.close();
        if (created_) {
            std::error_code ec;
            const auto made = config_.container == Container::kFmp4
                                  ? fmp4_.directory()
                                  : std::filesystem::path{path_};
            const auto status = std::filesystem::symlink_status(made, ec);
            const bool ours = config_.container == Container::kFmp4
                                  ? std::filesystem::is_directory(status)
                                  : std::filesystem::is_regular_file(status);
            if (ours) {
                std::filesystem::remove(made, ec);
            }
        }
        return kNothingEncoded;
    }
    if (config_.container == Container::kFmp4) {
        return fmp4_.close();
    }
    if (config_.container == Container::kSpdif) {
        // An E-AC-3 tail that never completed a burst is dropped, exactly
        // as the one-shot wrap_stream drops it.
        wav_.close();
        return {};
    }
    if (matroska_.has_value() && !write_file(matroska_->finalize())) {
        return write_failed_for(config_.container);
    }
    if (mpegts_.has_value()) {
        // Always empty by contract; called so the two writers age uniformly.
        std::ignore = mpegts_->finalize();
    }
    file_.close();
    if (file_.fail()) {
        return write_failed_for(config_.container);
    }
    return {};
}

bool RecordingSink::write_file(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return static_cast<bool>(file_);
    }
    file_.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file_);
}
