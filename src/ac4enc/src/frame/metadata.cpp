#include "frame/metadata.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "tables/huffman_codes.hpp"
#include "tables/huffman_tables.hpp"

namespace ac4::detail {
namespace {

// An 11-bit loudness value (Part 1 clauses 4.3.12.3.8 to 4.3.12.3.30):
// floor(value x 10 + 1/2) + 1 024, nothing where it does not fit.
[[nodiscard]] std::optional<int> loudness_code(std::optional<double> value) noexcept {
    if (!value) {
        return std::nullopt;
    }
    const double code = std::floor(*value * 10.0 + 0.5) + 1024.0;
    if (!(code >= 0.0 && code <= 2047.0)) {
        return std::nullopt;
    }
    return static_cast<int>(code);
}

// Table 149's code for a centre gain in dB, Table 149a's for a surround gain.
[[nodiscard]] std::optional<int> centre_code(double db) noexcept {
    if (std::isinf(db) && db < 0.0) {
        return 7;
    }
    constexpr std::array<double, 7> kDb = {3.0, 1.5, 0.0, -1.5, -3.0, -4.5, -6.0};
    for (std::size_t i = 0; i < kDb.size(); ++i) {
        if (db == kDb[i]) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> surround_code(double db) noexcept {
    if (std::isinf(db) && db < 0.0) {
        return 7;
    }
    constexpr std::array<double, 5> kDb = {0.0, -1.5, -3.0, -4.5, -6.0};
    for (std::size_t i = 0; i < kDb.size(); ++i) {
        if (db == kDb[i]) {
            return static_cast<int>(i) + 2;
        }
    }
    return std::nullopt;
}

// Part 2 Table 129, the custom downmix gains' code: 0 dB to -6 dB in steps of
// 1.5, then -9 and -12 dB, and 7 for -infinity.
[[nodiscard]] std::optional<int> custom_gain_code(double db) noexcept {
    if (std::isinf(db) && db < 0.0) {
        return 7;
    }
    constexpr std::array<double, 7> kDb = {0.0, -1.5, -3.0, -4.5, -6.0, -9.0, -12.0};
    for (std::size_t i = 0; i < kDb.size(); ++i) {
        if (db == kDb[i]) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

// A downmix loudness correction in dB2: (15 - x) / 2 (Part 1 clause
// 4.3.12.2.11), so x = 15 - 2 g, from 0 to 30.
[[nodiscard]] std::optional<int> correction_code(double db2) noexcept {
    const double x = 15.0 - 2.0 * db2;
    if (!(x >= 0.0 && x <= 30.0) || x != std::floor(x)) {
        return std::nullopt;
    }
    return static_cast<int>(x);
}

// Table 171: whether the channel mode has each of L, R and C.
[[nodiscard]] bool channel_mode_has(int ch_mode, int bit) noexcept {
    if (ch_mode == 0) {
        return bit == 1;  // mono: C
    }
    if (ch_mode == 1) {
        return bit != 1;  // stereo: L and R
    }
    return true;
}

void write_curve(BitWriter& w, const CurveCodes& c) {
    w.write(4, static_cast<std::uint64_t>(c.lev_nullband_low), "drc_lev_nullband_low");
    w.write(4, static_cast<std::uint64_t>(c.lev_nullband_high), "drc_lev_nullband_high");
    w.write(4, static_cast<std::uint64_t>(c.gain_max_boost), "drc_gain_max_boost");
    if (c.gain_max_boost > 0) {
        w.write(5, static_cast<std::uint64_t>(c.lev_max_boost), "drc_lev_max_boost");
        w.write(1, static_cast<std::uint64_t>(c.nr_boost_sections), "drc_nr_boost_sections");
        if (c.nr_boost_sections > 0) {
            w.write(4, static_cast<std::uint64_t>(c.gain_section_boost), "drc_gain_section_boost");
            w.write(5, static_cast<std::uint64_t>(c.lev_section_boost), "drc_lev_section_boost");
        }
    }
    w.write(5, static_cast<std::uint64_t>(c.gain_max_cut), "drc_gain_max_cut");
    if (c.gain_max_cut > 0) {
        w.write(6, static_cast<std::uint64_t>(c.lev_max_cut), "drc_lev_max_cut");
        w.write(1, static_cast<std::uint64_t>(c.nr_cut_sections), "drc_nr_cut_sections");
        if (c.nr_cut_sections > 0) {
            w.write(5, static_cast<std::uint64_t>(c.gain_section_cut), "drc_gain_section_cut");
            w.write(5, static_cast<std::uint64_t>(c.lev_section_cut), "drc_lev_section_cut");
        }
    }
    w.write(1, c.tc_default ? 1U : 0U, "drc_tc_default_flag");
    if (!c.tc_default) {
        w.write(8, static_cast<std::uint64_t>(c.tc_attack), "drc_tc_attack");
        w.write(8, static_cast<std::uint64_t>(c.tc_release), "drc_tc_release");
        w.write(8, static_cast<std::uint64_t>(c.tc_attack_fast), "drc_tc_attack_fast");
        w.write(8, static_cast<std::uint64_t>(c.tc_release_fast), "drc_tc_release_fast");
        w.write(1, c.adaptive_smoothing ? 1U : 0U, "drc_adaptive_smoothing_flag");
        if (c.adaptive_smoothing) {
            w.write(5, static_cast<std::uint64_t>(c.attack_threshold), "drc_attack_threshold");
            w.write(5, static_cast<std::uint64_t>(c.release_threshold), "drc_release_threshold");
        }
    }
}

}  // namespace

int de_channel_count(int channel_config) noexcept {
    return std::popcount(static_cast<unsigned>(channel_config) & 7U);
}

CurveCodes curve_codes(DrcProfile profile) noexcept {
    // Table 162 in Table 166's terms. With no boost section, L_maxboost is
    // L0low less 1 + drc_lev_max_boost; with one cut section, L_sectioncut is
    // L0high plus 1 + drc_lev_section_cut and L_maxcut L_sectioncut plus 1 +
    // drc_lev_max_cut, and G_sectioncut is -(1 + drc_gain_section_cut). The
    // time constants are sent in Table 73's units (attack and attack_fast in
    // 5 ms, release in 40 ms, release_fast in 20 ms), smoothing adaptively.
    struct Row {
        int null_low, null_high, max_boost, max_boost_level, max_cut, section_cut_level,
            max_cut_level, section_cut, release_ms, release_fast_ms, attack_threshold,
            release_threshold;
    };
    // Film standard, film light, music standard, music light, speech; 0 where
    // the profile has no cut section.
    constexpr std::array<Row, 5> kRows{{
        {0, 5, 6, -12, 24, 15, 35, -5, 3000, 1000, 15, 20},
        {-10, 10, 6, -22, 24, 20, 40, -5, 3000, 1000, 15, 20},
        {0, 5, 12, -24, 24, 15, 35, -5, 10000, 1000, 15, 20},
        {-10, 10, 12, -34, 15, 0, 40, 0, 3000, 1000, 15, 20},
        {0, 5, 15, -19, 24, 15, 35, -5, 1000, 200, 10, 10},
    }};
    CurveCodes c;
    if (profile == DrcProfile::kNone) {
        return c;  // no gain anywhere, and Table 167's default time constants
    }
    const Row& r = kRows[static_cast<std::size_t>(profile) - 1];
    c.lev_nullband_low = -r.null_low;
    c.lev_nullband_high = r.null_high;
    c.gain_max_boost = r.max_boost;
    c.lev_max_boost = r.null_low - r.max_boost_level - 1;
    c.gain_max_cut = r.max_cut;
    const bool section = r.section_cut != 0;
    c.nr_cut_sections = section ? 1 : 0;
    const int section_level = section ? r.section_cut_level : r.null_high;
    if (section) {
        c.gain_section_cut = -r.section_cut - 1;
        c.lev_section_cut = r.section_cut_level - r.null_high - 1;
    }
    c.lev_max_cut = r.max_cut_level - section_level - 1;
    c.tc_default = false;
    c.tc_attack = 100 / 5;
    c.tc_release = r.release_ms / 40;
    c.tc_attack_fast = 10 / 5;
    c.tc_release_fast = r.release_fast_ms / 20;
    c.adaptive_smoothing = true;
    c.attack_threshold = r.attack_threshold;
    c.release_threshold = r.release_threshold;
    return c;
}

std::optional<LoudnessCodes> resolve_loudness(const FurtherLoudness& l) {
    if (l.practice == LoudnessPractice::kNotIndicated &&
        (l.corrected_with_gating || l.corrected_in_real_time)) {
        return std::nullopt;  // the correction's flags follow a practice
    }
    LoudnessCodes codes;
    codes.loud_prac_type = static_cast<int>(l.practice);
    if (l.corrected_with_gating) {
        codes.dialgate_prac_type = static_cast<int>(*l.corrected_with_gating);
    }
    codes.loudcorr_type = l.corrected_in_real_time;
    const auto code = [](std::optional<double> value, std::optional<int>& to) {
        if (!value) {
            return true;
        }
        to = loudness_code(value);
        return to.has_value();
    };
    if (!code(l.integrated_lkfs, codes.loudrelgat) ||
        !code(l.speech_gated_lkfs, codes.loudspchgat) ||
        !code(l.max_short_term_lufs, codes.max_loudstrm3s) ||
        !code(l.max_true_peak_dbtp, codes.max_truepk) ||
        !code(l.max_momentary_lufs, codes.max_loudmntry)) {
        return std::nullopt;
    }
    codes.speech_dialgate_prac_type = static_cast<int>(l.speech_gating);
    if (l.loudness_range_lu) {
        const double lra = std::floor(*l.loudness_range_lu * 10.0 + 0.5);
        if (!(lra >= 0.0 && lra <= 1023.0)) {
            return std::nullopt;
        }
        codes.lra = static_cast<int>(lra);
    }
    codes.lra_prac_type = l.loudness_range_v2 ? 1 : 0;
    return codes;
}

std::optional<DrcCodes> resolve_drc(const DrcConfig& d, bool experimental_gains) {
    DrcCodes codes;
    codes.eac3_profile = static_cast<int>(d.profile);
    std::vector<DrcModeConfig> modes = d.modes;
    if (modes.empty()) {
        for (int id = 0; id < 4; ++id) {
            modes.push_back(DrcModeConfig{.id = id,
                                          .output_level_from_db = 0,
                                          .output_level_to_db = 0,
                                          .profile = std::nullopt,
                                          .repeat_of = std::nullopt,
                                          .gains_config = std::nullopt});
        }
    }
    if (modes.size() > 8) {
        return std::nullopt;
    }
    for (std::size_t m = 0; m < modes.size(); ++m) {
        const DrcModeConfig& mode = modes[m];
        if (mode.id < 0 || mode.id > 7) {
            return std::nullopt;
        }
        for (std::size_t other = 0; other < m; ++other) {
            if (modes[other].id == mode.id) {
                return std::nullopt;
            }
        }
        DrcModeCodes c;
        c.id = mode.id;
        if (mode.id > 3) {
            // Part 1 clause 4.3.13.3.2: L_out,min is -drc_output_level_from
            // and L_out,max -drc_output_level_to.
            if (!(mode.output_level_from_db >= -31 &&
                  mode.output_level_from_db <= mode.output_level_to_db &&
                  mode.output_level_to_db <= 0)) {
                return std::nullopt;
            }
            c.output_level_from = -mode.output_level_from_db;
            c.output_level_to = -mode.output_level_to_db;
        }
        if (mode.repeat_of) {
            const bool known = std::ranges::any_of(modes, [&](const DrcModeConfig& o) {
                return o.id == *mode.repeat_of && o.id != mode.id && !o.repeat_of;
            });
            if (!known) {
                return std::nullopt;
            }
            c.repeat_id = *mode.repeat_of;
            c.default_profile = false;
        } else if (mode.gains_config) {
            // Transmitted gains, experimental (planning/ac4.md, "What
            // the encoder writes by default").
            if (!experimental_gains || *mode.gains_config < 0 || *mode.gains_config > 3) {
                return std::nullopt;
            }
            c.default_profile = false;
            c.gains_config = mode.gains_config;
            c.gains_curve = curve_codes(mode.profile.value_or(d.profile));
            codes.gains = true;
        } else if (mode.profile && *mode.profile != d.profile) {
            c.default_profile = false;
            c.curve = curve_codes(*mode.profile);
        }
        codes.modes.push_back(c);
    }
    return codes;
}

std::optional<DownmixCodes> resolve_downmix(const DownmixConfig& m, int ch_mode) {
    if (ch_mode < 3) {
        return std::nullopt;  // custom_dmx_data() sends coefficients for 5.X and 7.X
    }
    DownmixCodes codes;
    if (m.height) {
        // The immersive layouts' downmix to 5.X (tool_t4_to_f_s()), as DEE's
        // height_dmx_mode sends it: front both top pairs to L and R, surround
        // both to Ls and Rs, and front_and_surround the top front pair to L and
        // R and the top back pair to Ls and Rs, each at the one gain.
        if (ch_mode != 11 && ch_mode != 12) {
            return std::nullopt;
        }
        const std::optional<int> height = custom_gain_code(m.height_db);
        const std::optional<int> back = custom_gain_code(m.back_db);
        if (!height || !back) {
            return std::nullopt;
        }
        HeightDownmixCodes h;
        h.top_front_to_front = *m.height != HeightDownmix::kSurround;
        h.top_back_to_front = *m.height == HeightDownmix::kFront;
        h.top_front_code = *height;
        h.top_back_code = *height;
        h.back_code = *back;
        codes.height = h;
    }
    const std::optional<int> centre = centre_code(m.loro_centre_db);
    const std::optional<int> surround = surround_code(m.loro_surround_db);
    if (!centre || !surround) {
        return std::nullopt;
    }
    codes.loro_centre_mixgain = *centre;
    codes.loro_surround_mixgain = *surround;
    if (m.ltrt_centre_db || m.ltrt_surround_db) {
        const std::optional<int> ltrt_centre =
            centre_code(m.ltrt_centre_db.value_or(m.loro_centre_db));
        const std::optional<int> ltrt_surround =
            surround_code(m.ltrt_surround_db.value_or(m.loro_surround_db));
        if (!ltrt_centre || !ltrt_surround) {
            return std::nullopt;
        }
        codes.ltrt_mixgain = std::array<int, 2>{*ltrt_centre, *ltrt_surround};
    }
    if (m.lfe_db) {
        const double code = 5.5 - *m.lfe_db;
        if (!(code >= 0.0 && code <= 31.0) || code != std::floor(code)) {
            return std::nullopt;
        }
        codes.lfe_mixgain = static_cast<int>(code);
    }
    codes.preferred_dmx_method = static_cast<int>(m.preferred);
    if (m.loro_correction_db2) {
        codes.loro_dmx_loud_corr = correction_code(*m.loro_correction_db2);
        if (!codes.loro_dmx_loud_corr) {
            return std::nullopt;
        }
    }
    if (m.ltrt_correction_db2) {
        codes.ltrt_dmx_loud_corr = correction_code(*m.ltrt_correction_db2);
        if (!codes.ltrt_dmx_loud_corr) {
            return std::nullopt;
        }
    }
    return codes;
}

std::optional<DeConfigCodes> resolve_dialogue(const DialogueConfig& de, int ch_mode) {
    DeConfigCodes codes;
    codes.channel_config = (de.left ? 4 : 0) | (de.right ? 2 : 0) | (de.centre ? 1 : 0);
    for (const int bit : {4, 2, 1}) {
        if ((codes.channel_config & bit) != 0 && !channel_mode_has(ch_mode, bit)) {
            return std::nullopt;
        }
    }
    if (de.max_gain_db != 3 && de.max_gain_db != 6 && de.max_gain_db != 9 && de.max_gain_db != 12) {
        return std::nullopt;
    }
    codes.max_gain = de.max_gain_db / 3 - 1;
    switch (de.method) {
        case DialogueMethod::kChannelIndependent:
            break;
        case DialogueMethod::kMid:
            // de_ms_proc_flag is sent for two channels alone: L and R.
            if (codes.channel_config != 6) {
                return std::nullopt;
            }
            codes.mid = true;
            break;
        case DialogueMethod::kCrossChannel:
            // Panning needs two channels or three, and marked channels,
            // dialogue alone, are the channel-independent method's.
            if (de_channel_count(codes.channel_config) < 2 || de.source != DialogueSource::kStem) {
                return std::nullopt;
            }
            codes.method = 1;
            break;
    }
    if (de.hybrid) {
        // Table 170: the hybrid methods are 2 and 3, the channel-independent
        // and cross-channel ones with the waveform beside them, whose share
        // de_signal_contribution sends in thirty-firsts.
        if (!(de.waveform_share >= 0.0 && de.waveform_share <= 1.0) ||
            de_channel_count(codes.channel_config) == 0) {
            return std::nullopt;
        }
        codes.method += 2;
        codes.signal_contribution = static_cast<int>(std::lround(de.waveform_share * 31.0));
    }
    return codes;
}

std::optional<int> pan_code(double degrees) noexcept {
    // 0 to 239: codes 0xf0 to 0xff are not to be used (clause 4.3.12.4.9).
    const double code = degrees / 1.5;
    if (!(code >= 0.0 && code <= 239.0) || code != std::floor(code)) {
        return std::nullopt;
    }
    return static_cast<int>(code);
}

std::optional<DialogueMixCodes> resolve_dialogue_mix(const DialogueMix& mix, int ch_mode) {
    DialogueMixCodes codes;
    if (mix.max_gain_db) {
        const int gain = *mix.max_gain_db;
        if (gain != 3 && gain != 6 && gain != 9 && gain != 12) {
            return std::nullopt;
        }
        codes.dialog_max_gain = gain / 3 - 1;
    }
    if (!mix.pan_degrees.empty()) {
        // One angle for a mono dialogue, one for each of a stereo one's two
        // channels (Part 1 clause 6.2.16.1); a 3.0 dialogue's two angles are
        // the simplified decoding's, which this encoder does not send.
        const std::size_t angles = ch_mode == 0 ? 1 : (ch_mode == 1 ? 2 : 0);
        if (mix.pan_degrees.size() != angles) {
            return std::nullopt;
        }
        std::array<int, 2> pan{};
        for (std::size_t i = 0; i < angles; ++i) {
            const std::optional<int> code = pan_code(mix.pan_degrees[i]);
            if (!code) {
                return std::nullopt;
            }
            pan[i] = *code;
        }
        codes.pan_dialog = pan;
    }
    return codes;
}

std::optional<EmdfPayloadCodes> resolve_emdf(const EmdfPayload& payload) {
    // Table 79's values: the variable_bits() ones from 0 up, codecdata in 8
    // bits, priority in 5, proc_allowed in 2; and an id from 1, 0 ending a
    // list.
    if (payload.id < 1 || payload.sample_offset.value_or(0) < 0 ||
        payload.duration.value_or(0) < 0 || payload.group_id.value_or(0) < 0 ||
        payload.codec_data.value_or(0) < 0 || payload.codec_data.value_or(0) > 255 ||
        payload.priority < 0 || payload.priority > 31 || payload.processing_allowed < 0 ||
        payload.processing_allowed > 3) {
        return std::nullopt;
    }
    EmdfPayloadCodes codes;
    codes.id = static_cast<std::uint64_t>(payload.id);
    if (payload.sample_offset) {
        codes.smpoffst = static_cast<std::uint64_t>(*payload.sample_offset);
    }
    if (payload.duration) {
        codes.duration = static_cast<std::uint64_t>(*payload.duration);
    }
    if (payload.group_id) {
        codes.groupid = static_cast<std::uint64_t>(*payload.group_id);
    }
    codes.codecdata = payload.codec_data;
    codes.discard_unknown = payload.discard_unknown;
    codes.frame_aligned = payload.frame_aligned;
    codes.create_duplicate = payload.create_duplicate;
    codes.remove_duplicate = payload.remove_duplicate;
    codes.priority = payload.priority;
    codes.proc_allowed = payload.processing_allowed;
    codes.bytes = payload.bytes;
    return codes;
}

std::optional<StreamMetadata> resolve_metadata(const EncoderConfig& config, int ch_mode) {
    StreamMetadata out;
    if (config.loudness) {
        out.loudness = resolve_loudness(*config.loudness);
        if (!out.loudness) {
            return std::nullopt;
        }
    }
    if (config.drc) {
        out.drc = resolve_drc(*config.drc, config.experimental.drc_gains);
        if (!out.drc) {
            return std::nullopt;
        }
    }
    if (config.downmix) {
        out.downmix = resolve_downmix(*config.downmix, ch_mode);
        if (!out.downmix) {
            return std::nullopt;
        }
    }
    if (config.dialogue) {
        out.de = resolve_dialogue(*config.dialogue, ch_mode);
        if (!out.de) {
            return std::nullopt;
        }
    }
    return out;
}

void write_further_loudness_info(BitWriter& w, const LoudnessCodes& codes, bool iframe) {
    // b_presentation_ldn is 1 in a presentation substream, so the header is
    // the version and the practice, and the correction's flags only with a
    // practice.
    w.write(2, 0, "loudness_version");
    w.write(4, static_cast<std::uint64_t>(codes.loud_prac_type), "loud_prac_type");
    if (codes.loud_prac_type != 0) {
        w.write(1, codes.dialgate_prac_type ? 1U : 0U, "b_loudcorr_dialgate");
        if (codes.dialgate_prac_type) {
            w.write(3, static_cast<std::uint64_t>(*codes.dialgate_prac_type), "dialgate_prac_type");
        }
        w.write(1, codes.loudcorr_type ? 1U : 0U, "b_loudcorr_type");
    }
    const auto value = [&](std::optional<int> code, unsigned bits, std::string_view flag,
                           std::string_view name) {
        const bool present = iframe && code.has_value();
        w.write(1, present ? 1U : 0U, flag);
        if (present) {
            w.write(bits, static_cast<std::uint64_t>(*code), name);
        }
        return present;
    };
    value(codes.loudrelgat, 11, "b_loudrelgat", "loudrelgat");
    if (value(codes.loudspchgat, 11, "b_loudspchgat", "loudspchgat")) {
        w.write(3, static_cast<std::uint64_t>(codes.speech_dialgate_prac_type),
                "dialgate_prac_type");
    }
    w.write(1, 0, "b_loudstrm3s");
    value(codes.max_loudstrm3s, 11, "b_max_loudstrm3s", "max_loudstrm3s");
    w.write(1, 0, "b_truepk");
    value(codes.max_truepk, 11, "b_max_truepk", "max_truepk");
    w.write(1, 0, "b_prgmbndy");
    if (value(codes.lra, 10, "b_lra", "lra")) {
        w.write(3, static_cast<std::uint64_t>(codes.lra_prac_type), "lra_prac_type");
    }
    w.write(1, 0, "b_loudmntry");
    value(codes.max_loudmntry, 11, "b_max_loudmntry", "max_loudmntry");
    // sus_ver 1.
    w.write(1, 0, "b_rtllcomp");
    w.write(1, 0, "b_extension");
}

void write_drc_frame(BitWriter& w, const DrcCodes* codes, bool iframe,
                     std::span<const DrcModeGains> gains) {
    if (codes == nullptr || (!iframe && !codes->gains)) {
        w.write(1, 0, "b_drc_present");
        return;
    }
    w.write(1, 1, "b_drc_present");
    if (iframe) {
        // drc_config().
        w.write(3, codes->modes.size() - 1, "drc_decoder_nr_modes");
        for (const DrcModeCodes& mode : codes->modes) {
            w.write(3, static_cast<std::uint64_t>(mode.id), "drc_decoder_mode_id");
            if (mode.id > 3) {
                w.write(5, static_cast<std::uint64_t>(mode.output_level_from),
                        "drc_output_level_from");
                w.write(5, static_cast<std::uint64_t>(mode.output_level_to), "drc_output_level_to");
            }
            w.write(1, mode.repeat_id ? 1U : 0U, "drc_repeat_profile_flag");
            if (mode.repeat_id) {
                w.write(3, static_cast<std::uint64_t>(*mode.repeat_id), "drc_repeat_id");
                continue;
            }
            w.write(1, mode.default_profile ? 1U : 0U, "drc_default_profile_flag");
            if (!mode.default_profile) {
                w.write(1, mode.gains_config ? 0U : 1U, "drc_compression_curve_flag");
                if (mode.gains_config) {
                    w.write(2, static_cast<std::uint64_t>(*mode.gains_config), "drc_gains_config");
                } else {
                    write_curve(w, *mode.curve);
                }
            }
        }
        w.write(3, static_cast<std::uint64_t>(codes->eac3_profile), "drc_eac3_profile");
    }
    // drc_data() (Table 74): a gainset for each mode that sends gains, a
    // repeat taking the mode it repeats; and after them, where any mode has
    // a curve, the reset flag.
    bool curve = false;
    for (std::size_t m = 0; m < codes->modes.size(); ++m) {
        const DrcModeCodes* mode = &codes->modes[m];
        if (mode->repeat_id) {
            // The id is read once: the mode found repeats nothing, so its own
            // repeat_id is empty.
            const int repeated = *mode->repeat_id;
            for (const DrcModeCodes& other : codes->modes) {
                if (other.id == repeated) {
                    mode = &other;
                }
            }
        }
        if (!mode->gains_config) {
            curve = true;
            continue;
        }
        // drc_gains() (Table 75), under the reading src/ac4dec/ERRATA.md
        // "drc_gains() is a brace short" takes; drc_gainset_size counts
        // drc_version, as bits_left's formula does (src/ac4enc/ERRATA.md,
        // "drc_gainset_size counts drc_version").
        const DrcModeGains& set = gains[m];
        BitWriter body = BitWriter::buffered();
        body.write(7, static_cast<std::uint64_t>(set.at(0, 0, 0) + 64), "drc_gain_val");
        if (*mode->gains_config > 0) {
            int ref = set.at(0, 0, 0);
            for (int ch = 0; ch < set.groups; ++ch) {
                for (int band = 0; band < set.bands; ++band) {
                    for (int sf = 0; sf < set.subframes; ++sf) {
                        if (sf != 0 || band != 0 || ch != 0) {
                            body.write_codeword(
                                tables::kDrcHcbCodes,
                                static_cast<std::size_t>(set.at(ch, sf, band) - ref +
                                                         tables::kDrcHcb.cb_off),
                                "drc_gain_code");
                        }
                        ref = set.at(ch, sf, band);
                    }
                    ref = set.at(ch, 0, band);
                }
                ref = set.at(ch, 0, 0);
            }
        }
        const std::size_t size = 2 + body.bit_position();
        w.write(6, size & 63U, "drc_gainset_size_value");
        w.write(1, size > 63 ? 1U : 0U, "b_more_bits");
        if (size > 63) {
            w.write_variable_bits(2, size >> 6U, "drc_gainset_size");
        }
        w.write(2, 0, "drc_version");
        w.append(body);
    }
    if (curve) {
        w.write(1, 0, "drc_reset_flag");
        w.write(2, 0, "drc_reserved");
    }
}

int bs_ch_config(const PresentationChannels& p) noexcept {
    if (p.ch_mode < 11 || p.ch_mode > 14) {
        return -1;
    }
    const bool nine = p.ch_mode >= 13;
    if (p.top_channel_pairs == 2) {
        if (nine) {
            return p.back ? 0 : -1;
        }
        return p.back ? 1 : 2;
    }
    if (p.top_channel_pairs == 1) {
        if (nine) {
            return p.back ? 3 : -1;
        }
        return p.back ? 4 : 5;
    }
    return -1;
}

void write_downmix(BitWriter& w, const PresentationChannels& p, const DownmixCodes* codes,
                   bool iframe) {
    const DownmixCodes* sent = iframe ? codes : nullptr;
    const int ch_mode = p.ch_mode;
    const bool has_lfe = p.lfe;
    // custom_dmx_data(): the immersive channel modes' bs_ch_config first, and
    // where the height downmix is configured one configuration for
    // out_ch_config 0, 5.X.0 (clause 6.2.9.3): tool_t4_to_f_s() for 5.X.4 and
    // 7.X.4, and for 7.X.4 tool_b4_to_b2(). The X.2 configurations, which the
    // encoder does not write, would take tool_t2_to_f_s().
    const int bs = bs_ch_config(p);
    if (bs >= 0) {
        const bool custom = sent != nullptr && sent->height && (bs == 1 || bs == 2);
        w.write(1, custom ? 1U : 0U, "b_cdmx_data_present");
        if (custom) {
            const HeightDownmixCodes& h = *sent->height;
            w.write(2, 0, "n_cdmx_configs_minus1");
            w.write(bs == 2 ? 1U : 3U, 0, "out_ch_config");
            w.write(1, h.top_front_to_front ? 1U : 0U, "b_top_front_to_front");
            w.write(3, static_cast<std::uint64_t>(h.top_front_code),
                    h.top_front_to_front ? "gain_t2a_code" : "gain_t2b_code");
            w.write(1, h.top_back_to_front ? 1U : 0U, "b_top_back_to_front");
            w.write(3, static_cast<std::uint64_t>(h.top_back_code),
                    h.top_back_to_front ? "gain_t2d_code" : "gain_t2e_code");
            if (bs == 1) {
                w.write(3, static_cast<std::uint64_t>(h.back_code), "gain_b_code");
            }
        }
    }
    if (ch_mode >= 3 || p.ch_mode_core >= 3) {
        w.write(1, sent != nullptr ? 1U : 0U, "b_stereo_dmx_coeff");
        if (sent != nullptr) {
            w.write(3, static_cast<std::uint64_t>(sent->loro_centre_mixgain),
                    "loro_centre_mixgain");
            w.write(3, static_cast<std::uint64_t>(sent->loro_surround_mixgain),
                    "loro_surround_mixgain");
            w.write(1, sent->ltrt_mixgain ? 1U : 0U, "b_ltrt_mixinfo");
            if (sent->ltrt_mixgain) {
                w.write(3, static_cast<std::uint64_t>((*sent->ltrt_mixgain)[0]),
                        "ltrt_centre_mixgain");
                w.write(3, static_cast<std::uint64_t>((*sent->ltrt_mixgain)[1]),
                        "ltrt_surround_mixgain");
            }
            if (has_lfe) {
                w.write(1, sent->lfe_mixgain ? 1U : 0U, "b_lfe_mixinfo");
                if (sent->lfe_mixgain) {
                    w.write(5, static_cast<std::uint64_t>(*sent->lfe_mixgain), "lfe_mixgain");
                }
            }
            w.write(2, static_cast<std::uint64_t>(sent->preferred_dmx_method),
                    "preferred_dmx_method");
        }
    }
    // loud_corr(pres_ch_mode, pres_ch_mode_core, 0), with no corrections for
    // the immersive outputs (b_corr_for_immersive_out 0).
    if (ch_mode > 4) {
        w.write(1, 0, "b_corr_for_immersive_out");
    }
    if (ch_mode > 1) {
        const std::optional<int> loro = sent != nullptr ? sent->loro_dmx_loud_corr : std::nullopt;
        const std::optional<int> ltrt = sent != nullptr ? sent->ltrt_dmx_loud_corr : std::nullopt;
        w.write(1, loro ? 1U : 0U, "b_loro_loud_comp");
        if (loro) {
            w.write(5, static_cast<std::uint64_t>(*loro), "loro_dmx_loud_corr");
        }
        w.write(1, ltrt ? 1U : 0U, "b_ltrt_loud_comp");
        if (ltrt) {
            w.write(5, static_cast<std::uint64_t>(*ltrt), "ltrt_dmx_loud_corr");
        }
    }
    if (ch_mode > 4) {
        w.write(1, 0, "b_loud_comp");  // loud_corr_5_X
    }
    if (p.ch_mode_core >= 5) {
        w.write(1, 0, "b_loud_comp");  // loud_corr_core_5_X_2
    }
    if (p.ch_mode_core >= 3) {
        w.write(1, 0, "b_loud_comp");  // loud_corr_core_5_X
        w.write(1, 0, "b_loud_comp");  // loud_corr_core_loro and _ltrt
    }
}

void write_dialog_enhancement(BitWriter& w, const DeConfigCodes* config,
                              const DeFrameParameters* parameters,
                              const DeFrameParameters* previous, bool iframe) {
    w.write(1, config != nullptr ? 1U : 0U, "b_de_data_present");
    if (config == nullptr) {
        return;
    }
    if (iframe) {
        w.write(2, static_cast<std::uint64_t>(config->method), "de_method");
        w.write(2, static_cast<std::uint64_t>(config->max_gain), "de_max_gain");
        w.write(3, static_cast<std::uint64_t>(config->channel_config), "de_channel_config");
    } else {
        w.write(1, 0, "b_de_config_flag");
    }
    // de_data(de_method, de_nr_channels, b_iframe), Part 1 Table 78: in the
    // cross-channel method the panning, kept where it is the last frame's;
    // then the parameters, of each channel or with de_ms_proc_flag of the
    // Mid alone, kept where they are the last frame's.
    const int nr_channels = de_channel_count(config->channel_config);
    if (nr_channels == 0) {
        return;
    }
    const DeFrameParameters& frame = *parameters;
    const bool cross = config->method == 1 || config->method == 3;
    if (cross && nr_channels > 1) {
        bool keep_pos = false;
        if (!iframe) {
            keep_pos = previous != nullptr && frame.mix == previous->mix;
            w.write(1, keep_pos ? 1U : 0U, "de_keep_pos_flag");
        }
        if (!keep_pos) {
            w.write(5, static_cast<std::uint64_t>(frame.mix[0]), "de_mix_coef1_idx");
            if (nr_channels == 3) {
                w.write(5, static_cast<std::uint64_t>(frame.mix[1]), "de_mix_coef2_idx");
            }
        }
    }
    const bool ms = (config->method == 0 || config->method == 2) && nr_channels == 2 && config->mid;
    const auto channels = static_cast<std::size_t>(nr_channels - (ms ? 1 : 0));
    const bool hybrid = config->method >= 2;
    bool keep = false;
    if (!iframe) {
        keep =
            previous != nullptr &&
            std::equal(frame.par.begin(), frame.par.begin() + static_cast<std::ptrdiff_t>(channels),
                       previous->par.begin()) &&
            (!hybrid || frame.signal_contribution == previous->signal_contribution);
        w.write(1, keep ? 1U : 0U, "de_keep_data_flag");
    }
    if (keep) {
        return;
    }
    if ((config->method == 0 || config->method == 2) && nr_channels == 2) {
        w.write(1, ms ? 1U : 0U, "de_ms_proc_flag");
    }
    const std::array<std::array<int, kDeBands>, 3>& par = frame.par;
    const bool second = config->method % 2 != 0;
    const std::span<const HuffCode> abs_codes =
        second ? std::span<const HuffCode>(tables::kDeHcbAbs1Codes)
               : std::span<const HuffCode>(tables::kDeHcbAbs0Codes);
    const std::span<const HuffCode> diff_codes =
        second ? std::span<const HuffCode>(tables::kDeHcbDiff1Codes)
               : std::span<const HuffCode>(tables::kDeHcbDiff0Codes);
    const int abs_off = second ? tables::kDeHcbAbs1.cb_off : tables::kDeHcbAbs0.cb_off;
    const int diff_off = second ? tables::kDeHcbDiff1.cb_off : tables::kDeHcbDiff0.cb_off;
    const auto code = [&](std::span<const HuffCode> codes, int value) {
        w.write_codeword(codes, static_cast<std::size_t>(value), "de_par_code");
    };
    int ref = 0;
    for (std::size_t ch = 0; ch < channels; ++ch) {
        const std::array<int, kDeBands>& row = par[ch];
        if (iframe && ch == 0) {
            code(abs_codes, row[0] + abs_off);
            ref = row[0];
            for (std::size_t band = 1; band < kDeBands; ++band) {
                code(diff_codes, row[band] - ref + diff_off);
                ref = row[band];
            }
        } else {
            for (std::size_t band = 0; band < kDeBands; ++band) {
                if (iframe) {
                    // Part 1's reading, src/ac4dec/ERRATA.md "de_data() predicts
                    // from the wrong channel": along the channel's own bands.
                    code(diff_codes, row[band] - ref + diff_off);
                    ref = row[band];
                } else {
                    code(diff_codes, row[band] - previous->par[ch][band] + diff_off);
                }
            }
        }
        ref = row[0];
    }
    if (hybrid) {
        w.write(5, static_cast<std::uint64_t>(frame.signal_contribution), "de_signal_contribution");
    }
}

void write_presentation_mix(BitWriter& w, const PresentationMixCodes& codes) {
    if (codes.n_substream_groups > 1) {
        w.write(1, codes.sg_gain ? 1U : 0U, "b_substream_group_gains_present");
        if (codes.sg_gain) {
            w.write(1, codes.keep ? 1U : 0U, "b_keep");
            if (!codes.keep) {
                for (int sg = 0; sg < codes.n_substream_groups; ++sg) {
                    const auto at = static_cast<std::size_t>(sg);
                    w.write(6, static_cast<std::uint64_t>(at < codes.sg_gain->size() ? (*codes.sg_gain)[at] : 0),
                            "sg_gain");
                }
            }
        }
    }
    w.write(1, codes.associated ? 1U : 0U, "b_associated");
    if (!codes.associated) {
        return;
    }
    const AssociatedMixCodes& a = *codes.associated;
    const auto optional_8 = [&w](const std::optional<int>& value, std::string_view flag, std::string_view name) {
        w.write(1, value ? 1U : 0U, flag);
        if (value) {
            w.write(8, static_cast<std::uint64_t>(*value), name);
        }
    };
    optional_8(a.scale_main, "b_scale_main", "scale_main");
    optional_8(a.scale_main_centre, "b_scale_main_centre", "scale_main_centre");
    optional_8(a.scale_main_front, "b_scale_main_front", "scale_main_front");
    optional_8(a.pan_associated, "b_associate_is_mono", "pan_associated");
}

void write_extended_metadata(BitWriter& w, int ch_mode, const DialogueMixCodes* dialogue) {
    w.write(1, dialogue != nullptr ? 1U : 0U, "b_dialog");
    if (dialogue != nullptr) {
        w.write(1, dialogue->dialog_max_gain ? 1U : 0U, "b_dialog_max_gain");
        if (dialogue->dialog_max_gain) {
            w.write(2, static_cast<std::uint64_t>(*dialogue->dialog_max_gain), "dialog_max_gain");
        }
        w.write(1, dialogue->pan_dialog ? 1U : 0U, "b_pan_dialog_present");
        if (dialogue->pan_dialog) {
            w.write(8, static_cast<std::uint64_t>((*dialogue->pan_dialog)[0]), "pan_dialog");
            if (ch_mode != 0) {
                w.write(8, static_cast<std::uint64_t>((*dialogue->pan_dialog)[1]), "pan_dialog");
                w.write(2, static_cast<std::uint64_t>(dialogue->pan_signal_selector), "pan_signal_selector");
            }
        }
    }
    w.write(1, 0, "b_channels_classifier");
    w.write(1, 0, "b_event_probability");
}

void write_emdf_payloads(BitWriter& w, std::span<const EmdfPayloadCodes> payloads) {
    const auto escaped = [&w](unsigned bits, std::uint64_t value, std::string_view name) {
        // A 5-bit id whose all-ones value escapes to variable_bits(5) for
        // what is above it.
        const std::uint64_t escape = (std::uint64_t{1} << bits) - 1;
        w.write(bits, std::min(value, escape), name);
        if (value >= escape) {
            w.write_variable_bits(bits, value - escape, name);
        }
    };
    for (const EmdfPayloadCodes& p : payloads) {
        escaped(5, p.id, "emdf_payload_id");
        // emdf_payload_config() (Part 1 Table 79).
        w.write(1, p.smpoffst ? 1U : 0U, "b_smpoffst");
        if (p.smpoffst) {
            w.write_variable_bits(11, *p.smpoffst, "smpoffst");
        }
        w.write(1, p.duration ? 1U : 0U, "b_duration");
        if (p.duration) {
            w.write_variable_bits(11, *p.duration, "duration");
        }
        w.write(1, p.groupid ? 1U : 0U, "b_groupid");
        if (p.groupid) {
            w.write_variable_bits(2, *p.groupid, "groupid");
        }
        w.write(1, p.codecdata ? 1U : 0U, "b_codecdata");
        if (p.codecdata) {
            w.write(8, static_cast<std::uint64_t>(*p.codecdata), "codecdata");
        }
        w.write(1, p.discard_unknown ? 1U : 0U, "b_discard_unknown_payload");
        if (!p.discard_unknown) {
            bool aligned = false;
            if (!p.smpoffst) {
                aligned = p.frame_aligned;
                w.write(1, aligned ? 1U : 0U, "b_payload_frame_aligned");
                if (aligned) {
                    w.write(1, p.create_duplicate ? 1U : 0U, "b_create_duplicate");
                    w.write(1, p.remove_duplicate ? 1U : 0U, "b_remove_duplicate");
                }
            }
            if (p.smpoffst || aligned) {
                w.write(5, static_cast<std::uint64_t>(p.priority), "priority");
                w.write(2, static_cast<std::uint64_t>(p.proc_allowed), "proc_allowed");
            }
        }
        w.write_variable_bits(8, p.bytes.size(), "emdf_payload_size");
        for (const std::uint8_t b : p.bytes) {
            w.write(8, b, "emdf_payload_byte");
        }
    }
    // while (emdf_payload_id != 0): the 0 that ends the list, and nothing
    // after it but the alignment (src/ac4dec/ERRATA.md, "The end of an EMDF
    // payload list").
    w.write(5, 0, "emdf_payload_id");
    w.align();
}

void write_alternative(BitWriter& w, const AlternativeCodes& codes) {
    // The name as one chunk (Part 2 clause 6.3.3.1.4): its bytes and a 0, so
    // that byte[name_len - 1] = 0 says the name is whole; name_len in 5 bits,
    // or 32 bytes without it.
    w.write(1, codes.name.empty() ? 0U : 1U, "b_name_present");
    if (!codes.name.empty()) {
        const std::size_t name_len = codes.name.size() + 1;
        w.write(1, name_len < 32 ? 1U : 0U, "b_length");
        if (name_len < 32) {
            w.write(5, name_len, "name_len");
        }
        for (const std::uint8_t b : codes.name) {
            w.write(8, b, "presentation_name");
        }
        w.write(8, 0, "presentation_name");
    }
    // One target: the presentation's level, every device category, and every
    // substream active with no alternative data set (src/ac4enc/ERRATA.md,
    // "An alternative presentation's target").
    w.write(2, 0, "n_targets_minus1");
    w.write(3, static_cast<std::uint64_t>(codes.target_level), "target_level");
    w.write(4, 0b1111, "target_device_category");
    w.write(1, 0, "b_tdc_extension");
    w.write(1, 0, "b_ducking_depth_present");
    w.write(1, 0, "b_loud_corr_target");
    for (int sus = 0; sus < codes.substreams; ++sus) {
        w.write(1, 1, "b_active");
        w.write(1, 0, "alt_data_set_index");
    }
}

}  // namespace ac4::detail
