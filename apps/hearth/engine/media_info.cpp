#include "media_info.hpp"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <span>
#include <utility>

#include "ac3/analysis/levels.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/version.hpp"
#include "ac4/ac4.hpp"

// See media_info.hpp.

namespace ac3::hearth {

namespace {

// TS 103 190 Annex G's sync words, 0xAC40 and 0xAC41 (the second with a CRC):
// what tells an AC-4 stream from an AC-3 or E-AC-3 one, whose 0x0B77 no AC-4
// stream starts with.
[[nodiscard]] bool is_ac4(std::span<const std::byte> bytes) {
    return bytes.size() >= 2 && bytes[0] == std::byte{0xAC} &&
           (bytes[1] == std::byte{0x40} || bytes[1] == std::byte{0x41});
}

[[nodiscard]] MediaCodec codec_of(io::StreamKind kind) {
    switch (kind) {
        case io::StreamKind::kAc3: return MediaCodec::kAc3;
        case io::StreamKind::kEac3: return MediaCodec::kEac3;
        case io::StreamKind::kAc3CoreEac3Extension: return MediaCodec::kAc3WithEac3;
    }
    return MediaCodec::kEac3;
}

// The first syncframe's bitstream information - an AC-3 frame's, a legacy
// core's, or an E-AC-3 independent substream's - parsed as ac3::io::probe
// parses it, with the transform left out. Dependent substreams extend the
// programme's channels, not its bitstream information.
[[nodiscard]] std::optional<MediaBitstream> read_bitstream(std::span<const std::byte> unit) {
    DecoderConfig config;
    config.skip_reconstruction = true;
    std::size_t offset = 0;
    while (offset < unit.size()) {
        const auto rest = unit.subspan(offset);
        const auto header = io::read_frame_header(rest);
        if (!header || header->bytes == 0 || header->bytes > rest.size()) {
            return std::nullopt;
        }
        const auto frame = rest.first(header->bytes);
        offset += header->bytes;
        if (header->kind == io::StreamKind::kAc3) {
            FrameDecoder decoder{config};
            const auto decoded = decoder.decode_frame(frame);
            if (!decoded) {
                return std::nullopt;
            }
            return MediaBitstream{
                .acmod = decoded->acmod,
                .info = decoded->info,
                .alternate_bsi = decoded->alternate_bsi,
                .cmixlev = decoded->cmixlev,
                .surmixlev = decoded->surmixlev,
                .mixing = std::nullopt,
                .levels = mix_levels(decoded->acmod, decoded->cmixlev, decoded->surmixlev,
                                     decoded->alternate_bsi)};
        }
        if (header->strmtyp == eac3::StreamType::kDependent) {
            continue;
        }
        Eac3Decoder decoder{config};
        const auto decoded = decoder.decode_substream(frame);
        if (!decoded || !decoded->has_value()) {
            return std::nullopt;
        }
        const DecodedSubstream& sub = **decoded;
        return MediaBitstream{.acmod = sub.acmod,
                              .info = sub.info,
                              .alternate_bsi = std::nullopt,
                              .cmixlev = std::nullopt,
                              .surmixlev = std::nullopt,
                              .mixing = sub.mixing,
                              .levels = mix_levels(sub.mixing)};
    }
    return std::nullopt;
}

// ac3::sendspin's writer, taken as the sink probe_json writes to.
class StringSink final : public apps::JsonSink {
public:
    explicit StringSink(std::string& out) : writer_(out) {}

    void begin_object() override { writer_.begin_object(); }
    void end_object() override { writer_.end_object(); }
    void begin_array() override { writer_.begin_array(); }
    void end_array() override { writer_.end_array(); }
    void key(std::string_view name) override { writer_.key(name); }
    void value(std::string_view text) override { writer_.string(text); }
    void value(const char* text) override { writer_.string(std::string_view{text}); }
    void value(bool flag) override { writer_.boolean(flag); }
    void value(std::int64_t number) override { writer_.integer(number); }
    void value(std::uint64_t number) override { writer_.unsigned_integer(number); }
    void value(double number, int decimals) override { writer_.number(number, decimals); }
    void value_null() override { writer_.null(); }

private:
    sendspin::json::Writer writer_;
};

using apps::JsonSink;

void member_or_null(JsonSink& json, std::string_view name, const std::optional<int>& value) {
    if (value) {
        json.member(name, static_cast<std::int64_t>(*value));
    } else {
        json.member_null(name);
    }
}

void member_or_null(JsonSink& json, std::string_view name, const std::optional<bool>& value) {
    if (value) {
        json.member(name, *value);
    } else {
        json.member_null(name);
    }
}

void text_or_null(JsonSink& json, std::string_view name, std::string_view text) {
    if (text.empty()) {
        json.member_null(name);
    } else {
        json.member(name, text);
    }
}

// A downmix coefficient as dB; a silent one is written as null.
[[nodiscard]] double level_db(double coefficient) {
    return 20.0 * std::log10(coefficient);
}

// A transmitted code and what it means, as {code, db}.
void write_level(JsonSink& json, std::string_view name, int code, double db) {
    json.key(name);
    json.begin_object();
    json.member("code", static_cast<std::int64_t>(code));
    json.member("db", db, 2);
    json.end_object();
}

void write_container(JsonSink& json, const apps::ContainerFacts& facts) {
    json.key("container");
    if (facts.kind == apps::ContainerKind::kUnknown) {
        json.value_null();
        return;
    }
    json.begin_object();
    json.member("format", apps::container_token(facts.kind));
    text_or_null(json, "codec_id", facts.codec_id);
    json.member("track", static_cast<std::uint64_t>(facts.track));
    text_or_null(json, "language", facts.language);
    json.member("samples", static_cast<std::uint64_t>(facts.samples));
    if (facts.sample_rate != 0) {
        json.member("sample_rate_hz", static_cast<std::uint64_t>(facts.sample_rate));
    } else {
        json.member_null("sample_rate_hz");
    }
    if (facts.channels != 0) {
        json.member("channels", static_cast<std::int64_t>(facts.channels));
    } else {
        json.member_null("channels");
    }

    json.key("mp4");
    if (facts.kind == apps::ContainerKind::kMp4) {
        json.begin_object();
        json.member("timescale", static_cast<std::uint64_t>(facts.timescale));
        json.member("movie_timescale", static_cast<std::uint64_t>(facts.movie_timescale));
        json.member("edits", static_cast<std::uint64_t>(facts.edits));
        json.key("codec_box");
        if (facts.codec_box) {
            const apps::CodecBox& box = *facts.codec_box;
            json.begin_object();
            json.member("type", box.type);
            json.member("bytes", static_cast<std::uint64_t>(box.bytes));
            json.member("fscod", static_cast<std::int64_t>(box.fscod));
            json.member("bsid", static_cast<std::int64_t>(box.bsid));
            json.member("bsmod", static_cast<std::int64_t>(box.bsmod));
            json.member("bsmod_label",
                        apps::probe_json::bsmod_label(box.bsmod, static_cast<Acmod>(box.acmod)));
            json.member("acmod", static_cast<std::int64_t>(box.acmod));
            json.member("lfeon", box.lfeon);
            json.member("bit_rate_code", static_cast<std::int64_t>(box.bit_rate_code));
            json.member("data_rate_kbps", static_cast<std::int64_t>(box.data_rate_kbps));
            json.member("independent_substreams",
                        static_cast<std::int64_t>(box.independent_substreams));
            json.member("num_dep_sub", static_cast<std::int64_t>(box.num_dep_sub));
            json.member("chan_loc", static_cast<std::int64_t>(box.chan_loc));
            json.member("asvc", box.asvc);
            json.member("asvc_label", apps::probe_json::asvc_label(box.asvc));
            member_or_null(json, "complexity_index", box.complexity_index);
            json.end_object();
        } else {
            json.value_null();
        }
        json.end_object();
    } else {
        json.value_null();
    }

    json.key("mpegts");
    if (facts.kind == apps::ContainerKind::kMpegTs) {
        json.begin_object();
        json.member("program_number", static_cast<std::int64_t>(facts.program_number));
        json.member("pmt_pid", static_cast<std::int64_t>(facts.pmt_pid));
        json.member("stream_type", static_cast<std::int64_t>(facts.stream_type));
        json.member("signalling", facts.signalling);
        json.member("packet_size", static_cast<std::uint64_t>(facts.packet_size));
        json.key("service");
        if (facts.service_present) {
            json.begin_object();
            json.member("bsmod", static_cast<std::int64_t>(facts.service_bsmod));
            json.member("bsmod_present", facts.service_bsmod_present);
            // No bsmod_label here: bsmod 7's label depends on acmod (voice
            // over vs. karaoke), and channel_flags() - the only acmod-shaped
            // thing this descriptor carries - is a many-to-one summary that
            // cannot be read back into an exact acmod (see
            // mpegts::parse_service_descriptor's own comment). Showing one
            // label anyway would sometimes just be wrong; a caller that has
            // the elementary stream can label service_bsmod itself with the
            // acmod ac3::io::scan() actually read.
            member_or_null(json, "full_service", facts.service_full_service);
            json.member("bsid", static_cast<std::int64_t>(facts.service_bsid));
            member_or_null(json, "mainid", facts.service_mainid);
            json.member("priority", static_cast<std::int64_t>(facts.service_priority));
            member_or_null(json, "asvc", facts.service_asvc);
            json.member("mix_metadata", facts.service_mix_metadata);
            json.end_object();
        } else {
            json.value_null();
        }
        json.end_object();
    } else {
        json.value_null();
    }
    json.end_object();
}

void write_playback(JsonSink& json, const MediaInfo& info) {
    json.key("playback");
    json.begin_object();
    if (info.sample_rate != 0) {
        json.member("sample_rate_hz", static_cast<std::uint64_t>(info.sample_rate));
    } else {
        json.member_null("sample_rate_hz");
    }
    json.member("stream_samples", static_cast<std::uint64_t>(info.stream_samples));
    json.member("skip_samples", static_cast<std::uint64_t>(info.skip_samples));
    if (info.play_samples) {
        json.member("play_samples", static_cast<std::uint64_t>(*info.play_samples));
    } else {
        json.member_null("play_samples");
    }
    const std::uint64_t played = info.played_samples();
    json.member("played_samples", static_cast<std::uint64_t>(played));
    if (info.sample_rate != 0) {
        json.member("duration_seconds",
                    static_cast<double>(played) / static_cast<double>(info.sample_rate), 6);
    } else {
        json.member_null("duration_seconds");
    }
    text_or_null(json, "note", info.note);
    json.end_object();
}

void write_programmes(JsonSink& json, const MediaInfo& info) {
    json.key("programmes");
    json.begin_array();
    for (const MediaProgramme& programme : info.programmes) {
        json.begin_object();
        json.member("substream_id", static_cast<std::int64_t>(programme.substreamid));
        json.member("acmod", static_cast<std::int64_t>(programme.acmod));
        json.member("lfeon", programme.lfe);
        json.member("layout_label", analysis::layout_name(programme.acmod, programme.lfe));
        json.member("channels", static_cast<std::int64_t>(programme.channels));
        json.member("bsid", static_cast<std::int64_t>(programme.bsid));
        json.member("bsmod", static_cast<std::int64_t>(programme.bsmod));
        json.member("bsmod_label", apps::probe_json::bsmod_label(programme.bsmod, programme.acmod));
        json.member("substreams_per_access_unit",
                    static_cast<std::uint64_t>(programme.substreams_per_unit));
        member_or_null(json, "complexity_index", programme.complexity_index);
        json.member("access_units", static_cast<std::uint64_t>(programme.access_units));
        json.end_object();
    }
    json.end_array();

    json.key("associated_services");
    json.begin_array();
    for (std::size_t index = 0; index < info.associated_services.size(); ++index) {
        const io::SubstreamService& service = info.associated_services[index];
        if (!service.present) {
            continue;
        }
        json.begin_object();
        json.member("substream_id", static_cast<std::int64_t>(index + 1));
        json.member("bsmod", static_cast<std::int64_t>(service.bsmod));
        json.member("bsmod_present", service.bsmod_present);
        json.member("bsmod_label", apps::probe_json::bsmod_label(service.bsmod, service.acmod));
        json.member("acmod", static_cast<std::int64_t>(service.acmod));
        json.member("lfeon", service.lfe);
        json.member("mix_metadata", service.mix_metadata);
        json.end_object();
    }
    json.end_array();
}

void write_production(JsonSink& json, std::string_view name,
                      const std::optional<meta::AudioProduction>& production) {
    json.key(name);
    if (!production) {
        json.value_null();
        return;
    }
    json.begin_object();
    json.member("mixlevel", static_cast<std::int64_t>(production->mixlevel));
    json.member("mix_level_db_spl",
                static_cast<std::int64_t>(meta::mix_level_db_spl(production->mixlevel)));
    json.member("roomtyp", static_cast<std::int64_t>(production->roomtyp));
    json.member("roomtyp_label", meta::describe(production->roomtyp));
    json.member("adconvtyp", static_cast<std::int64_t>(production->adconvtyp));
    json.member("adconvtyp_label", meta::describe(production->adconvtyp));
    json.end_object();
}

// §5.4.2 / Table E1.2's informational group.
void write_info(JsonSink& json, const meta::BsiInfo& info, Acmod acmod) {
    json.begin_object();
    json.member("bsmod", static_cast<std::int64_t>(info.bsmod));
    json.member("bsmod_label", apps::probe_json::bsmod_label(static_cast<int>(info.bsmod), acmod));
    json.member("dsurmod", static_cast<std::int64_t>(info.dsurmod));
    json.member("dsurmod_label", meta::describe(info.dsurmod));
    json.member("dheadphonmod", static_cast<std::int64_t>(info.dheadphonmod));
    json.member("dheadphonmod_label", meta::describe(info.dheadphonmod));
    json.member("dsurexmod", static_cast<std::int64_t>(info.dsurexmod));
    json.member("dsurexmod_label", meta::describe(info.dsurexmod));
    json.member("copyright", info.copyrightb);
    json.member("original", info.origbs);
    json.member("langcod", info.langcod);
    json.member("langcod2", info.langcod2);
    write_production(json, "audio_production", info.audprod);
    write_production(json, "audio_production2", info.audprod2);
    json.key("timecode1");
    if (info.timecod1) {
        json.begin_object();
        json.member("hours", static_cast<std::int64_t>(info.timecod1->hours));
        json.member("minutes", static_cast<std::int64_t>(info.timecod1->minutes));
        json.member("eight_seconds", static_cast<std::int64_t>(info.timecod1->eight_seconds));
        json.end_object();
    } else {
        json.value_null();
    }
    json.key("timecode2");
    if (info.timecod2) {
        json.begin_object();
        json.member("seconds", static_cast<std::int64_t>(info.timecod2->seconds));
        json.member("frames", static_cast<std::int64_t>(info.timecod2->frames));
        json.member("sixty_fourths", static_cast<std::int64_t>(info.timecod2->sixty_fourths));
        json.end_object();
    } else {
        json.value_null();
    }
    json.member("sourcefscod", info.sourcefscod);
    json.end_object();
}

// Table D2.2's downmix preference, as {code, label}.
void write_downmix_mode(JsonSink& json, std::string_view name, meta::DownmixMode mode) {
    json.key(name);
    json.begin_object();
    json.member("code", static_cast<std::int64_t>(mode));
    json.member("label", meta::describe(mode));
    json.end_object();
}

// mixmdate's fold levels and programme scales (Table E1.2), or Annex D's
// xbsi1, which fills the first five.
void write_mix(JsonSink& json, const meta::MixMetadata& mix) {
    json.begin_object();
    write_downmix_mode(json, "dmixmod", mix.dmixmod);
    const auto fold_level = [&json](std::string_view name, meta::MixLevel code) {
        write_level(json, name, static_cast<int>(code), level_db(meta::coefficient(code)));
    };
    fold_level("ltrtcmixlev", mix.ltrtcmixlev);
    fold_level("lorocmixlev", mix.lorocmixlev);
    fold_level("ltrtsurmixlev", mix.ltrtsurmixlev);
    fold_level("lorosurmixlev", mix.lorosurmixlev);
    json.key("lfemixlevcod");
    if (mix.lfemixlevcod) {
        json.begin_object();
        json.member("code", static_cast<std::int64_t>(*mix.lfemixlevcod));
        json.member("db", meta::lfe_mix_level_db(*mix.lfemixlevcod), 2);
        json.end_object();
    } else {
        json.value_null();
    }
    const auto scale = [&json](std::string_view name, const std::optional<int>& code) {
        json.key(name);
        if (!code) {
            json.value_null();
            return;
        }
        json.begin_object();
        json.member("code", static_cast<std::int64_t>(*code));
        // Code 0 is mute, which has no level in dB.
        if (*code == meta::kPgmScaleMute) {
            json.member_null("db");
        } else {
            json.member("db", meta::pgm_scale_db(*code), 2);
        }
        json.end_object();
    };
    scale("pgmscl", mix.pgmscl);
    scale("pgmscl2", mix.pgmscl2);
    scale("extpgmscl", mix.extpgmscl);

    // Table E2.7's premix-compression triple, shared by mixdef 0x1 (carried
    // directly) and mixdef 0x3 (carried again inside mixdata2e - see
    // MixingParameters::premix's own comment).
    const auto write_premix = [&json](const meta::PremixCompression& premix) {
        json.begin_object();
        json.member("premixcmpsel", static_cast<std::int64_t>(premix.premixcmpsel));
        json.member("premixcmpsel_label", premix.premixcmpsel == meta::PremixCompressionSource::kDynrng
                                               ? "dynrng"
                                               : "compr");
        json.member("drcsrc", static_cast<std::int64_t>(premix.drcsrc));
        json.member("drcsrc_label",
                    premix.drcsrc == meta::DrcSource::kExternal ? "external" : "this_substream");
        json.member("premixcmpscl", static_cast<std::int64_t>(premix.premixcmpscl));
        json.end_object();
    };
    // Table E2.8's per-channel external-programme scale, code 15 is mute.
    const auto write_scale_value = [&json](const std::optional<int>& code) {
        if (!code) {
            json.value_null();
            return;
        }
        json.begin_object();
        json.member("code", static_cast<std::int64_t>(*code));
        if (*code == 15) {
            json.member_null("db");
        } else {
            json.member("db", meta::kExternalScaleDb[static_cast<std::size_t>(*code)], 2);
        }
        json.end_object();
    };
    const auto write_scale_opt = [&json, &write_scale_value](std::string_view name,
                                                              const std::optional<int>& code) {
        json.key(name);
        write_scale_value(code);
    };

    json.key("mixdef");
    json.begin_object();
    json.member("code", static_cast<std::int64_t>(mix.mixing.mixdef));
    switch (mix.mixing.mixdef) {
        case meta::MixDefinition::kNone:
            json.member("label", "none");
            break;
        case meta::MixDefinition::kPremix:
            json.member("label", "premix");
            json.key("premix");
            write_premix(mix.mixing.premix);
            break;
        case meta::MixDefinition::kReserved:
            json.member("label", "reserved");
            // §E2.3.1.23: twelve reserved bits, carried verbatim.
            json.member("reserved", static_cast<std::int64_t>(mix.mixing.reserved));
            break;
        case meta::MixDefinition::kExtended:
            json.member("label", "extended");
            json.key("external");
            if (mix.mixing.external) {
                const auto& external = *mix.mixing.external;
                json.begin_object();
                json.key("premix");
                write_premix(external.premix);
                write_scale_opt("left", external.left);
                write_scale_opt("centre", external.centre);
                write_scale_opt("right", external.right);
                write_scale_opt("left_surround", external.left_surround);
                write_scale_opt("right_surround", external.right_surround);
                write_scale_opt("lfe", external.lfe);
                write_scale_opt("dmixscl", external.dmixscl);
                json.key("auxiliary");
                if (external.auxiliary) {
                    json.begin_array();
                    write_scale_value((*external.auxiliary)[0]);
                    write_scale_value((*external.auxiliary)[1]);
                    json.end_array();
                } else {
                    json.value_null();
                }
                json.end_object();
            } else {
                json.value_null();
            }
            json.key("speech");
            if (mix.mixing.speech) {
                const auto& speech = *mix.mixing.speech;
                json.begin_object();
                json.member("spchdat", static_cast<std::int64_t>(speech.spchdat));
                json.key("additional");
                if (speech.additional) {
                    const auto& additional = *speech.additional;
                    json.begin_object();
                    json.member("spchdat1", static_cast<std::int64_t>(additional.spchdat1));
                    json.member("spchan1att", static_cast<std::int64_t>(additional.spchan1att));
                    json.key("more");
                    if (additional.more) {
                        json.begin_object();
                        json.member("spchdat2", static_cast<std::int64_t>(additional.more->spchdat2));
                        json.member("spchan2att",
                                    static_cast<std::int64_t>(additional.more->spchan2att));
                        json.end_object();
                    } else {
                        json.value_null();
                    }
                    json.end_object();
                } else {
                    json.value_null();
                }
                json.end_object();
            } else {
                json.value_null();
            }
            break;
    }
    json.end_object();

    // §E2.3.1.53-58: placement for a mono or 1+1 programme. pan2 is Ch2's own,
    // 1+1 only.
    const auto write_pan = [&json](const std::optional<meta::PanInfo>& pan) {
        if (!pan) {
            json.value_null();
            return;
        }
        json.begin_object();
        json.member("panmean", static_cast<std::int64_t>(pan->panmean));
        json.member("degrees", static_cast<double>(pan->panmean) * meta::kPanMeanDegreesPerStep, 1);
        json.member("paninfo", static_cast<std::int64_t>(pan->paninfo));
        json.end_object();
    };
    json.key("pan");
    write_pan(mix.pan);
    json.key("pan2");
    write_pan(mix.pan2);

    // §E2.3.1.59-61: one 5-bit word per block, each independently optional.
    json.key("blkmixcfginfo");
    if (mix.blkmixcfginfo) {
        json.begin_array();
        for (const auto& word : *mix.blkmixcfginfo) {
            if (word) {
                json.begin_object();
                json.member("code", static_cast<std::int64_t>(*word));
                json.end_object();
            } else {
                json.value_null();
            }
        }
        json.end_array();
    } else {
        json.value_null();
    }

    json.end_object();
}

void write_bitstream(JsonSink& json, const std::optional<MediaBitstream>& bits) {
    json.key("bitstream");
    if (!bits) {
        json.value_null();
        return;
    }
    json.begin_object();
    json.key("info");
    if (bits->info) {
        write_info(json, *bits->info, bits->acmod);
    } else {
        json.value_null();
    }

    json.key("alternate_bsi");
    if (bits->alternate_bsi) {
        const meta::AlternateBsi& alternate = *bits->alternate_bsi;
        json.begin_object();
        json.key("xbsi1");
        if (alternate.mix) {
            write_mix(json, *alternate.mix);
        } else {
            json.value_null();
        }
        json.key("xbsi2");
        if (alternate.extended) {
            const meta::ExtendedBsi& extended = *alternate.extended;
            json.begin_object();
            json.member("dsurexmod", static_cast<std::int64_t>(extended.dsurexmod));
            json.member("dsurexmod_label", meta::describe(extended.dsurexmod));
            json.member("dheadphonmod", static_cast<std::int64_t>(extended.dheadphonmod));
            json.member("dheadphonmod_label", meta::describe(extended.dheadphonmod));
            json.member("adconvtyp", static_cast<std::int64_t>(extended.adconvtyp));
            json.member("adconvtyp_label", meta::describe(extended.adconvtyp));
            json.member("xbsi2", static_cast<std::int64_t>(extended.xbsi2));
            json.member("encinfo", extended.encinfo);
            json.end_object();
        } else {
            json.value_null();
        }
        json.end_object();
    } else {
        json.value_null();
    }

    if (bits->cmixlev) {
        write_level(json, "cmixlev", static_cast<int>(*bits->cmixlev),
                    level_db(meta::coefficient(*bits->cmixlev)));
    } else {
        json.member_null("cmixlev");
    }
    if (bits->surmixlev) {
        write_level(json, "surmixlev", static_cast<int>(*bits->surmixlev),
                    level_db(meta::coefficient(*bits->surmixlev)));
    } else {
        json.member_null("surmixlev");
    }

    json.key("mixing");
    if (bits->mixing) {
        write_mix(json, *bits->mixing);
    } else {
        json.value_null();
    }

    const MixLevels& levels = bits->levels;
    json.key("fold_levels");
    json.begin_object();
    json.member("loro_centre_db", level_db(levels.loro_clev), 2);
    json.member("loro_surround_db", level_db(levels.loro_slev), 2);
    json.member("ltrt_centre_db", level_db(levels.ltrt_clev), 2);
    json.member("ltrt_surround_db", level_db(levels.ltrt_slev), 2);
    if (levels.lfe_mix_level_db) {
        json.member("lfe_db", *levels.lfe_mix_level_db, 2);
    } else {
        json.member_null("lfe_db");
    }
    write_downmix_mode(json, "preferred", levels.preferred);
    json.end_object();

    json.end_object();
}

void write_probe(JsonSink& json, const MediaInfo& info) {
    json.key("probe");
    if (info.probe) {
        json.begin_object();
        json.member("schema", "ac3forge.probe/1");
        apps::probe_json::write_stream(json, *info.probe);
        json.end_object();
        return;
    }
    if (info.ac4 && info.ac4->sync_frames > 0) {
        json.begin_object();
        json.member("schema", "ac3forge.probe/1");
        apps::probe_json::write_ac4_stream(json, *info.ac4);
        json.end_object();
        return;
    }
    json.value_null();
}

}  // namespace

std::string_view codec_token(MediaCodec codec) {
    switch (codec) {
        case MediaCodec::kAc3: return "ac3";
        case MediaCodec::kEac3: return "eac3";
        case MediaCodec::kAc3WithEac3: return "ac3+eac3";
        case MediaCodec::kAc4: return "ac4";
    }
    return "eac3";
}

std::uint64_t MediaInfo::played_samples() const {
    const std::uint64_t after = stream_samples > skip_samples ? stream_samples - skip_samples : 0;
    return play_samples ? std::min(*play_samples, after) : after;
}

MediaInfo describe_media(const std::string& path, const LoadedItem& loaded) {
    MediaInfo info;
    info.path = path;
    info.container = loaded.container;
    info.skip_samples = loaded.skip_samples;
    info.play_samples = loaded.play_samples;
    info.note = loaded.note;
    const std::span<const std::byte> bytes{loaded.bytes};

    if (is_ac4(bytes)) {
        info.codec = MediaCodec::kAc4;
        auto summary = apps::probe_json::summarize_ac4(bytes);
        if (summary.first_frame) {
            const ac4::Toc& toc = summary.first_frame->toc;
            info.sample_rate = static_cast<std::uint32_t>(toc.sample_rate_hz);
            if (const auto per_frame = ac4::samples_per_frame(toc)) {
                info.stream_samples = static_cast<std::uint64_t>(*per_frame) * summary.sync_frames;
            }
        }
        if (summary.sync_frames == 0) {
            info.error = fmt::format(
                "No AC-4 sync frame could be read: {}.",
                summary.parse_error ? ac4::describe(*summary.parse_error) : "none was found");
        }
        info.ac4 = std::move(summary);
        return info;
    }

    const auto scanned = io::scan(bytes);
    if (!scanned) {
        info.error = fmt::format("The stream could not be read: {}.", io::describe(scanned.error()));
        return info;
    }
    info.codec = codec_of(scanned->kind);
    info.sample_rate = sample_rate_hz(scanned->sample_rate);
    info.stream_samples = io::stream_duration_samples(*scanned);
    for (const io::ScannedProgramme& programme : scanned->programmes) {
        info.programmes.push_back(MediaProgramme{
            .substreamid = programme.substreamid,
            .acmod = programme.acmod,
            .lfe = programme.lfe,
            .channels = programme.channels,
            .bsid = programme.bsid,
            .bsmod = programme.bsmod,
            .substreams_per_unit = programme.substreams_per_unit,
            .complexity_index = programme.oba_complexity_index,
            .access_units = programme.access_units.size()});
    }
    info.associated_services = scanned->associated_substreams;
    info.channel_map = scanned->channel_map;

    // As ac3cli probe walks a stream, over the same units the player plays:
    // the lead programme's.
    io::ProbeOptions options;
    options.authenticity = [](std::span<const std::byte> frame) {
        return signing::has_authenticity_tag(frame);
    };
    io::Prober prober{std::move(options)};
    for (std::size_t index = 0; index < scanned->access_units.size(); ++index) {
        if (const auto pushed = prober.push(scanned->access_units[index]); !pushed) {
            info.error = fmt::format("Access unit {} could not be read: {}.", index,
                                     io::describe(pushed.error()));
            break;
        }
    }
    info.probe = prober.report();
    if (!scanned->access_units.empty()) {
        info.bitstream = read_bitstream(scanned->access_units.front());
    }
    return info;
}

std::string media_info_json(const MediaInfo& info) {
    std::string out;
    StringSink json{out};
    json.begin_object();
    json.member("schema", "ac3forge.hearth.media/1");
    json.member("generator", version_full);
    json.member("file", info.path);
    if (info.codec) {
        json.member("codec", codec_token(*info.codec));
    } else {
        json.member_null("codec");
    }
    text_or_null(json, "error", info.error);
    write_container(json, info.container);
    write_playback(json, info);
    write_programmes(json, info);
    if (info.channel_map) {
        json.member("channel_map", static_cast<std::int64_t>(*info.channel_map));
    } else {
        json.member_null("channel_map");
    }
    write_bitstream(json, info.bitstream);
    write_probe(json, info);
    json.end_object();
    return out;
}

}  // namespace ac3::hearth
