#include "analysis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fstream>
#include <ios>
#include <iostream>
#include <istream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../platform/stdio_binary.hpp"
#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/qc.hpp"
#include "ac3/sinks/iec61937.hpp"

namespace ac3cli::commands {

namespace {

// --- qc (roadmap C2) --------------------------------------------------------
// Bitstream-aware loudness QC: decode a whole stream, measure it with the
// real BS.1770-4/EBU Tech 3342 meter (the same ac3::meta::LoudnessMeter
// dialnorm=auto already uses), and compare the result against what the
// stream's own dialnorm/compr claim and, optionally, a named delivery-spec
// gate (ac3::meta::qc_preset - see ac3/meta/qc.hpp for the cited sources).

// One decoded programme this command measures and reports on - the whole
// soundfield for every layout except 1+1 dual mono, which is two of these
// (Ch1, Ch2): §E1.3 makes them unrelated, unmixed programmes sharing one
// syncframe rather than a single soundfield BS.1770 could measure as one, the
// same reason measured_dialnorm_channel exists alongside measured_dialnorm
// above.
struct QcProgrammeResult {
    std::string_view label = {};  // "" (whole programme) or "Ch1"/"Ch2" for 1+1
    // Every field below has an explicit default member initializer, even the
    // ones std::optional's own default constructor would already give -
    // every construction of this type in this file is a PARTIAL designated
    // initializer (only the fields relevant at that call site named), and
    // GCC's -Wmissing-field-initializers (on under -Wextra, and this project
    // builds -Werror) fires on any member without one, regardless of what
    // its type's own default constructor would produce.
    std::optional<double> integrated_lkfs = std::nullopt;
    std::optional<double> lra_lu = std::nullopt;
    std::optional<double> true_peak_dbtp = std::nullopt;
    int dialnorm = 31;
    std::optional<std::uint8_t> compr = std::nullopt;
};

struct QcResult {
    std::string_view codec_label;  // "AC-3" / "E-AC-3"
    std::string_view unit_label;   // "frame(s)" / "access unit(s)"
    std::string layout_label;
    std::uint32_t sample_rate_hz = 0;
    std::size_t unit_count = 0;
    double seconds = 0.0;
    // Which of the two BS.1770 algorithms produced the figures below - Annex
    // 3's extended one over the whole rendered program (layout=rendered), or
    // Annex 1's basic one over the Table 5.8 bed (layout=bed). Reported, not
    // just chosen: the same stream can legitimately measure differently
    // through the two, and a QC figure without its algorithm is ambiguous.
    bool rendered = false;
    // layout=bed only: the stream carried at least one dependent substream,
    // whose channels this measurement therefore never saw. Drives run_qc's
    // hint that layout=rendered has more to measure - silence about it would
    // read as "5.1 is all there is".
    bool bed_hid_dependents = false;
    std::vector<QcProgrammeResult> programmes;
};

// A rendered layout's own name. Table E2.5 has no short label the way Table
// 5.8's acmods do (there is no "5.1.4" in the bitstream, only a bit mask), so
// the locations are listed in coded order - the same naming run_levels_eac3
// gives each of its channel rows.
std::string rendered_layout_label(const ac3::eac3::chanmap::Layout& layout) {
    std::string out;
    for (int ch = 0; ch < layout.count; ++ch) {
        if (!out.empty()) {
            out += ' ';
        }
        out += ac3::eac3::chanmap::name(layout[ch]);
    }
    return out;
}

// AC-3 (bsid <= 8): straightforward per-frame decode, same loop shape as
// run_decode above, feeding ac3::meta::LoudnessMeter instead of accumulating
// PCM - qc never writes audio out, so there is nothing to buffer.
std::optional<QcResult> measure_qc_ac3(std::span<const std::byte> stream, bool rendered) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        fmt::println(stderr, "error: not a valid AC-3 stream");
        return std::nullopt;
    }
    ac3::FrameDecoder decoder;
    QcResult result;
    result.codec_label = "AC-3";
    result.unit_label = "frame(s)";
    result.unit_count = frames->size();
    result.rendered = rendered;

    bool have_first = false;
    bool dual_mono = false;
    std::optional<ac3::meta::LoudnessMeter> meter;      // whole programme
    std::optional<ac3::meta::LoudnessMeter> meter_ch1;  // dual mono only
    std::optional<ac3::meta::LoudnessMeter> meter_ch2;

    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
        if (!decoded) {
            fmt::println(stderr, "error: {}", ac3::describe(decoded.error()));
            return std::nullopt;
        }
        if (!have_first) {
            have_first = true;
            dual_mono = decoded->acmod == ac3::Acmod::kDualMono;
            result.sample_rate_hz = sample_rate_hz(decoded->sample_rate);
            if (dual_mono) {
                result.layout_label = "1+1 dual mono";
                meter_ch1.emplace(decoded->sample_rate, ac3::Acmod::k1_0, false);
                meter_ch2.emplace(decoded->sample_rate, ac3::Acmod::k1_0, false);
                result.programmes.push_back(
                    QcProgrammeResult{.label = "Ch1", .dialnorm = decoded->dialnorm,
                                      .compr = decoded->compr});
                result.programmes.push_back(QcProgrammeResult{
                    .label = "Ch2", .dialnorm = decoded->dialnorm2.value_or(31),
                    .compr = decoded->compr2});
            } else if (rendered) {
                // AC-3 has no dependent substreams, so its rendered layout is
                // its bed - the same channels either way. What changes is the
                // algorithm: Annex 3 weights by position, which for every
                // Table 5.8 layout with a discrete surround PAIR agrees with
                // Annex 1 channel for channel. The one place the two really
                // differ is the lone surround of 2/1 and 3/1: Annex 1 reads
                // it as the surround field (+1.5 dB), Annex 3 as Table E2.5's
                // Cs at 180 degrees (unity). See LoudnessMeter's own
                // constructor comments.
                const auto layout = ac3::eac3::chanmap::expand(
                    ac3::eac3::chanmap::acmod_map(decoded->acmod, decoded->lfe));
                result.layout_label = rendered_layout_label(layout);
                meter.emplace(decoded->sample_rate, layout);
                result.programmes.push_back(
                    QcProgrammeResult{.dialnorm = decoded->dialnorm, .compr = decoded->compr});
            } else {
                result.layout_label =
                    std::string{ac3::analysis::layout_name(decoded->acmod, decoded->lfe)};
                meter.emplace(decoded->sample_rate, decoded->acmod, decoded->lfe);
                result.programmes.push_back(
                    QcProgrammeResult{.dialnorm = decoded->dialnorm, .compr = decoded->compr});
            }
        }
        if (dual_mono) {
            const std::array<std::span<const float>, 1> ch1{decoded->channels[0]};
            const std::array<std::span<const float>, 1> ch2{decoded->channels[1]};
            meter_ch1->push(ch1);
            meter_ch2->push(ch2);
        } else {
            std::vector<std::span<const float>> views;
            views.reserve(decoded->channels.size());
            for (const auto& channel : decoded->channels) {
                views.emplace_back(channel);
            }
            meter->push(views);
        }
    }
    // frames is checked non-empty above, so the loop ran at least once and
    // its first iteration always emplaces meter_ch1/meter_ch2 or meter,
    // matching dual_mono - both are always engaged by this point.
    if (dual_mono) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[0].integrated_lkfs = meter_ch1->integrated_lkfs();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[0].lra_lu = meter_ch1->loudness_range();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[0].true_peak_dbtp = meter_ch1->true_peak_dbtp();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[1].integrated_lkfs = meter_ch2->integrated_lkfs();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[1].lra_lu = meter_ch2->loudness_range();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[1].true_peak_dbtp = meter_ch2->true_peak_dbtp();
    } else {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[0].integrated_lkfs = meter->integrated_lkfs();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[0].lra_lu = meter->loudness_range();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[0].true_peak_dbtp = meter->true_peak_dbtp();
    }
    result.seconds = static_cast<double>(result.unit_count) *
                     static_cast<double>(ac3::kSamplesPerFrame) /
                     static_cast<double>(result.sample_rate_hz);
    return result;
}

// E-AC-3 (bsid 11-16): measures the INDEPENDENT substream's own bed audio
// only, never a dependent's - the same "bed acmod/lfe, never the wider
// rendered layout" scope run_encode/run_eac3_encode's own pre-encode
// measured_dialnorm(cp.bed_acmod, cp.bed_lfe, ...) pass already uses (see
// above). BS.1770's channel weighting is defined over Table 5.8 acmod/lfe,
// which a dependent substream's own extension channels (height, wide, Ts,
// etc.) are not members of - the bed is always a Table 5.8 layout, so
// measuring it is what makes this comparable to the encoder's own dialnorm
// derivation for the identical programme. Dual mono (1+1) is always a lone
// independent substream with no dependents (decoder.hpp's own doc comment on
// DecodedAccessUnit), so the same independent-substream-only filtering
// naturally covers it too, exactly like the AC-3 path above.
//
// Walked at the raw-syncframe level (ac3::split_frames, NOT split_access_units
// - decoder.hpp's own doc comment on split_frames says it "handles both
// generations"), calling Eac3Decoder::decode_substream directly on every
// frame so dependent-substream frames are still decoded (consuming their own
// overlap-add state and catching any parse error) even though this only ever
// measures what comes back independent.
// §E2.3.1.2: one programme is measured. A second independent substream is a
// different piece of audio - a commentary, a second language - levelled to
// its own dialnorm, so folding it into the same BS.1770 meter would report a
// loudness neither programme has. `current_programme` tracks the last
// independent substream's own id seen while walking frames in order, because
// a DEPENDENT substream's own substreamid numbers in its parent's space
// (§E2.3.1.2) and says nothing about which programme it belongs to -
// adjacency to the independent substream it follows is the only thing that
// does.
std::optional<QcResult> measure_qc_eac3_bed(std::span<const std::byte> stream, int programme) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        fmt::println(stderr, "error: not a valid E-AC-3 stream");
        return std::nullopt;
    }
    // Heap-allocated (PREfast's C6262, alert #93): Eac3Decoder's per-block
    // scratch members pushed this stack declaration over the threshold -
    // same pattern as examples/atmos_objects.cpp (PR #295).
    auto decoder = std::make_unique<ac3::Eac3Decoder>();
    QcResult result;
    result.codec_label = "E-AC-3";
    result.unit_label = "access unit(s)";

    bool have_first = false;
    bool dual_mono = false;
    std::optional<ac3::meta::LoudnessMeter> meter;
    std::optional<ac3::meta::LoudnessMeter> meter_ch1;
    std::optional<ac3::meta::LoudnessMeter> meter_ch2;
    // The programme the substream CURRENTLY being ingested belongs to - the
    // last independent substream's own id, which a following dependent
    // inherits by adjacency (its own substreamid numbers in its parent's
    // space and says nothing on its own - see this function's own comment).
    // -1 until the first independent substream arrives, which a legal stream
    // always leads with.
    int current_programme = -1;

    // Shared by the main decode loop below and the end-of-stream flush() -
    // both hand this a released, independent-or-dependent DecodedSubstream;
    // only an independent one of the SELECTED programme is ever measured (see
    // this function's own comment above).
    auto ingest = [&](const ac3::DecodedSubstream& sub) {
        if (sub.strmtyp != ac3::eac3::StreamType::kDependent) {
            current_programme = sub.substreamid;
        }
        if (current_programme != programme) {
            // Neither this programme's own frames, nor a hint about them:
            // a dependent belonging to another programme entirely is not
            // "this programme's bed hiding something".
            return;
        }
        if (sub.strmtyp == ac3::eac3::StreamType::kDependent) {
            // Decoded (above) so its overlap-add state advances and its parse
            // errors still surface, but never measured here - and remembered,
            // so the report can say that layout=rendered would have more to
            // measure than this pass just did.
            result.bed_hid_dependents = true;
            return;
        }
        if (!have_first) {
            have_first = true;
            dual_mono = sub.acmod == ac3::Acmod::kDualMono;
            result.sample_rate_hz = sample_rate_hz(sub.sample_rate);
            if (dual_mono) {
                result.layout_label = "1+1 dual mono";
                meter_ch1.emplace(sub.sample_rate, ac3::Acmod::k1_0, false);
                meter_ch2.emplace(sub.sample_rate, ac3::Acmod::k1_0, false);
                result.programmes.push_back(QcProgrammeResult{
                    .label = "Ch1", .dialnorm = sub.dialnorm, .compr = sub.compr});
                result.programmes.push_back(QcProgrammeResult{
                    .label = "Ch2", .dialnorm = sub.dialnorm2.value_or(31), .compr = sub.compr2});
            } else {
                result.layout_label = std::string{ac3::analysis::layout_name(sub.acmod, sub.lfe)};
                meter.emplace(sub.sample_rate, sub.acmod, sub.lfe);
                result.programmes.push_back(
                    QcProgrammeResult{.dialnorm = sub.dialnorm, .compr = sub.compr});
            }
        }
        ++result.unit_count;
        if (dual_mono) {
            const std::array<std::span<const float>, 1> ch1{sub.channels[0]};
            const std::array<std::span<const float>, 1> ch2{sub.channels[1]};
            meter_ch1->push(ch1);
            meter_ch2->push(ch2);
        } else {
            std::vector<std::span<const float>> views;
            views.reserve(sub.channels.size());
            for (const auto& channel : sub.channels) {
                views.emplace_back(channel);
            }
            meter->push(views);
        }
    };

    for (const auto& frame : *frames) {
        const auto decoded = decoder->decode_substream(frame);
        if (!decoded) {
            fmt::println(stderr, "error: decode failed (code {})",
                         static_cast<int>(decoded.error()));
            return std::nullopt;
        }
        // §3.7: this substream's frame is being held back pending transient
        // pre-noise processing (Eac3Decoder::decode_substream's own doc
        // comment) - nothing new to ingest yet, not an error.
        if (decoded->has_value()) {
            ingest(**decoded);
        }
    }
    // Whatever transient pre-noise processing was still holding back at
    // end-of-stream, same convention run_decode_eac3 follows.
    for (const auto& sub : decoder->flush()) {
        ingest(sub);
    }

    if (!have_first) {
        fmt::println(stderr, "error: no independent substream frames for programme {}",
                     programme);
        return std::nullopt;
    }
    // have_first is checked just above and only ingest() sets it, in the same
    // branch that emplaces meter_ch1/meter_ch2 or meter (matching dual_mono),
    // so whichever one dual_mono selects is always engaged by this point.
    if (dual_mono) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[0].integrated_lkfs = meter_ch1->integrated_lkfs();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[0].lra_lu = meter_ch1->loudness_range();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[0].true_peak_dbtp = meter_ch1->true_peak_dbtp();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[1].integrated_lkfs = meter_ch2->integrated_lkfs();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[1].lra_lu = meter_ch2->loudness_range();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[1].true_peak_dbtp = meter_ch2->true_peak_dbtp();
    } else {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[0].integrated_lkfs = meter->integrated_lkfs();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[0].lra_lu = meter->loudness_range();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        result.programmes[0].true_peak_dbtp = meter->true_peak_dbtp();
    }
    result.seconds = static_cast<double>(result.unit_count) *
                     static_cast<double>(ac3::kSamplesPerFrame) /
                     static_cast<double>(result.sample_rate_hz);
    return result;
}

// layout=rendered's E-AC-3 counterpart (roadmap IO10): measures the whole
// assembled program - the independent substream's bed with every dependent's
// height, wide and rear channels laid over it, in Table E2.5 location order -
// through BS.1770-5 Annex 3's extended algorithm, which weights each channel
// by its position rather than by its slot in a Table 5.8 acmod. That is what
// lets 7.1, 5.1.2, 5.1.4 and 7.1.4 be metered at all: those channels are not
// members of Table 5.8, so the bed pass above has no weight to give them and
// simply never sees them.
//
// Walked with ac3::split_access_units and Eac3Decoder::decode_access_unit
// (NOT split_frames/decode_substream, which is exactly the difference from
// measure_qc_eac3_bed above) - the assembled unit is the only place a
// dependent's channels exist as speaker feeds rather than as a substream.
//
// A unit still held back at end-of-stream by §3.7 transient pre-noise
// processing is lost to this measurement: decoder.flush() releases raw
// per-substream results, and by definition their assembly never completed,
// so there is no rendered program to meter. run_levels_eac3 above makes the
// same trade for the same reason, and a stream that never turns the tool on -
// which is every stream this project encodes - never holds anything back.
// `programme` selects the same way measure_qc_eac3_bed's own does, but the
// work is already done: decode_access_unit skips a unit belonging to another
// programme before any decoding at all (Eac3Decoder's own doc comment), so
// there is no adjacency to track here the way the raw-syncframe bed pass
// needs - one DecoderConfig::programme setting is the whole of it.
std::optional<QcResult> measure_qc_eac3_rendered(std::span<const std::byte> stream,
                                                 int programme) {
    const auto units = ac3::split_access_units(stream);
    if (!units || units->empty()) {
        fmt::println(stderr, "error: not a valid E-AC-3 stream");
        return std::nullopt;
    }
    // Heap-allocated for the same PREfast C6262 reason measure_qc_eac3_bed
    // gives above.
    auto decoder = std::make_unique<ac3::Eac3Decoder>(ac3::DecoderConfig{.programme = programme});
    QcResult result;
    result.codec_label = "E-AC-3";
    result.unit_label = "access unit(s)";
    result.rendered = true;

    bool have_first = false;
    std::optional<ac3::meta::LoudnessMeter> meter;

    for (const auto& unit : *units) {
        const auto decoded = decoder->decode_access_unit(unit);
        if (!decoded) {
            fmt::println(stderr, "error: decode failed (code {})",
                         static_cast<int>(decoded.error()));
            return std::nullopt;
        }
        if (!decoded->has_value()) {
            continue;  // §3.7 hold-back, see this function's own comment
        }
        const auto& out = **decoded;
        if (!have_first) {
            have_first = true;
            if (out.acmod == ac3::Acmod::kDualMono) {
                // §E1.3: 1+1 is two unrelated programmes sharing a syncframe,
                // not one soundfield - it has no Table E2.5 layout at all
                // (decode_access_unit leaves `layout` empty for exactly this
                // case), so there is no position for Annex 3 to weight. It is
                // also always a lone independent substream with no
                // dependents, so its rendered program IS its bed and the two
                // passes have to agree by construction. Hand the whole stream
                // to the bed pass rather than restate its two-programme
                // handling here: that pass reads DecodedSubstream, which
                // carries the second programme's own dialnorm2/compr2 -
                // fields the assembled DecodedAccessUnit does not have, so
                // measuring 1+1 from here would silently report Ch2's
                // metadata as absent.
                auto bed = measure_qc_eac3_bed(stream, programme);
                if (bed) {
                    // Still what layout=rendered was asked for, and the same
                    // answer it would have produced; nothing went unmeasured,
                    // so there is no wider layout to hint about either.
                    bed->rendered = true;
                    bed->bed_hid_dependents = false;
                }
                return bed;
            }
            result.sample_rate_hz = sample_rate_hz(out.sample_rate);
            result.layout_label = rendered_layout_label(out.layout);
            meter.emplace(out.sample_rate, out.layout);
            result.programmes.push_back(
                QcProgrammeResult{.dialnorm = out.dialnorm, .compr = out.compr});
        }
        ++result.unit_count;
        std::vector<std::span<const float>> views;
        views.reserve(out.channels.size());
        for (const auto& channel : out.channels) {
            views.emplace_back(channel);
        }
        // meter is engaged on every path that reaches here: either this very
        // iteration's !have_first block just emplaced it (the dual-mono arm
        // above returns instead of falling through), or a PRIOR iteration's
        // !have_first block did and have_first now skips straight past it.
        // clang-tidy's checker does not carry that across the loop's back
        // edge - the same reasoning the sibling meter->push() calls in
        // measure_qc_ac3/measure_qc_eac3_bed rely on, where a same-iteration
        // dual_mono guard happens to keep it within the checker's reach.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        meter->push(views);
    }

    if (!have_first) {
        fmt::println(stderr, "error: no decodable access units");
        return std::nullopt;
    }
    // have_first is only ever set in the branch that emplaces meter - the
    // dual-mono branch beside it returns instead of falling through - so it
    // is engaged by this point, the same reasoning measure_qc_eac3_bed states
    // for its own pair.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    result.programmes[0].integrated_lkfs = meter->integrated_lkfs();
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    result.programmes[0].lra_lu = meter->loudness_range();
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    result.programmes[0].true_peak_dbtp = meter->true_peak_dbtp();
    result.seconds = static_cast<double>(result.unit_count) *
                     static_cast<double>(ac3::kSamplesPerFrame) /
                     static_cast<double>(result.sample_rate_hz);
    return result;
}

// Prints one programme's measurement (the empty-label whole-programme case,
// or "Ch1"/"Ch2" for 1+1 dual mono) and, if `preset_arg` names one (or
// "all"), checks it against the requested preset(s). Returns true iff every
// requested gate passed (or none was requested at all) - run_qc's own exit
// code is exactly this, ANDed across every programme it reports.
bool report_qc_programme(const QcProgrammeResult& p, const std::optional<std::string>& preset_arg) {
    const std::string heading = p.label.empty() ? std::string{} : fmt::format("{}: ", p.label);
    fmt::println("{}measured (BS.1770-4 gated / EBU Tech 3342 / BS.1770-4 Annex 2):", heading);
    if (p.integrated_lkfs) {
        fmt::println("  integrated loudness  {:>+8.2f} LKFS", *p.integrated_lkfs);
        fmt::println("  loudness range       {}", p.lra_lu ? fmt::format("{:>7.2f} LU", *p.lra_lu)
                                                             : std::string{"n/a"});
    } else {
        fmt::println("  integrated loudness  no audio above the -70 LKFS absolute gate");
        fmt::println("  loudness range       n/a");
    }
    fmt::println("  true peak            {}",
                 p.true_peak_dbtp ? fmt::format("{:>+8.2f} dBTP", *p.true_peak_dbtp)
                                   : std::string{"n/a"});
    fmt::println("{}embedded metadata:", heading);
    fmt::println("  dialnorm             {:>3}  (claims dialogue at {:.2f} LKFS)", p.dialnorm,
                 -static_cast<double>(p.dialnorm));
    if (p.compr) {
        fmt::println("  compr                present, {:+.2f} dB",
                     ac3::meta::to_db(ac3::meta::compr_gain(*p.compr)));
    } else {
        fmt::println("  compr                absent");
    }
    if (p.integrated_lkfs) {
        // §5.4.2.8: dialnorm states how far dialogue sits below digital
        // 100%, so the stream's own claimed programme level is simply its
        // negation - delta is measured minus that claim, positive meaning
        // the real programme is louder than dialnorm says.
        const double claimed_lkfs = -static_cast<double>(p.dialnorm);
        const double delta = *p.integrated_lkfs - claimed_lkfs;
        const int implied = ac3::meta::dialnorm_from_lkfs(*p.integrated_lkfs);
        fmt::println("{}dialnorm check:", heading);
        fmt::println("  claimed              {:>+8.2f} LKFS  (from dialnorm {})", claimed_lkfs,
                     p.dialnorm);
        fmt::println("  delta                {:>+8.2f} dB    (measured - claimed; positive = "
                     "measured is louder)",
                     delta);
        fmt::println("  measurement-derived dialnorm would be {}{}", implied,
                     implied == p.dialnorm ? " (matches)" : fmt::format(", not {}", p.dialnorm));
    }

    if (!preset_arg) {
        return true;
    }
    bool all_pass = true;
    fmt::println("{}gates:", heading);
    const auto check_one = [&](ac3::meta::QcPresetId id) {
        const auto preset = ac3::meta::qc_preset(id);
        const auto name = ac3::meta::qc_preset_name(id);
        const auto verdict = ac3::meta::evaluate_qc_gate(preset, p.integrated_lkfs, p.true_peak_dbtp);
        fmt::println("  {}:  [{}]", name, preset.source);
        // A band preset prints its tolerance; a ceiling preset has none to
        // print, and showing "+/-0.0" would read as an impossibly tight band
        // rather than as the one-sided limit the source actually states.
        const std::string loudness_limit =
            preset.loudness_limit == ac3::meta::QcLoudnessLimit::kCeiling
                ? fmt::format("limit  <= {:+.1f} LKFS", preset.target_lkfs)
                : fmt::format("target {:+.1f} +/-{:.1f} LKFS", preset.target_lkfs,
                              preset.tolerance_lu);
        if (p.integrated_lkfs) {
            fmt::println("    loudness   {}   measured {:+.2f} LKFS   delta {:+.2f} LU   {}",
                         loudness_limit, *p.integrated_lkfs, *verdict.loudness_delta_lu,
                         verdict.loudness_pass ? "PASS" : "FAIL");
        } else {
            fmt::println("    loudness   {}   measured n/a   FAIL", loudness_limit);
        }
        if (p.true_peak_dbtp) {
            fmt::println("    true peak  limit  <= {:+.1f} dBTP        measured {:+.2f} dBTP        "
                         "{}",
                         preset.max_true_peak_dbtp, *p.true_peak_dbtp,
                         verdict.true_peak_pass ? "PASS" : "FAIL");
        } else {
            fmt::println("    true peak  limit  <= {:+.1f} dBTP        measured n/a   FAIL",
                         preset.max_true_peak_dbtp);
        }
        fmt::println("    verdict: {}", verdict.pass() ? "PASS" : "FAIL");
        if (!verdict.pass()) {
            all_pass = false;
        }
    };
    if (*preset_arg == "all") {
        for (const auto id : ac3::meta::kQcPresetIds) {
            check_one(id);
        }
    } else {
        ac3::meta::QcPresetId id{};
        if (ac3::meta::parse_qc_preset(*preset_arg, id)) {
            check_one(id);
        } else {
            // parse_options already validates preset= against
            // kQcPresetNames/"all" before dispatch ever reaches here (see
            // its own "preset" handling) - kept as a defensive fallback
            // rather than an assert, since main.cpp has no
            // exception-based unreachable() convention of its own.
            fmt::println(stderr, "error: unknown qc preset '{}'", *preset_arg);
            all_pass = false;
        }
    }
    return all_pass;
}

// E-AC-3's own level report. The rendered layout is a chanmap rather than an
// acmod, so it cannot go through LevelMeter's Table 5.8 naming; the figures
// still come from ac3::analysis, so a level reads the same here as anywhere.
int run_levels_eac3(std::span<const std::byte> stream, std::string_view in_path,
                    std::optional<int> want_programme) {
    const auto ids = ac3::programme_ids(stream);
    if (!ids || ids->empty()) {
        fmt::println(stderr, "error: {} is not a valid E-AC-3 stream", in_path);
        return 1;
    }
    // §E2.3.1.2: levels are per programme. Two independent substreams are two
    // separate pieces of audio, so one set of per-channel figures across both
    // would describe neither.
    const auto programme = ac3cli::choose_programme(*ids, want_programme);
    if (!programme) {
        return 1;
    }
    const auto units = ac3::split_access_units(stream, *programme);
    if (!units || units->empty()) {
        fmt::println(stderr, "error: {} is not a valid E-AC-3 stream", in_path);
        return 1;
    }
    if (ids->size() > 1) {
        fmt::println("{}: programme {} of {} ({})", in_path, *programme, ids->size(),
                     ac3cli::format_programme_ids(*ids));
    }
    ac3::Eac3Decoder decoder{{.programme = programme}};
    std::vector<ac3::analysis::ChannelSummary> totals;
    ac3::DecodedAccessUnit first{};
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        if (!decoded) {
            fmt::println(stderr, "error: {}: decode failed (code {})", in_path,
                         static_cast<int>(decoded.error()));
            return 1;
        }
        if (!decoded->has_value()) {
            // §3.7: held back pending transient pre-noise processing
            // (Eac3Decoder::decode_access_unit's own doc comment) - this
            // report accepts losing the very last frame's stats to that
            // rather than draining decoder.flush() for a metering tool.
            continue;
        }
        const auto& out = **decoded;
        if (totals.empty()) {
            first = out;
            totals.resize(out.channels.size());
            fmt::println("{}: {} access units, {} substreams each, {} channels, {} Hz",
                         in_path, units->size(), out.substream_count,
                         out.channels.size(), sample_rate_hz(out.sample_rate));
        }
        for (std::size_t ch = 0; ch < out.channels.size(); ++ch) {
            auto& stats = totals[ch];
            for (const float sample : out.channels[ch]) {
                const double magnitude = std::abs(static_cast<double>(sample));
                stats.peak = std::max(stats.peak, magnitude);
                stats.sum_squares += magnitude * magnitude;
                ++stats.samples;
                if (magnitude >= static_cast<double>(ac3::analysis::kFullScale)) {
                    ++stats.clipped_samples;
                }
            }
        }
    }
    fmt::println("");
    fmt::println("per-channel levels:");
    fmt::println("  {:<6} {:>8} {:>8}  {:<20} {}", "ch", "peak", "rms", "peak (-60..0 dBFS)",
                 "clipped");
    // Dual mono has no Table E2.5 location - `layout` is left empty for
    // exactly that case (see decode_access_unit) - so Ch1/Ch2 name themselves
    // by coded position instead of a speaker name that would not apply.
    const bool dual_mono = first.acmod == ac3::Acmod::kDualMono;
    for (std::size_t ch = 0; ch < totals.size(); ++ch) {
        const auto& stats = totals[ch];
        const std::string name = dual_mono ? fmt::format("Ch{}", ch + 1)
                                           : std::string{ac3::eac3::chanmap::name(
                                                 first.layout[static_cast<int>(ch)])};
        fmt::println("  {:<6} {:>8.2f} {:>8.2f}  [{}] {}", name, stats.peak_db(),
                     stats.rms_db(), meter_bar(stats.peak_db(), 18),
                     stats.clipped_samples > 0 ? std::to_string(stats.clipped_samples) : "-");
    }
    return 0;
}

// Wrap a raw AC-3 stream into IEC 61937 bursts inside a PCM16 stereo WAV:
// played BIT-EXACTLY (volume 100%, no mixing) into an S/PDIF or HDMI output,
// a receiver locks onto the bursts and lights up "Dolby Digital".
// AC-3 frames wrap one-to-one; an E-AC-3 access unit may need several
// consecutive ones to fill a burst (Eac3BurstPacker accumulates internally).
// Feeds each formed burst to `push` rather than accumulating them: the
// E-AC-3 carrier runs at 4x the content rate (~0.7 MB per second), which
// made the whole-payload form the largest O(duration) term the CLI had
// left. `rate_out` is set before the first push, so a caller may open its
// destination lazily from inside `push`. False means the stream is not a
// valid frame sequence - or that `push` itself said stop, which the
// caller can tell apart because it was its own push that failed.
template <typename Push>
bool wrap_ac3_stream(std::span<const std::byte> stream, std::uint32_t& rate_out, Push&& push) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        return false;
    }
    const auto fscod = std::to_integer<std::uint32_t>((*frames)[0][4]) >> 6;
    rate_out = sample_rate_hz(static_cast<ac3::SampleRate>(fscod));

    for (const auto& frame : *frames) {
        const auto burst = ac3::iec61937::wrap_frame(frame);
        if (!burst) {
            return false;
        }
        if (!push(std::span<const std::byte>{*burst})) {
            return false;
        }
    }
    return true;
}

template <typename Push>
bool wrap_eac3_stream(std::span<const std::byte> stream, std::uint32_t& rate_out, Push&& push) {
    const auto units = ac3::split_access_units(stream);
    if (!units || units->empty()) {
        return false;
    }
    const auto byte4 = std::to_integer<std::uint32_t>((*units)[0][4]);
    rate_out = sample_rate_hz(static_cast<ac3::SampleRate>(byte4 >> 6));

    ac3::iec61937::Eac3BurstPacker packer;
    for (const auto& unit : *units) {
        const auto burst = packer.push(unit);
        if (!burst) {
            return false;
        }
        if (*burst && !push(std::span<const std::byte>{**burst})) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<StreamLoudness> measure_stream_loudness(std::span<const std::byte> stream) {
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        fmt::println(stderr, "error: too short to hold a syncframe");
        return std::nullopt;
    }
    // The same two measurement passes `qc layout=bed` (the default, and what
    // this measured before that option existed) runs, which is the point: a
    // stream's loudness must not depend on which command asked.
    const auto result = *bsid > 8 ? measure_qc_eac3_bed(stream) : measure_qc_ac3(stream, false);
    if (!result) {
        return std::nullopt;
    }
    StreamLoudness out;
    for (const auto& programme : result->programmes) {
        if (programme.label == "Ch1") {
            out.ch1_lkfs = programme.integrated_lkfs;
        } else if (programme.label == "Ch2") {
            out.ch2_lkfs = programme.integrated_lkfs;
        } else {
            out.integrated_lkfs = programme.integrated_lkfs;
        }
    }
    // Dual mono has no whole-programme figure of its own; Ch1's is what a
    // caller wanting "the" loudness of such a stream means, and reporting it
    // here keeps every caller from having to special-case the layout.
    if (!out.integrated_lkfs) {
        out.integrated_lkfs = out.ch1_lkfs;
    }
    return out;
}

int run_qc(std::string_view in_path, const std::optional<std::string>& preset_arg,
           bool rendered_layout, std::optional<int> want_programme) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        fmt::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        fmt::println(stderr, "error: {} is too short to hold a syncframe", in_path);
        return 1;
    }
    std::optional<QcResult> result;
    if (*bsid > 8) {
        // §E2.3.1.2: one programme is measured - see measure_qc_eac3_bed's own
        // ingest() for why folding two into one meter reports a loudness
        // neither of them has.
        const auto ids = ac3::programme_ids(stream);
        if (!ids || ids->empty()) {
            fmt::println(stderr, "error: {} is not a valid E-AC-3 stream", in_path);
            return 1;
        }
        const auto programme = ac3cli::choose_programme(*ids, want_programme);
        if (!programme) {
            return 1;
        }
        if (ids->size() > 1) {
            fmt::println("qc: programme {} of {} ({})", *programme, ids->size(),
                         ac3cli::format_programme_ids(*ids));
        }
        result = rendered_layout ? measure_qc_eac3_rendered(stream, *programme)
                                 : measure_qc_eac3_bed(stream, *programme);
    } else {
        result = measure_qc_ac3(stream, rendered_layout);
    }
    if (!result) {
        return 1;
    }
    fmt::println("qc: {} ({}, {}, {} Hz, {} {}, {:.2f} s)", in_path, result->codec_label,
                 result->layout_label, result->sample_rate_hz, result->unit_count,
                 result->unit_label, result->seconds);
    // Which algorithm the figures below came out of. Two different BS.1770
    // algorithms over the same stream are both correct and need not agree, so
    // a loudness figure that does not say which one produced it is ambiguous.
    fmt::println("  layout={}  ({})", result->rendered ? "rendered" : "bed",
                 result->rendered ? "BS.1770-5 Annex 3, weighted by channel position"
                                  : "BS.1770 Annex 1, Table 3 weights over the Table 5.8 bed");
    if (result->bed_hid_dependents) {
        fmt::println("  note: this stream carries dependent substreams whose channels "
                     "(height, wide, rear)");
        fmt::println("        are NOT in the figures above - layout=rendered measures them "
                     "as well");
    }
    bool all_pass = true;
    for (const auto& programme : result->programmes) {
        if (!report_qc_programme(programme, preset_arg)) {
            all_pass = false;
        }
    }
    return all_pass ? 0 : 1;
}

// What is actually in a file, channel by channel — the answer both front ends
// are built to show, without having to encode anything to get it.
int run_levels(std::string_view in_path, std::optional<int> want_programme) {
    const auto bytes = read_all(in_path);
    if (bytes.empty()) {
        fmt::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    // A syncframe opens with 0x0B77 (§5.4.1.1); anything else is treated as a
    // WAV, whose reader reports its own diagnosis if it is neither.
    const bool syncword = bytes.size() >= 6 && std::to_integer<int>(bytes[0]) == 0x0B &&
                          std::to_integer<int>(bytes[1]) == 0x77;

    if (syncword) {
        // E-AC-3 has its own decoder here now, so this is no longer a wall to
        // turn a wider syntax away at - bsid only decides which reader runs.
        const auto bsid = ac3::stream_bsid(bytes);
        if (bsid && *bsid > 8) {
            return run_levels_eac3(bytes, in_path, want_programme);
        }
        const auto frames = ac3::split_frames(bytes);
        if (!frames || frames->empty()) {
            fmt::println(stderr, "error: {} is not a valid AC-3 stream", in_path);
            return 1;
        }
        ac3::FrameDecoder decoder;
        std::optional<ac3::analysis::LevelMeter> meter;
        for (const auto& frame : *frames) {
            const auto decoded = decoder.decode_frame(frame);
            if (!decoded) {
                fmt::println(stderr, "error: {}: {}", in_path, ac3::describe(decoded.error()));
                return 1;
            }
            if (!meter) {
                meter.emplace(decoded->acmod, decoded->lfe,
                              sample_rate_hz(decoded->sample_rate));
                fmt::println("{}: {} frames, {}, {} kbps, {} Hz", in_path, frames->size(),
                             ac3::analysis::layout_name(decoded->acmod, decoded->lfe),
                             decoded->bitrate_kbps, sample_rate_hz(decoded->sample_rate));
            }
            std::vector<std::span<const float>> views;
            views.reserve(decoded->channels.size());
            for (const auto& channel : decoded->channels) {
                views.emplace_back(channel);
            }
            // meter is engaged by the !meter check a few lines up, in this
            // same iteration on the first pass and an earlier one thereafter.
            // clang-tidy's bugprone-unchecked-optional-access and MSVC
            // /analyze's C26829 both flag it anyway: neither does the
            // cross-iteration reasoning needed to see it's always engaged
            // by the time this runs. #pragma warning(suppress: 26829) would
            // silence /analyze too, but it is not a portable pragma - GCC/
            // clang both treat an unrecognized #pragma as -Wunknown-pragmas,
            // and this project builds with -Werror, so emitting it here
            // would fail every non-MSVC leg. The C26829 alert on both this
            // line and the one below is dismissed separately with this same
            // justification instead.
            meter->process(views); // NOLINT(bugprone-unchecked-optional-access)
        }
        // The `!frames || frames->empty()` check above guarantees the loop
        // ran at least once, and its first iteration always emplaces meter.
        print_channel_summary(*meter); // NOLINT(bugprone-unchecked-optional-access)
        return 0;
    }

    const auto wav = ac3::io::read_wav(std::string{in_path});
    if (!wav) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return 1;
    }
    const auto layout = ac3::io::ac3_layout_for(wav->channels.size());
    if (!layout) {
        fmt::println(stderr, "error: levels handles 1 to 6 channels ({} given)",
                     wav->channels.size());
        return 1;
    }
    const double seconds = wav->sample_rate > 0
                               ? static_cast<double>(wav->frame_count()) / wav->sample_rate
                               : 0.0;
    fmt::println("{}: {} Hz, {:.2f} s, shown in A/52 order as {}", in_path, wav->sample_rate,
                 seconds, ac3::analysis::layout_name(layout->acmod, layout->lfe));

    ac3::analysis::LevelMeter meter{layout->acmod, layout->lfe, wav->sample_rate};
    std::vector<std::span<const float>> views(layout->wav_index.size());
    for (std::size_t ch = 0; ch < layout->wav_index.size(); ++ch) {
        views[ch] = wav->channels[layout->wav_index[ch]];
    }
    meter.process(views);
    print_channel_summary(meter);
    return 0;
}

// Measure a WAV and report what dialnorm it implies. §5.4.2.8 wants dialogue
// level below full scale and A/52 predates any standard way to measure it;
// BS.1770 gated loudness is the modern answer, so this is the number the
// encoder would put on the stream for dialnorm=auto.
int run_loudness(std::string_view in_path) {
    const auto wav = ac3::io::read_wav(std::string{in_path});
    if (!wav) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return 1;
    }
    ac3::SampleRate sr{};
    switch (wav->sample_rate) {
        case 48000: sr = ac3::SampleRate::k48000; break;
        case 44100: sr = ac3::SampleRate::k44100; break;
        case 32000: sr = ac3::SampleRate::k32000; break;
        default:
            fmt::println(stderr, "error: sample rate {} is not legal for AC-3", wav->sample_rate);
            return 1;
    }
    // The BS.1770 channel weighting depends on which coded positions are
    // surrounds, so the layout has to be inferred from the channel count
    // (Table 5.8) rather than assumed.
    const auto layout = ac3::io::ac3_layout_for(wav->channels.size());
    if (!layout) {
        fmt::println(stderr, "error: {} channels is not an AC-3 layout",
                     wav->channels.size());
        return 1;
    }
    const auto dialnorm = measured_dialnorm(*wav, sr, layout->acmod, layout->lfe);
    if (!dialnorm) {
        fmt::println("no audio above the -70 LKFS absolute gate: loudness undefined");
        return 1;
    }
    // Reporting the answer was missing where this came from, so the command
    // measured the programme and then said nothing about it.
    fmt::println("{}: {} Hz, {}", in_path, wav->sample_rate,
                 ac3::analysis::layout_name(layout->acmod, layout->lfe));
    fmt::println("  dialogue level -{} LKFS -> dialnorm {}", *dialnorm, *dialnorm);
    return 0;
}

int run_spdif(std::string_view in_path, std::string_view out_path) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        fmt::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        fmt::println(stderr, "error: {} is too short to hold a syncframe", in_path);
        return 1;
    }
    const bool eac3 = *bsid > 8;

    // The WAV carrier itself runs at 4x the content rate for E-AC-3 (Dolby
    // Digital Plus over IEC 60958/61937 - Microsoft's "Representing Formats
    // for IEC 61937 Transmissions"), matching WASAPI's make_eac3_format.
    // The sink opens lazily on the first burst - the rate is only known
    // once the wrapper has parsed the first frame, and a stream the
    // wrapper rejects must leave no file, exactly as the whole-payload
    // write it replaces never ran at all on failure.
    std::uint32_t content_rate = 0;
    Pcm16RawWavSink sink;
    bool sink_failed = false;
    const auto push = [&](std::span<const std::byte> burst) {
        if (!sink.is_open() &&
            !sink.open(out_path, eac3 ? content_rate * 4 : content_rate, 2)) {
            sink_failed = true;
            return false;
        }
        if (!sink.push(burst)) {
            sink_failed = true;
            return false;
        }
        return true;
    };
    const auto ok = eac3 ? wrap_eac3_stream(stream, content_rate, push)
                         : wrap_ac3_stream(stream, content_rate, push);
    if (!ok) {
        sink.abort();
        if (!sink_failed) {
            fmt::println(stderr, "error: {} is not a valid {} stream", in_path,
                         eac3 ? "E-AC-3" : "AC-3");
        }
        return 1;
    }
    const auto carrier_rate = eac3 ? content_rate * 4 : content_rate;
    // A valid stream whose units never completed a burst (an E-AC-3 input
    // shorter than one burst set) still produced a header-only WAV before,
    // so the never-opened sink opens for exactly that here.
    if (!sink.is_open() && !sink.open(out_path, carrier_rate, 2)) {
        return 1;
    }
    if (!sink.close()) {
        return 1;
    }
    fmt::println("wrapped {} into IEC 61937 bursts -> {} ({} Hz carrier)",
                 eac3 ? "E-AC-3 access units" : "AC-3 frames", out_path, carrier_rate);
    fmt::println("play bit-exactly (100% volume, exclusive/passthrough output) to light up");
    fmt::println("a receiver's Dolby Digital{} indicator.", eac3 ? " Plus" : "");
    return 0;
}

namespace {

// How much carrier to hand the reader at a time. Nothing here scales with the
// input's length: this buffer, one burst inside BurstReader, and whatever one
// burst's payload comes to are the whole of unspdif's memory, so a two-hour
// capture costs exactly what a two-second one does.
constexpr std::size_t kCarrierChunkBytes = 64 * 1024;

// Walks a RIFF file's chunk list to its data chunk, leaving `in` positioned
// at the first payload byte. Deliberately not read_wav/WavStreamReader: both
// hand back floats, and a burst carrier's value is in its exact 16-bit words
// - a trip through float and back is a conversion this has no reason to make
// and no way to prove it survived. Returns the data chunk's byte length
// (clamped to what the file actually holds), or nullopt if `in` is not a
// PCM16 RIFF/WAVE file, which is how a raw carrier is told from a wrapped one.
struct CarrierChunk {
    std::uint64_t bytes = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
};

std::optional<CarrierChunk> seek_riff_data(std::istream& in, std::uint64_t file_bytes) {
    std::array<char, 12> riff{};
    if (!in.read(riff.data(), riff.size())) {
        return std::nullopt;
    }
    if (std::string_view{riff.data(), 4} != "RIFF" ||
        std::string_view{riff.data() + 8, 4} != "WAVE") {
        return std::nullopt;
    }
    const auto read_u16 = [](const char* p) {
        return static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[0]) |
                                          (static_cast<std::uint8_t>(p[1]) << 8));
    };
    const auto read_u32 = [](const char* p) {
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[0])) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[1])) << 8) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[2])) << 16) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[3])) << 24);
    };

    CarrierChunk found;
    std::uint64_t at = riff.size();
    // A chunk header is 8 bytes; a chunk of odd length is followed by a pad
    // byte. Bounded by the file's own length, so a chunk size claiming more
    // than the file holds cannot walk the cursor off the end.
    while (at + 8 <= file_bytes) {
        std::array<char, 8> header{};
        if (!in.read(header.data(), header.size())) {
            return std::nullopt;
        }
        const std::string_view id{header.data(), 4};
        const auto size = read_u32(header.data() + 4);
        at += 8;
        const auto available = file_bytes - at;
        if (id == "fmt " && size >= 16) {
            std::array<char, 16> fmt{};
            if (!in.read(fmt.data(), fmt.size())) {
                return std::nullopt;
            }
            found.channels = read_u16(fmt.data() + 2);
            found.sample_rate = read_u32(fmt.data() + 4);
            in.seekg(static_cast<std::streamoff>(at + size + (size & 1u)), std::ios::beg);
        } else if (id == "data") {
            found.bytes = std::min<std::uint64_t>(size, available);
            return found;
        } else {
            in.seekg(static_cast<std::streamoff>(at + size + (size & 1u)), std::ios::beg);
        }
        at += size + (size & 1u);
    }
    return std::nullopt;
}

}  // namespace

int run_unspdif(std::string_view in_path, std::string_view out_path, bool keep_partial) {
    // "-" reads the carrier from stdin, so a capture tool can be piped
    // straight in - which on a machine with a real S/PDIF input is the
    // natural shape of this ("arecord ... | ac3cli unspdif - out.ec3"). The
    // RIFF walk below needs to seek and stdin does not, but it does not need
    // to run at all: BurstReader resyncs on Pa/Pb, and a WAV header cannot
    // contain a preamble followed by a syncframe, so the header simply gets
    // scanned past. All that is lost is the carrier's declared rate, which
    // is a reported detail rather than something the unwrap depends on.
    const bool stdio = is_stdio_path(in_path);
    std::ifstream file;
    if (stdio) {
        ac3::cli::platform::set_stdio_binary();
    } else {
        file.open(std::string{in_path}, std::ios::binary);
        if (!file) {
            fmt::println(stderr, "error: cannot read {}", in_path);
            return 1;
        }
    }
    std::istream& in = stdio ? std::cin : file;

    std::uint64_t file_bytes = 0;
    if (!stdio) {
        in.seekg(0, std::ios::end);
        const auto end = in.tellg();
        if (end < 0) {
            fmt::println(stderr, "error: cannot read {}", in_path);
            return 1;
        }
        file_bytes = static_cast<std::uint64_t>(end);
        in.seekg(0, std::ios::beg);
    }

    // A WAV carrier (what 'spdif' writes, and what a capture tool saves) or a
    // bare dump of carrier bytes. Both are ordinary inputs here, so neither
    // is an error: if the RIFF walk does not find a data chunk, the whole
    // file is the carrier.
    const auto chunk = stdio ? std::nullopt : seek_riff_data(in, file_bytes);
    // Unbounded for stdin, whose length nothing knows until it ends; the
    // read loop below stops on the first short read either way.
    std::uint64_t remaining = stdio ? UINT64_MAX : file_bytes;
    if (chunk) {
        remaining = chunk->bytes;
    } else if (!stdio) {
        in.clear();
        in.seekg(0, std::ios::beg);
    }

    EncodedStreamSink sink;
    if (!sink.open(out_path, keep_partial)) {
        return 1;
    }
    ac3::iec61937::BurstReader reader;
    std::vector<std::byte> carrier(kCarrierChunkBytes);
    std::vector<std::byte> payload;
    std::uint64_t elementary_bytes = 0;

    while (remaining > 0) {
        const auto want = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, kCarrierChunkBytes));
        in.read(reinterpret_cast<char*>(carrier.data()), static_cast<std::streamsize>(want));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) {
            break;
        }
        remaining -= got;
        payload.clear();
        const auto pushed = reader.push(std::span{carrier}.first(got), payload);
        if (!pushed) {
            sink.abort();
            fmt::println(stderr, "error: {}: {}", in_path,
                         ac3::iec61937::describe(pushed.error()));
            return 1;
        }
        if (!payload.empty()) {
            elementary_bytes += payload.size();
            if (!sink.push(payload)) {
                sink.abort();
                return 1;
            }
        }
    }

    // A capture stopped mid-burst is worth saying so about rather than
    // silently keeping a stream one frame short of what the operator saw.
    const auto finished = reader.finish();
    if (reader.bursts() == 0) {
        sink.abort();
        fmt::println(stderr, "error: {} holds no AC-3 or E-AC-3 bursts{}", in_path,
                     reader.skipped_bursts() > 0
                         ? " (its bursts are another data type)"
                         : " - is it ordinary PCM rather than an IEC 61937 carrier?");
        return 1;
    }
    if (!sink.close()) {
        return 1;
    }

    // Compared as an optional rather than dereferenced: bursts() > 0 does
    // guarantee data_type() is engaged, but that is an invariant of the
    // reader rather than something visible here, and the answer wanted is a
    // bool either way - std::optional's own operator== gives it directly.
    const bool eac3 = reader.data_type() == ac3::iec61937::BurstDataType::kEac3;
    // stderr when the elementary stream itself is going to stdout, the same
    // convention encode/decode follow (see status_stream's own comment):
    // this report must never land in the middle of the bytes a pipeline is
    // reading.
    auto* status = status_stream(out_path);
    fmt::println(status, "unwrapped {} {} burst{} -> {} ({} bytes)", reader.bursts(),
                 eac3 ? "E-AC-3" : "AC-3", reader.bursts() == 1 ? "" : "s", out_path,
                 elementary_bytes);
    if (chunk) {
        // The carrier rate, not the content rate: an E-AC-3 carrier runs at
        // 4x, so 192000 here means a 48 kHz programme.
        fmt::println(status, "carrier: {} Hz, {} ch, {} words", chunk->sample_rate,
                     chunk->channels,
                     reader.word_order() == ac3::iec61937::WordOrder::kBigEndian
                         ? "big-endian"
                         : "little-endian");
    }
    if (reader.skipped_bursts() > 0) {
        fmt::println(status, "skipped {} burst(s) of another data type", reader.skipped_bursts());
    }
    if (reader.false_syncs() > 0) {
        fmt::println(status, "resynced past {} preamble pattern(s) with no syncframe behind them",
                     reader.false_syncs());
    }
    if (!finished) {
        fmt::println(status, "warning: {}", ac3::iec61937::describe(finished.error()));
    }
    return 0;
}

}  // namespace ac3cli::commands
