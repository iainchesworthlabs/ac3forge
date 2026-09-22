#include "channel_geometry.hpp"

namespace ac3gui {

std::optional<double> location_azimuth_deg(ac3::eac3::chanmap::Location location) {
    using ac3::eac3::chanmap::Location;
    switch (location) {
        case Location::kLeft: return 30.0;
        case Location::kCentre: return 0.0;
        case Location::kRight: return -30.0;
        case Location::kLeftSurround: return 110.0;
        case Location::kRightSurround: return -110.0;
        case Location::kLc: return 15.0;
        case Location::kRc: return -15.0;
        case Location::kLrs: return 135.0;
        case Location::kRrs: return -135.0;
        case Location::kCs: return 180.0;
        case Location::kTs: return 180.0;    // ceiling: overhead-rear
        case Location::kLsd: return 90.0;
        case Location::kRsd: return -90.0;
        case Location::kLw: return 60.0;
        case Location::kRw: return -60.0;
        case Location::kVhl: return 45.0;    // ceiling: front height
        case Location::kVhr: return -45.0;   // ceiling: front height
        case Location::kVhc: return 0.0;     // ceiling: centre height
        case Location::kLts: return 110.0;   // ceiling: rear height
        case Location::kRts: return -110.0;  // ceiling: rear height
        case Location::kLfe2:
        case Location::kLfe:
            return std::nullopt;
    }
    return std::nullopt;
}

bool is_ceiling_location(ac3::eac3::chanmap::Location location) {
    using ac3::eac3::chanmap::Location;
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

std::vector<ac3::eac3::chanmap::Location> ac3_bed_locations(ac3::Acmod acmod, bool lfe) {
    using ac3::Acmod;
    using ac3::eac3::chanmap::Location;
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
