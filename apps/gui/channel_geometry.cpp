#include "channel_geometry.hpp"

#include "ac4_objects_core.hpp"

namespace ac3gui {

std::optional<double> location_azimuth_deg(iclforge::eac3::chanmap::Location location) {
    // The one table the soundfield ring and the AC-4 pins read, in apps/common so that
    // ac3cli's atmos-encode reads it too.
    return iclforge::apps::location_azimuth_deg(location);
}

bool is_ceiling_location(iclforge::eac3::chanmap::Location location) {
    using iclforge::eac3::chanmap::Location;
    switch (location) {
        case Location::kTs:
        case Location::kVhl:
        case Location::kVhr:
        case Location::kVhc:
        case Location::kLts:
        case Location::kRts:
            return true;
        default:
            return false;
    }
}

std::vector<iclforge::eac3::chanmap::Location> ac3_bed_locations(iclforge::Acmod acmod, bool lfe) {
    using iclforge::Acmod;
    using iclforge::eac3::chanmap::Location;
    std::vector<Location> out;
    switch (acmod) {
        case Acmod::kDualMono:
            // 1+1: two independent programmes, not a soundfield - no speaker
            // location to sort by, the same reasoning plan::monitor_order's
            // own comment gives for leaving DecodedAccessUnit::layout empty
            // here.
            return {};
        case Acmod::k1_0:
            out = {Location::kCentre};
            break;
        case Acmod::k2_0:
            out = {Location::kLeft, Location::kRight};
            break;
        case Acmod::k3_0:
            out = {Location::kLeft, Location::kCentre, Location::kRight};
            break;
        case Acmod::k2_1:
            out = {Location::kLeft, Location::kRight, Location::kCs};
            break;
        case Acmod::k3_1:
            out = {Location::kLeft, Location::kCentre, Location::kRight, Location::kCs};
            break;
        case Acmod::k2_2:
            out = {Location::kLeft, Location::kRight, Location::kLeftSurround,
                   Location::kRightSurround};
            break;
        case Acmod::k3_2:
            out = {Location::kLeft, Location::kCentre, Location::kRight, Location::kLeftSurround,
                   Location::kRightSurround};
            break;
    }
    if (lfe) {
        out.push_back(Location::kLfe);
    }
    return out;
}

}  // namespace ac3gui
