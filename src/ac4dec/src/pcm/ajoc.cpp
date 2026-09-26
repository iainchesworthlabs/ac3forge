#include "pcm/ajoc.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ac4::detail {
namespace {

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

using Bands = std::array<int, ajoc::kMaxBands>;

// One coefficient at one data point: ajoc_huff_data()'s values (the F0
// codeword's index as it is, the others less their codebook's cb_off) through
// Pseudocode 16, from `previous`; an entry a sparse object did not send stands
// at the range's centre.
[[nodiscard]] ParseResult decode_entry(const AjocData::Entry& entry, AjocDataType type, int quant,
                                       int bands, const Bands& previous, Bands& out) {
    const bool wet = type == AjocDataType::kWet;
    const int n = ajoc::nquant(wet, quant);
    if (!entry.sent) {
        out.fill((n - 1) / 2);
        return {};
    }
    const bool diff_time = entry.data.diff_type == 1;
    std::array<int, ajoc::kMaxBands> values{};
    for (int pb = 0; pb < bands; ++pb) {
        const int index = entry.data.index[at(pb)];
        if (!diff_time && pb == 0) {
            values[at(pb)] = index;
        } else {
            const Codebook& cb =
                ajoc_codebook(type, quant, diff_time ? AjocHcbType::kDt : AjocHcbType::kDf);
            values[at(pb)] = index - cb.cb_off;
        }
    }
    if (!ajoc::differential_decode(diff_time, n, bands,
                                   std::span<const int>(values).first(at(bands)), previous, out)) {
        return fail(DecodeError::kInvalidStream,
                    "an A-JOC coefficient outside its quantisation range");
    }
    return {};
}

}  // namespace

ParseResult ajoc_values(const AjocData& data, const AjocDmxDeData& de, AjocQuantHistory& history,
                        AjocFrameValues& out) {
    const int m = data.num_dmx;
    const int n = data.num_umx;
    ajoc::FrameParameters& p = out.params;
    p.resize(m, n);
    p.num_decorr = data.num_decorr;
    p.decorr_enable = data.decorr_enable;
    p.num_dpoints = data.num_dpoints;
    p.start_pos = data.start_pos;
    p.ramp_len = data.ramp_len;
    history.objects.resize(at(n));
    for (int o = 0; o < n; ++o) {
        const AjocObjectConfig& config = data.objects[at(o)];
        AjocQuantHistory::Object& h = history.objects[at(o)];
        p.num_bands[at(o)] = std::max(config.num_bands, 1);
        if (!config.present) {
            // 5.7.3.3: an inactive object's coefficients are 0; its values
            // stand at the centre for a later data point to differ from.
            if (data.num_dpoints > 0) {
                h = AjocQuantHistory::Object{};
                h.valid = true;
                h.centre = true;
            }
            continue;
        }
        if (data.num_dpoints == 0) {
            continue;
        }
        const int quant = config.quant_select;
        const int bands = config.num_bands;
        // The values each coefficient differs from along time: the last data
        // point's, of this frame or the one before.
        std::array<Bands, kMaxAjocDmxSignals> dry_prev{};
        std::array<Bands, kMaxAjocDecorr> wet_prev{};
        bool prev_ok = h.valid && (h.centre || (h.quant_select == quant && h.num_bands == bands));
        if (h.valid && h.centre) {
            for (Bands& b : dry_prev) {
                b.fill((ajoc::nquant(false, quant) - 1) / 2);
            }
            for (Bands& b : wet_prev) {
                b.fill((ajoc::nquant(true, quant) - 1) / 2);
            }
        } else {
            dry_prev = h.dry;
            wet_prev = h.wet;
        }
        for (int dp = 0; dp < data.num_dpoints; ++dp) {
            const auto needs_prev = [&](const AjocData::Entry& entry) {
                return entry.sent && entry.data.diff_type == 1 && !prev_ok;
            };
            for (int ch = 0; ch < m; ++ch) {
                const AjocData::Entry& entry = data.dry_at(o, dp, ch);
                if (needs_prev(entry)) {
                    return fail(h.valid ? DecodeError::kInvalidStream : DecodeError::kMissingIFrame,
                                "an A-JOC coefficient differential in time from no data point of "
                                "its bands");
                }
                Bands q{};
                if (auto ok =
                        decode_entry(entry, AjocDataType::kDry, quant, bands, dry_prev[at(ch)], q);
                    !ok) {
                    return ok;
                }
                dry_prev[at(ch)] = q;
                for (int pb = 0; pb < bands; ++pb) {
                    p.dry_at(o, dp, ch, pb) = ajoc::dequantise(false, quant, q[at(pb)]);
                }
            }
            for (int d = 0; d < data.num_decorr; ++d) {
                const AjocData::Entry& entry = data.wet_at(o, dp, d);
                if (needs_prev(entry)) {
                    return fail(h.valid ? DecodeError::kInvalidStream : DecodeError::kMissingIFrame,
                                "an A-JOC coefficient differential in time from no data point of "
                                "its bands");
                }
                Bands q{};
                if (auto ok =
                        decode_entry(entry, AjocDataType::kWet, quant, bands, wet_prev[at(d)], q);
                    !ok) {
                    return ok;
                }
                wet_prev[at(d)] = q;
                for (int pb = 0; pb < bands; ++pb) {
                    p.wet_at(o, dp, d, pb) = ajoc::dequantise(true, quant, q[at(pb)]);
                }
            }
            prev_ok = true;
        }
        h.valid = true;
        h.centre = false;
        h.quant_select = quant;
        h.num_bands = bands;
        h.dry = dry_prev;
        h.wet = wet_prev;
    }

    // Dialogue enhancement's configuration and coefficients, where in force.
    out.de = de.config.has_value();
    out.dialogue.clear();
    out.coeff.clear();
    out.gmax_db = 0.0;
    if (de.config) {
        out.dialogue.assign(de.config->de_main_dlg_flag.begin(), de.config->de_main_dlg_flag.end());
        out.gmax_db = 3.0 * static_cast<double>(1 + de.config->de_max_gain);
        for (const std::uint8_t c : de.coeff) {
            out.coeff.push_back(static_cast<double>(c) / 15.0);
        }
    }
    return {};
}

AjocStage::AjocStage() : reconstruction_(std::make_unique<ajoc::Reconstruction<double>>()) {}

void AjocStage::reset() {
    reconstruction_->reset();
}

void AjocStage::reconstruct(const AjocFrameValues& values, double dialogue_db, int num_ts,
                            std::span<const std::vector<QmfValue>* const> inputs,
                            std::vector<std::vector<QmfValue>>& objects) {
    objects.resize(at(values.params.num_umx));
    outputs_.clear();
    for (std::vector<QmfValue>& object : objects) {
        outputs_.push_back(&object);
    }
    // Clause 5.8.2.3: 10^(G_DE / 20), at most 10^(G_max / 20), on the
    // dialogue objects where it is above 1.
    double de_gain = 1.0;
    if (values.de && dialogue_db > 0.0) {
        de_gain = std::pow(10.0, std::min(dialogue_db, values.gmax_db) / 20.0);
    }
    reconstruction_->reconstruct(values.params, num_ts, inputs, outputs_, de_gain, values.dialogue);
}

void AjocStage::enhance_core(const AjocFrameValues& values, double dialogue_db, int num_ts,
                             std::span<std::vector<QmfValue>* const> inputs) {
    if (dialogue_db <= 0.0) {
        return;
    }
    // Clause 5.8.2.4: 10^(G_DE / 20) - 1, at most 10^(G_max / 20) - 1. The
    // interpolation runs in every frame while dialogue enhancement is asked
    // for, so that it is ready when the stream names dialogue objects.
    const double de_gain =
        values.de ? std::pow(10.0, std::min(dialogue_db, values.gmax_db) / 20.0) - 1.0 : 0.0;
    reconstruction_->enhance_core(values.params, num_ts, inputs, de_gain, values.dialogue,
                                  values.coeff);
}

}  // namespace ac4::detail
