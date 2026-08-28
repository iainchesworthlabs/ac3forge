#include "ac3/admbridge/coordinates.hpp"

#include <cmath>
#include <numbers>
#include <type_traits>
#include <variant>

namespace ac3::admbridge {

ac3adm::CartesianPosition polar_to_adm_cartesian(const ac3adm::PolarPosition& polar) {
    const double azimuth_rad = polar.azimuth_deg * std::numbers::pi / 180.0;
    const double elevation_rad = polar.elevation_deg * std::numbers::pi / 180.0;
    const double r = polar.distance;
    const double horizontal = r * std::cos(elevation_rad);
    // Clause 8: azimuth 0 = straight ahead (+Y), positive = left; X is right-positive, so
    // "positive azimuth" moves toward negative X. Elevation 0 = level, positive = up (+Z).
    return {.x = -horizontal * std::sin(azimuth_rad),
            .y = horizontal * std::cos(azimuth_rad),
            .z = r * std::sin(elevation_rad)};
}

ac3::oba::Position adm_cartesian_to_room(const ac3adm::CartesianPosition& cartesian) {
    return {.x = (cartesian.x + 1.0) / 2.0,
            .y = (1.0 - cartesian.y) / 2.0,
            .z = cartesian.z};
}

ac3adm::CartesianPosition room_to_adm_cartesian(const ac3::oba::Position& room) {
    return {.x = 2.0 * room.x - 1.0, .y = 1.0 - 2.0 * room.y, .z = room.z};
}

ac3::oba::Position iab_position_to_room(const ac3iab::Position& position) {
    // Direct passthrough - see coordinates.hpp's own comment on iab_position_to_room for why no
    // formula is needed: x/y already share oba::Position's convention exactly, and z is already
    // anchored at the same screen/ear-height zero, just never negative.
    return {.x = position.x, .y = position.y, .z = position.z};
}

ac3::oba::Position adm_position_to_room(const ac3adm::Position& position) {
    return std::visit(
        [](const auto& p) -> ac3::oba::Position {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, ac3adm::PolarPosition>) {
                return adm_cartesian_to_room(polar_to_adm_cartesian(p));
            } else {
                static_assert(std::is_same_v<T, ac3adm::CartesianPosition>);
                return adm_cartesian_to_room(p);
            }
        },
        position);
}

}  // namespace ac3::admbridge
