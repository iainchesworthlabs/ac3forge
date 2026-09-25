#pragma once

// ETSI TS 103 190-1 V1.4.1 Tables 178 and 179 and clause 5.3.3.4's matrix, as
// printed: each entry the product of the parameters it names, "a0a1a3" for
// a0 * a1 * a3, and "0". A transcription of their own, separate from the
// decoder's (src/ac4dec/src/pcm/multichannel.cpp), which
// test_ac4dec_multichannel.cpp holds to these, and which the constructed
// streams of ac4dec_constructed.cpp invert to code their tracks.

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ac4dec_test {

using Abcd = std::array<double, 4>;

// Table 178, chel_matsel 0 to 11, rows O0 to O2 between bars.
inline constexpr std::array<std::string_view, 12> kTable178 = {
    "a0a1 b0a1 b1 | c0 d0 0 | a0c1 b0c1 d1",
    "d0 c0 0 | b0a1 a0a1 b1 | b0c1 a0c1 d1",
    "a0a1 b1 b0a1 | a0c1 d1 b0c1 | c0 0 d0",
    "a1 c0b1 d0b1 | 0 a0 b0 | c1 c0d1 d0d1",
    "a0 0 b0 | c0b1 a1 d0b1 | c0d1 c1 d0d1",
    "a1 d0b1 c0b1 | c1 d0d1 c0d1 | 0 b0 a0",
    "d0d1 c0d1 c1 | b0 a0 0 | d0b1 c0b1 a1",
    "a0 b0 0 | c0d1 d0d1 c1 | c0b1 d0b1 a1",
    "d0d1 c1 c0d1 | d0b1 a1 c0b1 | b0 0 a0",
    "d1 b0c1 a0c1 | 0 d0 c0 | b1 b0a1 a0a1",
    "d0 0 c0 | b0c1 d1 a0c1 | b0a1 b1 a0a1",
    "d1 a0c1 b0c1 | b1 a0a1 b0a1 | 0 c0 d0",
};

// Table 179, rows O0 to O4.
inline constexpr std::array<std::string_view, 12> kTable179 = {
    "a0a1a3 b0a1a3 b1a3 a2b3 b2b3 | c0a4 d0a4 0 c2b4 d2b4 | a0c1 b0c1 d1 0 0 | "
    "a0a1c3 b0a1c3 b1c3 a2d3 b2d3 | c0c4 d0c4 0 c2d4 d2d4",
    "d0a3 c0a3 0 a2b3 b2b3 | b0a1a4 a0a1a4 b1a4 c2b4 d2b4 | b0c1 a0c1 d1 0 0 | "
    "d0c3 c0c3 0 a2d3 b2d3 | b0a1c4 a0a1c4 b1c4 c2d4 d2d4",
    "a0a1a3 b1a3 b0a1a3 a2b3 b2b3 | a0c1a4 d1a4 b0c1a4 c2b4 d2b4 | c0 0 d0 0 0 | "
    "a0a1c3 b1c3 b0a1c3 a2d3 b2d3 | a0c1c4 d1c4 b0c1c4 c2d4 d2d4",
    "a1a3 c0b1a3 d0b1a3 a2b3 b2b3 | 0 a0a4 b0a4 c2b4 d2b4 | c1 c0d1 d0d1 0 0 | "
    "a1c3 c0b1c3 d0b1c3 a2d3 b2d3 | 0 a0c4 b0c4 c2d4 d2d4",
    "a0a3 0 b0a3 a2b3 b2b3 | c0b1a4 a1a4 d0b1a4 c2b4 d2b4 | c0d1 c1 d0d1 0 0 | "
    "a0c3 0 b0c3 a2d3 b2d3 | c0b1c4 a1c4 d0b1c4 c2d4 d2d4",
    "a1a3 d0b1a3 c0b1a3 a2b3 b2b3 | c1a4 d0d1a4 c0d1a4 c2b4 d2b4 | 0 b0 a0 0 0 | "
    "a1c3 d0b1c3 c0b1c3 a2d3 b2d3 | c1c4 d0d1c4 c0d1c4 c2d4 d2d4",
    "d0d1a3 c0d1a3 c1a3 a2b3 b2b3 | b0a4 a0a4 0 c2b4 d2b4 | d0b1 c0b1 a1 0 0 | "
    "d0d1c3 c0d1c3 c1c3 a2d3 b2d3 | b0c4 a0c4 0 c2d4 d2d4",
    "a0a3 b0a3 0 a2b3 b2b3 | c0d1a4 d0d1a4 c1a4 c2b4 d2b4 | c0b1 d0b1 a1 0 0 | "
    "a0c3 b0c3 0 a2d3 b2d3 | c0d1c4 d0d1c4 c1c4 c2d4 d2d4",
    "d0d1a3 c1a3 c0d1a3 a2b3 b2b3 | d0b1a4 a1a4 c0b1a4 c2b4 d2b4 | b0 0 a0 0 0 | "
    "d0d1c3 c1c3 c0d1c3 a2d3 b2d3 | d0b1c4 a1c4 c0b1c4 c2d4 d2d4",
    "d1a3 b0c1a3 a0c1a3 a2b3 b2b3 | 0 d0a4 c0a4 c2b4 d2b4 | b1 b0a1 a0a1 0 0 | "
    "d1c3 b0c1c3 a0c1c3 a2d3 b2d3 | 0 d0c4 c0c4 c2d4 d2d4",
    "d0a3 0 c0a3 a2b3 b2b3 | b0c1a4 d1a4 a0c1a4 c2b4 d2b4 | b0a1 b1 a0a1 0 0 | "
    "d0c3 0 c0c3 a2d3 b2d3 | b0c1c4 d1c4 a0c1c4 c2d4 d2d4",
    "d1a3 a0c1a3 b0c1a3 a2b3 b2b3 | b1a4 a0a1a4 b0a1a4 c2b4 d2b4 | 0 c0 d0 0 0 | "
    "d1c3 a0c1c3 b0c1c3 a2d3 b2d3 | b1c4 a0a1c4 b0a1c4 c2d4 d2d4",
};

// Clause 5.3.3.4's matrix, rows O0 to O3.
inline constexpr std::string_view kFourChannel =
    "a0a2 b0a2 a1b2 b1b2 | c0a3 d0a3 c1b3 d1b3 | a0c2 b0c2 a1d2 b1d2 | c0c3 d0c3 c1d3 d1d3";

// A printed entry's value: the product of the parameters it names, a letter
// and the parameter set's number each. Nothing for text that is neither.
inline double entry(std::string_view text, std::span<const Abcd> p) {
    if (text == "0") {
        return 0.0;
    }
    double product = 1.0;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        const auto which = static_cast<std::size_t>(text[i] - 'a');
        const auto set = static_cast<std::size_t>(text[i + 1] - '0');
        product *= (which < 4 && set < p.size()) ? p[set][which] : 0.0;
    }
    return product;
}

// The printed rows, each a list of entries.
inline std::vector<std::vector<std::string>> rows_of(std::string_view printed) {
    std::vector<std::vector<std::string>> rows(1);
    std::string word;
    const auto flush = [&] {
        if (!word.empty()) {
            rows.back().push_back(word);
            word.clear();
        }
    };
    for (const char c : printed) {
        if (c == ' ') {
            flush();
        } else if (c == '|') {
            flush();
            rows.emplace_back();
        } else {
            word += c;
        }
    }
    flush();
    return rows;
}

// The printed matrix's values for the parameter sets `p`.
inline std::vector<std::vector<double>> printed_matrix(std::string_view printed, std::span<const Abcd> p) {
    std::vector<std::vector<double>> m;
    for (const auto& row : rows_of(printed)) {
        std::vector<double>& out = m.emplace_back();
        for (const auto& text : row) {
            out.push_back(entry(text, p));
        }
    }
    return m;
}

}  // namespace ac4dec_test
