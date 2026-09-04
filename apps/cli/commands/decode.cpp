#include "decode.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "../adm/atmos_adm.hpp"
#include "../adm/decode_adm.hpp"
#include "../exit_codes.hpp"
#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/verify/bap_census.hpp"
#include "ac3/verify/eac3_mirror.hpp"
#include "ac3/verify/mirror.hpp"

namespace ac3cli::commands {

namespace {

namespace plan = ac3::plan;

// bap-census= output. Failure to write is an error rather than a warning: the
// census is evidence a CI check is about to gate on, and a decode that was
// asked for it and silently produced none would leave that check passing on a
// stale file from a previous run.
bool write_bap_census(const ac3::verify::BapCensus& census, const std::string& path) {
    const std::string json = census.to_json();
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        fmt::println(stderr, "error: cannot open bap-census output {}", path);
        return false;
    }
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    out.close();
    if (!out) {
        fmt::println(stderr, "error: cannot write bap-census output {}", path);
        return false;
    }
    return true;
}

// Whether the §7.8 output stage is going to fold this programme, which
// decides what the sink is opened for: a fold's own channels are already L/R
// (or a single mono channel) in that order, so the coded-layout permutation
// every other decode applies would be wrong for them. Dual mono is never
// folded (OutputStage refuses it - 1+1 is two programmes, not a soundfield),
// so it keeps the coded path whatever the target says.
bool folding(const ac3cli::Options& meta, ac3::Acmod acmod) {
    return meta.output.target != ac3::DownmixTarget::kAsCoded && acmod != ac3::Acmod::kDualMono;
}

// The one-line name for whatever the fold produced, for the status report.
std::string_view fold_name(ac3::DownmixTarget target) {
    switch (target) {
        case ac3::DownmixTarget::kLoRo: return "Lo/Ro stereo";
        case ac3::DownmixTarget::kLtRt: return "Lt/Rt stereo";
        case ac3::DownmixTarget::kMono: return "mono";
        case ac3::DownmixTarget::kAsCoded: break;
    }
    return "as coded";
}

// What §7.7 actually did, which drcmode= can decide as well as drc=/heavy.
// The two named modes OVERRIDE those switches inside the decoder
// (resolve_operating_mode), so a report reading only meta.drc_scale would say
// "not applied" about a line-mode decode that applied every word in full.
std::string dynrng_note(const ac3cli::Options& meta) {
    switch (meta.output.mode) {
        case ac3::OperatingMode::kLine:
            return ", applied in full (drcmode=line)";
        case ac3::OperatingMode::kRf:
            return ", applied only where no compr word exists (drcmode=rf, §7.7.2.1)";
        case ac3::OperatingMode::kCustom:
            break;
    }
    return meta.drc_scale != 0.0 ? fmt::format(", applied at scale {}", meta.drc_scale)
                                 : ", not applied";
}

std::string compr_note(const ac3cli::Options& meta) {
    switch (meta.output.mode) {
        case ac3::OperatingMode::kRf:
            return ", applied (drcmode=rf)";
        case ac3::OperatingMode::kLine:
            return ", not applied (drcmode=line uses dynrng)";
        case ac3::OperatingMode::kCustom:
            break;
    }
    return meta.p.heavy ? ", applied" : ", not applied";
}

// §5.4.2.8. Both named modes normalise, and so does apply_dialnorm on its own.
std::string dialnorm_note(const ac3cli::Options& meta, int dialnorm) {
    if (!meta.output.apply_dialnorm && meta.output.mode == ac3::OperatingMode::kCustom) {
        return {};
    }
    return fmt::format(", normalised to the -31 dBFS reference ({:+.2f} dB)",
                       ac3::meta::to_db(ac3::meta::dialnorm_gain(dialnorm)));
}

// §7.10: what a run's concealed frames should say afterwards. Silent when
// nothing was concealed, which is every ordinary decode.
void print_concealment_summary(FILE* status, std::size_t concealed, std::size_t total,
                               std::string_view unit) {
    if (concealed == 0) {
        return;
    }
    fmt::println(status, "  concealed {} of {} {} (§7.10)", concealed, total, unit);
}

// Reports the object layer (if any) an E-AC-3 decode found - the decode-side
// mirror of run_atmos_encode's own "{N} dynamic objects + the bed's LFE = {M}
// objects" line. Shared between run_decode_eac3's dual-mono and ordinary
// return paths, even though this project's own AtmosEncoder never emits dual
// mono alongside an object container. The object WAVs themselves are
// streamed out by per-object sinks as the decode runs (run_decode_eac3's
// append_objects) - by the time this prints, the files are already closed;
// this only says what happened.
int report_decoded_objects(FILE* status, const std::optional<ac3::oba::DecodedProgram>& metadata,
                           bool have_object_audio, std::size_t objects_written,
                           std::string_view objects_dir) {
    if (metadata) {
        const auto& program = metadata->program;
        const char* joc = have_object_audio ? ", JOC audio reconstructed"
                                            : " (JOC audio not reconstructed)";
        if (program.dynamic_only) {
            status_println(status, "  {} dynamic objects{} = {} objects, OAMD present{}",
                           metadata->objects.size(), program.lfe ? " + the bed's LFE" : "",
                           ac3::oba::object_count(program), joc);
        } else {
            // A bed program - what channel-based-immersive third-party
            // content is. Naming the bed's channels is the useful half here:
            // "12 objects" says nothing, "L R C LFE Ls Rs Lb Rb Tfl Tfr Tbl
            // Tbr" says what the stream actually carries.
            std::string labels;
            for (const auto label : ac3::oba::bed_labels(program.bed)) {
                if (!labels.empty()) {
                    labels += ' ';
                }
                labels += ac3::oba::describe(label);
            }
            if (labels.empty()) {
                labels = fmt::format("{} channels", ac3::oba::bed_channel_count(program));
            }
            status_println(status, "  bed [{}] + {} dynamic objects = {} objects, OAMD present{}",
                           labels, program.dynamic_objects, ac3::oba::object_count(program), joc);
        }
        if (metadata->trim) {
            status_println(status, "  OAMD trim element: warp mode {}, global trim mode {}",
                           metadata->trim->warp_mode, metadata->trim->global_trim_mode);
        }
        if (!metadata->skipped_elements.empty()) {
            std::string ids;
            for (const int id : metadata->skipped_elements) {
                if (!ids.empty()) {
                    ids += ", ";
                }
                ids += std::to_string(id);
            }
            status_println(status, "  OAMD elements skipped by size (unrecognised id): {}", ids);
        }
        if (metadata->blocks.size() > 1) {
            status_println(status, "  {} metadata update blocks per frame",
                           metadata->blocks.size());
        }
    }
    if (objects_dir.empty()) {
        return 0;
    }
    if (objects_written == 0) {
        fmt::println(stderr,
                     "warning: objects_dir given but there is no reconstructed object audio to "
                     "export");
        return 0;
    }
    status_println(status, "  wrote {} object WAV(s) to {}", objects_written, objects_dir);
    return 0;
}

// The dynrng/compr half of run_decode's own status report (main.cpp, further
// down), factored out so run_decode_eac3 can report the same two figures -
// range actually carried, and whether drc=/heavy asked for them to be
// applied - without duplicating run_decode's own dialnorm-anchored
// indentation, which this command's report has no dialnorm line to anchor to.
void print_drc_summary(FILE* status, double dynrng_min_db, double dynrng_max_db,
                       double compr_min_db, double compr_max_db, std::size_t compr_frames,
                       const ac3cli::Options& meta) {
    status_println(status, "  dynrng {:+.2f} .. {:+.2f} dB{}", dynrng_min_db, dynrng_max_db,
                   dynrng_note(meta));
    if (compr_frames > 0) {
        status_println(status, "  compr  {:+.2f} .. {:+.2f} dB over {} access units{}",
                       compr_min_db, compr_max_db, compr_frames, compr_note(meta));
    } else {
        status_println(status, "  compr  absent");
    }
}

// The informational bit stream information (§5.4.2 / Table E1.2's infomdat)
// and the downmix/mixing group beside it, reported only where the stream
// actually says something with them.
//
// Silence is the point. A complete-main programme with no production notes,
// no Surround flags and no second programme to mix against has nothing here
// worth a line, and that describes almost every stream this tool decodes - so
// a plain decode's report reads exactly as it always did, and anything that
// does appear below is a claim the encoder deliberately made.
void print_bsi_summary(FILE* status, const ac3::meta::BsiInfo& info, ac3::Acmod acmod) {
    if (info.bsmod != ac3::meta::BitstreamMode::kCompleteMain) {
        fmt::println(status, "  service: {}", ac3::meta::describe(info.bsmod, acmod));
    }
    if (info.dsurmod != ac3::meta::SurroundMode::kNotIndicated) {
        fmt::println(status, "  dsurmod: {}", ac3::meta::describe(info.dsurmod));
    }
    if (info.dsurexmod != ac3::meta::SurroundExMode::kNotIndicated) {
        fmt::println(status, "  dsurexmod: {}", ac3::meta::describe(info.dsurexmod));
    }
    if (info.dheadphonmod != ac3::meta::HeadphoneMode::kNotIndicated) {
        fmt::println(status, "  dheadphonmod: {}", ac3::meta::describe(info.dheadphonmod));
    }
    // The A/D converter clause is only ever appended for HDCD: "standard" is
    // what §D2.3.1.10 tells an encoder to send when it does not know, so it
    // is an absence of information rather than a claim - and on AC-3 the
    // field is not part of audprodie at all (it lives in xbsi2), where
    // printing it would suggest a bit that was never read.
    const auto production = [&](std::string_view prefix,
                                const ac3::meta::AudioProduction& value) {
        fmt::println(status, "  {}mixed at {} dB SPL, {}{}", prefix,
                     ac3::meta::mix_level_db_spl(value.mixlevel),
                     ac3::meta::describe(value.roomtyp),
                     value.adconvtyp == ac3::meta::AdConverterType::kHdcd ? ", A/D HDCD" : "");
    };
    if (info.audprod) {
        production("", *info.audprod);
    }
    if (info.audprod2) {
        production("Ch2 ", *info.audprod2);
    }
    // origbs defaults set, so only a stream declaring itself a COPY is news.
    if (info.copyrightb || !info.origbs) {
        fmt::println(status, "  {}{}{}", info.copyrightb ? "copyright asserted" : "",
                     info.copyrightb && !info.origbs ? ", " : "",
                     info.origbs ? "" : "a copy, not the original bit stream");
    }
    if (info.sourcefscod) {
        fmt::println(status, "  source sampled at twice the coded rate (§E2.3.1.63)");
    }
    if (info.timecod1 || info.timecod2) {
        fmt::println(status, "  timecode: {}",
                     ac3::meta::format_timecode(info.timecod1.value_or(ac3::meta::TimeCodeCoarse{}),
                                                info.timecod2.value_or(ac3::meta::TimeCodeFine{})));
    }
}

// The programme-mixing half of mixmdate: what a receiver would use to fold
// this substream against another programme. The five downmix levels are left
// out - they are present on every mixmdate group and say nothing about
// whether this stream is an associated service.
void print_mix_summary(FILE* status, const ac3::meta::MixMetadata& mix) {
    if (mix.pgmscl) {
        fmt::println(status, "  programme scale: {}",
                     *mix.pgmscl == ac3::meta::kPgmScaleMute
                         ? std::string{"mute"}
                         : fmt::format("{:+.0f} dB", ac3::meta::pgm_scale_db(*mix.pgmscl)));
    }
    if (mix.extpgmscl) {
        fmt::println(status, "  external programme scale: {}",
                     *mix.extpgmscl == ac3::meta::kPgmScaleMute
                         ? std::string{"mute"}
                         : fmt::format("{:+.0f} dB", ac3::meta::pgm_scale_db(*mix.extpgmscl)));
    }
    if (mix.mixing.mixdef != ac3::meta::MixDefinition::kNone) {
        fmt::println(status, "  mixdef {} ({}{}{})",
                     static_cast<int>(mix.mixing.mixdef),
                     mix.mixing.external ? "external channel scales" : "",
                     mix.mixing.external && mix.mixing.speech ? ", " : "",
                     mix.mixing.speech ? "speech enhancement data"
                                       : (mix.mixing.external ? "" : "no sub-fields"));
    }
    if (mix.pan) {
        fmt::println(status, "  pan: {:.1f} degrees clockwise from centre",
                     static_cast<double>(mix.pan->panmean) * ac3::meta::kPanMeanDegreesPerStep);
    }
    if (mix.blkmixcfginfo) {
        fmt::println(status, "  per-block mixing configuration present");
    }
}

int run_decode_eac3(std::span<const std::byte> stream, std::string_view out_path,
                     const ac3cli::Options& meta, std::string_view objects_dir, std::string_view adm_out) {
    if (!adm_out.empty() && !ac3cli::adm_capability().available) {
        fmt::println(stderr, "error: {}", ac3cli::adm_capability().reason);
        return kExitInput;
    }
    // §E2.3.1.2: one programme is decoded, never a fold of several. A stream
    // carrying a second independent substream carries an ALTERNATIVE - a
    // second language, an audio description - so writing both into one WAV
    // would splice two unrelated pieces of audio together.
    const auto ids = ac3::programme_ids(stream);
    if (!ids) {
        fmt::println(stderr, "error: stream framing failed: {}",
                     ac3::describe(ids.error()));
        return 1;
    }
    if (ids->empty()) {
        fmt::println(stderr, "error: no programmes in stream");
        return 1;
    }
    const auto programme = choose_programme(*ids, meta.programme);
    if (!programme) {
        return 1;
    }
    // Access units, not syncframes: a dependent substream is only meaningful
    // alongside the independent one it extends, and the two are rendered
    // together into one set of speaker feeds.
    const auto units = ac3::split_access_units(stream, *programme);
    if (!units) {
        fmt::println(stderr, "error: stream framing failed: {}",
                     ac3::describe(units.error()));
        return kExitInput;
    }
    if (ids->size() > 1) {
        fmt::println(status_stream(out_path), "  programme {} of {} ({})", *programme,
                     ids->size(), format_programme_ids(*ids));
    }
    // Same convention as the AC-3 path below: null unless bap-census= asked
    // for it, so an ordinary decode pays nothing.
    const bool census_wanted = !meta.bap_census_path.empty();
    ac3::verify::Eac3AccessUnitTrace census_trace;
    ac3::verify::BapCensus census;
    ac3::Eac3Decoder decoder{{.drc_scale = meta.drc_scale,
                             .fast_imdct = meta.fast_imdct,
                             .heavy_compression = meta.p.heavy.has_value(),
                             .output = meta.output,
                             .concealment = meta.concealment,
                             .fast_mdct = meta.fast_mdct,
                             .joc_domain = meta.joc_domain,
                             .eac3_trace = census_wanted ? &census_trace : nullptr,
                             .programme = programme}};
    // The decoded programme goes out through the sink as units decode - the
    // sink's per-slot carry absorbs the one place slots advance unevenly
    // (the transient-pre-noise flush below).
    PlanarWavSink sink;
    std::size_t sink_slots = 0;
    const auto open_sink = [&](const ac3::DecodedAccessUnit& unit,
                               std::size_t slots) -> bool {
        sink_slots = slots;
        // Dual mono has no Table E2.5 location to order by, so Ch1 and Ch2
        // go out in coded order - the same identity the whole-buffer write
        // fell back to. Everyone else gets the WAV speaker order the encode
        // side reads a file in.
        std::vector<std::size_t> order;
        // A fold has already put its own channels in their own order, so like
        // dual mono it takes the identity permutation rather than the
        // rendered layout's.
        if (unit.acmod != ac3::Acmod::kDualMono && !folding(meta, unit.acmod)) {
            order = plan::wav_order(std::span{unit.layout.items}.first(
                static_cast<std::size_t>(unit.layout.count)));
        }
        if (!sink.open(out_path, sample_rate_hz(unit.sample_rate), slots, order)) {
            fmt::println(stderr, "error: cannot open {} for writing", out_path);
            return false;
        }
        return true;
    };
    // JOC's reconstructed per-object audio - parallel to
    // first.object_metadata->objects (same index, same object). With no
    // objects_dir nothing keeps it: only the fact that some arrived matters
    // to the report. With one, each object streams to its own mono WAV. An
    // access unit whose object_audio size doesn't match the sinks is
    // skipped rather than resized into: DecodedSubstream's own comment
    // documents this as reachable (a program-shape mismatch JOC's ordering
    // can't be lined up against), not something worth failing the whole
    // decode over.
    bool have_object_audio = false;
    std::vector<PlanarWavSink> object_sinks;
    const auto abort_all = [&] {
        sink.abort();
        for (auto& object_sink : object_sinks) {
            object_sink.abort();
        }
    };
    const auto append_objects = [&](const std::vector<std::vector<float>>& object_audio,
                                    std::uint32_t sample_rate) -> bool {
        if (object_audio.empty()) {
            return true;
        }
        have_object_audio = true;
        if (objects_dir.empty()) {
            return true;
        }
        if (object_sinks.empty()) {
            std::error_code ec;
            const std::filesystem::path dir{std::string{objects_dir}};
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                fmt::println(stderr, "error: cannot create directory {} ({})", objects_dir,
                             ec.message());
                return false;
            }
            object_sinks.resize(object_audio.size());
            for (std::size_t i = 0; i < object_sinks.size(); ++i) {
                const auto object_path = dir / fmt::format("object_{:02}.wav", i);
                if (!object_sinks[i].open(object_path.string(), sample_rate, 1, {})) {
                    fmt::println(stderr, "error: cannot open {} for writing",
                                 object_path.string());
                    return false;
                }
            }
        }
        if (object_audio.size() != object_sinks.size()) {
            return true;  // shape mismatch: skipped, same as the old append
        }
        for (std::size_t i = 0; i < object_sinks.size(); ++i) {
            if (!object_sinks[i].append(0, object_audio[i])) {
                fmt::println(stderr, "error: cannot write object audio under {}", objects_dir);
                return false;
            }
        }
        return true;
    };
    // Roadmap IM2: the ADM master accumulates in memory across the whole decode (the bed's own
    // LFE channel plus each JOC-reconstructed dynamic object's full-duration PCM, and every OAMD
    // update block's absolute-sample-timestamped position/gain) and is written once, after the
    // decode loop below finishes - unlike the streaming per-object WAVs objects_dir writes above,
    // the ADM master's own <axml> chunk needs every dynamic object's own final duration known
    // before it can be built at all (ac3::admbridge::write() computes each audioBlockFormat's
    // duration from it - see bridge.cpp's own build_block_formats).
    const bool have_adm_output = !adm_out.empty();
    ac3cli::AdmMasterInput adm_input;
    bool adm_input_ready = false;
    bool adm_bed_warned = false;
    std::uint64_t adm_samples_emitted = 0;
    const auto accumulate_adm = [&](const std::vector<std::vector<float>>& object_audio,
                                    const std::optional<ac3::oba::DecodedProgram>& object_metadata,
                                    std::span<const std::vector<float>> channels,
                                    ac3::eac3::chanmap::Layout layout, std::uint32_t sample_rate) {
        if (!have_adm_output || object_audio.empty() || !object_metadata) {
            return;
        }
        const auto& program = object_metadata->program;
        if (!program.dynamic_only) {
            // A genuine bed program (third-party channel-based-immersive content, oamd.hpp's
            // own Program comment) is out of this writer's current scope - see
            // ac3::admbridge::WriteInput's own doc comment. Warned once; the WAV/objects_dir
            // outputs this decode already produces are unaffected.
            if (!adm_bed_warned) {
                fmt::println(stderr,
                             "warning: {} only supports dynamic-object-only Atmos programmes "
                             "today; this stream carries a bed program, no ADM master written",
                             adm_out);
                adm_bed_warned = true;
            }
            return;
        }
        const int lfe_slot = layout.index_of(ac3::eac3::chanmap::Location::kLfe);
        const bool have_lfe =
            program.lfe && lfe_slot >= 0 && static_cast<std::size_t>(lfe_slot) < channels.size();
        if (!adm_input_ready) {
            adm_input.sample_rate = sample_rate;
            adm_input.channels.resize(object_audio.size() + (have_lfe ? 1 : 0));
            for (std::size_t i = 0; i < object_audio.size(); ++i) {
                adm_input.channels[i].name = fmt::format("Object {}", i + 1);
            }
            if (have_lfe) {
                adm_input.channels.back().name = "LFE";
                adm_input.channels.back().bed_label = ac3::oba::BedLabel::kLfe;
            }
            adm_input_ready = true;
        }
        if (object_audio.size() + (have_lfe ? 1 : 0) != adm_input.channels.size()) {
            return;  // shape mismatch: skipped, same convention as append_objects above
        }
        for (std::size_t i = 0; i < object_audio.size(); ++i) {
            auto& pcm = adm_input.channels[i].pcm;
            pcm.insert(pcm.end(), object_audio[i].begin(), object_audio[i].end());
        }
        if (have_lfe) {
            auto& pcm = adm_input.channels.back().pcm;
            const auto& lfe_channel = channels[static_cast<std::size_t>(lfe_slot)];
            pcm.insert(pcm.end(), lfe_channel.begin(), lfe_channel.end());
        }
        // §5.6.2.1: sample_offset is already in samples from THIS access unit's own first
        // sample - adm_samples_emitted (bumped at the bottom of this lambda by exactly the
        // number of samples object_audio just contributed) turns it into an absolute offset
        // from the start of the whole decode, which is what ac3::admbridge::WriteObjectUpdate
        // wants (bridge.hpp's own doc comment).
        for (const auto& block : object_metadata->blocks) {
            const auto sample_offset =
                adm_samples_emitted + static_cast<std::uint64_t>(std::max(block.sample_offset, 0));
            for (std::size_t i = 0; i < block.objects.size() && i < object_audio.size(); ++i) {
                adm_input.channels[i].updates.push_back({.sample_offset = sample_offset,
                                                          .ramp_duration_samples = block.ramp_duration,
                                                          .state = block.objects[i]});
            }
        }
        adm_samples_emitted += object_audio.front().size();
    };
    ac3::DecodedAccessUnit first{};
    // What the independent (bed) substream actually carried, reported whether
    // or not it was applied - same convention as run_decode's own dynrng_min_db/
    // dynrng_max_db/compr_min_db/compr_max_db above, except both are seeded
    // from the first real word rather than from 0.0: a stream whose transmitted
    // dynrng/compr never happens to cross exactly unity would otherwise have
    // its true min or max silently clamped to 0 dB by the seed itself.
    double dynrng_min_db = 0.0;
    double dynrng_max_db = 0.0;
    std::size_t dynrng_words = 0;
    double compr_min_db = 0.0;
    double compr_max_db = 0.0;
    std::size_t compr_frames = 0;
    // §7.10: access units that came back reconstructed, bed-only or otherwise
    // concealed rather than decoded. Zero unless conceal= asked for it.
    std::size_t concealed_units = 0;
    // numblkscod bounds how many of `dynrng`'s kBlocksPerFrame entries are
    // real: E-AC-3 (unlike AC-3) can code as few as one block per syncframe,
    // and the rest of the fixed-size array is never written (DecodedSubstream::
    // dynrng's own comment) - folding those unwritten, always-unity entries in
    // here would understate the true range for any such stream.
    const auto track_metadata = [&](const std::array<std::uint8_t, ac3::kBlocksPerFrame>& dynrng,
                                    int numblkscod, std::optional<std::uint8_t> compr) {
        const auto nblks =
            static_cast<std::size_t>(ac3::eac3::blocks_per_syncframe(numblkscod));
        for (std::size_t i = 0; i < nblks; ++i) {
            const double db = ac3::meta::to_db(ac3::meta::dynrng_gain(dynrng[i]));
            dynrng_min_db = dynrng_words == 0 ? db : std::min(dynrng_min_db, db);
            dynrng_max_db = dynrng_words == 0 ? db : std::max(dynrng_max_db, db);
            ++dynrng_words;
        }
        if (compr) {
            const double db = ac3::meta::to_db(ac3::meta::compr_gain(*compr));
            compr_min_db = compr_frames == 0 ? db : std::min(compr_min_db, db);
            compr_max_db = compr_frames == 0 ? db : std::max(compr_max_db, db);
            ++compr_frames;
        }
    };
    Progress progress;
    progress.start("decoding", units->size());
    std::uint64_t units_done = 0;
    for (const auto& unit : *units) {
        progress.tick(++units_done);
        const auto decoded = decoder.decode_access_unit(unit);
        if (!decoded) {
            // describe(), not the raw enumerator: this is the line a CI
            // log shows when a third-party stream will not decode, and
            // "decode failed (code 3)" sent the reader to the enum
            // definition to learn it meant a reserved header field.
            fmt::println(stderr, "error: decode failed: {}",
                         ac3::describe(decoded.error()));
            abort_all();
            return kExitInput;
        }
        if (census_wanted) {
            census.observe(census_trace);
        }
        if (!decoded->has_value()) {
            // §3.7: this access unit's frame(s) are being held back pending
            // transient pre-noise processing (Eac3Decoder::decode_access_unit's
            // own doc comment) - nothing new to append yet, not an error.
            continue;
        }
        const auto& out = **decoded;
        if (!sink.is_open()) {
            first = out;
            if (!open_sink(first, out.channels.size())) {
                return kExitOutput;
            }
        }
        if (out.concealed) {
            ++concealed_units;
        }
        track_metadata(out.dynrng, out.numblkscod, out.compr);
        for (std::size_t ch = 0; ch < out.channels.size(); ++ch) {
            if (!sink.append(ch, out.channels[ch])) {
                fmt::println(stderr, "error: cannot write to {}", out_path);
                abort_all();
                return kExitOutput;
            }
        }
        if (!append_objects(out.object_audio, sample_rate_hz(first.sample_rate))) {
            abort_all();
            return kExitOutput;
        }
        accumulate_adm(out.object_audio, out.object_metadata, out.channels, out.layout,
                       sample_rate_hz(first.sample_rate));
    }
    // Whatever transient pre-noise processing was still holding back at
    // end-of-stream. flush() returns raw per-substream results rather than
    // assembled access units (see its own doc comment) - placed at the SAME
    // pcm slot decode_access_unit's own §E3.8.2 assembly would have used
    // (via location_map()), not assumed to already sit at that slot: a lone
    // independent substream's coded order happens to agree with pcm's, but
    // a dependent carrying only its own smaller channel set does not, and
    // naively appending it by coded index corrupts already-established
    // channels (e.g. a bed's L/R) with a dependent's height audio instead.
    const auto flushed = decoder.flush();
    if (!flushed.empty()) {
        // §7.7 words are meaningful at this report's level only from the
        // independent (bed) substream - same convention as
        // DecodedAccessUnit::dynrng/compr above; a dependent flushed here
        // (only possible when transient pre-noise processing has left
        // substreams of one access unit desynchronised at end-of-stream) is
        // never the figure this report promises.
        for (const auto& substream : flushed) {
            if (substream.strmtyp == ac3::eac3::StreamType::kIndependent) {
                track_metadata(substream.dynrng, substream.numblkscod, substream.compr);
            }
        }
        const bool dual_mono = sink.is_open() ? first.acmod == ac3::Acmod::kDualMono
                                              : flushed.front().acmod == ac3::Acmod::kDualMono;
        if (dual_mono) {
            // No Table E2.5 location to place by - dual mono is always a
            // lone substream with no dependents and no spatial layout
            // (decode_access_unit's own comment) - so its channels go
            // straight out in coded order, same as decode_access_unit.
            for (const auto& substream : flushed) {
                if (!sink.is_open()) {
                    first.acmod = ac3::Acmod::kDualMono;
                    first.sample_rate = substream.sample_rate;
                    first.dialnorm = substream.dialnorm;
                    first.substream_count = 1;
                    first.object_metadata = substream.object_metadata;
                    if (!open_sink(first, substream.channels.size())) {
                        return kExitOutput;
                    }
                }
                for (std::size_t ch = 0; ch < substream.channels.size(); ++ch) {
                    if (!sink.append(ch, substream.channels[ch])) {
                        fmt::println(stderr, "error: cannot write to {}", out_path);
                        abort_all();
                        return kExitOutput;
                    }
                }
            }
        } else {
            if (!sink.is_open()) {
                // No access unit ever completed - synthesize the program's
                // layout by unioning every flushed substream's own
                // locations, exactly like decode_access_unit's own §E3.8.2
                // assembly.
                std::uint16_t occupied = 0;
                for (const auto& substream : flushed) {
                    occupied = static_cast<std::uint16_t>(occupied | substream.location_map());
                }
                ac3::DecodedAccessUnit synthesized;
                synthesized.sample_rate = flushed.front().sample_rate;
                synthesized.acmod = flushed.front().acmod;
                synthesized.dialnorm = flushed.front().dialnorm;
                synthesized.substream_count = static_cast<int>(flushed.size());
                synthesized.layout = ac3::eac3::chanmap::expand(occupied);
                // Object audio only ever rides in the bed (the independent
                // substream) - see DecodedAccessUnit::object_metadata's own
                // comment - so at most one flushed substream carries it.
                for (const auto& substream : flushed) {
                    if (substream.object_metadata) {
                        synthesized.object_metadata = substream.object_metadata;
                        break;
                    }
                }
                first = synthesized;
                if (!open_sink(first, static_cast<std::size_t>(first.layout.count))) {
                    return kExitOutput;
                }
            }
            // §E3.8.2 placement: each flushed substream's own channels land
            // at whichever slot their Table E2.5 location occupies in
            // `first.layout`, mirroring decode_access_unit's own assembly
            // loop. Different substreams may append different lengths to
            // different slots here; the sink's per-slot carry absorbs it.
            for (const auto& substream : flushed) {
                // A fold leaves the substream with its own channels in their
                // own order and no Table E2.5 location left to place them by
                // - Eac3Decoder::flush() folds these for exactly the reason
                // this loop exists, so that every frame of the stream leaves
                // at the same width the sink was opened for.
                if (folding(meta, substream.acmod)) {
                    for (std::size_t ch = 0; ch < substream.channels.size() && ch < sink_slots;
                         ++ch) {
                        if (!sink.append(ch, substream.channels[ch])) {
                            fmt::println(stderr, "error: cannot write to {}", out_path);
                            abort_all();
                            return 1;
                        }
                    }
                    continue;
                }
                const auto locations = ac3::eac3::chanmap::expand(substream.location_map());
                for (int i = 0; i < locations.count; ++i) {
                    const int slot = first.layout.index_of(locations[i]);
                    if (slot < 0) {
                        continue;
                    }
                    if (!sink.append(static_cast<std::size_t>(slot),
                                     substream.channels[static_cast<std::size_t>(i)])) {
                        fmt::println(stderr, "error: cannot write to {}", out_path);
                        abort_all();
                        return kExitOutput;
                    }
                }
            }
        }
        // JOC's reconstructed per-object audio, streamed the same way the
        // main access-unit loop's is - see append_objects for why a size
        // mismatch is skipped rather than resized into.
        for (const auto& substream : flushed) {
            if (!append_objects(substream.object_audio,
                                sample_rate_hz(substream.sample_rate))) {
                abort_all();
                return kExitOutput;
            }
            accumulate_adm(substream.object_audio, substream.object_metadata, substream.channels,
                           ac3::eac3::chanmap::expand(substream.location_map()),
                           sample_rate_hz(substream.sample_rate));
        }
    }
    progress.finish();
    if (!sink.is_open()) {
        fmt::println(stderr, "error: no access units");
        return kExitInput;
    }
    // Same point in the sequence as run_decode's: after the loop proved it
    // decoded something, before the sink closes. The accumulation above is
    // per access unit rather than per syncframe because BapCensus::observe
    // folds an access unit's substreams together by stream index itself.
    if (census_wanted && !write_bap_census(census, meta.bap_census_path)) {
        return kExitOutput;
    }
    // Dual mono has no Table E2.5 location to order by - decode_access_unit
    // leaves `layout` empty for exactly this case - so Ch1 and Ch2 go out in
    // coded order, the same identity write_wav_f32 falls back to itself.
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the WAV bytes the write below produces already own stdout in that
    // case, and this report must not land in the middle of them.
    const auto status = status_stream(out_path);
    const auto written = sink.close();
    if (!written) {
        fmt::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return kExitOutput;
    }
    std::size_t objects_written = 0;
    for (auto& object_sink : object_sinks) {
        if (const auto closed = object_sink.close(); !closed) {
            fmt::println(stderr, "error: {}", ac3::io::describe(closed.error()));
            return kExitOutput;        }
        ++objects_written;
    }
    if (have_adm_output) {
        if (!adm_input_ready) {
            fmt::println(stderr, "warning: {} given but no dynamic-object-only Atmos programme was decoded",
                         adm_out);
        } else {
            const auto written_adm = ac3cli::write_adm_atmos_master(adm_out, adm_input);
            if (!written_adm) {
                fmt::println(stderr, "error: {}", written_adm.error());
                return kExitOutput;
            }
            status_println(status, "  wrote ADM master ({} objects) to {}", adm_input.channels.size(), adm_out);
        }
    }
    if (first.acmod == ac3::Acmod::kDualMono) {
        status_println(status, "decoded {} E-AC-3 access units ({} substreams each) -> {}",
                       units->size(), first.substream_count, out_path);
        status_println(status,
                       "  {} channels, {} Hz: Ch1 Ch2 (1+1 dual mono - two programmes, not a "
                       "soundfield)",
                       sink_slots, sample_rate_hz(first.sample_rate));
        print_drc_summary(status, dynrng_min_db, dynrng_max_db, compr_min_db, compr_max_db,
                          compr_frames, meta);
        if (first.info) {
            print_bsi_summary(status, *first.info, first.acmod);
        }
        if (first.mixing) {
            print_mix_summary(status, *first.mixing);
        }
        print_concealment_summary(status, concealed_units, units->size(), "access units");
        return report_decoded_objects(status, first.object_metadata, have_object_audio,
                                      objects_written, objects_dir);
    }
    // The same WAV speaker order the encode side reads a file in, so a stream
    // decoded here and re-encoded lands every channel back where it started -
    // recomputed here only for the speaker-name report; the sink applied it.
    const auto map = plan::wav_order(
        std::span{first.layout.items}.first(static_cast<std::size_t>(first.layout.count)));
    std::string speakers;
    for (const auto index : map) {
        speakers += ac3::eac3::chanmap::name(first.layout[static_cast<int>(index)]);
        speakers += ' ';
    }
    status_println(status, "decoded {} E-AC-3 access units ({} substreams each) -> {}",
                   units->size(), first.substream_count, out_path);
    if (folding(meta, first.acmod)) {
        // The rendered layout is still worth naming: it is what the fold was
        // taken FROM, and a 7.1.4 folded to stereo is a materially different
        // claim from a 5.1 folded to stereo.
        status_println(status, "  {} channels, {} Hz: {} -> {}", sink_slots,
                       sample_rate_hz(first.sample_rate), speakers,
                       fold_name(meta.output.target));
    } else {
        status_println(status, "  {} channels, {} Hz: {}", map.size(),
                       sample_rate_hz(first.sample_rate), speakers);
    }
    print_drc_summary(status, dynrng_min_db, dynrng_max_db, compr_min_db, compr_max_db,
                      compr_frames, meta);
    if (first.info) {
        print_bsi_summary(status, *first.info, first.acmod);
    }
    if (first.mixing) {
        print_mix_summary(status, *first.mixing);
    }
    print_concealment_summary(status, concealed_units, units->size(), "access units");
    return report_decoded_objects(status, first.object_metadata, have_object_audio,
                                  objects_written, objects_dir);
}

}  // namespace

int run_decode(std::string_view in_path, std::string_view out_path, const ac3cli::Options& meta,
               std::string_view objects_dir, std::string_view adm_out) {
    const auto stream = read_elementary_stream(in_path);
    if (stream.empty()) {
        return kExitInput;
    }
    if (!apply_object_verification(stream, meta)) {
        return kExitInput;
    }
    // bsid at bit 40 says which syntax this is, before either is assumed.
    // spdif and play branch on it the same way now that both packers handle
    // E-AC-3 (Eac3BurstPacker alongside AC-3's wrap_frame).
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        fmt::println(stderr, "error: {} is too short to hold a syncframe", in_path);
        return kExitInput;
    }
    // ...except for §E2.3.1.2's legacy core, where the first frame is AC-3 and
    // the stream is not: an AC-3 bed with Annex E dependents extending it goes
    // down the access-unit path too, which reads the core natively.
    if (*bsid > 8 || ac3::has_eac3_extension_substreams(stream)) {
        return run_decode_eac3(stream, out_path, meta, objects_dir, adm_out);
    }
    if (!objects_dir.empty()) {
        fmt::println(stderr,
                     "warning: objects_dir given but {} is plain AC-3 - it has no object layer",
                     in_path);
    }
    if (!adm_out.empty()) {
        fmt::println(stderr, "warning: {} given but {} is plain AC-3 - it has no object layer", adm_out, in_path);
    }
    const auto frames = ac3::split_frames(stream);
    if (!frames) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::describe(frames.error()));
        return kExitInput;
    }
    // bap-census= only. The trace pointer stays null otherwise, which is what
    // keeps an ordinary decode at one null test per block and no allocation -
    // the convention DecoderConfig::trace already sets.
    const bool census_wanted = !meta.bap_census_path.empty();
    ac3::verify::FrameTrace census_trace;
    ac3::verify::BapCensus census;
    ac3::FrameDecoder decoder{{.drc_scale = meta.drc_scale,
                              .fast_imdct = meta.fast_imdct,
                              .heavy_compression = meta.p.heavy.has_value(),
                              .output = meta.output,
                              .concealment = meta.concealment,
                              .trace = census_wanted ? &census_trace : nullptr}};
    PlanarWavSink sink;
    std::optional<ac3::analysis::LevelMeter> meter;
    ac3::DecodedFrame first{};
    bool have_first = false;
    // What the stream actually carried, reported whether or not it was applied.
    double dynrng_min_db = 0.0;
    double dynrng_max_db = 0.0;
    std::size_t dynrng_words = 0;
    double compr_min_db = 0.0;
    double compr_max_db = 0.0;
    std::size_t compr_frames = 0;
    Progress progress;
    progress.start("decoding", frames->size());
    std::uint64_t frames_done = 0;
    // §7.10: frames that came back reconstructed rather than decoded. Zero
    // unless conceal= asked for it, since without it a damaged frame fails
    // the command outright a few lines down.
    std::size_t concealed_frames = 0;
    for (const auto& frame : *frames) {
        progress.tick(++frames_done);
        const auto decoded = decoder.decode_frame(frame);
        if (!decoded) {
            fmt::println(stderr, "error: {}: {}", in_path, ac3::describe(decoded.error()));
            sink.abort();
            return kExitInput;
        }
        if (census_wanted) {
            // After the success check, so a refused frame contributes nothing.
            // BapCensus::observe also skips un-allocated blocks itself; both
            // guards exist because a partially-filled trace read as real
            // evidence is exactly the misreading a census must not make.
            census.observe(census_trace);
        }
        if (decoded->concealed) {
            ++concealed_frames;
        }
        for (const auto word : decoded->dynrng) {
            const double db = ac3::meta::to_db(ac3::meta::dynrng_gain(word));
            dynrng_min_db = dynrng_words == 0 ? db : std::min(dynrng_min_db, db);
            dynrng_max_db = dynrng_words == 0 ? db : std::max(dynrng_max_db, db);
            ++dynrng_words;
        }
        if (decoded->compr) {
            const double db = ac3::meta::to_db(ac3::meta::compr_gain(*decoded->compr));
            compr_min_db = compr_frames == 0 ? db : std::min(compr_min_db, db);
            compr_max_db = compr_frames == 0 ? db : std::max(compr_max_db, db);
            ++compr_frames;
        }
        if (!have_first) {
            first = *decoded;
            // The channel permutation the whole-buffer write used to apply
            // at the end is fixed from the first frame's layout - the same
            // values, just needed up front now that samples leave as they
            // decode.
            // A fold has already put its own channels in their own order -
            // L then R, or the one mono channel - so it takes the identity
            // permutation rather than the coded layout's.
            const bool folded = folding(meta, decoded->acmod);
            if (!sink.open(out_path, sample_rate_hz(decoded->sample_rate),
                           decoded->channels.size(),
                           folded ? std::vector<std::size_t>{}
                                  : ac3::io::wav_channel_order(decoded->acmod, decoded->lfe))) {
                fmt::println(stderr, "error: cannot open {} for writing", out_path);
                return kExitOutput;
            }
            // The meter reports what was WRITTEN, so a folded run meters the
            // fold rather than the coded layout it no longer carries.
            meter.emplace(folded ? (decoded->channels.size() == 1 ? ac3::Acmod::k1_0
                                                                 : ac3::Acmod::k2_0)
                                 : decoded->acmod,
                          folded ? false : decoded->lfe,
                          sample_rate_hz(decoded->sample_rate));
            have_first = true;
        }
        std::vector<std::span<const float>> views;
        views.reserve(decoded->channels.size());
        for (std::size_t ch = 0; ch < decoded->channels.size(); ++ch) {
            if (!sink.append(ch, decoded->channels[ch])) {
                fmt::println(stderr, "error: cannot write to {}", out_path);
                sink.abort();
                return kExitOutput;
            }
            views.emplace_back(decoded->channels[ch]);
        }
        // have_first gates meter.emplace() a few lines up, in this same
        // iteration on the first pass and an earlier one on every pass
        // after, so meter is always engaged by the time this line runs.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        meter->process(views);
    }
    progress.finish();
    if (!have_first) {
        fmt::println(stderr, "error: no frames");
        return kExitInput;
    }
    if (census_wanted && !write_bap_census(census, meta.bap_census_path)) {
        return kExitOutput;
    }
    const auto written = sink.close();
    if (!written) {
        fmt::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return kExitOutput;
    }
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the WAV bytes just written above already own stdout in that case,
    // and this report must not land in the middle of them.
    const auto status = status_stream(out_path);
    const bool folded = folding(meta, first.acmod);
    status_println(status, "decoded {} frames -> {} ({}, {} Hz)", frames->size(), out_path,
                   folded ? fmt::format("{} -> {}",
                                        ac3::analysis::layout_name(first.acmod, first.lfe),
                                        fold_name(meta.output.target))
                          : std::string{ac3::analysis::layout_name(first.acmod, first.lfe)},
                   sample_rate_hz(first.sample_rate));
    status_println(status, "metadata: dialnorm {} (dialogue at -{} dBFS){}", first.dialnorm,
                   first.dialnorm, dialnorm_note(meta, first.dialnorm));
    if (first.dialnorm2) {
        status_println(status, "          dialnorm2 {} (Ch2, dialogue at -{} dBFS){}",
                       *first.dialnorm2, *first.dialnorm2, first.compr2 ? ", compr2 present" : "");
    }
    status_println(status, "          dynrng {:+.2f} .. {:+.2f} dB{}", dynrng_min_db, dynrng_max_db,
                   dynrng_note(meta));
    if (compr_frames > 0) {
        status_println(status, "          compr  {:+.2f} .. {:+.2f} dB over {} frames{}",
                       compr_min_db, compr_max_db, compr_frames, compr_note(meta));
    } else {
        status_println(status, "          compr  absent");
    }
    // bsid 6 is worth a line of its own: it changes how a decoder reads the
    // last 28 bits of bsi, so "this stream is Annex D" is not an aside.
    if (first.bsid != 8) {
        fmt::println(status, "          bsid {} (Annex D alternate syntax)", first.bsid);
    }
    print_bsi_summary(status, first.info, first.acmod);
    // xbsi2's three flags are AC-3's only home for what E-AC-3 puts in
    // infomdat, so they are reported here rather than folded into `info` -
    // which bits a field came off is part of what a decode report is for.
    if (first.alternate_bsi && first.alternate_bsi->extended) {
        const auto& extended = *first.alternate_bsi->extended;
        if (extended.dsurexmod != ac3::meta::SurroundExMode::kNotIndicated) {
            fmt::println(status, "  dsurexmod: {}", ac3::meta::describe(extended.dsurexmod));
        }
        if (extended.dheadphonmod != ac3::meta::HeadphoneMode::kNotIndicated) {
            fmt::println(status, "  dheadphonmod: {}",
                         ac3::meta::describe(extended.dheadphonmod));
        }
        if (extended.adconvtyp != ac3::meta::AdConverterType::kStandard) {
            fmt::println(status, "  A/D converter: {}", ac3::meta::describe(extended.adconvtyp));
        }
    }
    if (first.alternate_bsi && first.alternate_bsi->mix) {
        const auto& mix = *first.alternate_bsi->mix;
        fmt::println(status, "  xbsi1: preferred downmix {}, Lt/Rt {:+.1f}/{:+.1f} dB, "
                             "Lo/Ro {:+.1f}/{:+.1f} dB (centre/surround)",
                     mix.dmixmod == ac3::meta::DownmixMode::kLtRt   ? "Lt/Rt"
                     : mix.dmixmod == ac3::meta::DownmixMode::kLoRo ? "Lo/Ro"
                                                                   : "not indicated",
                     ac3::meta::to_db(ac3::meta::coefficient(mix.ltrtcmixlev)),
                     ac3::meta::to_db(ac3::meta::coefficient(mix.ltrtsurmixlev)),
                     ac3::meta::to_db(ac3::meta::coefficient(mix.lorocmixlev)),
                     ac3::meta::to_db(ac3::meta::coefficient(mix.lorosurmixlev)));
    }
    print_concealment_summary(status, concealed_frames, frames->size(), "frames");
    // The have_first check above already returned if the frame loop never
    // ran, and it is that same loop's first iteration that emplaces meter.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    print_channel_summary(*meter, status);
    return 0;
}

}  // namespace ac3cli::commands
