#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ac3/export.hpp"

// Named loudness/true-peak delivery gates a decoded stream's measurement can
// be checked against - roadmap items C2 (`ac3cli qc`) and IO11. Each preset
// states a target integrated loudness, how that target is enforced, a true
// peak ceiling, and the document/clause/date it was read out of - see
// qc_preset()'s own comment on each case for the exact wording cited; every
// number here was read out of the primary document, not recalled from memory.
//
// Deliberately the same shape ac3::meta::Profile/ProfileId (drc.hpp) uses for
// the §7.7.1 DRC profile table: a small enum naming the presets, a constexpr
// accessor returning the numbers, and a name<->id parser - so a caller
// (ac3cli qc, and the GUI's own QC panel) reads one table instead of
// hand-copying the same magic numbers more than once.
//
// Three specifications considered for this table are deliberately NOT in it,
// because adding them would mean shipping a second name for a verdict already
// on offer:
//
//   * EBU R 128 s4 "Loudness Normalisation of Cinematic Content" (Geneva,
//     November 2023). Its recommendation (m) normalises Programme Loudness to
//     "a Target Level of -23.0 LUFS" and defers tolerances to R 128 itself,
//     and its recommendation (l) repeats the -1 dBTP ceiling - so numerically
//     it IS ebu-r128-s2 below. What s4 adds that R 128 does not have is
//     recommendation (j), a Loudness-to-Dialogue Ratio "not exceed[ing] 5 LU"
//     - a Programme-minus-Dialogue Loudness figure, and LoudnessMeter has no
//     dialogue gate, so the one clause that would distinguish an s4 preset is
//     also the one clause this meter cannot evaluate.
//
//   * Netflix "Dolby Atmos Home Mix Deliverable Requirements" v2.3. Its
//     loudness spec is "-27db LKFS +/- 2 LU 1770-1 dialog-gated" with peaks
//     that "should generally not exceed -2dBFS true-peak", which is exactly
//     the kNetflix row below. What it adds is scope, not numbers: "Loudness
//     and peaks should be measured via a 5.1 rerender." That is a choice of
//     what to meter rather than what to compare against, so it belongs to
//     `ac3cli qc`'s own layout= switch, not to a preset.
//
//   * Amazon. Figures for Prime Video delivery are widely repeated at
//     -24 LKFS/-2 dBTP, but every source found for them is a third-party
//     summary; Amazon's own delivery specifications sit behind a partner
//     portal. Nothing here is cited to a document that was not read, so this
//     row is absent rather than guessed - and -24/+/-2/-2 would in any case
//     restate kAtscA85.

namespace ac3::meta {

// How a preset enforces its loudness target. Most delivery specs state a
// target to hit and a symmetric tolerance around it; a distribution platform
// that normalises on playback instead states only a level not to exceed, and
// quieter content is compliant however quiet it is. Gating a ceiling as a
// band would fail material the specification actually accepts, so the two
// cases are distinguished rather than approximated.
enum class QcLoudnessLimit : std::uint8_t {
    kBand,     // |measured - target| <= tolerance
    kCeiling,  // measured <= target; tolerance_lu is 0 and unused
};

struct QcPreset {
    double target_lkfs = 0.0;
    double tolerance_lu = 0.0;        // +/- around target_lkfs, kBand only
    double max_true_peak_dbtp = 0.0;  // a ceiling, not a tolerance band
    QcLoudnessLimit loudness_limit = QcLoudnessLimit::kBand;
    // The primary document this row was read out of, with its version and
    // date - printed beside the verdict so a report says which edition it was
    // judged against rather than just a preset nickname.
    std::string_view source = {};
};

enum class QcPresetId : std::uint8_t {
    kEbuR128S2,
    kAtscA85,
    kAtscA85Streaming,
    kNetflix,
    kAppleMusicAtmos,
};

// The numbers themselves, each with its primary source cited beside the
// value it backs - the same layout drc.hpp's profile() uses for its derived
// boost edges.
[[nodiscard]] constexpr QcPreset qc_preset(QcPresetId id) {
    switch (id) {
        case QcPresetId::kEbuR128S2:
            // EBU R 128 s2 "Loudness in Streaming" (Geneva, November 2023,
            // v3) recommendation (e): "programmes should be streamed
            // unchanged, that is at -23.0 LUFS" - s2 itself defers tolerance
            // and true peak to the parent recommendation ("for production
            // and QC tolerances, also refer to [1]"). EBU R 128 (Geneva,
            // November 2023, v5) recommendation (h): "a tolerance of +/-1.0
            // LU is permitted" where hitting the Target Level is "not
            // achievable practically"; recommendation (m): "the True Peak
            // Level of a programme shall not exceed -1 dBTP during
            // production".
            return {.target_lkfs = -23.0,
                    .tolerance_lu = 1.0,
                    .max_true_peak_dbtp = -1.0,
                    .loudness_limit = QcLoudnessLimit::kBand,
                    .source = "EBU R 128 s2 v3 + R 128 v5 (November 2023)"};
        case QcPresetId::kAtscA85:
            // ATSC A/85:2026-07 "Techniques for Establishing and Maintaining
            // Audio Loudness for Digital Television" (approved 8 July 2026,
            // the first full revision since A/85:2013) Section 6 "Target
            // Loudness and True Peak Levels for Content Delivery or
            // Exchange": "For delivery or exchange of Content without
            // metadata (and where there is no prior arrangement by the
            // parties regarding Loudness), the Target Loudness value should
            // be -24 LKFS. Measurement tolerance of up to approximately
            // +/-2 dB around this value is anticipated... The True Peak level
            // should be kept below -2 dBTP".
            //
            // The 2026 revision restates all three numbers unchanged from
            // A/85:2013 (with Corrigendum No. 1, 11 February 2021), which
            // this row previously cited; only the citation moves. Annex M's
            // Table M.1 quick reference repeats the same -24 LKFS / -2 dBTP
            // pair.
            return {.target_lkfs = -24.0,
                    .tolerance_lu = 2.0,
                    .max_true_peak_dbtp = -2.0,
                    .loudness_limit = QcLoudnessLimit::kBand,
                    .source = "ATSC A/85:2026-07 Sec. 6 (8 July 2026)"};
        case QcPresetId::kAtscA85Streaming:
            // The same document's Annex L "Guidelines for Establishing and
            // Maintaining Audio Loudness of Internet Streaming Services When
            // Using Metadata-based and Non-metadata-based Codecs", new in the
            // 2026 revision. L.5: "It is strongly recommended that all
            // Content (Long-form and Short-form) on a Streaming delivery
            // service be presented at only one specific and consistent Target
            // Loudness. Selecting a Loudness value between -23 and -27 LKFS
            // is recommended, unless there is a prior arrangement otherwise."
            // Annex M repeats it: "For Streaming delivery services only, a
            // single Target Loudness between -23 to -27 LKFS is recommended".
            //
            // The primary datum is the BAND, not a point: [-27, -23]. This
            // table's shape is target-plus-tolerance, and -25.0 +/-2.0 is
            // that band exactly, so nothing is lost or invented in the
            // translation - but the midpoint is an artefact of the encoding
            // and is NOT a level the Annex asks anyone to aim for (it names
            // -23, -24 and -27 as the values real operators use). True peak
            // is Section 6's -2 dBTP, which Annex M's Table M.1 applies to
            // Long-form and Short-form alike with no streaming carve-out.
            return {.target_lkfs = -25.0,
                    .tolerance_lu = 2.0,
                    .max_true_peak_dbtp = -2.0,
                    .loudness_limit = QcLoudnessLimit::kBand,
                    .source = "ATSC A/85:2026-07 Annex L.5 (8 July 2026)"};
        case QcPresetId::kNetflix:
            // Netflix "Sound Mix Specifications & Best Practices" v1.6
            // (partnerhelp.netflixstudios.com), Near-field Audio
            // Prerequisites for Mix Facilities: "Set average loudness at -27
            // LKFS with a tolerance of +/-2 LU, dialog-gated. Peaks must not
            // exceed -2dB True Peak."
            //
            // Netflix's "Dolby Atmos Home Mix Deliverable Requirements" v2.3
            // states the same three numbers for an Atmos deliverable and adds
            // that "Loudness and peaks should be measured via a 5.1
            // rerender" - see this header's own opening note on why that is a
            // layout= choice rather than a second preset.
            //
            // Both documents say dialog-gated, which LoudnessMeter is not: it
            // measures programme loudness. On material whose anchor element
            // is the whole mix the two coincide, and on dialogue-led material
            // this gate reads high - a known conservatism, not a silent
            // approximation.
            return {.target_lkfs = -27.0,
                    .tolerance_lu = 2.0,
                    .max_true_peak_dbtp = -2.0,
                    .loudness_limit = QcLoudnessLimit::kBand,
                    .source = "Netflix Sound Mix Specs v1.6 / Atmos Home Mix v2.3"};
        case QcPresetId::kAppleMusicAtmos:
            // Apple "Immersive Audio Source Profile" (Apple Video and Audio
            // Asset Guide, help.apple.com/itc/videoaudioassetguide), Dolby
            // Atmos music deliverables: "The integrated loudness value should
            // not exceed -18 LKFS measured as per ITU-R BS. 1770-4" and
            // "True-peak level should not exceed -1 dB TP measured as per
            // ITU-R BS. 1770-4".
            //
            // "should not exceed" on BOTH halves, so loudness is a ceiling
            // here and not a band - a -25 LKFS master satisfies this clause,
            // and a +/-band around -18 would wrongly fail it. This is the row
            // QcLoudnessLimit::kCeiling exists for.
            return {.target_lkfs = -18.0,
                    .tolerance_lu = 0.0,
                    .max_true_peak_dbtp = -1.0,
                    .loudness_limit = QcLoudnessLimit::kCeiling,
                    .source = "Apple Immersive Audio Source Profile (BS.1770-4)"};
    }
    return {};
}

[[nodiscard]] constexpr std::string_view qc_preset_name(QcPresetId id) {
    switch (id) {
        case QcPresetId::kEbuR128S2: return "ebu-r128-s2";
        case QcPresetId::kAtscA85: return "atsc-a85";
        case QcPresetId::kAtscA85Streaming: return "atsc-a85-streaming";
        case QcPresetId::kNetflix: return "netflix";
        case QcPresetId::kAppleMusicAtmos: return "apple-music-atmos";
    }
    return "";
}

// Names accepted on the command line, in QcPresetId order.
inline constexpr std::string_view kQcPresetNames =
    "ebu-r128-s2 | atsc-a85 | atsc-a85-streaming | netflix | apple-music-atmos";

[[nodiscard]] AC3FORGE_EXPORT bool parse_qc_preset(std::string_view name, QcPresetId& out);

// Every preset, in declaration order - for a caller that wants to check a
// measurement against all of them (ac3cli qc's own preset=all).
inline constexpr std::array<QcPresetId, 5> kQcPresetIds{
    QcPresetId::kEbuR128S2, QcPresetId::kAtscA85, QcPresetId::kAtscA85Streaming,
    QcPresetId::kNetflix, QcPresetId::kAppleMusicAtmos};

// One preset's verdict against one measurement. Loudness gates on the
// preset's own QcLoudnessLimit - a band around the target, or a ceiling the
// measurement must not exceed; true peak always gates on not exceeding the
// ceiling (a one-sided limit, matching every source cited in qc_preset()
// above). Either half is left at its not-passing default when the
// corresponding measurement itself is std::nullopt - LoudnessMeter's own "no
// meaningful loudness"/"no sample yet" stance on material this gate cannot
// actually judge.
struct QcVerdict {
    std::optional<double> loudness_delta_lu;      // measured - target
    bool loudness_pass = false;
    std::optional<double> true_peak_margin_dbtp;  // ceiling - measured; >= 0 passes
    bool true_peak_pass = false;

    [[nodiscard]] bool pass() const { return loudness_pass && true_peak_pass; }
};

[[nodiscard]] AC3FORGE_EXPORT QcVerdict evaluate_qc_gate(const QcPreset& preset,
                                                         std::optional<double> integrated_lkfs,
                                                         std::optional<double> true_peak_dbtp);

}  // namespace ac3::meta
