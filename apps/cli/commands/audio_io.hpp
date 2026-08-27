#pragma once

#include <cstdint>
#include <string_view>

#include "../support.hpp"

// The one-shot (non-continuous) audio-hardware commands: enumerate capture/render endpoints and
// run a single record-to-file or play-one-file operation. 'monitor' and 'live' (the two
// continuous/streaming commands - decode-and-play-back, and capture-encode-monitor-and/or-
// passthrough) stay in main.cpp for a later step; run_live alone is ~800 lines and deserves its
// own focused PR rather than being folded in here. Split out as part of the repo-structure
// review's H4 monolith split - see support.hpp's own top comment for the overall plan.
namespace ac3cli::commands {

int run_devices();

// Capture live audio and encode it straight to AC-3. The capture thread fills
// a lock-free ring; this thread drains it a frame at a time.
int run_record(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
               int device_index, const ac3cli::Options& meta);

int run_outputs();

// Stream an AC-3 or E-AC-3 file to a receiver in real time via exclusive-mode
// IEC 61937. The sink's render thread pulls bursts; this loop keeps it fed.
// bsid picks the branch: AC-3 wraps one frame per burst, E-AC-3 wraps one
// access unit at a time through a persistent Eac3BurstPacker, which may hold
// bytes back until enough have accumulated to fill a burst (see
// Eac3BurstPacker's own comment on why - Annex E frames can cover as few as
// one of the six blocks a burst period spans).
//
// Roadmap UX9: when a specific device_index is named, this first asks what
// it actually accepts - ac3::audio::read_sink_capabilities' EDID read where
// a backend has one, enumerate_render_devices' own live probe otherwise (see
// that header's comment for why the two are kept apart). A source format the
// sink rejects gets an automatic fallback rather than a refusal: E-AC-3 on an
// AC-3-only sink is transcoded to AC-3 first (roadmap DC9's transcode, feeding
// this same passthrough path); a sink that bitstreams neither format falls
// back to decoded PCM (run_monitor). meta.follow_sink (follow=off) restores
// the plain refusal 'play' always gave before this roadmap item, the same
// on-by-default shape 'live's downmix_leg (downmix=off) already uses.
int run_play(std::string_view in_path, int device_index, const ac3cli::Options& meta);

}  // namespace ac3cli::commands
