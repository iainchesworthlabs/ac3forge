#pragma once

#include <optional>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"

// The two pure per-location facts the soundfield ring (SoundfieldView.qml)
// and its channelMeta() feed need, shared by EncoderController (the
// authoring/live side) and StreamPlayerController (the decode-and-play side)
// so the ring geometry agrees no matter which controller computed it -
// previously defined once, privately, inside encoder_controller.cpp.
namespace ac3gui {

// Where a Table E2.5 location sits on the soundfield plans. This is a GUI-
// only convention - nothing about encoding or decoding reads it - extending
// the ITU-R BS.775 ring ac3::spatial::kSpeakerAzimuthDeg already fixes for
// the bed's five positions (L +30, C 0, R -30, Ls +110, Rs -110, degrees CCW
// from front) to the wider set of channels the general channel model can
// carry. Without this, a plan wider than a plain 5.1 bed had no way to place
// its extra channels at all: ac3::analysis::channel_azimuth_deg(acmod, lfe,
// index) only ever knew about indices inside the BED's own acmod, so a
// dependent substream's channels always came back non-directional and simply
// never appeared on the ring, ceiling or otherwise. LFE/LFE2 stay
// non-directional; every other location gets a plausible placement instead
// of vanishing.
[[nodiscard]] std::optional<double> location_azimuth_deg(ac3::eac3::chanmap::Location location);

// The two soundfield rings: everything overhead goes on the ceiling plan,
// everything else - however far back or wide - stays on the ear-level one.
[[nodiscard]] bool is_ceiling_location(ac3::eac3::chanmap::Location location);

// AC-3's own Table 5.8 bed, in Table E2.5 Location terms so the same ring
// geometry above applies to a plain AC-3/E-AC-3-bed decode exactly as it
// does to a wide E-AC-3 stream's own eac3::chanmap::Layout. Coded order:
// the full-bandwidth channels acmod names, LFE last when present - the same
// order DecodedFrame::channels/DecodedAccessUnit::channels arrive in.
[[nodiscard]] std::vector<ac3::eac3::chanmap::Location> ac3_bed_locations(ac3::Acmod acmod,
                                                                          bool lfe);

}  // namespace ac3gui
