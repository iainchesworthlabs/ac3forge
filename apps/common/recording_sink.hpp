#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>

#include "ac3/io/wav.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "fmp4_folder_writer.hpp"
#include "matroska/matroska.hpp"
#include "mpegts/mpegts.hpp"

// Streams a recording's encoded frames to their destination as they are
// produced, for the containers whose format allows it - which is what turns
// a take's memory footprint from "the whole session" (~3.4 MB per minute at
// 448 kbps, unbounded until Stop) into one frame, and puts the take's bytes
// on disk continuously instead of only at a clean stop, so a crash an hour
// in no longer loses the hour.
//
// The five containers here are exactly the streamable ones: a bare
// elementary stream appends; Matroska streams via matroska::Writer (EBML's
// own unknown-size Segment pattern - the file differs from the one-shot
// mux()'s by exactly that, as that class's comment describes); MPEG-TS via
// mpegts::Writer (whose bytes are contract-identical to mux()'s); fragmented
// MP4/CMAF into a DIRECTORY rather than a file, through the Fmp4FolderWriter
// EncoderController's own live session shares (whose media segments are
// likewise contract-identical to mp4::fragment()'s);
// the IEC 61937 WAV carrier via per-frame wrapping into
// ac3::io::WavPcm16StreamWriter, whose closed file is byte-identical to
// write_wav_pcm16_raw over the same bursts. Plain MP4 is the one deliberately
// absent: moov/stco need every frame's final offset, so the
// accumulate-then-mux shape in EncoderController::writeOutput IS its design,
// and the recording loop keeps that shape for it.
//
// Qt-free on purpose: everything here is std:: and the container libraries,
// so tests' plain C++ test can drive it against the one-shot writers without
// a QML engine in the room. Errors come back as the user-facing strings
// EncoderController's status line already shows - empty means fine.
//
// Shared by BOTH front ends since wide-layout record/live paths, which is why it lives in
// apps/common rather than apps/gui: `ac3cli record`/`ac3cli live` write their
// takes through this same class, so a CLI take and a GUI take of the same
// container are the same bytes produced the same way, and the crash-safety
// and bounded-memory properties above are one implementation rather than
// two. Neither app owns it; both compile it in directly (there is no
// library target to link, the same arrangement the CLI's own support.cpp
// has).
class RecordingSink {
   public:
    enum class Container : std::uint8_t {
        kElementary,
        kMatroska,
        kSpdif,
        kMpegts,
        kFmp4,
    };

    // An AC-4 take (ETSI TS 103 190), which this class reads no syntax of, so
    // its caller describes the stream to each container: the frames pushed
    // are sync frames (TS 103 190-2 Annex G), push() is told which are
    // I-frames, and Matroska, which registers no AC-4 codec ID, is refused.
    struct Ac4Carriage {
        // Samples a frame decodes to: an MPEG-TS PES timestamp's step.
        std::uint32_t samples_per_frame = 2048;
        // kSpdif: the IEC 61937-14 burst type the stream's largest frame
        // needs, and the carrier a WAV file holds its link in (HBR16's eight
        // channels at a quarter of the link's rate).
        ac3::iec61937::BurstDataType burst_type = ac3::iec61937::BurstDataType::kAc4;
        std::uint32_t carrier_rate_hz = 48000;
        std::uint16_t carrier_channels = 2;
        // kFmp4: the track, each sample the raw frame alone (Annex E.4).
        Fmp4FolderWriter::Track fmp4{};
    };

    struct Config {
        Container container = Container::kElementary;
        bool eac3 = false;
        std::uint32_t sample_rate = 48000;
        int channels = 2;
        // kFmp4 only: how many of the most recent media segments the HLS
        // playlist and DASH MPD list - a rolling live window
        // (Fmp4FolderWriter::open's own parameter). 0, the default, lists
        // every segment.
        std::uint32_t fmp4_window_segments = 0;
        // Set for an AC-4 take; `eac3` is then ignored.
        std::optional<Ac4Carriage> ac4 = std::nullopt;
    };

    // Empty on success. A failure here happens before any capture is worth
    // starting - the file could not be created, or the track refused to
    // validate - so the caller can surface it immediately rather than at
    // the end of a take. For kFmp4 `path` names a DIRECTORY (the same choice
    // EncoderController::outputIsFolder already makes for that container),
    // created here; its writer, though, cannot exist until the first frame -
    // see push().
    [[nodiscard]] std::string open(const std::string& path, const Config& config);

    // Empty on success. On failure the bytes already written stay on disk -
    // a partial take is a take, unlike a failed file encode whose input
    // still exists - and the caller decides whether to keep recording.
    // `sync`: an AC-4 frame's b_iframe_global, where a fragmented MP4's
    // fragments may start; every AC-3 and E-AC-3 frame is one.
    [[nodiscard]] std::string push(std::span<const std::byte> frame, bool sync = true);

    // Finalizes the container. With zero frames pushed, removes the file
    // and reports "Nothing was encoded." - the whole-buffer path this
    // replaces never created a file at all in that case. Only a file (for
    // kFmp4, a folder) that open() created is removed: a path that already
    // held something before the take - a file, a symlink, a device node -
    // keeps it, truncated by open() as a take to an existing path always is,
    // but never deleted.
    [[nodiscard]] std::string close();

    [[nodiscard]] std::size_t frames() const { return frames_; }

   private:
    [[nodiscard]] bool write_file(std::span<const std::byte> bytes);

    Config config_;
    std::string path_;
    // Whether nothing was at path_ before open(); see close().
    bool created_ = false;
    bool open_ = false;
    std::size_t frames_ = 0;
    // kElementary / kMatroska / kMpegts write here...
    std::ofstream file_;
    // ...through these; kSpdif goes through wav_ instead.
    std::optional<matroska::Writer> matroska_;
    std::optional<mpegts::Writer> mpegts_;
    ac3::io::WavPcm16StreamWriter wav_;
    ac3::iec61937::Eac3BurstPacker packer_;
    std::optional<ac3::iec61937::Ac4BurstPacker> ac4_packer_;
    // ...and kFmp4 writes a folder of its own files through this.
    Fmp4FolderWriter fmp4_;
};
