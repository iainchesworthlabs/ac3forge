// The encoder's A-CPL syntax writer (src/ac4enc/src/acpl/acpl_syntax.hpp) read
// back by the decoder's parser (src/ac4dec/src/syntax/acpl.hpp): every
// configuration, framing and parameter kind, in both differencing directions,
// uses every bit and records what was written; and the writer takes exactly
// the values each of the 24 codebooks holds.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/syntax.hpp"
#include "acpl/acpl_syntax.hpp"
#include "bit_reader.hpp"
#include "bit_writer.hpp"
#include "syntax/acpl.hpp"
#include "syntax/context.hpp"

namespace {

using ac4::SyntaxRecord;
using ac4::detail::AcplConfig1chFields;
using ac4::detail::AcplConfig2chFields;
using ac4::detail::AcplFramingFields;
using ac4::detail::AcplKind;
using ac4::detail::AcplParamFields;
using ac4::detail::BitReader;
using ac4::detail::BitWriter;

// A trace kept here; ac4::SyntaxSink refers to its callable without owning it.
struct Recording {
    std::vector<SyntaxRecord> records;
    std::function<void(const SyntaxRecord&)> push = [this](const SyntaxRecord& r) { records.push_back(r); };
    Recording() = default;
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;
    [[nodiscard]] ac4::SyntaxSink sink() { return ac4::SyntaxSink(push); }
};

void require_same(std::span<const SyntaxRecord> written, std::span<const SyntaxRecord> read) {
    REQUIRE(written.size() == read.size());
    for (std::size_t i = 0; i < written.size(); ++i) {
        CAPTURE(i, written[i].name, read[i].name);
        CHECK(written[i].name == read[i].name);
        CHECK(written[i].bit_offset == read[i].bit_offset);
        CHECK(written[i].bits == read[i].bits);
        CHECK(written[i].value == read[i].value);
    }
}

struct Lcg {
    std::uint32_t state = 2468;
    int below(int n) {
        state = state * 1664525U + 1013904223U;
        return static_cast<int>((state >> 8) % static_cast<std::uint32_t>(n));
    }
};

// One parameter's sets: values the codebooks hold, drawn at random from the
// widest range each codebook takes (codable ones kept).
AcplParamFields param(Lcg& rng, AcplKind kind, int quant_mode, const AcplFramingFields& framing, int first,
                      int bands) {
    AcplParamFields out;
    for (int ps = 0; ps < framing.num_param_sets; ++ps) {
        ac4::detail::AcplSetFields set;
        set.diff_type = rng.below(2);
        for (int i = first; i < bands; ++i) {
            int value = 0;
            do {
                value = rng.below(81) - 40;
            } while (!ac4::detail::acpl_codable(kind, quant_mode, set.diff_type, i == first, value));
            set.values.push_back(value);
        }
        out.push_back(set);
    }
    return out;
}

AcplFramingFields framing(Lcg& rng) {
    AcplFramingFields f;
    f.interpolation_type = rng.below(2);
    f.num_param_sets = 1 + rng.below(2);
    f.param_timeslot = {rng.below(32), rng.below(32)};
    return f;
}

}  // namespace

TEST_CASE("acpl_config_1ch() and acpl_data_1ch() read back as written", "[ac4enc][acpl]") {
    Lcg rng;
    for (int id = 0; id < 4; ++id) {
        for (const bool partial : {false, true}) {
            for (int quant = 0; quant < 2; ++quant) {
                for (int qmf_band = 1; qmf_band <= (partial ? 8 : 1); ++qmf_band) {
                    CAPTURE(id, partial, quant, qmf_band);
                    const AcplConfig1chFields config{
                        .partial = partial, .num_param_bands_id = id, .quant_mode = quant, .qmf_band = qmf_band};
                    const int bands = ac4::detail::acpl_num_param_bands(id);
                    const int first = ac4::detail::acpl_param_band(config);
                    ac4::detail::AcplData1chFields data;
                    data.framing = framing(rng);
                    data.alpha1 = param(rng, AcplKind::kAlpha, quant, data.framing, first, bands);
                    data.beta1 = param(rng, AcplKind::kBeta, quant, data.framing, first, bands);

                    Recording written;
                    BitWriter w(0, written.sink());
                    ac4::detail::write_acpl_config_1ch(w, config);
                    ac4::detail::write_acpl_data_1ch(w, config, data);
                    Recording read;
                    BitReader r(w.bytes(), 0, read.sink());
                    ac4::detail::AcplConfig1ch parsed;
                    const auto kind =
                        partial ? ac4::detail::AcplConfigKind::kPartial : ac4::detail::AcplConfigKind::kFull;
                    REQUIRE(ac4::detail::parse_acpl_config_1ch(r, kind, parsed));
                    CHECK(parsed.num_param_bands == bands);
                    CHECK(parsed.param_band == first);
                    CHECK(parsed.qmf_band == (partial ? qmf_band : 0));
                    ac4::detail::AcplData1ch out;
                    REQUIRE(ac4::detail::parse_acpl_data_1ch(r, {}, parsed, out));
                    CHECK_FALSE(r.overflow());
                    CHECK(r.position() == w.bit_position());
                    require_same(written.records, read.records);
                }
            }
        }
    }
}

TEST_CASE("acpl_config_2ch() and acpl_data_2ch() read back as written", "[ac4enc][acpl]") {
    Lcg rng;
    for (int id = 0; id < 4; ++id) {
        for (int quant_0 = 0; quant_0 < 2; ++quant_0) {
            for (int quant_1 = 0; quant_1 < 2; ++quant_1) {
                CAPTURE(id, quant_0, quant_1);
                const AcplConfig2chFields config{
                    .num_param_bands_id = id, .quant_mode_0 = quant_0, .quant_mode_1 = quant_1};
                const int bands = ac4::detail::acpl_num_param_bands(id);
                ac4::detail::AcplData2chFields data;
                data.framing = framing(rng);
                for (auto& alpha : data.alpha) {
                    alpha = param(rng, AcplKind::kAlpha, quant_0, data.framing, 0, bands);
                }
                for (auto& beta : data.beta) {
                    beta = param(rng, AcplKind::kBeta, quant_0, data.framing, 0, bands);
                }
                data.beta3 = param(rng, AcplKind::kBeta3, quant_0, data.framing, 0, bands);
                for (auto& gamma : data.gamma) {
                    gamma = param(rng, AcplKind::kGamma, quant_1, data.framing, 0, bands);
                }

                Recording written;
                BitWriter w(0, written.sink());
                ac4::detail::write_acpl_config_2ch(w, config);
                ac4::detail::write_acpl_data_2ch(w, config, data);
                Recording read;
                BitReader r(w.bytes(), 0, read.sink());
                ac4::detail::AcplConfig2ch parsed;
                REQUIRE(ac4::detail::parse_acpl_config_2ch(r, parsed));
                ac4::detail::AcplData2ch out;
                REQUIRE(ac4::detail::parse_acpl_data_2ch(r, {}, parsed, out));
                CHECK_FALSE(r.overflow());
                CHECK(r.position() == w.bit_position());
                require_same(written.records, read.records);
            }
        }
    }
}

TEST_CASE("the A-CPL writer takes the values each codebook holds and no others", "[ac4enc][acpl]") {
    for (const AcplKind kind : {AcplKind::kAlpha, AcplKind::kBeta, AcplKind::kBeta3, AcplKind::kGamma}) {
        for (int quant = 0; quant < 2; ++quant) {
            for (int diff_type = 0; diff_type < 2; ++diff_type) {
                for (const bool first : {true, false}) {
                    if (diff_type == 1 && !first) {
                        continue;  // DIFF_TIME has one codebook for every band
                    }
                    int count = 0;
                    for (int value = -80; value <= 80; ++value) {
                        count += ac4::detail::acpl_codable(kind, quant, diff_type, first, value) ? 1 : 0;
                    }
                    CAPTURE(static_cast<int>(kind), quant, diff_type, first);
                    // The codebook lengths of Annex A.3: alpha 17/33 (F0) and
                    // 33/65, beta 5/9 and 9/17, beta3 9/17 and 17/33, gamma
                    // 21/41 and 41/81, coarse then fine.
                    const std::array<std::array<int, 2>, 4> f0 = {{{17, 33}, {5, 9}, {9, 17}, {21, 41}}};
                    const std::array<std::array<int, 2>, 4> rest = {{{33, 65}, {9, 17}, {17, 33}, {41, 81}}};
                    const auto k = static_cast<std::size_t>(kind);
                    const std::size_t q = quant == 0 ? 1 : 0;
                    CHECK(count == (diff_type == 0 && first ? f0[k][q] : rest[k][q]));
                }
            }
        }
    }
}
