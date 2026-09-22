#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/eac3_tables.hpp"

// The channel mask WAVEFORMATEXTENSIBLE defines, and how its speaker positions
// relate to the locations the renderer places (ac3::eac3::chanmap::Location,
// Table E2.5).
//
// Every output backend here already speaks some version of this vocabulary:
// WASAPI carries it as dwChannelMask, ALSA answers snd_pcm_query_chmaps with
// SND_CHMAP_* positions, PipeWire lists audio.position names, and CoreAudio
// gives channel labels in an AudioChannelLayout. They name the same speakers,
// so this file is the one place that maps between them and the renderer's
// locations - a device record can then say which speaker each of its channels
// is (passthrough.hpp's RenderDeviceInfo), and a routing patch from the
// renderer's slots to the device's channels can be built from that
// (render::Routing, A1) rather than guessed from a channel count.
//
// The bit values are ksmedia.h's SPEAKER_* constants, spelled out because
// nothing in src/ includes a Windows header on another platform, and because
// the ALSA, PipeWire and CoreAudio backends need the same numbers to report a
// mask of their own.

namespace ac3::audio {

using Location = ac3::eac3::chanmap::Location;

inline constexpr std::uint32_t kSpeakerFrontLeft = 0x1;
inline constexpr std::uint32_t kSpeakerFrontRight = 0x2;
inline constexpr std::uint32_t kSpeakerFrontCentre = 0x4;
inline constexpr std::uint32_t kSpeakerLowFrequency = 0x8;
inline constexpr std::uint32_t kSpeakerBackLeft = 0x10;
inline constexpr std::uint32_t kSpeakerBackRight = 0x20;
inline constexpr std::uint32_t kSpeakerFrontLeftOfCentre = 0x40;
inline constexpr std::uint32_t kSpeakerFrontRightOfCentre = 0x80;
inline constexpr std::uint32_t kSpeakerBackCentre = 0x100;
inline constexpr std::uint32_t kSpeakerSideLeft = 0x200;
inline constexpr std::uint32_t kSpeakerSideRight = 0x400;
inline constexpr std::uint32_t kSpeakerTopCentre = 0x800;
inline constexpr std::uint32_t kSpeakerTopFrontLeft = 0x1000;
inline constexpr std::uint32_t kSpeakerTopFrontCentre = 0x2000;
inline constexpr std::uint32_t kSpeakerTopFrontRight = 0x4000;
inline constexpr std::uint32_t kSpeakerTopBackLeft = 0x8000;
inline constexpr std::uint32_t kSpeakerTopBackCentre = 0x10000;
inline constexpr std::uint32_t kSpeakerTopBackRight = 0x20000;
// Every bit above, which is every position WAVEFORMATEXTENSIBLE names; the
// rest of dwChannelMask is reserved, and SPEAKER_ALL (0x80000000) says "all
// of them" without saying which.
inline constexpr std::uint32_t kSpeakerAllPositions = 0x3FFFF;

// KSAUDIO_SPEAKER_* arrangements, for the widths a device is taken to have
// when it does not say which speakers its channels are.
inline constexpr std::uint32_t kSpeakersMono = kSpeakerFrontCentre;
inline constexpr std::uint32_t kSpeakersStereo = kSpeakerFrontLeft | kSpeakerFrontRight;
inline constexpr std::uint32_t kSpeakersQuad = kSpeakersStereo | kSpeakerBackLeft | kSpeakerBackRight;
inline constexpr std::uint32_t kSpeakers5_1 =
    kSpeakersStereo | kSpeakerFrontCentre | kSpeakerLowFrequency | kSpeakerBackLeft | kSpeakerBackRight;
// KSAUDIO_SPEAKER_7POINT1_SURROUND, the arrangement Windows reports for eight
// channels today: the surrounds at the sides and the rears behind, not
// 7POINT1's two front-of-centre speakers, which no consumer installation has.
inline constexpr std::uint32_t kSpeakers7_1 = kSpeakersStereo | kSpeakerFrontCentre | kSpeakerLowFrequency |
                                              kSpeakerBackLeft | kSpeakerBackRight | kSpeakerSideLeft |
                                              kSpeakerSideRight;
inline constexpr std::uint32_t kSpeakers5_1_2 = kSpeakers5_1 | kSpeakerTopFrontLeft | kSpeakerTopFrontRight;
inline constexpr std::uint32_t kSpeakers5_1_4 = kSpeakers5_1_2 | kSpeakerTopBackLeft | kSpeakerTopBackRight;
inline constexpr std::uint32_t kSpeakers7_1_2 = kSpeakers7_1 | kSpeakerTopFrontLeft | kSpeakerTopFrontRight;
inline constexpr std::uint32_t kSpeakers7_1_4 = kSpeakers7_1_2 | kSpeakerTopBackLeft | kSpeakerTopBackRight;

// How many speakers a mask names.
[[nodiscard]] std::uint16_t speaker_count(std::uint32_t mask);

// The location one SPEAKER_* bit names on its own. Nothing for a bit with no
// location of its own: SPEAKER_TOP_BACK_CENTRE, which Table E2.5 has no
// position for, anything outside kSpeakerAllPositions, and a value with more
// than one bit set. SPEAKER_BACK_LEFT and SPEAKER_BACK_RIGHT answer with the
// surrounds of a 5.1 ring here; in a mask that also names the sides they are
// the rear surrounds instead, which is what locations_of() resolves.
[[nodiscard]] std::optional<Location> location_of(std::uint32_t speaker);

// The bit that carries `location`, or 0 for a location WAVEFORMATEXTENSIBLE
// has no position for: the two wides (Lw, Rw), the two surround-direct
// positions (Lsd, Rsd) and the second LFE feed.
[[nodiscard]] std::uint32_t speaker_of(Location location);

// The locations `mask` names, in the ascending bit order an interleaved stream
// with that mask carries its channels in.
//
// Whether SPEAKER_BACK_LEFT and SPEAKER_BACK_RIGHT are the surrounds of a 5.1
// ring (Ls and Rs, at +-110 degrees) or the rear surrounds of a 7.1 room (Lrs
// and Rrs, with Ls and Rs moved to the sides at +-90) depends on the company
// they keep, so it cannot be read off one bit: with SPEAKER_SIDE_LEFT and
// SPEAKER_SIDE_RIGHT present the sides are the surrounds and the backs the
// rears, which is how ITU-R BS.2051 lays 7.1 out and how
// ac3::spatial::direction_of resolves the same pair of names
// (render/layout.hpp's header). A bit with no location of its own is left out,
// so the result can be shorter than speaker_count(mask) - a caller that needs
// one entry per channel must check.
[[nodiscard]] std::vector<Location> locations_of(std::uint32_t mask);

// The mask that names every location in `locations`, ignoring the ones
// WAVEFORMATEXTENSIBLE cannot name. A list holding both the surrounds and the
// rear surrounds puts the surrounds at the sides, as locations_of() reads them
// back.
[[nodiscard]] std::uint32_t speakers_of(std::span<const Location> locations);

// The arrangement a device of `channels` channels is taken to have when it
// does not say: the KSAUDIO_SPEAKER_* constant of that width, and 0 for a
// width with no standard arrangement. 0 means "unknown", never "no speakers",
// and a caller must treat it as such - the only safe reading of a mask it does
// not have is to leave the channels where they are.
[[nodiscard]] std::uint32_t default_speakers(std::uint16_t channels);

// The locations a mask names, by the names the bitstream gives them
// (ac3::eac3::chanmap::name): "L R C LFE Ls Rs". Empty for an empty mask, and
// a trailing count of the positions with no location of their own.
[[nodiscard]] std::string describe_speakers(std::uint32_t mask);

// The abbreviation WAVEFORMATEXTENSIBLE's own convention gives one speaker
// bit - "FL", "LFE", "SL" and so on for the rest of kSpeakerAllPositions - for
// the Speakers page's routing grid to name a device OUTPUT by. A different
// vocabulary from chanmap::name(), which names the RENDERER's locations in
// the bitstream's own terms ("L", "Ls") and folds SPEAKER_BACK_LEFT/RIGHT into
// the rear surrounds when the mask also names the sides (locations_of()'s own
// comment); an output's own name does not depend on the company its mask
// keeps, so this is a plain one-bit-to-one-name table, not a location lookup.
// Nothing for a bit outside kSpeakerAllPositions or with more than one bit
// set.
[[nodiscard]] std::optional<std::string> mask_position_name(std::uint32_t speaker);

// One name per output, in the ascending bit order an interleaved stream
// carries them (locations_of()'s own order) - what the Speakers page's
// routing grid header names each column by, alongside its 1-based output
// number. `mask` is normally the open device's own (PcmSink::speaker_mask());
// with none (0), default_speakers(outputs) is tried the same way
// speaker_routing() falls back for its default patch, and a width neither can
// name (default_speakers()'s own comment: ten channels is 5.1.4 or 7.1.2, and
// nothing says which) gives back `outputs` empty strings, for a caller to
// show a bare output number instead. Always exactly `outputs` entries, even
// when the resolved mask names more or fewer speakers than that: a short
// mask pads with empty strings, and a long one is truncated, since the
// caller's own output count is the one the routing grid actually draws.
[[nodiscard]] std::vector<std::string> output_names(std::uint32_t mask, std::uint16_t outputs);

}  // namespace ac3::audio
