#include <optional>

#include "internal.hpp"

namespace {
ac3::meta::QcPreset to_cpp(const ac3forge_qc_preset_t& preset) {
    return ac3::meta::QcPreset{
        .target_lkfs = preset.target_lkfs,
        .tolerance_lu = preset.tolerance_lu,
        .max_true_peak_dbtp = preset.max_true_peak_dbtp,
        .loudness_limit = static_cast<ac3::meta::QcLoudnessLimit>(preset.loudness_limit),
        .source = preset.source == nullptr ? std::string_view{} : std::string_view{preset.source}};
}
}  // namespace

extern "C" {

size_t ac3forge_qc_preset_count(void) { return ac3::meta::kQcPresetIds.size(); }

ac3forge_qc_preset_t ac3forge_qc_preset(ac3forge_qc_preset_id_t id) {
    const auto preset = ac3::meta::qc_preset(static_cast<ac3::meta::QcPresetId>(id));
    return ac3forge_qc_preset_t{
        .target_lkfs = preset.target_lkfs,
        .tolerance_lu = preset.tolerance_lu,
        .max_true_peak_dbtp = preset.max_true_peak_dbtp,
        .loudness_limit = static_cast<ac3forge_qc_loudness_limit_t>(preset.loudness_limit),
        .source = preset.source.data()};
}

const char* ac3forge_qc_preset_name(ac3forge_qc_preset_id_t id) {
    return ac3::meta::qc_preset_name(static_cast<ac3::meta::QcPresetId>(id)).data();
}

int ac3forge_parse_qc_preset(const char* name, ac3forge_qc_preset_id_t* out_id) {
    if (name == nullptr || out_id == nullptr) {
        return 0;
    }
    ac3::meta::QcPresetId id{};
    if (!ac3::meta::parse_qc_preset(name, id)) {
        return 0;
    }
    *out_id = static_cast<ac3forge_qc_preset_id_t>(id);
    return 1;
}

int ac3forge_qc_verdict_pass(const ac3forge_qc_verdict_t* verdict) {
    return verdict != nullptr && verdict->loudness_pass && verdict->true_peak_pass ? 1 : 0;
}

ac3forge_qc_verdict_t ac3forge_evaluate_qc_gate(const ac3forge_qc_preset_t* preset,
                                                 int has_integrated_lkfs, double integrated_lkfs,
                                                 int has_true_peak_dbtp, double true_peak_dbtp) {
    const ac3forge_qc_verdict_t empty{.has_loudness_delta_lu = 0,
                                      .loudness_delta_lu = 0.0,
                                      .loudness_pass = 0,
                                      .has_true_peak_margin_dbtp = 0,
                                      .true_peak_margin_dbtp = 0.0,
                                      .true_peak_pass = 0};
    if (preset == nullptr) {
        return empty;
    }
    const auto verdict = ac3::meta::evaluate_qc_gate(
        to_cpp(*preset), has_integrated_lkfs != 0 ? std::optional<double>(integrated_lkfs) : std::nullopt,
        has_true_peak_dbtp != 0 ? std::optional<double>(true_peak_dbtp) : std::nullopt);
    return ac3forge_qc_verdict_t{
        .has_loudness_delta_lu = verdict.loudness_delta_lu.has_value() ? 1 : 0,
        .loudness_delta_lu = verdict.loudness_delta_lu.value_or(0.0),
        .loudness_pass = verdict.loudness_pass ? 1 : 0,
        .has_true_peak_margin_dbtp = verdict.true_peak_margin_dbtp.has_value() ? 1 : 0,
        .true_peak_margin_dbtp = verdict.true_peak_margin_dbtp.value_or(0.0),
        .true_peak_pass = verdict.true_peak_pass ? 1 : 0};
}

}  // extern "C"
