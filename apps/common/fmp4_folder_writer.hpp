#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include "mp4/dash.hpp"
#include "mp4/hls.hpp"
#include "mp4/mp4.hpp"

// Writes a fragmented MP4/CMAF FOLDER as encoded access units arrive, through
// mp4::FragmentWriter: init.mp4, one segment<N>.m4s each time a fragment
// closes, and audio.m3u8/master.m3u8/manifest.mpd rewritten beside them on
// every close. Live-shaped while the take runs - no #EXT-X-ENDLIST, a
// type="dynamic" MPD with an availabilityStartTime, so the folder is a
// servable origin mid-take - and closed to the VOD/static forms by close().
//
// Shared by every write-as-you-go path in BOTH front ends: RecordingSink
// (apps/cli's record/live and the GUI's Record button take, both via
// RecordingSink::Container::kFmp4) and the GUI's own live session
// (EncoderController's LiveOutputWriters). fMP4 is the one container all
// three reach for the same code here rather than each growing its own copy -
// which a fourth, near-identical copy in ac3cli (apps/cli/support.hpp's
// Fmp4SessionWriter) used to be, before moving this class to apps/common
// gave it the same shared home RecordingSink already has.
//
// Qt-free on purpose, matching RecordingSink: everything here is std:: and
// mp4::, so tests' plain C++ test drives it without a QML engine. Errors
// come back as user-facing strings - empty means fine.
class Fmp4FolderWriter {
   public:
    // Creates the folder. Deliberately does NOT write anything into it yet:
    // mp4::AudioTrack's dac3/dec3 payload is bitstream syntax, so the
    // fragmenter cannot exist until the first access unit does (see push()).
    // Creating the folder here still means an unwritable destination refuses
    // the take before capture starts.
    //
    // `window_segments`: how many of the most recent media segments the HLS
    // playlist and DASH MPD list - a rolling live window
    // (mp4::FragmentOptions::playlist_window_segments). 0, the default,
    // lists every segment, which is what every existing caller except
    // ac3cli's own `fmp4-window=` token wants.
    [[nodiscard]] std::string open(const std::string& directory,
                                   std::uint32_t window_segments = 0);

    // Scans the first frame to build the track (once), then buffers into the
    // current fragment; writes a segment and refreshes the manifests whenever
    // one closes.
    [[nodiscard]] std::string push(std::span<const std::byte> frame);

    // Flushes the trailing partial fragment and rewrites the manifests in
    // their finished form. A no-op if nothing was ever pushed.
    [[nodiscard]] std::string close();

    [[nodiscard]] const std::filesystem::path& directory() const { return dir_; }
    [[nodiscard]] std::size_t segments() const { return segments_; }
    // Whether a first frame has arrived and the folder therefore holds an
    // init segment - false means close() has nothing to finish.
    [[nodiscard]] bool started() const { return writer_.has_value(); }

   private:
    [[nodiscard]] std::string start(std::span<const std::byte> first_frame);
    [[nodiscard]] std::string write_manifests(const mp4::FragmentWriter& writer, bool finished);

    std::filesystem::path dir_;
    std::uint32_t window_segments_ = 0;
    std::size_t segments_ = 0;
    mp4::AudioTrack track_;
    mp4::HlsOptions hls_;
    mp4::DashOptions dash_;
    // ISO 8601 UTC, stamped once when the first segment's timeline starts: a
    // live MPD's @availabilityStartTime must not move as the session runs,
    // and mp4:: has no clock of its own to read (mp4::MpdOptions).
    std::string availability_start_;
    std::optional<mp4::FragmentWriter> writer_;
};
