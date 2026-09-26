#include "asf/coder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "tables/huffman_codes.hpp"
#include "tables/huffman_tables.hpp"
#include "tables/sfb_tables.hpp"

namespace ac4::detail {
namespace {

constexpr int kCodebooks = 12;          // 0, and Tables A.2 to A.12's 1 to 11
constexpr std::int32_t kMaxQuant = 8191; // what ext_code (Pseudocode 20) reaches
constexpr int kEscape = 16;              // codebook 11's escape value
constexpr int kMaxDelta = 60;            // Table A.1's deltas run -60 to 60
// The quantiser's rounding offset: q = floor(|x/g|^(3/4) + kRounding). 0.5
// would round in the |q| domain; less than that lowers the error in the line
// domain, where the reconstruction's 4/3 power stretches the upper half of
// each interval.
constexpr double kRounding = 0.4054;

// Table 39: section lengths in 3-bit or 5-bit increments.
[[nodiscard]] int section_bits_width(int index) noexcept {
    return index <= 2 ? 3 : 5;
}

[[nodiscard]] std::size_t section_header_bits(int band_count, int width) {
    const int escape = (1 << width) - 1;
    const int increments = (band_count - 1) / escape + 1;
    return 4 + static_cast<std::size_t>(width * increments);
}

// The largest magnitude a codebook codes directly, and its values' range.
struct CodebookShape {
    int dim = 0;
    bool is_unsigned = false;
    int min_value = 0;
    int max_value = 0;  // codebook 11: 16, the escape
    int mod = 0, mod2 = 0, mod3 = 0, off = 0;
};

[[nodiscard]] CodebookShape shape(int cb) {
    const Codebook& book = *tables::kAsfSpectrumCodebooks[static_cast<std::size_t>(cb)];
    CodebookShape s;
    s.dim = tables::kCbDim[static_cast<std::size_t>(cb)];
    s.is_unsigned = tables::kUnsignedCb[static_cast<std::size_t>(cb)];
    s.mod = book.cb_mod;
    s.mod2 = book.cb_mod2;
    s.mod3 = book.cb_mod3;
    s.off = book.cb_off;
    if (s.is_unsigned) {
        s.min_value = 0;
        s.max_value = book.cb_mod - 1;
    } else {
        s.min_value = -book.cb_off;
        s.max_value = book.cb_mod - 1 - book.cb_off;
    }
    return s;
}

const std::array<CodebookShape, kCodebooks>& shapes() {
    static const std::array<CodebookShape, kCodebooks> kShapes = [] {
        std::array<CodebookShape, kCodebooks> all{};
        for (int cb = 1; cb < kCodebooks; ++cb) {
            all[static_cast<std::size_t>(cb)] = shape(cb);
        }
        return all;
    }();
    return kShapes;
}

// Whether every line of a band fits codebook `cb`'s values.
[[nodiscard]] bool fits(const CodebookShape& s, int cb, std::int32_t max_abs) noexcept {
    if (cb == 11) {
        return true;
    }
    if (s.is_unsigned) {
        return max_abs <= s.max_value;
    }
    return max_abs <= s.max_value && -max_abs >= s.min_value;
}

// Pseudocode 19 in reverse: the index of the codeword for `dim` values.
[[nodiscard]] std::size_t codeword_index(const CodebookShape& s, int cb, std::span<const std::int32_t> values) {
    std::array<int, 4> v{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        int x = values[i];
        if (s.is_unsigned) {
            x = std::abs(x);
            if (cb == 11) {
                x = std::min(x, kEscape);
            }
        }
        v[i] = x + s.off;
    }
    if (s.dim == 4) {
        return static_cast<std::size_t>(v[0] * s.mod3 + v[1] * s.mod2 + v[2] * s.mod + v[3]);
    }
    return static_cast<std::size_t>(v[0] * s.mod + v[1]);
}

// Pseudocode 20 in reverse: N_ext leading ones and a zero, then N_ext + 4 bits
// of the magnitude less 2^(N_ext + 4).
struct ExtCode {
    unsigned bits = 0;
    std::uint64_t raw = 0;
};

[[nodiscard]] ExtCode ext_code(std::int32_t magnitude) {
    int n_ext = 0;
    while ((1 << (n_ext + 5)) <= magnitude) {
        ++n_ext;
    }
    ExtCode code;
    const auto ones = static_cast<unsigned>(n_ext);
    const std::uint64_t prefix = ((std::uint64_t{1} << ones) - 1) << 1U;  // n_ext ones, then a zero
    const auto payload = static_cast<unsigned>(n_ext + 4);
    const auto value = static_cast<std::uint64_t>(magnitude - (1 << (n_ext + 4)));
    code.bits = ones + 1 + payload;
    code.raw = (prefix << payload) | value;
    return code;
}

// The bits of one band's lines in codebook `cb`: codewords, sign bits and
// escapes.
[[nodiscard]] std::size_t band_cost(std::span<const std::int32_t> q, int cb) {
    const CodebookShape& s = shapes()[static_cast<std::size_t>(cb)];
    const std::span<const HuffCode> codes = tables::kAsfSpectrumCodes[static_cast<std::size_t>(cb)];
    const auto dim = static_cast<std::size_t>(s.dim);
    std::size_t bits = 0;
    for (std::size_t k = 0; k + dim <= q.size(); k += dim) {
        const std::span<const std::int32_t> values = q.subspan(k, dim);
        bits += codes[codeword_index(s, cb, values)].bits;
        if (s.is_unsigned) {
            for (const std::int32_t x : values) {
                bits += x != 0 ? 1U : 0U;
                if (cb == 11 && std::abs(x) >= kEscape) {
                    bits += ext_code(std::abs(x)).bits;
                }
            }
        }
    }
    return bits;
}

[[nodiscard]] std::int32_t quantize_line(double x, double inverse_gain) {
    const double scaled = std::pow(std::abs(x) * inverse_gain, 0.75) + kRounding;
    const auto magnitude = static_cast<std::int32_t>(std::min(scaled, static_cast<double>(kMaxQuant)));
    return x < 0.0 ? -magnitude : magnitude;
}

[[nodiscard]] std::int32_t quantize_band(std::span<const double> lines, int sf, std::span<std::int32_t> out) {
    const double inverse_gain = std::pow(2.0, -0.25 * static_cast<double>(sf - 100));
    std::int32_t max_abs = 0;
    for (std::size_t k = 0; k < lines.size(); ++k) {
        out[k] = quantize_line(lines[k], inverse_gain);
        max_abs = std::max(max_abs, std::abs(out[k]));
    }
    return max_abs;
}

}  // namespace

std::span<const std::uint16_t> band_offsets(int transform_length) {
    return tables::sfb_offsets_48(transform_length);
}

Grouped regroup(std::span<const double> spectrum, const FrameLayout& layout, std::array<int, 2> max_sfb) {
    Grouped out;
    std::vector<std::size_t> window_start;
    std::size_t total = 0;
    for (const int length : layout.window_length) {
        window_start.push_back(total);
        total += static_cast<std::size_t>(length);
    }
    std::size_t base = 0;
    std::size_t window = 0;
    for (std::size_t g = 0; g < layout.group_windows.size(); ++g) {
        const auto windows = static_cast<std::size_t>(layout.group_windows[g]);
        const std::span<const std::uint16_t> bands = band_offsets(layout.group_length[g]);
        const int available = std::max(0, static_cast<int>(bands.size()) - 1);
        const int limit = std::clamp(max_sfb[static_cast<std::size_t>(layout.group_half[g])], 0, available);
        out.max_sfb.push_back(limit);
        std::vector<std::size_t> offsets(static_cast<std::size_t>(limit) + 1);
        for (std::size_t b = 0; b < offsets.size(); ++b) {
            offsets[b] = base + static_cast<std::size_t>(bands[b]) * windows;
        }
        for (int b = 0; b < limit; ++b) {
            const auto bi = static_cast<std::size_t>(b);
            for (std::size_t w = 0; w < windows; ++w) {
                const std::size_t first = window_start[window + w];
                for (std::size_t l = bands[bi]; l < bands[bi + 1]; ++l) {
                    out.lines.push_back(spectrum[first + l]);
                }
            }
        }
        base = offsets[static_cast<std::size_t>(limit)];
        out.offset.push_back(std::move(offsets));
        window += windows;
    }
    return out;
}

CodedTrack code_track(const Grouped& grouped, const std::vector<std::vector<int>>& sf, int offset,
                      const FrameLayout& layout) {
    CodedTrack t;
    t.q.assign(grouped.lines.size(), 0);
    t.offset = grouped.offset;
    const std::size_t groups = grouped.offset.size();
    t.cb.resize(groups);
    t.sf.resize(groups);
    t.sections.resize(groups);
    std::vector<std::vector<std::int32_t>>& max_abs = t.max_abs;
    max_abs.resize(groups);

    // Scale factors: each band's, offset and clamped, then held to the deltas
    // Table A.1 can send from one transmitted band to the next.
    bool first = true;
    int previous = 0;
    for (std::size_t g = 0; g < groups; ++g) {
        const auto bands = static_cast<std::size_t>(grouped.max_sfb[g]);
        t.sf[g].assign(bands, 0);
        max_abs[g].assign(bands, 0);
        for (std::size_t b = 0; b < bands; ++b) {
            const std::size_t begin = grouped.offset[g][b];
            const std::size_t end = grouped.offset[g][b + 1];
            const std::span<const double> lines = std::span<const double>(grouped.lines).subspan(begin, end - begin);
            const std::span<std::int32_t> q = std::span<std::int32_t>(t.q).subspan(begin, end - begin);
            int s = std::clamp(sf[g][b] + offset, 0, 255);
            if (!first) {
                s = std::clamp(s, std::max(0, previous - kMaxDelta), std::min(255, previous + kMaxDelta));
            }
            std::int32_t peak = quantize_band(lines, s, q);
            // A line past what ext_code reaches clips; coarser steps avoid it
            // where the delta from the band before allows.
            while (peak >= kMaxQuant && s < 255 && (first || s < previous + kMaxDelta)) {
                ++s;
                peak = quantize_band(lines, s, q);
            }
            t.sf[g][b] = s;
            max_abs[g][b] = peak;
            if (peak > 0) {
                first = false;
                previous = s;
            }
        }
    }

    // Codebooks and sections, by exact cost: each band priced in every
    // codebook its values fit, then the cheapest division into sections,
    // counting each section's header.
    for (std::size_t g = 0; g < groups; ++g) {
        const int bands = grouped.max_sfb[g];
        const int width = section_bits_width(group_transf_index(layout, g));
        std::vector<std::array<std::size_t, kCodebooks>> cost(static_cast<std::size_t>(bands));
        constexpr std::size_t kNever = std::numeric_limits<std::size_t>::max() / 4;
        for (int b = 0; b < bands; ++b) {
            const auto bi = static_cast<std::size_t>(b);
            const std::size_t begin = grouped.offset[g][bi];
            const std::size_t end = grouped.offset[g][bi + 1];
            const std::span<const std::int32_t> q = std::span<const std::int32_t>(t.q).subspan(begin, end - begin);
            for (int cb = 0; cb < kCodebooks; ++cb) {
                std::size_t c = kNever;
                if (cb == 0) {
                    c = max_abs[g][bi] == 0 ? 0 : kNever;
                } else if (fits(shapes()[static_cast<std::size_t>(cb)], cb, max_abs[g][bi])) {
                    c = band_cost(q, cb);
                }
                cost[bi][static_cast<std::size_t>(cb)] = c;
            }
        }
        // best[b]: the cheapest coding of bands [0, b); from[b] and book[b]
        // say where its last section started and which codebook it took.
        std::vector<std::size_t> best(static_cast<std::size_t>(bands) + 1, kNever);
        std::vector<int> from(static_cast<std::size_t>(bands) + 1, 0);
        std::vector<int> book(static_cast<std::size_t>(bands) + 1, 0);
        best[0] = 0;
        for (int end = 1; end <= bands; ++end) {
            for (int cb = 0; cb < kCodebooks; ++cb) {
                std::size_t run = 0;
                for (int start = end - 1; start >= 0; --start) {
                    const std::size_t c = cost[static_cast<std::size_t>(start)][static_cast<std::size_t>(cb)];
                    if (c >= kNever) {
                        break;
                    }
                    run += c;
                    const std::size_t total =
                        best[static_cast<std::size_t>(start)] + run + section_header_bits(end - start, width);
                    if (total < best[static_cast<std::size_t>(end)]) {
                        best[static_cast<std::size_t>(end)] = total;
                        from[static_cast<std::size_t>(end)] = start;
                        book[static_cast<std::size_t>(end)] = cb;
                    }
                }
            }
        }
        std::vector<Section> sections;
        for (int end = bands; end > 0; end = from[static_cast<std::size_t>(end)]) {
            sections.push_back(Section{book[static_cast<std::size_t>(end)], from[static_cast<std::size_t>(end)], end});
        }
        std::reverse(sections.begin(), sections.end());
        t.cb[g].assign(static_cast<std::size_t>(bands), 0);
        for (const Section& section : sections) {
            t.section_bits += section_header_bits(section.end - section.start, width);
            for (int b = section.start; b < section.end; ++b) {
                t.cb[g][static_cast<std::size_t>(b)] = section.cb;
                t.spectral_bits += section.cb == 0 ? 0 : cost[static_cast<std::size_t>(b)][static_cast<std::size_t>(section.cb)];
            }
        }
        t.sections[g] = std::move(sections);
    }

    // asf_scalefac_data(): the reference, then a delta for each later band
    // that sends a scale factor.
    t.scalefac_bits = 8;
    bool found = false;
    int last = 0;
    for (std::size_t g = 0; g < groups; ++g) {
        for (std::size_t b = 0; b < t.cb[g].size(); ++b) {
            if (t.cb[g][b] == 0 || max_abs[g][b] == 0) {
                continue;
            }
            if (found) {
                const int index = t.sf[g][b] - last + kMaxDelta;
                t.scalefac_bits += tables::kAsfHcbScalefacCodes[static_cast<std::size_t>(index)].bits;
            }
            found = true;
            last = t.sf[g][b];
        }
    }
    return t;
}

std::size_t sf_info_bits(const FrameLayout& layout, std::array<int, 2> max_sfb) {
    BitWriter w;
    write_sf_info(w, layout, max_sfb);
    return w.bit_position();
}

namespace {

void write_sf_info(BitWriter& w, const FrameLayout& layout, std::array<int, 2> max_sfb,
                   const std::array<int, 2>* max_sfb_side) {
    // Table 37: from frame_len_base 1 536, b_long_frame and a transform length
    // per half; below it, one for the frame.
    if (layout.single) {
        w.write(2, static_cast<std::uint64_t>(layout.transf_length[0]), "transf_length");
    } else {
        w.write(1, layout.long_frame ? 1U : 0U, "b_long_frame");
        if (!layout.long_frame) {
            w.write(2, static_cast<std::uint64_t>(layout.transf_length[0]), "transf_length");
            w.write(2, static_cast<std::uint64_t>(layout.transf_length[1]), "transf_length");
        }
    }
    const int length0 = layout.window_length.front();
    // Table 38, without b_side_limited.
    const auto bits0 = static_cast<unsigned>(max_sfb_bits(length0));
    w.write(bits0, static_cast<std::uint64_t>(max_sfb[0]), "max_sfb");
    if (max_sfb_side != nullptr) {
        w.write(bits0, static_cast<std::uint64_t>((*max_sfb_side)[0]), "max_sfb_side");
    }
    if (layout.different_framing) {
        const auto bits1 = static_cast<unsigned>(max_sfb_bits(layout.window_length.back()));
        w.write(bits1, static_cast<std::uint64_t>(max_sfb[1]), "max_sfb");
        if (max_sfb_side != nullptr) {
            w.write(bits1, static_cast<std::uint64_t>((*max_sfb_side)[1]), "max_sfb_side");
        }
    }
    for (const std::uint8_t bit : layout.grouping_bits) {
        w.write(1, bit, "scale_factor_grouping_bit");
    }
}

}  // namespace

void write_sf_info(BitWriter& w, const FrameLayout& layout, std::array<int, 2> max_sfb) {
    write_sf_info(w, layout, max_sfb, nullptr);
}

void write_sf_info_dual(BitWriter& w, const FrameLayout& layout, std::array<int, 2> max_sfb,
                        std::array<int, 2> max_sfb_side) {
    write_sf_info(w, layout, max_sfb, &max_sfb_side);
}

void write_sf_data(BitWriter& w, const CodedTrack& track, const FrameLayout& layout) {
    const std::size_t groups = track.sections.size();
    // Table 39.
    for (std::size_t g = 0; g < groups; ++g) {
        const int width = section_bits_width(group_transf_index(layout, g));
        const int escape = (1 << width) - 1;
        for (const Section& section : track.sections[g]) {
            w.write(4, static_cast<std::uint64_t>(section.cb), "sect_cb");
            int remaining = section.end - section.start - 1;
            while (remaining >= escape) {
                w.write(static_cast<unsigned>(width), static_cast<std::uint64_t>(escape), "sect_len_incr");
                remaining -= escape;
            }
            w.write(static_cast<unsigned>(width), static_cast<std::uint64_t>(remaining), "sect_len_incr");
        }
    }
    // Table 40.
    for (std::size_t g = 0; g < groups; ++g) {
        for (const Section& section : track.sections[g]) {
            if (section.cb == 0) {
                continue;
            }
            const CodebookShape& s = shapes()[static_cast<std::size_t>(section.cb)];
            const std::span<const HuffCode> codes = tables::kAsfSpectrumCodes[static_cast<std::size_t>(section.cb)];
            const auto dim = static_cast<std::size_t>(s.dim);
            const std::size_t begin = track.offset[g][static_cast<std::size_t>(section.start)];
            const std::size_t end = track.offset[g][static_cast<std::size_t>(section.end)];
            for (std::size_t k = begin; k < end; k += dim) {
                const std::span<const std::int32_t> values = std::span<const std::int32_t>(track.q).subspan(k, dim);
                w.write_codeword(codes, codeword_index(s, section.cb, values), "asf_qspec_hcw");
                if (!s.is_unsigned) {
                    continue;
                }
                unsigned count = 0;
                std::uint64_t signs = 0;
                for (const std::int32_t x : values) {
                    if (x != 0) {
                        signs = (signs << 1U) | (x < 0 ? 1U : 0U);
                        ++count;
                    }
                }
                w.write(count, signs, dim == 4 ? "quad_sign_bits" : "pair_sign_bits");
                if (section.cb == 11) {
                    for (const std::int32_t x : values) {
                        if (std::abs(x) >= kEscape) {
                            const ExtCode code = ext_code(std::abs(x));
                            w.write_as(code.bits, code.raw, static_cast<std::uint64_t>(std::abs(x)), "ext_code");
                        }
                    }
                }
            }
        }
    }
    // Table 41: the first band that sends a scale factor sends it whole, as
    // reference_scale_factor, and each later one sends its delta.
    const auto transmitted = [&](std::size_t g, std::size_t b) {
        return track.cb[g][b] != 0 && track.max_abs[g][b] > 0;
    };
    int reference = 0;
    bool found = false;
    for (std::size_t g = 0; g < groups && !found; ++g) {
        for (std::size_t b = 0; b < track.cb[g].size(); ++b) {
            if (transmitted(g, b)) {
                reference = track.sf[g][b];
                found = true;
                break;
            }
        }
    }
    w.write(8, static_cast<std::uint64_t>(reference), "reference_scale_factor");
    int last = reference;
    bool first = true;
    for (std::size_t g = 0; g < groups; ++g) {
        for (std::size_t b = 0; b < track.cb[g].size(); ++b) {
            if (!transmitted(g, b)) {
                continue;
            }
            if (!first) {
                const auto index = static_cast<std::size_t>(track.sf[g][b] - last + kMaxDelta);
                w.write_codeword(tables::kAsfHcbScalefacCodes, index, "asf_sf_hcw");
            }
            first = false;
            last = track.sf[g][b];
        }
    }
    // Table 42, without noise fill.
    w.write(1, 0, "b_snf_data_exists");
}

}  // namespace ac4::detail
