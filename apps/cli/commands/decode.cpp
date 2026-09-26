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

#include "../ac4_channels.hpp"
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
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/verify/bap_census.hpp"
#include "ac3/verify/eac3_mirror.hpp"
#include "ac3/verify/mirror.hpp"
#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "stream_playback.hpp"

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

// Shifts `pcm` later by `delay_samples`: result[n] is pcm[n - delay_samples]
// for n >= delay_samples, silence before it - the same length as `pcm`, not
// longer, so the true last delay_samples samples fall off the end rather than
// growing the file. accumulate_adm's own comment says why this has to happen
// to the bed's LFE before write_adm_atmos_master runs: those trailing samples
// describe a moment the dynamic object channels beside it were never decoded
// far enough to reach either, so there is nothing for them to align with.
std::vector<float> delay_pcm(std::span<const float> pcm, std::size_t delay_samples) {
    std::vector<float> out(pcm.size(), 0.0F);
    if (delay_samples < pcm.size()) {
        const std::size_t keep = pcm.size() - delay_samples;
        std::copy_n(pcm.begin(), keep, out.begin() + static_cast<std::ptrdiff_t>(delay_samples));
    }
    return out;
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
            // RF mode's 11 dB go with each word (ac3::meta::kRfModeGainDb), which is
            // what lifts dialogue from the -31 dBFS the dialnorm line reports to
            // RF mode's -20 dBFS.
            return ", applied with RF mode's +11 dB (drcmode=rf)";
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
    status_println(status, "  concealed {} of {} {} (§7.10)", concealed, total, unit);
}

// Reports the object layer (if any) an E-AC-3 decode found, then what
// objects_dir exported. The object layer's lines are print_object_summary's,
// which 'monitor' prints too - the decode-side mirror of run_atmos_encode's
// own "{N} dynamic objects + the bed's LFE = {M} objects" line. Shared
// between run_decode_eac3's dual-mono and ordinary return paths, even though
// this project's own AtmosEncoder never emits dual mono alongside an object
// container. The object WAVs themselves are streamed out by per-object sinks
// as the decode runs (run_decode_eac3's append_objects) - by the time this
// prints, the files are already closed; this only says what happened.
int report_decoded_objects(FILE* status, const std::optional<ac3::oba::DecodedProgram>& metadata,
                           bool have_object_audio, std::size_t objects_written,
                           std::string_view objects_dir) {
    print_object_summary(status, metadata,
                         have_object_audio ? ", JOC audio reconstructed"
                                           : " (JOC audio not reconstructed)");
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
        status_println(status, "  service: {}", ac3::meta::describe(info.bsmod, acmod));
    }
    if (info.dsurmod != ac3::meta::SurroundMode::kNotIndicated) {
        status_println(status, "  dsurmod: {}", ac3::meta::describe(info.dsurmod));
    }
    if (info.dsurexmod != ac3::meta::SurroundExMode::kNotIndicated) {
        status_println(status, "  dsurexmod: {}", ac3::meta::describe(info.dsurexmod));
    }
    if (info.dheadphonmod != ac3::meta::HeadphoneMode::kNotIndicated) {
        status_println(status, "  dheadphonmod: {}", ac3::meta::describe(info.dheadphonmod));
    }
    // The A/D converter clause is only ever appended for HDCD: "standard" is
    // what §D2.3.1.10 tells an encoder to send when it does not know, so it
    // is an absence of information rather than a claim - and on AC-3 the
    // field is not part of audprodie at all (it lives in xbsi2), where
    // printing it would suggest a bit that was never read.
    const auto production = [&](std::string_view prefix,
                                const ac3::meta::AudioProduction& value) {
        status_println(status, "  {}mixed at {} dB SPL, {}{}", prefix,
                       ac3::meta::mix_level_db_spl(value.mixlevel),
                       ac3::meta::describe(value.roomtyp),
                       value.adconvtyp == ac3::meta::AdConverterType::kHdcd ? ", A/D HDCD" : "");
    };
    if (info.audprod.has_value()) {
        production("", *info.audprod);
    }
    if (info.audprod2.has_value()) {
        production("Ch2 ", *info.audprod2);
    }
    // origbs defaults set, so only a stream declaring itself a COPY is news.
    if (info.copyrightb || !info.origbs) {
        status_println(status, "  {}{}{}", info.copyrightb ? "copyright asserted" : "",
                       info.copyrightb && !info.origbs ? ", " : "",
                       info.origbs ? "" : "a copy, not the original bit stream");
    }
    if (info.sourcefscod) {
        status_println(status, "  source sampled at twice the coded rate (§E2.3.1.63)");
    }
    if (info.timecod1.has_value() || info.timecod2.has_value()) {
        status_println(status, "  timecode: {}",
                       ac3::meta::format_timecode(
                           info.timecod1.value_or(ac3::meta::TimeCodeCoarse{}),
                           info.timecod2.value_or(ac3::meta::TimeCodeFine{})));
    }
}

// The programme-mixing half of mixmdate: what a receiver would use to fold
// this substream against another programme. The five downmix levels are left
// out - they are present on every mixmdate group and say nothing about
// whether this stream is an associated service.
void print_mix_summary(FILE* status, const ac3::meta::MixMetadata& mix) {
    const auto print_scale = [status](std::string_view label, const std::optional<int>& code) {
        if (!code.has_value()) {
            return;
        }
        status_println(status, "  {}: {}", label,
                       *code == ac3::meta::kPgmScaleMute
                           ? std::string{"mute"}
                           : fmt::format("{:+.0f} dB", ac3::meta::pgm_scale_db(*code)));
    };
    print_scale("programme scale", mix.pgmscl);
    print_scale("Ch2 programme scale", mix.pgmscl2);
    print_scale("external programme scale", mix.extpgmscl);

    const auto print_premix = [status](std::string_view indent,
                                       const ac3::meta::PremixCompression& premix) {
        status_println(status, "{}premix compression: {} word, {} source, {}/6", indent,
                       premix.premixcmpsel == ac3::meta::PremixCompressionSource::kDynrng
                           ? "dynrng"
                           : "compr",
                       premix.drcsrc == ac3::meta::DrcSource::kExternal ? "external"
                                                                        : "this substream",
                       premix.premixcmpscl);
    };
    switch (mix.mixing.mixdef) {
        case ac3::meta::MixDefinition::kNone:
            break;
        case ac3::meta::MixDefinition::kPremix:
            status_println(status, "  mixdef 1 (premix compression)");
            print_premix("    ", mix.mixing.premix);
            break;
        case ac3::meta::MixDefinition::kReserved:
            status_println(status, "  mixdef 2 (reserved): 0x{:03X}", mix.mixing.reserved);
            break;
        case ac3::meta::MixDefinition::kExtended: {
            status_println(status, "  mixdef 3 (extended)");
            if (mix.mixing.external.has_value()) {
                const auto& external = *mix.mixing.external;
                print_premix("    ", external.premix);
                const auto print_ext_scale = [status](std::string_view label,
                                                      const std::optional<int>& code) {
                    if (!code.has_value()) {
                        status_println(status, "    {}: off", label);
                        return;
                    }
                    status_println(
                        status, "    {}: {}", label,
                        *code == 15 ? std::string{"mute"}
                                   : fmt::format("{:+.0f} dB",
                                                 ac3::meta::kExternalScaleDb[
                                                     static_cast<std::size_t>(*code)]));
                };
                print_ext_scale("left", external.left);
                print_ext_scale("centre", external.centre);
                print_ext_scale("right", external.right);
                print_ext_scale("left surround", external.left_surround);
                print_ext_scale("right surround", external.right_surround);
                print_ext_scale("lfe", external.lfe);
                print_ext_scale("downmix", external.dmixscl);
                if (external.auxiliary.has_value()) {
                    print_ext_scale("aux 1", (*external.auxiliary)[0]);
                    print_ext_scale("aux 2", (*external.auxiliary)[1]);
                }
            }
            if (mix.mixing.speech.has_value()) {
                const auto& speech = *mix.mixing.speech;
                status_println(status, "    speech enhancement: spchdat={}", speech.spchdat);
                if (speech.additional.has_value()) {
                    status_println(status, "      spchdat1={} spchan1att={}",
                                   speech.additional->spchdat1, speech.additional->spchan1att);
                    if (speech.additional->more.has_value()) {
                        status_println(status, "        spchdat2={} spchan2att={}",
                                       speech.additional->more->spchdat2,
                                       speech.additional->more->spchan2att);
                    }
                }
            }
            break;
        }
    }

    const auto print_pan = [status](std::string_view label,
                                    const std::optional<ac3::meta::PanInfo>& pan) {
        if (!pan.has_value()) {
            return;
        }
        status_println(status, "  {}: {:.1f} degrees clockwise from centre (paninfo {})", label,
                       static_cast<double>(pan->panmean) * ac3::meta::kPanMeanDegreesPerStep,
                       pan->paninfo);
    };
    print_pan("pan", mix.pan);
    print_pan("Ch2 pan", mix.pan2);

    if (mix.blkmixcfginfo.has_value()) {
        status_println(status, "  per-block mixing configuration:");
        for (std::size_t blk = 0; blk < mix.blkmixcfginfo->size(); ++blk) {
            const auto& word = (*mix.blkmixcfginfo)[blk];
            status_println(status, "    block {}: {}", blk,
                           word.has_value() ? std::to_string(*word) : std::string{"-"});
        }
    }
}

// Options for a warning, space-separated.
std::string joined(const std::vector<std::string>& tokens) {
    std::string out;
    for (const std::string& token : tokens) {
        out += out.empty() ? "" : " ";
        out += token;
    }
    return out;
}

// AC-4 (ETSI TS 103 190), through ac4::Decoder: the presentation presentation=,
// presentation-id=, language= and associated= choose (ETSI TS 103 190-2 clause
// 4.8.2), its substreams mixed with the dialogue and associated audio at
// dialogue-gain= and associated-gain= (TS 103 190-1 clause 6.2.16), with the
// dialogue raised by dialogue-enhancement= (ETSI
// TS 103 190-1 clause 5.7.8), at the output level output-level= names and
// compressed in the DRC decoder mode drcmode= names (clause 5.7.9), in the
// layout channels= and downmix= ask for (6.2.17), and a damaged frame
// concealed as conceal= says. The object options are output processing the
// AC-4 decoder does not do yet, and are reported rather than applied.
int run_decode_ac4(std::span<const std::byte> stream, std::string_view in_path, std::string_view out_path,
                   const ac3cli::Options& meta, std::string_view objects_dir, std::string_view adm_out) {
    const auto status = status_stream(out_path);
    if (meta.output.mode != ac3::OperatingMode::kCustom) {
        fmt::println(
            stderr,
            "warning: {} is AC-4: drcmode=line and drcmode=rf are AC-3's and E-AC-3's; AC-4 takes "
            "output-level= and its own drcmode= names",
            in_path);
    }
    if (!meta.ac4_drc_mode.empty() && meta.ac4_drc_mode != "off" &&
        !meta.ac4_output_level.has_value()) {
        fmt::println(
            stderr,
            "error: AC-4's DRC works at an output level: add output-level=<dBFS> to drcmode={}",
            meta.ac4_drc_mode);
        return kExitUsage;
    }
    if (!objects_dir.empty() || !adm_out.empty()) {
        fmt::println(stderr, "warning: {} is AC-4, whose objects are not decoded yet - the object options are ignored",
                     in_path);
    }
    // Options AC-3's and E-AC-3's decode reads: said, not silently dropped.
    // The two that promise a result AC-4 cannot give are refused.
    if (!meta.bap_census_path.empty() || meta.verify_objects) {
        fmt::println(stderr, "error: {} is AC-4: {} AC-3's and E-AC-3's", in_path,
                     meta.verify_objects ? "verify-objects checks the EMDF object signatures of"
                                         : "bap-census= counts the bit allocation of");
        return kExitUsage;
    }
    if (!meta.eac3_decode_tokens.empty()) {
        fmt::println(stderr, "warning: {} is AC-4: {} {} AC-3's and E-AC-3's, and ignored", in_path,
                     joined(meta.eac3_decode_tokens),
                     meta.eac3_decode_tokens.size() == 1 ? "is" : "are");
    }
    const ac4::ScanResult scan = ac4::scan(stream);
    if (scan.frames.empty()) {
        fmt::println(stderr, "error: {} holds no AC-4 sync frame", in_path);
        return kExitInput;
    }
    if (scan.stopped_at.has_value()) {
        fmt::println(stderr, "warning: {}: the sync frames stop at byte {} ({}); decoding the {} before it",
                     in_path, scan.stopped_at_offset, ac4::describe(*scan.stopped_at), scan.frames.size());
    }
    // syntax-trace=: every record the decoder reads, frame by frame, as
    // ac4_syntax.py's `trace` writes what it reads.
    std::ofstream trace_file;
    std::size_t trace_frame = 0;
    const auto trace = [&trace_file, &trace_frame](const ac4::SyntaxRecord& r) {
        trace_file << trace_frame << '\t' << r.substream << '\t' << r.bit_offset << '\t' << r.bits << '\t'
                   << r.value << '\t' << r.name << '\n';
    };
    ac4::DecoderConfig config = ac4_decoder_config(meta);
    if (!meta.syntax_trace_path.empty()) {
        trace_file.open(std::filesystem::path{meta.syntax_trace_path}, std::ios::binary);
        if (!trace_file) {
            fmt::println(stderr, "error: cannot open {} for writing", meta.syntax_trace_path);
            return kExitOutput;
        }
        config.syntax = trace;
    }
    ac4::Decoder decoder(config);
    PlanarWavSink sink;
    std::optional<ac3::analysis::LevelMeter> meter;
    std::vector<std::size_t> meter_order;  // the decoded channel at each of the meter's places
    ac4::DecodedFrame first;
    std::size_t decoded_frames = 0;
    std::size_t waiting_frames = 0;
    std::size_t concealed_frames = 0;  // under conceal=, frames made in place of ones that failed
    Progress progress;
    progress.start("decoding", scan.frames.size());
    std::uint64_t frames_done = 0;
    for (const ac4::SyncFrame& frame : scan.frames) {
        progress.tick(++frames_done);
        trace_frame = static_cast<std::size_t>(frames_done - 1);
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        if (!decoded.has_value()) {
            fmt::println(stderr, "error: {}: frame {}: {}", in_path, frames_done, decoder.refusal_reason());
            sink.abort();
            return kExitInput;
        }
        if (!decoded->has_value()) {
            // A frame waiting for an I-frame, at the start or after a change
            // of source: nothing to write.
            ++waiting_frames;
            continue;
        }
        const ac4::DecodedFrame& pcm = **decoded;
        if (pcm.concealed.has_value()) {
            ++concealed_frames;
        }
        if (!sink.is_open()) {
            first = pcm;
            if (!sink.open(out_path, static_cast<std::uint32_t>(pcm.sample_rate_hz), pcm.channels.size(),
                           ac4_order(pcm.speakers, ac4_wav_rank))) {
                fmt::println(stderr, "error: cannot open {} for writing", out_path);
                return kExitOutput;
            }
            meter_order = ac4_order(pcm.speakers, ac4_meter_rank);
            const bool lfe = std::ranges::find(pcm.speakers, ac4::Speaker::kLfe) != pcm.speakers.end();
            meter.emplace(ac4_bed_acmod(pcm.speakers), lfe, static_cast<std::uint32_t>(pcm.sample_rate_hz),
                          static_cast<int>(pcm.channels.size()));
        }
        if (pcm.speakers != first.speakers || pcm.sample_rate_hz != first.sample_rate_hz) {
            fmt::println(stderr, "error: {}: frame {}: the channel layout or sample rate changes mid-stream",
                         in_path, frames_done);
            sink.abort();
            return kExitInput;
        }
        std::vector<std::span<const float>> views;
        views.reserve(pcm.channels.size());
        for (std::size_t ch = 0; ch < pcm.channels.size(); ++ch) {
            if (!sink.append(ch, pcm.channels[ch])) {
                fmt::println(stderr, "error: cannot write to {}", out_path);
                sink.abort();
                return kExitOutput;
            }
            views.emplace_back(pcm.channels[meter_order[ch]]);
        }
        // Emplaced with the sink's opening, a few lines up.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        meter->process(views);
        ++decoded_frames;
    }
    progress.finish();
    if (decoded_frames == 0) {
        fmt::println(stderr, "error: {}: no frame decoded; the stream sent no I-frame", in_path);
        return kExitInput;
    }
    const auto written = sink.close();
    if (!written.has_value()) {
        fmt::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return kExitOutput;
    }
    if (trace_file.is_open()) {
        trace_file.close();
        if (!trace_file) {
            fmt::println(stderr, "error: cannot write {}", meta.syntax_trace_path);
            return kExitOutput;
        }
    }
    // The channels in the order the file holds them.
    std::string layout;
    for (const std::size_t c : ac4_order(first.speakers, ac4_wav_rank)) {
        layout += layout.empty() ? "" : " ";
        layout += ac4::describe(first.speakers[c]);
    }
    status_println(status, "decoded {} AC-4 frames -> {} ({}, {} Hz)", decoded_frames, out_path, layout,
                   first.sample_rate_hz);
    status_println(status, "          presentation {}{}", first.presentation,
                   first.presentation_id ? fmt::format(" (presentation_id {})", *first.presentation_id)
                                         : std::string{});
    if (waiting_frames > 0) {
        status_println(status, "          {} frames waiting for an I-frame produced no output",
                       waiting_frames);
    }
    if (concealed_frames > 0) {
        status_println(
            status, "          {} of them concealed ({})", concealed_frames,
            config.concealment == ac4::ConcealmentPolicy::kMute ? "muted" : "repeated and faded");
    }
    status_println(status, "          {}", ac4_processing(config.output));
    // Emplaced with the first decoded frame, and decoded_frames > 0 here.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    print_channel_summary(*meter, status);
    return 0;
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
    if (!ids.has_value()) {
        fmt::println(stderr, "error: stream framing failed: {}",
                     ac3::describe(ids.error()));
        return 1;
    }
    if (ids->empty()) {
        fmt::println(stderr, "error: no programmes in stream");
        return 1;
    }
    const auto programme = choose_programme(*ids, meta.programme);
    if (!programme.has_value()) {
        return 1;
    }
    // Access units, not syncframes: a dependent substream is only meaningful
    // alongside the independent one it extends, and the two are rendered
    // together into one set of speaker feeds.
    const auto units = ac3::split_access_units(stream, *programme);
    if (!units.has_value()) {
        fmt::println(stderr, "error: stream framing failed: {}",
                     ac3::describe(units.error()));
        return kExitInput;
    }
    if (ids->size() > 1) {
        status_println(status_stream(out_path), "  programme {} of {} ({})", *programme,
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
                             .programme = programme,
                             .skip_object_reconstruction = meta.bed_only}};
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
    // legacy item IM2: the ADM master accumulates in memory across the whole decode (the bed's own
    // LFE channel plus each JOC-reconstructed dynamic object's full-duration PCM, and every OAMD
    // update block's absolute-sample-timestamped position/gain) and is written once, after the
    // decode loop below finishes - unlike the streaming per-object WAVs objects_dir writes above,
    // the ADM master's own <axml> chunk needs every dynamic object's own final duration known
    // before it can be built at all (ac3::admbridge::write() computes each audioBlockFormat's
    // duration from it - see bridge.cpp's own build_block_formats).
    //
    // The LFE channel this lambda appends below is NOT yet delayed to match the objects beside
    // it - decode_access_unit hands the two to it already ac3::oba::joc::reconstruction_delay()
    // samples apart (docs/library/decoding.md, "Atmos objects lag the bed"), and appending both
    // verbatim, unit by unit, carries that same gap straight into adm_input.channels. delay_pcm()
    // fixes it in one pass, once, on the finished LFE channel below rather than here per unit -
    // this lambda has no reason to know the decoder's own joc_domain.
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
    // The programme's layout, from the first unit decoded - the held-back
    // unit at end-of-stream is laid out against it (held_back_unit's own doc
    // comment). std::nullopt exactly when `first` is still default, i.e. the
    // sink never opened.
    std::optional<ac3::eac3::chanmap::Layout> programme_layout;
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
        if (compr.has_value()) {
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
        if (!decoded.has_value()) {
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
            programme_layout = out.layout;
            if (!open_sink(first, out.channels.size())) {
                return kExitOutput;
            }
        }
        if (out.concealed.has_value()) {
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
    // end-of-stream (§3.7). held_back_unit assembles the flushed substreams
    // the way decode_access_unit's own §E3.8.2 assembly would have, onto
    // `programme_layout` (or, when nothing ever decoded, a layout it synthesizes
    // itself by unioning the flushed substreams' own locations) - see its
    // own doc comment for the placement rules this used to duplicate here.
    const auto flushed = decoder.flush();
    if (!flushed.empty()) {
        const auto held = ac3::apps::held_back_unit(
            flushed, programme_layout, meta.output.target != ac3::DownmixTarget::kAsCoded);
        if (held.has_value()) {
            // §7.7 words are meaningful at this report's level only from the
            // independent (bed) substream - held_back_unit's dynrng/compr/
            // numblkscod already come from the lead substream alone, the
            // same convention DecodedAccessUnit's own fields follow for a
            // live unit.
            track_metadata(held->dynrng, held->numblkscod, held->compr);
            if (!sink.is_open()) {
                first = *held;
                if (!open_sink(first, held->channels.size())) {
                    return kExitOutput;
                }
            }
            for (std::size_t ch = 0; ch < held->channels.size() && ch < sink_slots; ++ch) {
                if (!sink.append(ch, held->channels[ch])) {
                    fmt::println(stderr, "error: cannot write to {}", out_path);
                    abort_all();
                    return kExitOutput;
                }
            }
            if (!append_objects(held->object_audio, sample_rate_hz(held->sample_rate))) {
                abort_all();
                return kExitOutput;
            }
            accumulate_adm(held->object_audio, held->object_metadata, held->channels,
                           held->layout, sample_rate_hz(held->sample_rate));
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
    if (!written.has_value()) {
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
            // accumulate_adm's own comment: the LFE channel it built is still
            // reconstruction_delay(meta.joc_domain) samples ahead of the object
            // channels beside it - the one channel here with bed_label set, so
            // there is no need to have tracked which index it landed at above.
            if (!adm_input.channels.empty() && adm_input.channels.back().bed_label.has_value()) {
                auto& lfe = adm_input.channels.back().pcm;
                lfe = delay_pcm(lfe, static_cast<std::size_t>(ac3::oba::joc::reconstruction_delay(
                                         meta.joc_domain)));
            }
            const auto written_adm = ac3cli::write_adm_atmos_master(adm_out, adm_input);
            if (!written_adm.has_value()) {
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
        if (first.info.has_value()) {
            print_bsi_summary(status, *first.info, first.acmod);
        }
        if (first.mixing.has_value()) {
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
    if (first.info.has_value()) {
        print_bsi_summary(status, *first.info, first.acmod);
    }
    if (first.mixing.has_value()) {
        print_mix_summary(status, *first.mixing);
    }
    print_concealment_summary(status, concealed_units, units->size(), "access units");
    return report_decoded_objects(status, first.object_metadata, have_object_audio,
                                  objects_written, objects_dir);
}

}  // namespace

int run_decode(std::string_view in_path, std::string_view out_path,
               const ac3cli::Options& requested, std::string_view objects_dir,
               std::string_view adm_out) {
    const auto stream = read_elementary_stream(in_path);
    if (stream.empty()) {
        return kExitInput;
    }
    // AC-4's sync words are 0xAC40 and 0xAC41 (TS 103 190-2 Annex G), where
    // AC-3's and E-AC-3's is 0x0B77; the first two bytes decide which decoder
    // reads the stream, before anything below reads it as AC-3.
    if (is_ac4_stream(stream)) {
        return run_decode_ac4(stream, in_path, out_path, requested, objects_dir, adm_out);
    }
    if (!requested.syntax_trace_path.empty()) {
        fmt::println(stderr, "error: syntax-trace= records AC-4 syntax, and {} is AC-3 or E-AC-3", in_path);
        return kExitUsage;
    }
    if (requested.ac4_fold_5x) {
        fmt::println(
            stderr, "error: channels=5.1 folds an AC-4 7.X stream to 5.X, and {} is AC-3 or E-AC-3",
            in_path);
        return kExitUsage;
    }
    if (!requested.ac4_decode_tokens.empty()) {
        fmt::println(stderr, "warning: {} is AC-3 or E-AC-3: {} {} AC-4's, and ignored", in_path,
                     joined(requested.ac4_decode_tokens),
                     requested.ac4_decode_tokens.size() == 1 ? "is" : "are");
    }
    // downmix=auto becomes a concrete fold here, once, from what the stream
    // itself prefers; everything below sees only the fold it settled on.
    auto meta = requested;
    meta.output = resolve_output(requested, stream, status_stream(out_path));
    if (!apply_object_verification(stream, meta, status_stream(out_path))) {
        return kExitInput;
    }
    // bsid at bit 40 says which syntax this is, before either is assumed.
    // spdif and play branch on it the same way now that both packers handle
    // E-AC-3 (Eac3BurstPacker alongside AC-3's wrap_frame).
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid.has_value()) {
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
    if (!frames.has_value()) {
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
        if (!decoded.has_value()) {
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
        if (decoded->concealed.has_value()) {
            ++concealed_frames;
        }
        for (const auto word : decoded->dynrng) {
            const double db = ac3::meta::to_db(ac3::meta::dynrng_gain(word));
            dynrng_min_db = dynrng_words == 0 ? db : std::min(dynrng_min_db, db);
            dynrng_max_db = dynrng_words == 0 ? db : std::max(dynrng_max_db, db);
            ++dynrng_words;
        }
        if (decoded->compr.has_value()) {
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
    if (!written.has_value()) {
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
    if (first.dialnorm2.has_value()) {
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
        status_println(status, "          bsid {} (Annex D alternate syntax)", first.bsid);
    }
    print_bsi_summary(status, first.info, first.acmod);
    // xbsi2's three flags are AC-3's only home for what E-AC-3 puts in
    // infomdat, so they are reported here rather than folded into `info` -
    // which bits a field came off is part of what a decode report is for.
    if (first.alternate_bsi.has_value() && first.alternate_bsi->extended.has_value()) {
        const auto& extended = *first.alternate_bsi->extended;
        if (extended.dsurexmod != ac3::meta::SurroundExMode::kNotIndicated) {
            status_println(status, "  dsurexmod: {}", ac3::meta::describe(extended.dsurexmod));
        }
        if (extended.dheadphonmod != ac3::meta::HeadphoneMode::kNotIndicated) {
            status_println(status, "  dheadphonmod: {}",
                           ac3::meta::describe(extended.dheadphonmod));
        }
        if (extended.adconvtyp != ac3::meta::AdConverterType::kStandard) {
            status_println(status, "  A/D converter: {}", ac3::meta::describe(extended.adconvtyp));
        }
    }
    if (first.alternate_bsi.has_value() && first.alternate_bsi->mix.has_value()) {
        const auto& mix = *first.alternate_bsi->mix;
        status_println(status, "  xbsi1: preferred downmix {}, Lt/Rt {:+.1f}/{:+.1f} dB, "
                               "Lo/Ro {:+.1f}/{:+.1f} dB (centre/surround)",
                       ac3::meta::describe(mix.dmixmod),
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
