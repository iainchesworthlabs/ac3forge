#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "asf/layout.hpp"
#include "bit_writer.hpp"

// The audio spectral frontend's coding of one track, ETSI TS 103 190-1 V1.4.1
// clauses 4.2.7 and 4.2.8 written in reverse, with the reconstruction of
// clause 5.1.3 inverted: quantisation to sign(x) |x/g|^(3/4) with
// g = 2^((sf - 100)/4), a Huffman codebook per scale factor band, sections of
// bands sharing one, and scale factors as a reference and Table A.1's deltas.
//
// Lines are handled in the grouped order the syntax sends them (Pseudocode 4
// and 25): by window group, then scale factor band, then window, then line.

namespace ac4::detail {

// A track's lines regrouped from window order, with where each group's bands
// start: offset[g][b] for b up to max_sfb[g], relative to the whole track.
struct Grouped {
    std::vector<double> lines;
    std::vector<std::vector<std::size_t>> offset;   // per group, max_sfb + 1 entries
    std::vector<int> max_sfb;                       // per group
};

// Regroups `spectrum` (lines in window order, as Analysis writes them) by the
// layout, keeping `max_sfb[half]` bands of each group.
[[nodiscard]] Grouped regroup(std::span<const double> spectrum, const FrameLayout& layout,
                              std::array<int, 2> max_sfb);

// The scale factor bands of a transform length at 44.1 and 48 kHz (Annex B).
[[nodiscard]] std::span<const std::uint16_t> band_offsets(int transform_length);

struct Section {
    int cb = 0;
    int start = 0;   // band
    int end = 0;     // one past
};

// One coded track: what sf_data() carries.
struct CodedTrack {
    std::vector<std::int32_t> q;                      // grouped order
    std::vector<std::vector<std::size_t>> offset;     // as Grouped::offset
    std::vector<std::vector<int>> cb;                 // per group and band
    std::vector<std::vector<int>> sf;                 // per group and band; used where transmitted
    std::vector<std::vector<std::int32_t>> max_abs;   // max_quant_idx, per group and band
    std::vector<std::vector<Section>> sections;       // per group
    std::size_t section_bits = 0;
    std::size_t spectral_bits = 0;
    std::size_t scalefac_bits = 0;
    [[nodiscard]] std::size_t bits() const noexcept { return section_bits + spectral_bits + scalefac_bits + 1; }
};

// Quantises with each band's scale factor plus `offset`, clamped to 0 to 255
// and to the deltas Table A.1 can send, and chooses codebooks and sections by
// their exact cost. `sf` is per group and band. A band whose lines all
// quantise to zero takes codebook 0 and sends no scale factor.
[[nodiscard]] CodedTrack code_track(const Grouped& grouped, const std::vector<std::vector<int>>& sf, int offset,
                                    const FrameLayout& layout);

// asf_transform_info() and asf_psy_info(0, 0) (Tables 37 and 38).
void write_sf_info(BitWriter& w, const FrameLayout& layout, std::array<int, 2> max_sfb);

// asf_transform_info() and asf_psy_info(1, 0): with b_dual_maxsfb, the second
// track's max_sfb_side after each max_sfb.
void write_sf_info_dual(BitWriter& w, const FrameLayout& layout, std::array<int, 2> max_sfb,
                        std::array<int, 2> max_sfb_side);

// The bits write_sf_info() takes.
[[nodiscard]] std::size_t sf_info_bits(const FrameLayout& layout, std::array<int, 2> max_sfb);

// sf_data(ASF): asf_section_data(), asf_spectral_data(), asf_scalefac_data()
// and asf_snf_data() without noise fill (Tables 36 and 39 to 42).
void write_sf_data(BitWriter& w, const CodedTrack& track, const FrameLayout& layout);

}  // namespace ac4::detail
