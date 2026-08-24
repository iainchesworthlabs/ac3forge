#include "ac3/oba/oamd.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"

namespace ac3::oba {

namespace {

// §5.5.1 variable_bits_max(n, max_num_groups). Same construction as EMDF's
// variable_bits - groups of n bits, each followed by read_more, with the
// group offset folded in - but the group count is capped, so the last group
// carries no read_more of any consequence and the field cannot run away.
void put_variable_bits_max(BitWriter& w, std::uint32_t value, int group_bits,
                           int max_groups) {
    int groups = 1;
    std::uint64_t offset = 0;
    while (groups < max_groups) {
        const std::uint64_t capacity = std::uint64_t{1} << (groups * group_bits);
        if (value < offset + capacity) {
            break;
        }
        offset += capacity;
        ++groups;
    }
    const auto encoded = static_cast<std::uint64_t>(value) - offset;
    for (int group = groups - 1; group >= 0; --group) {
        w.put(static_cast<std::uint32_t>((encoded >> (group * group_bits)) &
                                         ((std::uint64_t{1} << group_bits) - 1)),
              group_bits);
        w.put(group == 0 ? 0u : 1u, 1);  // read_more
    }
}

// The flag arrays in §5.6 are transmitted with array index 0 FIRST, so element
// n lands at bit (width - 1 - n) of the field, not at bit n.
//
// The tables read the other way round - Table 11a numbers the bed flag 3 and
// Table 12 numbers L/R 9 - which is what made the opposite reading look
// natural. Dolby's own encoder settles it: for a 7.1.4 input it writes
// bed_channel_assignment 0b0010111111, and only index-0-first turns that into
// L/R, C, LFE, Ls/Rs, Lb/Rb, Tfl/Tfr, Tbl/Tbr - twelve channels, which is
// exactly the object_count of 12 it declares in the same payload. Under the
// other reading the same bits name a bed with no left, right or centre.
[[nodiscard]] std::uint32_t flags_msb_first(std::uint32_t flags, int width) {
    std::uint32_t out = 0;
    for (int index = 0; index < width; ++index) {
        if (flags & (1u << index)) {
            out |= 1u << (width - 1 - index);
        }
    }
    return out;
}

// §5.6.1.1.8 and §5.6.1.1.9: x and y are the 6-bit field over 62, so 62 - not
// 63 - is the far wall and the two codes above it also mean "at the wall".
// Sending 63 would be legal and would still decode to 1 through the min(), but
// it wastes the only two codes a future extension could use.
[[nodiscard]] std::uint32_t quantize_xy(double value) {
    const double clamped = std::clamp(value, 0.0, 1.0);
    return static_cast<std::uint32_t>(std::lround(clamped * 62.0));
}

// §5.6.1.1.10 / §5.6.1.1.11: z is a sign bit and a 4-bit magnitude over 15.
// pos3D_Z_sign_bits == 0 means negative, so the floor is the zero code.
void put_z(BitWriter& w, double value) {
    const double clamped = std::clamp(value, -1.0, 1.0);
    w.put(clamped < 0.0 ? 0u : 1u, 1);
    w.put(static_cast<std::uint32_t>(std::lround(std::abs(clamped) * 15.0)), 4);
}

// §5.6.1.4, Table 18 and Table 19. The two ranges are not contiguous and the
// break is not where it looks: codes 0..14 mean 15 - code dB (so +15 down to
// +1) and codes 15..63 mean 14 - code dB (so -1 down to -49). Exactly 0 dB
// falls in neither, which is what object_gain_idx 0 is for.
void put_gain(BitWriter& w, double gain_db) {
    const long rounded = std::lround(gain_db);
    if (rounded == 0) {
        w.put(0b00, 2);  // object_gain_idx: 0 dB
        return;
    }
    w.put(0b10, 2);  // object_gain_idx: an explicit object_gain_bits follows
    const long clamped = std::clamp(rounded, -49L, 15L);
    const long code = clamped > 0 ? 15 - clamped : 14 - clamped;
    w.put(static_cast<std::uint32_t>(std::clamp(code, 0L, 63L)), 6);
}

// §5.6.1.3.2: object_priority = object_priority_bits / 32, with 1,0 reached
// only through b_default_object_priority. So the 5-bit field spans [0; 1),
// and a priority that rounds to 1 is the default flag, not code 32.
void put_priority(BitWriter& w, double priority) {
    const long code = std::lround(std::clamp(priority, 0.0, 1.0) * 32.0);
    if (code >= 32) {
        w.put(1, 1);  // b_default_object_priority
        return;
    }
    w.put(0, 1);
    w.put(static_cast<std::uint32_t>(code), 5);
}

// §5.6.1.2, Table 17. Three shapes and no more: zero, one value on all three
// axes, or three separate ones. Each axis is the field over 31.
void put_size(BitWriter& w, const ObjectSize& size) {
    const auto code = [](double value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0, 1.0) * 31.0));
    };
    if (size.is_point()) {
        w.put(0b00, 2);
        return;
    }
    if (size.is_isotropic()) {
        w.put(0b01, 2);
        w.put(code(size.width), 5);
        return;
    }
    w.put(0b10, 2);
    w.put(code(size.width), 5);
    w.put(code(size.depth), 5);
    w.put(code(size.height), 5);
}

// §5.5.9 object_info_block for the single update block this encoder sends.
// Everything that block 0 implies is implied here too, so nothing that the
// status indices would have carried is written: §5.5.9 fixes
// object_basic_info_status_idx to 0b01 for blk == 0, and §5.5.10 turns that
// into "both fields present" without a bit on the wire.
void put_object_info_block(BitWriter& w, const DynamicObject* dynamic) {
    w.put(0, 1);  // b_object_not_active: every object carries essence

    // object_basic_info(), status 0b01 => object_basic_info[] = {true, true}.
    put_gain(w, dynamic ? dynamic->gain_db : 0.0);
    put_priority(w, dynamic ? dynamic->priority : 1.0);

    // §5.5.9 again: an object in a bed has its position from its speaker
    // label, so object_render_info_status_idx is forced to 0b00 and the whole
    // block is absent. Only dynamic objects describe themselves.
    if (dynamic != nullptr) {
        // object_render_info(), status 0b01 => obj_render_info[] all true.
        // blk == 0 forces b_differential_position_specified to FALSE, and it
        // is implied rather than transmitted.
        w.put(quantize_xy(dynamic->position.x), 6);
        w.put(quantize_xy(dynamic->position.y), 6);
        put_z(w, dynamic->position.z);
        w.put(0, 1);  // b_object_distance_specified: inside the room

        w.put(static_cast<std::uint32_t>(dynamic->zone) & 0x7u, 3);  // zone_constraints_idx
        w.put(dynamic->enable_elevation ? 1u : 0u, 1);               // b_enable_elevation

        put_size(w, dynamic->size);

        w.put(0, 1);  // b_object_use_screen_ref: room-anchored, not screen
        w.put(dynamic->snap ? 1u : 0u, 1);  // b_object_snap (ADM channelLock)
    }

    w.put(0, 1);  // b_additional_table_data_exists
}

// §5.5.5 object_element. Written into whatever writer it is handed, so it can
// be emitted once into a probe to measure it and once into the payload for
// real - the element's bits do not start on a byte boundary, so they cannot be
// copied through a byte buffer without picking up padding that is not theirs.
void put_object_element(BitWriter& w, const Program& program,
                        std::span<const DynamicObject> objects) {
    // Objects that are anchored rather than placed: the LFE in the
    // dynamic-only branch, or the whole bed instance otherwise. They come
    // first (§5.6.4.8) and carry no render info.
    const int anchored = object_count(program) - program.dynamic_objects;
    const int total = object_count(program);

    // md_update_info (§5.5.6). One update per frame, aligned to its first
    // sample: sample_offset_code 0 is sample_offset 0, and
    // num_obj_info_blocks_bits 0 is a single block.
    w.put(0b00, 2);  // sample_offset_code
    w.put(0, 3);     // num_obj_info_blocks_bits => 1 block
    // block_update_info (§5.5.7).
    w.put(0, 6);     // block_offset_factor_bits: the update starts the frame
    // Table 24 code 2 is a 1 536-sample ramp - exactly one frame - so object
    // properties interpolate across the frame instead of stepping at its edge.
    // Code 0 would be a jump, and a moving object would zipper.
    w.put(0b10, 2);  // ramp_duration_code

    w.put(1, 1);  // b_reserved_data_not_present

    for (int object = 0; object < total; ++object) {
        const bool is_anchored = object < anchored;
        put_object_info_block(
            w, is_anchored ? nullptr
                           : &objects[static_cast<std::size_t>(object - anchored)]);
    }
}

// Decode-side inverse of put_variable_bits_max: same shape as the encoder's,
// with the same group-count ceiling so a stream that never sends a 0
// read_more bit still terminates.
[[nodiscard]] std::uint32_t read_variable_bits_max(BitReader& r, int group_bits, int max_groups) {
    std::uint32_t value = 0;
    for (int group = 1;; ++group) {
        value += r.read(group_bits);
        const bool read_more = r.read_bit() != 0;
        if (!read_more || group >= max_groups) {
            return value;
        }
        value <<= group_bits;
        value += 1u << group_bits;
    }
}

}  // namespace

std::vector<BedLabel> bed_labels(std::uint16_t assignment) {
    // Table 12 read from array index 9 downwards - see bed_labels()'s own
    // declaration for why that direction and not §5.6.1.1.4's literal one.
    struct Entry {
        std::uint16_t flag;
        BedLabel first;
        BedLabel second;
        bool pair;
    };
    static constexpr std::array<Entry, 10> kOrder = {{
        {bed::kLR, BedLabel::kL, BedLabel::kR, true},
        {bed::kC, BedLabel::kC, BedLabel::kC, false},
        {bed::kLfe, BedLabel::kLfe, BedLabel::kLfe, false},
        {bed::kLsRs, BedLabel::kLs, BedLabel::kRs, true},
        {bed::kLbRb, BedLabel::kLb, BedLabel::kRb, true},
        {bed::kTflTfr, BedLabel::kTfl, BedLabel::kTfr, true},
        {bed::kTslTsr, BedLabel::kTsl, BedLabel::kTsr, true},
        {bed::kTblTbr, BedLabel::kTbl, BedLabel::kTbr, true},
        {bed::kLwRw, BedLabel::kLw, BedLabel::kRw, true},
        {bed::kLfe2, BedLabel::kLfe2, BedLabel::kLfe2, false},
    }};
    std::vector<BedLabel> labels;
    for (const auto& entry : kOrder) {
        if ((assignment & entry.flag) == 0) {
            continue;
        }
        labels.push_back(entry.first);
        if (entry.pair) {
            labels.push_back(entry.second);
        }
    }
    return labels;
}

std::string_view describe(BedLabel label) {
    switch (label) {
        case BedLabel::kL: return "L";
        case BedLabel::kR: return "R";
        case BedLabel::kC: return "C";
        case BedLabel::kLfe: return "LFE";
        case BedLabel::kLs: return "Ls";
        case BedLabel::kRs: return "Rs";
        case BedLabel::kLb: return "Lb";
        case BedLabel::kRb: return "Rb";
        case BedLabel::kTfl: return "Tfl";
        case BedLabel::kTfr: return "Tfr";
        case BedLabel::kTsl: return "Tsl";
        case BedLabel::kTsr: return "Tsr";
        case BedLabel::kTbl: return "Tbl";
        case BedLabel::kTbr: return "Tbr";
        case BedLabel::kLw: return "Lw";
        case BedLabel::kRw: return "Rw";
        case BedLabel::kLfe2: return "LFE2";
    }
    return "?";
}

Position bed_label_position(BedLabel label) {
    // x: 0 left wall, 1 right. y: 0 front wall, 1 back. z: -1 floor, +1 ceiling.
    switch (label) {
        case BedLabel::kL:    return {.x = 0.0, .y = 0.0, .z = 0.0};
        case BedLabel::kR:    return {.x = 1.0, .y = 0.0, .z = 0.0};
        // The two LFEs share the centre's coordinates deliberately: an LFE
        // has no direction to point at (see atmos.hpp), so the front centre
        // is a placeholder for a view, not a claim about where it sits.
        case BedLabel::kC:
        case BedLabel::kLfe:
        case BedLabel::kLfe2: return {.x = 0.5, .y = 0.0, .z = 0.0};
        case BedLabel::kLs:   return {.x = 0.0, .y = 0.5, .z = 0.0};
        case BedLabel::kRs:   return {.x = 1.0, .y = 0.5, .z = 0.0};
        case BedLabel::kLb:   return {.x = 0.0, .y = 1.0, .z = 0.0};
        case BedLabel::kRb:   return {.x = 1.0, .y = 1.0, .z = 0.0};
        case BedLabel::kLw:   return {.x = 0.0, .y = 0.25, .z = 0.0};
        case BedLabel::kRw:   return {.x = 1.0, .y = 0.25, .z = 0.0};
        case BedLabel::kTfl:  return {.x = 0.0, .y = 0.0, .z = 1.0};
        case BedLabel::kTfr:  return {.x = 1.0, .y = 0.0, .z = 1.0};
        case BedLabel::kTsl:  return {.x = 0.0, .y = 0.5, .z = 1.0};
        case BedLabel::kTsr:  return {.x = 1.0, .y = 0.5, .z = 1.0};
        case BedLabel::kTbl:  return {.x = 0.0, .y = 1.0, .z = 1.0};
        case BedLabel::kTbr:  return {.x = 1.0, .y = 1.0, .z = 1.0};
    }
    return {};
}

std::vector<int> joc_object_indices(const Program& program) {
    std::vector<int> indices;
    const int total = object_count(program);
    indices.reserve(static_cast<std::size_t>(total));
    if (program.dynamic_only) {
        // The LFE, when present, is object 0 - build_payload's own ordering,
        // following §5.6.4.8's speaker-anchored-first rule.
        for (int object = program.lfe ? 1 : 0; object < total; ++object) {
            indices.push_back(object);
        }
        return indices;
    }
    // A bed program: walk the first instance's labels, dropping its LFEs, then
    // take every extra instance's channels and every dynamic object whole.
    // Extra instances and non-standard assignments are counted rather than
    // labelled, so their LFEs (if any) are not identifiable and stay in.
    int object = 0;
    for (const auto label : bed_labels(program.bed)) {
        if (label != BedLabel::kLfe && label != BedLabel::kLfe2) {
            indices.push_back(object);
        }
        ++object;
    }
    for (; object < total; ++object) {
        indices.push_back(object);
    }
    return indices;
}

std::vector<std::byte> build_payload(const Program& program,
                                     std::span<const DynamicObject> objects) {
    assert(static_cast<int>(objects.size()) == program.dynamic_objects);
    // The writer covers one standard bed instance and no ISF, which is every
    // program this project produces. parse_payload reads the wider shapes;
    // this side would silently drop them, so it refuses them instead.
    assert(program.extra_beds.empty() && program.extra_bed_channels == 0 &&
           program.nonstd_bed == 0 && program.isf_idx < 0);
    const int total = object_count(program);

    // §5.6.4.3: oa_element_size counts b_discard_unknown_element, the element
    // and its padding, so the flag bit is inside the measurement and the
    // padding is whatever rounds the three of them up to whole bytes.
    const std::size_t element_bits = [&] {
        BitWriter probe;
        put_object_element(probe, program, objects);
        return probe.bit_count() + 1;
    }();
    const auto element_bytes = static_cast<std::uint32_t>((element_bits + 7) / 8);
    const std::size_t element_padding = element_bytes * 8 - element_bits;

    BitWriter w;
    // --- object_audio_metadata_payload (§5.5.2) ---
    w.put(0, 2);  // oa_md_version_bits
    assert(total >= 1 && total <= 31);
    w.put(static_cast<std::uint32_t>(total - 1), 5);  // object_count_bits

    // --- program_assignment (§5.5.3) ---
    if (program.dynamic_only) {
        // The whole branch is two bits. The object count is object_count
        // above; the number of dynamic objects is what is left after the LFE.
        w.put(1, 1);  // b_dyn_object_only_program
        w.put(program.lfe ? 1 : 0, 1);  // b_lfe_present
    } else {
        w.put(0, 1);
        // content_description[] (Table 11a): element 3 a bed instance, 2 ISF,
        // 1 dynamic objects, 0 reserved - written index 0 first, so the bed
        // flag is the LAST bit of the four.
        const std::uint32_t content = (program.bed != 0 ? 1u << 3 : 0u) |
                                      (program.dynamic_objects > 0 ? 1u << 1 : 0u);
        w.put(flags_msb_first(content, 4), 4);
        if (program.bed != 0) {
            w.put(program.bed_chan_distribute ? 1u : 0u, 1);  // b_bed_chan_distribute
            w.put(0, 1);  // b_multiple_bed_instances_present => one instance
            // §5.6.1.1.6: b_lfe_only is not a shorthand for the assignment, it
            // REPLACES it - the 10-bit field is absent entirely.
            const bool lfe_only = program.bed == bed::kLfe;
            w.put(lfe_only ? 1 : 0, 1);  // b_lfe_only
            if (!lfe_only) {
                w.put(1, 1);  // b_standard_chan_assign
                w.put(flags_msb_first(program.bed, 10), 10);
            }
        }
        if (program.dynamic_objects > 0) {
            w.put(static_cast<std::uint32_t>(program.dynamic_objects - 1), 5);
        }
    }

    w.put(0, 1);  // b_alternate_object_data_present
    w.put(1, 4);  // oa_element_count_bits: one element, the object_element

    // --- oa_element_md (§5.5.4) ---
    w.put(1, 4);  // oa_element_id_idx: object_element (Table 26)
    put_variable_bits_max(w, element_bytes - 1, 4, 4);  // oa_element_size_bits
    // A decoder that does not know this element can skip it by its size, so
    // there is no reason to make it throw the payload away.
    w.put(0, 1);  // b_discard_unknown_element
    put_object_element(w, program, objects);
    for (std::size_t bit = 0; bit < element_padding; ++bit) {
        w.put(0, 1);  // §5.6.4.14 padding: zero bits, counted by the size
    }

    // §5.5.2's own trailing padding. It is NOT the same as the element's: the
    // element is a whole number of bytes measured from a bit offset that is
    // not itself byte-aligned, so the payload still has a few bits to go.
    return w.take();
}


namespace {

// §5.6.1.1.8/.9's inverse. The field is over 62 and the two codes above it
// also mean "at the wall", which is what the min() is for.
//
// std::min<std::uint32_t>, not deduced: `code` is std::uint32_t, which on a
// 32-bit target (arm-none-eabi, where the minimum-footprint decoder profile
// runs) is `unsigned long` while 62u is `unsigned int` - two different types,
// so deduction fails outright. Same everywhere else this file and
// core/bitalloc.cpp pin a std::min/std::max argument type.
[[nodiscard]] double xy_from_code(std::uint32_t code) {
    return static_cast<double>(std::min<std::uint32_t>(code, 62u)) / 62.0;
}

// §5.6.1.4 Table 19's inverse - see put_gain's own comment on why the two
// ranges are not contiguous.
[[nodiscard]] double gain_from_bits(std::uint32_t code) {
    return code <= 14 ? static_cast<double>(15 - code)
                      : static_cast<double>(14 - static_cast<int>(code));
}

// Table 25's ramp_duration_idx.
[[nodiscard]] int ramp_duration_from_idx(std::uint32_t idx) {
    constexpr std::array<int, 16> kRamps = {32,   64,   128,  256,  320,  480,  1000, 1001,
                                            1024, 1600, 1601, 1602, 1920, 2000, 2002, 2048};
    return kRamps[idx & 0xFu];
}

// Table 15's distance_factor.
[[nodiscard]] double distance_factor_from_idx(std::uint32_t idx) {
    constexpr std::array<double, 16> kFactors = {1.1,  1.3,  1.6,  2.0,  2.5,  3.2,  4.0,  5.0,
                                                 6.3,  7.9,  10.0, 12.6, 15.8, 20.0, 25.1, 50.1};
    return kFactors[idx & 0xFu];
}

// Table 16's depth_factor.
[[nodiscard]] double depth_factor_from_idx(std::uint32_t idx) {
    constexpr std::array<double, 4> kFactors = {0.25, 0.5, 1.0, 2.0};
    return kFactors[idx & 3u];
}

// Everything one object_element() needs that lives outside it: how many
// objects there are and which of them are speaker- or format-anchored.
struct ObjectLayout {
    int total = 0;
    int anchored = 0;  // bed channels plus ISF objects, always first (§5.6.4.8)
};

// §5.5.9 object_info_block, in full.
//
// `object` is the state this block updates: Table 28 "full reuse" and
// Table 29 "reuse" both mean "whatever the previous metadata update said",
// and blocks after the first may also code their position differentially
// against it. `previous_gain_db` is separate because Table 18 object_gain_idx
// 3 reuses the gain of the PREVIOUS OBJECT in this same block, not this
// object's own previous value.
//
// Returns false only on a read this parser cannot walk past.
[[nodiscard]] bool read_object_info_block(BitReader& r, int blk, bool in_bed_or_isf,
                                          DynamicObject& object, double& previous_gain_db) {
    const bool not_active = r.read(1) != 0;  // b_object_not_active
    object.active = !not_active;

    // §5.5.9: blk 0 forces status 0b01 ("full update") without a bit on the
    // wire; an inactive object forces 0b00 ("default") and carries nothing.
    const std::uint32_t basic_status = not_active ? 0u : (blk == 0 ? 1u : r.read(2));
    if (basic_status == 0) {
        object.gain_db = 0.0;
        object.priority = 1.0;
    }
    if (basic_status == 1 || basic_status == 3) {
        // §5.5.10 Table 30, index 0 first: index 1 is object_gain_idx and
        // index 0 is b_default_object_priority, so the priority flag is the
        // one transmitted FIRST. Status 0b01 implies both present.
        const std::uint32_t present = basic_status == 1 ? 0b11u : flags_msb_first(r.read(2), 2);
        if ((present & 0b10u) != 0) {
            const auto gain_idx = r.read(2);  // Table 18
            switch (gain_idx) {
                case 0b00:
                    object.gain_db = 0.0;
                    break;
                case 0b01:
                    // Table 18 calls this -inf dB. The nearest thing a finite
                    // dB figure can say is Table 19 own floor, and `active`
                    // already carries the silence.
                    object.gain_db = -49.0;
                    object.active = false;
                    break;
                case 0b10:
                    object.gain_db = gain_from_bits(r.read(6));
                    break;
                default:  // 0b11: the previous object gain in this block
                    object.gain_db = previous_gain_db;
                    break;
            }
            previous_gain_db = object.gain_db;
        }
        if ((present & 0b01u) != 0) {
            if (r.read(1) != 0) {  // b_default_object_priority
                object.priority = 1.0;
            } else {
                object.priority = static_cast<double>(r.read(5)) / 32.0;
            }
        }
    }

    // §5.5.9: an object in a bed, or an ISF object, takes its position from
    // its speaker label or its format, so its render info is forced absent.
    const std::uint32_t render_status =
        (not_active || in_bed_or_isf) ? 0u : (blk == 0 ? 1u : r.read(2));
    if (render_status == 0 && !in_bed_or_isf) {
        // Table 29 "default": every render value reverts, and only the basic
        // ones survive.
        object = DynamicObject{
            .gain_db = object.gain_db, .priority = object.priority, .active = object.active};
    }
    if (render_status == 1 || render_status == 3) {
        // §5.5.11 Table 31, index 0 first. The table and the §5.5.11
        // pseudocode number this array in opposite directions - the table
        // counts 3..0 down its rows in syntax order (position, zone, size,
        // screen), exactly as Tables 11a and 30 do for their own arrays,
        // while the four pseudocode ifs read [0]..[3] up. Tables 11a and 30
        // both agree with THEIR pseudocode, so the §5.5.11 ascending indices
        // are the outlier and Table 31 numbering is what is followed here:
        // the screen presence bit first on the wire, position last.
        //
        // Only object_render_info_status_idx 0b11 ever reaches this array;
        // 0b01 implies all four, which is what this encoder and every DEE
        // stream checked against it actually send, so the disagreement has no
        // reachable consequence for either.
        const std::uint32_t present = render_status == 1 ? 0b1111u : flags_msb_first(r.read(4), 4);
        constexpr std::uint32_t kScreen = 1u << 0;
        constexpr std::uint32_t kSize = 1u << 1;
        constexpr std::uint32_t kZone = 1u << 2;
        constexpr std::uint32_t kPosition = 1u << 3;

        if ((present & kPosition) != 0) {
            const bool differential = blk != 0 && r.read(1) != 0;
            if (differential) {
                // §5.6.1.1.12-.14: three signed 3-bit steps against the
                // previous update quantized position, in the same 1/62, 1/62,
                // 1/15 units the absolute form uses.
                const auto signed3 = [](std::uint32_t bits) {
                    return static_cast<int>(bits) - ((bits & 0b100u) != 0 ? 8 : 0);
                };
                const int dx = signed3(r.read(3));
                const int dy = signed3(r.read(3));
                const int dz = signed3(r.read(3));
                object.position.x =
                    std::clamp(object.position.x + static_cast<double>(dx) / 62.0, 0.0, 1.0);
                object.position.y =
                    std::clamp(object.position.y + static_cast<double>(dy) / 62.0, 0.0, 1.0);
                object.position.z =
                    std::clamp(object.position.z + static_cast<double>(dz) / 15.0, -1.0, 1.0);
            } else {
                object.position.x = xy_from_code(r.read(6));
                object.position.y = xy_from_code(r.read(6));
                const auto z_sign = r.read(1);
                const auto z_mag = r.read(4);
                object.position.z = (z_sign == 0 ? -1.0 : 1.0) * static_cast<double>(z_mag) / 15.0;
            }
            if (r.read(1) != 0) {  // b_object_distance_specified
                ObjectDistance distance;
                distance.at_infinity = r.read(1) != 0;  // b_object_at_infinity
                if (!distance.at_infinity) {
                    distance.factor = distance_factor_from_idx(r.read(4));
                }
                object.distance = distance;
            } else {
                object.distance.reset();
            }
        }
        if ((present & kZone) != 0) {
            object.zone = static_cast<ZoneConstraint>(static_cast<std::uint8_t>(r.read(3)));
            object.enable_elevation = r.read(1) != 0;
        }
        if ((present & kSize) != 0) {
            const auto size_idx = r.read(2);  // Table 17
            const auto axis = [&r] { return static_cast<double>(r.read(5)) / 31.0; };
            if (size_idx == 0b01) {
                const double shared = axis();
                object.size = {.width = shared, .depth = shared, .height = shared};
            } else if (size_idx == 0b10) {
                // The syntax order is width, depth, height - NOT the
                // width/height/depth an ADM reader thinks in.
                const double width = axis();
                const double depth = axis();
                object.size = {.width = width, .depth = depth, .height = axis()};
            } else if (size_idx == 0b00) {
                object.size = {};
            } else {
                return false;  // Table 17 reserves 0b11 and gives it no width
            }
        }
        if ((present & kScreen) != 0) {
            object.screen_reference = r.read(1) != 0;  // b_object_use_screen_ref
            if (object.screen_reference) {
                // §5.6.1.1.19: screen_factor = (screen_factor_bits + 1) / 8.
                object.screen_factor = static_cast<double>(r.read(3) + 1) / 8.0;
                object.depth_factor = depth_factor_from_idx(r.read(2));
            } else {
                object.screen_factor = 0.0;
                object.depth_factor = 1.0;
            }
        }
        object.snap = r.read(1) != 0;  // b_object_snap, outside every presence flag
    }

    if (r.read(1) != 0) {  // b_additional_table_data_exists
        // §5.6.4.11: the size covers the block AND its padding, so skipping
        // it whole is exactly right and needs no knowledge of its contents.
        const std::size_t bytes = static_cast<std::size_t>(r.read(4)) + 1;
        r.skip(bytes * 8);
    }
    return true;
}

// §5.5.5 object_element, in full: every md_update block, every object.
[[nodiscard]] bool read_object_element(BitReader& r, const ObjectLayout& layout,
                                       DecodedProgram& out, int& num_blocks) {
    // md_update_info (§5.5.6), Table 22.
    int sample_offset = 0;
    switch (r.read(2)) {  // sample_offset_code
        case 0b00:
            break;
        case 0b01: {
            constexpr std::array<int, 4> kOffsets = {8, 16, 18, 24};  // Table 23
            sample_offset = kOffsets[r.read(2) & 3u];
            break;
        }
        case 0b10:
            sample_offset = static_cast<int>(r.read(5));
            break;
        default:
            return false;  // Table 22 reserves 0b11 and names no field for it
    }
    num_blocks = static_cast<int>(r.read(3)) + 1;

    out.blocks.assign(static_cast<std::size_t>(num_blocks), UpdateBlock{});
    for (int blk = 0; blk < num_blocks; ++blk) {
        auto& block = out.blocks[static_cast<std::size_t>(blk)];
        // Only the first block inherits the md_update_info sample offset; the
        // rest place themselves with block_offset_factor.
        block.sample_offset = blk == 0 ? sample_offset : 0;
        block.block_offset_factor = static_cast<int>(r.read(6));
        switch (r.read(2)) {  // ramp_duration_code, Table 24
            case 0b00:
                block.ramp_duration = 0;
                break;
            case 0b01:
                block.ramp_duration = 512;
                break;
            case 0b10:
                block.ramp_duration = 1536;
                break;
            default:
                block.ramp_duration = r.read(1) != 0 ? ramp_duration_from_idx(r.read(4))
                                                     : static_cast<int>(r.read(11));
                break;
        }
    }

    if (r.read(1) == 0) {  // b_reserved_data_not_present
        r.skip(5);         // reserved
    }

    // §5.5.8: every block of one object, then the next object - not every
    // object of one block. So an object state carries down its own column.
    const int dynamic_objects = std::max(layout.total - layout.anchored, 0);
    for (auto& block : out.blocks) {
        block.objects.assign(static_cast<std::size_t>(dynamic_objects), DynamicObject{});
    }
    // Table 18 object_gain_idx 3 reuses the previous OBJECT gain within one
    // metadata update block, so this resets per block, not per object.
    std::vector<double> previous_gain(static_cast<std::size_t>(num_blocks), 0.0);
    for (int object = 0; object < layout.total; ++object) {
        const bool anchored = object < layout.anchored;
        DynamicObject state{};
        for (int blk = 0; blk < num_blocks; ++blk) {
            if (!read_object_info_block(r, blk, anchored, state,
                                        previous_gain[static_cast<std::size_t>(blk)])) {
                return false;
            }
            if (!anchored) {
                out.blocks[static_cast<std::size_t>(blk)]
                    .objects[static_cast<std::size_t>(object - layout.anchored)] = state;
            }
        }
    }
    return true;
}

// §5.5.12 trim_element - see TrimElement own comment on kNumTrimConfigs.
void read_trim_element(BitReader& r, int object_count, TrimElement& trim) {
    trim.warp_mode = static_cast<int>(r.read(2));
    r.skip(2);  // reserved
    trim.global_trim_mode = static_cast<int>(r.read(2));
    if (trim.global_trim_mode == 0b10) {  // custom_trim
        trim.configs.assign(static_cast<std::size_t>(kNumTrimConfigs), TrimConfig{});
        for (auto& config : trim.configs) {
            config.default_trim = r.read(1) != 0;
            if (config.default_trim) {
                continue;
            }
            config.disabled = r.read(1) != 0;
            if (config.disabled) {
                continue;
            }
            // Table 34, index 0 first: index 4 is trim_centre and index 0 is
            // the surround balance pair, so the balance flags lead on the wire.
            const std::uint32_t present = flags_msb_first(r.read(5), 5);
            if ((present & (1u << 4)) != 0) {
                config.centre = static_cast<int>(r.read(4));
            }
            if ((present & (1u << 3)) != 0) {
                config.surround = static_cast<int>(r.read(4));
            }
            if ((present & (1u << 2)) != 0) {
                config.height = static_cast<int>(r.read(4));
            }
            if ((present & (1u << 1)) != 0) {
                const bool sign = r.read(1) != 0;
                config.balance_top_bottom = std::pair{sign, static_cast<int>(r.read(4))};
            }
            if ((present & (1u << 0)) != 0) {
                const bool sign = r.read(1) != 0;
                config.balance_listener = std::pair{sign, static_cast<int>(r.read(4))};
            }
        }
    }
    if (r.read(1) != 0) {  // b_disable_trim_per_obj
        trim.disable_per_object.assign(static_cast<std::size_t>(std::max(object_count, 0)), false);
        for (std::size_t i = 0; i < trim.disable_per_object.size(); ++i) {
            trim.disable_per_object[i] = r.read(1) != 0;
        }
    }
}

// §5.5.13 extended_object_element. Only obj_div_block carries anything this
// model has a home for (DynamicObject::divergence); ext_prec_pos_block is a
// sub-quantization-step refinement of a position already decoded, and is
// walked past rather than folded in.
[[nodiscard]] bool read_extended_object_element(BitReader& r, const ObjectLayout& layout,
                                                int num_blocks, DecodedProgram& out) {
    if (num_blocks <= 0) {
        // §5.5.13 loops over num_obj_info_blocks, which only the
        // object_element establishes. An extended element arriving before one
        // has no loop bound at all.
        return false;
    }
    if (r.read(1) != 0) {  // b_obj_div_block
        for (int object = layout.anchored; object < layout.total; ++object) {
            for (int blk = 0; blk < num_blocks; ++blk) {
                if (r.read(1) == 0) {  // b_object_divergence
                    continue;
                }
                const auto mode = r.read(2);  // object_div_mode
                double divergence = 0.0;
                if (mode == 0) {
                    // A 2-bit table index whose table TS 103 420 does not
                    // print, so only its four steps even spacing is safe.
                    divergence = static_cast<double>(r.read(2)) / 3.0;
                } else if (mode == 2 || mode == 3) {
                    divergence = static_cast<double>(r.read(6)) / 63.0;
                }
                auto& block = out.blocks[static_cast<std::size_t>(blk)];
                const auto index = static_cast<std::size_t>(object - layout.anchored);
                if (index < block.objects.size()) {
                    block.objects[index].divergence = divergence;
                }
            }
        }
    }
    if (r.read(1) != 0) {  // b_ext_prec_pos_block
        for (int object = layout.anchored; object < layout.total; ++object) {
            for (int blk = 0; blk < num_blocks; ++blk) {
                if (r.read(1) == 0) {  // b_ext_prec_pos
                    continue;
                }
                const std::uint32_t present = flags_msb_first(r.read(3), 3);
                for (int axis = 0; axis < 3; ++axis) {
                    if ((present & (1u << axis)) != 0) {
                        r.skip(2);
                    }
                }
            }
        }
    }
    return true;
}

}  // namespace

std::vector<DisplayObject> describe_objects(const DecodedProgram& program, std::size_t block) {
    std::vector<DisplayObject> out;
    if (program.blocks.empty()) {
        return out;
    }
    const auto& update = program.blocks[std::min(block, program.blocks.size() - 1)];
    const auto labels = bed_labels(program.program.bed);
    const int anchored = object_count(program.program) - program.program.dynamic_objects;
    for (const int index : joc_object_indices(program.program)) {
        if (index >= anchored) {
            const auto dynamic = static_cast<std::size_t>(index - anchored);
            if (dynamic >= update.objects.size()) {
                out.push_back({});
                continue;
            }
            const auto& object = update.objects[dynamic];
            out.push_back({.position = object.position,
                           .size = object.size,
                           .gain_db = object.gain_db,
                           .snap = object.snap,
                           .active = object.active,
                           .label = {}});
            continue;
        }
        // A bed (or ISF) channel. Only a standard Table 12 assignment yields
        // a label, so an ISF or non-standard channel comes back placed at the
        // room centre with no name - which is honest: nothing named it.
        if (static_cast<std::size_t>(index) < labels.size()) {
            const auto label = labels[static_cast<std::size_t>(index)];
            out.push_back({.position = bed_label_position(label), .label = describe(label)});
        } else {
            out.push_back({});
        }
    }
    return out;
}

std::optional<DecodedProgram> parse_payload(std::span<const std::byte> payload) {
    BitReader r{payload};

    // §5.5.2. Version 3 escapes to a 3-bit extension, but §5.6.0.1 defines no
    // syntax for a version other than 0, so a higher one is refused rather
    // than read with this clause field layout.
    if (r.read(2) != 0) {  // oa_md_version_bits
        return std::nullopt;
    }
    auto total_bits = r.read(5);  // object_count_bits
    if (total_bits == 0x1F) {
        total_bits += r.read(7);  // object_count_bits_ext
    }
    const int total = static_cast<int>(total_bits) + 1;

    DecodedProgram out{};
    Program& program = out.program;
    program.dynamic_only = r.read(1) != 0;  // b_dyn_object_only_program
    if (program.dynamic_only) {
        program.lfe = r.read(1) != 0;  // b_lfe_present
        program.bed = 0;
        program.dynamic_objects = total - (program.lfe ? 1 : 0);
    } else {
        program.lfe = false;  // has_lfe() reads it from the bed in this branch
        // content_description[] (Table 11a), index 0 first - see
        // flags_msb_first own comment.
        const auto content = flags_msb_first(r.read(4), 4);
        constexpr std::uint32_t kReserved = 1u << 0;
        constexpr std::uint32_t kDynFlag = 1u << 1;
        constexpr std::uint32_t kIsfFlag = 1u << 2;
        constexpr std::uint32_t kBedFlag = 1u << 3;
        if ((content & kBedFlag) != 0) {
            program.bed_chan_distribute = r.read(1) != 0;  // b_bed_chan_distribute
            const bool multiple = r.read(1) != 0;          // b_multiple_bed_instances_present
            const int instances = multiple ? static_cast<int>(r.read(3)) + 2 : 1;
            for (int instance = 0; instance < instances; ++instance) {
                std::uint16_t standard = 0;
                std::uint32_t nonstandard = 0;
                if (r.read(1) != 0) {  // b_lfe_only
                    // §5.6.1.1.6: this REPLACES the assignment rather than
                    // abbreviating it - no 10-bit field follows.
                    standard = bed::kLfe;
                } else if (r.read(1) != 0) {  // b_standard_chan_assign
                    standard = static_cast<std::uint16_t>(flags_msb_first(r.read(10), 10));
                } else {
                    nonstandard = flags_msb_first(r.read(17), 17);
                }
                if (instance == 0) {
                    program.bed = standard;
                    program.nonstd_bed = nonstandard;
                } else if (nonstandard != 0) {
                    program.extra_bed_channels += nonstd_bed_channel_count(nonstandard);
                } else {
                    program.extra_beds.push_back(standard);
                }
            }
        }
        if ((content & kIsfFlag) != 0) {
            program.isf_idx = static_cast<int>(r.read(3));
            if (isf_object_count(program.isf_idx) == 0) {
                // Table 11b reserves 6 and 7 and gives them no object count,
                // so the bed/ISF/dynamic split below is unknowable.
                return std::nullopt;
            }
        }
        if ((content & kDynFlag) != 0) {
            auto dynamic_bits = r.read(5);  // num_dynamic_objects_bits
            if (dynamic_bits == 0x1F) {
                dynamic_bits += r.read(7);
            }
            program.dynamic_objects = static_cast<int>(dynamic_bits) + 1;
        }
        if ((content & kReserved) != 0) {
            // §5.6.4.1: reserved_data_size covers the data AND its padding,
            // so this skips whole and needs to know nothing about it.
            const std::size_t bytes = static_cast<std::size_t>(r.read(4)) + 1;
            r.skip(bytes * 8);
        }
    }
    if (object_count(program) != total) {
        // The wire own object_count disagrees with what it just described.
        return std::nullopt;
    }

    const bool alternate_object_data = r.read(1) != 0;  // b_alternate_object_data_present
    auto element_count = r.read(4);                     // oa_element_count_bits
    if (element_count == 0xF) {
        element_count += r.read(5);  // oa_element_count_bits_ext
    }

    const ObjectLayout layout{.total = total, .anchored = total - program.dynamic_objects};
    int num_blocks = 0;
    bool saw_object_element = false;

    for (std::uint32_t element = 0; element < element_count; ++element) {
        // --- oa_element_md (§5.5.4) ---
        const auto element_id = r.read(4);  // oa_element_id_idx, Table 26
        const std::size_t element_bytes =
            static_cast<std::size_t>(read_variable_bits_max(r, 4, 4)) + 1;
        // §5.6.4.3 measures from alternate_object_data_id_idx - or, absent
        // that, from b_discard_unknown_element - through the padding.
        const std::size_t element_start = r.bit_position();
        if (alternate_object_data) {
            r.skip(4);  // alternate_object_data_id_idx: Table 27 defines only 0
        }
        r.skip(1);  // b_discard_unknown_element: known here regardless of its value

        bool ok = true;
        bool interpreted = true;
        switch (element_id) {
            case 1:  // object_element
                ok = read_object_element(r, layout, out, num_blocks);
                saw_object_element = saw_object_element || ok;
                break;
            case 2: {  // trim_element
                TrimElement trim{};
                read_trim_element(r, total, trim);
                out.trim = std::move(trim);
                break;
            }
            case 5:  // extended_object_element
                ok = read_extended_object_element(r, layout, num_blocks, out);
                break;
            default:
                // Table 26 reserves every other id. §5.6.4.3 is exactly the
                // mechanism for walking past one, so this is a seek, not a
                // failure - and the other elements still decode.
                out.skipped_elements.push_back(static_cast<int>(element_id));
                interpreted = false;
                break;
        }
        if (!ok || r.overflowed()) {
            return std::nullopt;
        }
        const std::size_t element_end = element_start + element_bytes * 8;
        if (element_end > payload.size() * 8 || r.bit_position() > element_end) {
            return std::nullopt;
        }
        // An EXACT byte-rounding match, not just an upper bound, is what
        // catches a corrupt object count that makes an element own loops stop
        // short: reading too few objects still lands inside the payload, just
        // at the wrong bit, and only checking WHICH byte that lands in
        // notices the difference. A skipped element is exempt - nothing was
        // read from it to be consistent with.
        if (interpreted) {
            const std::size_t consumed = r.bit_position() - element_start;
            if (element_bytes * 8 != (consumed + 7) / 8 * 8) {
                return std::nullopt;
            }
        }
        r.skip(element_end - r.bit_position());
    }

    if (r.overflowed() || !saw_object_element) {
        // Without an object_element there is no md_update_info and so no
        // object state at all - a payload that describes a program and then
        // says nothing about it.
        return std::nullopt;
    }
    out.objects = out.blocks.front().objects;
    if (static_cast<int>(out.objects.size()) != program.dynamic_objects) {
        return std::nullopt;
    }
    return out;
}

}  // namespace ac3::oba
