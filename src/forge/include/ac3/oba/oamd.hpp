#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/export.hpp"

// Object Audio Metadata - ETSI TS 103 420 clause 5. The payload that says what
// the objects ARE: how many, where each one sits in the room, how loud, and
// which of them are nailed to speakers rather than free to move.
//
// OAMD carries no audio. The essences come out of the JOC decoder, one per
// object, and OAMD is matched to them BY POSITION (§4.3 decodes the essences
// at step 3 and the properties at step 5, and never pairs them by name) - so
// object order is a wire contract, not a presentation detail. §5.6.4.8 fixes
// it: objects contained in a bed first, then ISF objects, then dynamic ones.
//
// Two channel orderings collide here and they are not the same:
//   - a bed's own order is Table 12's, read from array index 9 downwards, so
//     5.1 is L, R, C, LFE, Ls, Rs;
//   - AC-3 codes 3/2 + LFE as L, C, R, Ls, Rs, LFE (Table 5.8).
// C and R swap and the LFE moves. The JOC downmix (Table 47) agrees with the
// former, so the encoder permutes once on the way in and everything object-
// facing stays in Table 12 order.

namespace ac3::oba {

// §4.2.1's room-anchored system: left-handed and normalized to the room
// cuboid. x runs 0 at the left wall to 1 at the right, y 0 at the front wall
// to 1 at the back, z -1 at the floor to +1 at the ceiling. The centre of the
// front wall is (0.5, 0, 0).
struct Position {
    double x = 0.5;
    double y = 0.5;
    double z = 0.0;
};

// §5.6.1.2 Table 17. An object's extent along each room axis, 0 (a point
// source) to 1, quantized on the wire to 31 steps per axis. Table 17 has
// three shapes and only three: a point source, one value shared by all three
// axes, and three separate ones - so a size that is neither zero nor
// isotropic always costs the full 15 bits.
struct ObjectSize {
    double width = 0.0;
    double depth = 0.0;
    double height = 0.0;

    [[nodiscard]] constexpr bool is_point() const {
        return width == 0.0 && depth == 0.0 && height == 0.0;
    }
    [[nodiscard]] constexpr bool is_isotropic() const {
        return width == depth && width == height;
    }
};

// §5.6.1.1.15-.17: an object that sits OUTSIDE the room. The position stays
// the direction; the distance says how much further along it than the wall.
struct ObjectDistance {
    bool at_infinity = false;
    // Table 15, 1,1 to 50,1. Meaningless when at_infinity.
    double factor = 1.1;
};

// §5.6.1.6.1 Table 20. Which horizontal zones a renderer may put the object
// in; the Top-Bottom zone is separate (b_enable_elevation).
enum class ZoneConstraint : std::uint8_t {
    kNone = 0,
    kBackExcluded = 1,
    kSideExcluded = 2,
    kCentreAndBackOnly = 3,
    kScreenOnly = 4,
    kSurroundOnly = 5,
};

// An object placed in the room rather than assigned to a speaker.
//
// Every field past `gain_db` defaults to what Table 29's
// object_render_info_status_idx 0b00 ("default") and Table 28's own defaults
// say an absent block means, so a default-constructed DynamicObject is
// exactly the object OAMD describes when it says nothing at all - which is
// what makes it safe for parse_payload to return partial results.
struct DynamicObject {
    Position position{};
    // §5.6.1.4. Table 19 covers [-49, -1] and [1, 15] dB; exactly 0 dB is
    // unreachable through object_gain_bits and is sent as object_gain_idx 0
    // instead, which is why this is a dB figure rather than an index.
    double gain_db = 0.0;
    // §5.6.1.2. A point source unless told otherwise.
    ObjectSize size{};
    // §5.6.1.3, in [0, 1]. 1 is b_default_object_priority's own default.
    double priority = 1.0;
    // §5.6.1.6.
    ZoneConstraint zone = ZoneConstraint::kNone;
    bool enable_elevation = true;
    // §5.6.1.5.1 b_object_snap - ADM calls the same idea channelLock.
    bool snap = false;
    // §5.6.1.1.18-.20. screen_factor is 0 when the position is room-anchored
    // (b_object_use_screen_ref false), which is also screen_factor's own
    // "no scaling" value; depth_factor is Table 16's, 1 by default.
    bool screen_reference = false;
    double screen_factor = 0.0;
    double depth_factor = 1.0;
    // §5.6.1.1.15. Unset means "inside the room", the ordinary case.
    std::optional<ObjectDistance> distance{};
    // §5.6.4.6 b_object_not_active - the object's essence is silent for this
    // update. An inactive object carries no basic or render info at all, so
    // every other field here holds its default.
    bool active = true;
    // §5.5.14 obj_div_block, from the extended_object_element (§5.5.13).
    // ADM's objectDivergence. 0 is "no divergence", the default when no
    // extended element rides along.
    double divergence = 0.0;
};

// §5.6.1.1.4 Table 12. Each flag names one channel label or a PAIR of them, so
// a set bit is not always one channel. The array index IS the bit position,
// index 9 being the most significant of the 10-bit field.
namespace bed {

inline constexpr std::uint16_t kLR = 1 << 9;  // 2 channels
inline constexpr std::uint16_t kC = 1 << 8;
inline constexpr std::uint16_t kLfe = 1 << 7;
inline constexpr std::uint16_t kLsRs = 1 << 6;    // 2
inline constexpr std::uint16_t kLbRb = 1 << 5;    // 2
inline constexpr std::uint16_t kTflTfr = 1 << 4;  // 2
inline constexpr std::uint16_t kTslTsr = 1 << 3;  // 2
inline constexpr std::uint16_t kTblTbr = 1 << 2;  // 2
inline constexpr std::uint16_t kLwRw = 1 << 1;    // 2
inline constexpr std::uint16_t kLfe2 = 1 << 0;

inline constexpr std::uint16_t kPairs = kLR | kLsRs | kLbRb | kTflTfr | kTslTsr | kTblTbr | kLwRw;

inline constexpr std::uint16_t k51 = kLR | kC | kLfe | kLsRs;

[[nodiscard]] constexpr int channel_count(std::uint16_t assignment) {
    int count = 0;
    for (int bit = 0; bit < 10; ++bit) {
        if (assignment & (1 << bit)) {
            count += (assignment & kPairs & (1 << bit)) ? 2 : 1;
        }
    }
    return count;
}

static_assert(channel_count(k51) == 6);
static_assert(channel_count(kLfe) == 1);

}  // namespace bed

// The program: either §5.6.0.5's dynamic-object-only branch, or a bed instance
// with dynamic objects beside it.
//
// The default is dynamic-object-only with an LFE, because that is what Dolby's
// own reference streams carry - checked against the DD+ JOC test signals from
// their Online Delivery Kit, which declare object_count 16,
// b_dyn_object_only_program 1, b_lfe_present 1. It also happens to be the
// honest description of what this encoder produces: the 5.1 downmix IS the
// objects' VBAP render, so declaring those five channels as a BED as well
// would make a renderer play every object twice, once as bed and once as
// object.
//
// The branch signals no object count of its own - object_count above it covers
// the whole program - and says nothing about where the LFE falls in the object
// order. Dolby's streams settle the count question: joc_num_objects is 15
// against an object_count of 16, so exactly one object, the LFE, is not a JOC
// output. They do not settle the ordering, and this encoder puts the LFE
// first, following §5.6.4.8's rule that speaker-anchored objects precede
// dynamic ones.
//
// A bed program can carry MORE than one bed instance (§5.6.0.9), and each
// instance can name its channels through Table 13's 17-flag array instead of
// Table 12's 10-flag one, so `bed` alone cannot describe every legal
// program. It stays the shorthand for the single standard instance that
// build_payload writes and that every caller in this repo constructs;
// `extra_beds` carries instances 2..N when a decoded stream has them, and
// `nonstd_bed` replaces `bed` when instance 1 used Table 13.
struct Program {
    bool dynamic_only = true;
    bool lfe = true;        // b_lfe_present, dynamic_only branch only
    std::uint16_t bed = 0;  // first bed instance, Table 12, when !dynamic_only
    int dynamic_objects = 0;

    // --- decode-side detail; all inert for a program this encoder writes ---
    // §5.6.0.8. A renderer may spread a bed channel over several speakers.
    // Dolby's own DD+ JOC encoder sets this on channel-based immersive input.
    bool bed_chan_distribute = false;
    // §5.6.1.1.5 Table 13, when the first instance is not b_standard_chan_assign.
    // Non-zero exactly when `bed` is not the authority for instance 1.
    std::uint32_t nonstd_bed = 0;
    // Bed instances after the first. Each entry is a Table 12 assignment; a
    // non-standard instance is carried by its channel count alone, in
    // `extra_bed_channels`, since Table 13 does not fit 16 bits.
    std::vector<std::uint16_t> extra_beds{};
    int extra_bed_channels = 0;
    // §5.6.0.11 Table 11b, -1 when the program carries no ISF objects.
    int isf_idx = -1;
};

// §5.6.1.1.5 Table 13: 17 individual channel labels, one flag each, so the
// count is just a popcount - unlike Table 12, where a set bit can name a pair.
[[nodiscard]] constexpr int nonstd_bed_channel_count(std::uint32_t assignment) {
    int count = 0;
    for (int bit = 0; bit < 17; ++bit) {
        count += (assignment >> bit) & 1u;
    }
    return count;
}

// Table 11b: how many objects each intermediate spatial format contributes.
// Indices 6 and 7 are reserved and contribute nothing this parser can count.
[[nodiscard]] constexpr int isf_object_count(int isf_idx) {
    constexpr std::array<int, 6> kCounts = {4, 8, 10, 14, 15, 30};
    return (isf_idx >= 0 && isf_idx < 6) ? kCounts[static_cast<std::size_t>(isf_idx)] : 0;
}

// Channels in every bed instance the program declares.
[[nodiscard]] constexpr int bed_channel_count(const Program& program) {
    if (program.dynamic_only) {
        return program.lfe ? 1 : 0;
    }
    int count = program.nonstd_bed != 0 ? nonstd_bed_channel_count(program.nonstd_bed)
                                        : bed::channel_count(program.bed);
    for (const auto extra : program.extra_beds) {
        count += bed::channel_count(extra);
    }
    return count + program.extra_bed_channels;
}

// Whether the program carries an LFE, by either route. Only the first
// instance is consulted for a bed program: §6.3.2.2's bypass is about the one
// LFE the downmix has, and a second instance's LFE would be a second one.
[[nodiscard]] constexpr bool has_lfe(const Program& program) {
    if (program.dynamic_only) {
        return program.lfe;
    }
    return program.nonstd_bed != 0 ? (program.nonstd_bed & (1u << 13)) != 0
                                   : (program.bed & bed::kLfe) != 0;
}

// Objects in the program, bed then ISF then dynamic (§5.6.4.8). This is
// object_count in the payload and complexity_index_type_a in addbsi
// (TS 103 420 §8.3.2.2).
[[nodiscard]] constexpr int object_count(const Program& program) {
    return bed_channel_count(program) + isf_object_count(program.isf_idx) +
           program.dynamic_objects;
}

// Objects the JOC tool has to reconstruct. §6.3.2.2's note bypasses the LFE
// rather than matrixing it, so it costs no JOC output even though it is an
// object like any other.
[[nodiscard]] constexpr int joc_object_count(const Program& program) {
    return object_count(program) - (has_lfe(program) ? 1 : 0) -
           (!program.dynamic_only && (program.bed & bed::kLfe2) ? 1 : 0);
}

// Which program object each JOC output reconstructs, as an index into the
// payload's own object order (bed channels, then ISF, then dynamic objects -
// §5.6.4.8). The result has joc_object_count(program) entries, and it is the
// LFE positions that are missing from it, since §6.3.2.2 bypasses them.
//
// For a dynamic-object-only program with an LFE - the only shape AtmosEncoder
// produces - this is {1, 2, ... }, so JOC output i is dynamic object i and
// the identity every caller already assumed still holds. For a bed program it
// is what says which bed channel came back.
[[nodiscard]] AC3FORGE_EXPORT std::vector<int> joc_object_indices(const Program& program);

// The Table 12 channel labels a bed instance's assignment names, in the order
// its channels occupy in the payload's object list - the order
// joc_object_indices() indexes into. One entry per channel, so a set pair bit
// contributes two.
//
// §5.6.1.1.4 says the list is "ordered like the bed_channel_assignment[]
// array elements, starting with bit 0", which read literally puts LFE2 and
// the wides ahead of L/R. Every real bed disagrees: this file's own header
// comment records the descending reading (index 9 down, so 5.1 is
// L, R, C, LFE, Ls, Rs), and a DD+ JOC stream from the Dolby Encoding Engine
// confirms it - reconstructing its eleven JOC objects and identifying each by
// the tone that went into that speaker recovers the descending order and not
// the ascending one (tests/oba/test_dee_joc_fixture.cpp).
enum class BedLabel : std::uint8_t {
    kL, kR, kC, kLfe, kLs, kRs, kLb, kRb, kTfl, kTfr, kTsl, kTsr, kTbl, kTbr, kLw, kRw, kLfe2,
};

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(BedLabel label);

// Channel labels of the program's first bed instance, in payload order.
// Empty for a dynamic-object-only program and for a non-standard assignment.
[[nodiscard]] AC3FORGE_EXPORT std::vector<BedLabel> bed_labels(std::uint16_t assignment);

// Where a bed channel's loudspeaker nominally sits in §4.2.1's room cuboid.
//
// This is NOT transmitted: a bed channel is speaker-anchored precisely so that
// no position has to be, and a renderer places it from the label alone. What
// this returns is the room-anchored position that label MEANS - front wall at
// y = 0, ceiling at z = +1, sides at x = 0 and 1, with the surround pair
// halfway back and the back pair at the rear wall, which is the layout the
// labels are named for. It exists so a view that draws objects in a room has
// somewhere to draw a bed channel, and nothing in encode or decode reads it.
[[nodiscard]] AC3FORGE_EXPORT Position bed_label_position(BedLabel label);

// One object_audio_metadata_payload (§5.5.2), padded to whole bytes because
// emdf_payload_size counts bytes. `objects` describes the dynamic objects in
// order; the bed's are implied by the channel assignment and are sent at unity
// gain and default priority.
//
// object_count(program) must be in [1, 31]. §5.5.2 has an escape for larger
// counts - object_count_bits 0x1F plus a 7-bit extension - and it is not
// implemented, because two other clauses forbid ever needing it: §8.3.2.2 caps
// complexity_index_type_a at 16 objects and §6.3.2.4 caps joc_num_objects at
// the same. A stream that got past this would be rejected by the frame writer
// (FrameError::kInvalidObjectAudio) before any of it reached a file.
[[nodiscard]] AC3FORGE_EXPORT std::vector<std::byte> build_payload(
    const Program& program, std::span<const DynamicObject> objects);

// --- Decode ------------------------------------------------------------

// One md_update_info/block_update_info pair (§5.5.6, §5.5.7) and the object
// state it establishes. A frame can carry several, each taking effect at its
// own offset into the frame - which is how an object moves faster than one
// position per frame.
struct UpdateBlock {
    // §5.6.2.1 Table 22/23, in samples from the frame's first.
    int sample_offset = 0;
    // §5.6.2.5, the raw 6-bit field. §5.3 turns it into a sample offset
    // against the block count, which is program-dependent, so it is left as
    // the codeword rather than guessed into samples here.
    int block_offset_factor = 0;
    // §5.6.2.6 Table 24/25, in samples. -1 for the one ramp_duration_bits
    // value the tables do not name.
    int ramp_duration = 1536;
    // Dynamic objects only, in payload order - the bed's and ISF's are
    // speaker- and format-anchored and carry no render info of their own.
    std::vector<DynamicObject> objects;
};

// §5.5.12 / §5.6.5, one trim configuration's worth. `centre`/`surround`/
// `height` are Tables 35-37's codewords and the balance pairs are
// §5.6.5.9-.12's sign/amount; all are left as codewords because applying
// them is a renderer's job, not a decoder's.
struct TrimConfig {
    bool default_trim = true;
    bool disabled = false;
    std::optional<int> centre{};
    std::optional<int> surround{};
    std::optional<int> height{};
    std::optional<std::pair<bool, int>> balance_top_bottom{};
    std::optional<std::pair<bool, int>> balance_listener{};
};

// §5.5.12's trim_element (oa_element_id_idx 2) - what a renderer should do to
// the mix when it folds this program down to a smaller layout.
//
// NUM_TRIM_CONFIGS is used by §5.5.12's own pseudocode but never given a
// value anywhere in TS 103 420. Six is what a real stream proves: a DD+ JOC
// trim_element from the Dolby Encoding Engine parses to exactly its declared
// oa_element_size with six configurations and to a byte-count mismatch with
// any other number.
inline constexpr int kNumTrimConfigs = 6;

struct TrimElement {
    int warp_mode = 0;         // Table 32
    int global_trim_mode = 0;  // Table 33
    std::vector<TrimConfig> configs{};
    // §5.5.12's b_disable_trim_per_obj tail, one flag per object in payload
    // order (bed and ISF included), empty when the flag was not set.
    std::vector<bool> disable_per_object{};
};

// What one object_audio_metadata_payload actually describes.
//
// `objects` is the FIRST update block's dynamic objects, in the same
// bed/ISF-then-dynamic order build_payload's own `objects` parameter takes,
// so `objects.size() == program.dynamic_objects` always - the same shape
// this struct has always had, and what a caller that does not care about
// intra-frame motion should read. `blocks` is every update block including
// that one.
struct DecodedProgram {
    Program program;
    std::vector<DynamicObject> objects;
    std::vector<UpdateBlock> blocks;
    // §5.5.12, when a trim_element rode along.
    std::optional<TrimElement> trim{};
    // oa_element_id_idx values that were present but skipped by their own
    // oa_element_size rather than interpreted - Table 26's reserved ids, and
    // any future element this parser does not know. The payload is still
    // returned: §5.6.4.3 exists precisely so an unknown element costs a
    // decoder nothing but a seek.
    std::vector<int> skipped_elements{};
};

// Decode-side inverse of build_payload(), and rather more: it reads the
// whole of §5.5's object_element and trim_element, not just the corner this
// encoder writes.
//
// Understood: several md_update blocks at any sample offset and ramp
// duration; object size, zone constraints, elevation gating, snap, screen
// reference, distance, explicit priority, non-default gain including
// Table 18's "reuse the previous object's"; differential positions in blocks
// after the first; inactive objects; several bed instances, standard or
// Table 13 non-standard, with or without bed channel distribution; ISF
// programs; alternate object data; additional_table_data and reserved_data
// (skipped by their own size fields); the trim_element; and any number of
// oa_elements, with unrecognised ids skipped by oa_element_size.
//
// Partial rather than nothing: an element this parser cannot interpret is
// skipped and named in `skipped_elements`, and every object field that was
// not transmitted keeps DynamicObject's own default, which is exactly what
// Tables 28 and 29 say an absent block means.
//
// std::nullopt is left for the cases where the bits genuinely cannot be
// walked: an oa_md_version other than 0 (§5.6.0.1 gives no syntax for
// another), a reserved content_description[0], an ISF index of 6 or 7
// (Table 11b names no object count, so the bed/ISF/dynamic split is
// unknowable), an object count that disagrees with the program that was just
// described, an element whose contents do not end where its own
// oa_element_size says, or a read past the end of the payload.
[[nodiscard]] AC3FORGE_EXPORT std::optional<DecodedProgram> parse_payload(
    std::span<const std::byte> payload);

// One JOC output as something that draws objects in a room needs it: where
// it is, how big, how loud and what it is called.
//
// A dynamic object supplies all of that itself. A bed channel supplies none
// of it - it is anchored to a speaker, so its position comes from its label
// (bed_label_position) and its extent is a point by definition. `label` is
// empty for a dynamic object, which has an index and no name.
struct DisplayObject {
    Position position{};
    ObjectSize size{};
    double gain_db = 0.0;
    bool snap = false;
    bool active = true;
    std::string_view label{};
};

// The program's objects as a view sees them, one entry per JOC output and in
// joc_object_indices() order - so parallel to DecodedSubstream::object_audio
// for both a dynamic-object-only program and a bed one. `block` selects which
// metadata update block to read; out-of-range clamps to the last.
[[nodiscard]] AC3FORGE_EXPORT std::vector<DisplayObject> describe_objects(
    const DecodedProgram& program, std::size_t block = 0);

}  // namespace ac3::oba
