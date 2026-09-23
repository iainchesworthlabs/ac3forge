#include "hearth_controller.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

#include "ac3/version.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

// hearth_controller.hpp's Qt headers define `slots` as a macro for the
// classic SIGNAL/SLOT syntax (unless QT_NO_KEYWORDS is set, which this
// project's Qt targets do not - CrucibleController and EncoderController
// both rely on the bare `signals:` keyword this class also uses). Hearth's
// engine is Qt-free by design and apps/hearth/ui is the first place in the
// tree to include both Qt and ac3::render::OutputLayout in one translation
// unit, so this is the first place the collision can happen:
// OutputLayout::slots() is a real method name, and left alone the macro
// rewrites its declaration into nonsense. Undefined here, before anything
// that spells the word - nothing below still needs Qt's old-style slots:
// poll() connects through the modern function-pointer connect(), which
// needs no access-specifier keyword at all.
#undef slots

#include "ac3/analysis/levels.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/speakers.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "decoder_settings.hpp"
#include "diagnostics_report.hpp"
#include "engine_thread.hpp"
#include "item_loader.hpp"
#include "media_inspector.hpp"
#include "output_decision.hpp"
#include "output_selector.hpp"
#include "pairing_store.hpp"
#include "pcm_sink.hpp"
#include "probe_json.hpp"
#include "queue.hpp"
#include "settings_model.hpp"
#include "transport.hpp"

namespace ac3::hearth::ui {

namespace {

// CrucibleController's own rate: fast enough that a command's effect shows
// up within a frame or two, slow enough that a snapshot copy sixteen times a
// second costs nothing worth measuring.
constexpr int kPollMs = 60;

[[nodiscard]] QString transport_state_name(ac3::hearth::TransportState state) {
    switch (state) {
        case ac3::hearth::TransportState::kPlaying:
            return QStringLiteral("playing");
        case ac3::hearth::TransportState::kPaused:
            return QStringLiteral("paused");
        case ac3::hearth::TransportState::kStopped:
        default:
            return QStringLiteral("stopped");
    }
}

[[nodiscard]] QString stream_kind_name(const ac3::hearth::ItemFacts& facts) {
    if (!facts.stream.has_value()) {
        return QString();
    }
    if (facts.has_objects) {
        return QStringLiteral("E-AC-3 JOC");
    }
    return *facts.stream == audio::BitstreamFormat::kAc3 ? QStringLiteral("AC-3") : QStringLiteral("E-AC-3");
}

// The queue row's own codec chip (main-play.png, "01 QUEUE"): "A3"/"E3" from
// what a probe already found (ItemFacts.stream), or "" before that - an AC-4
// item never gets this far (item_loader.cpp accepts .ac4 for reading, but
// io::scan() still refuses it, so ItemFacts.stream stays unset); PlayPage.qml
// falls back to the file's own extension for that one case, a presentation
// question this controller does not need to answer twice.
[[nodiscard]] QString codec_badge(const ac3::hearth::ItemFacts& facts) {
    if (!facts.stream.has_value()) {
        return QString();
    }
    return *facts.stream == audio::BitstreamFormat::kAc3 ? QStringLiteral("A3") : QStringLiteral("E3");
}

// --- MediaInfo -> QVariantMap, field by field (media_info.hpp) -------------
// Follows media_info_json()'s own document shape (see that function's
// comment) so a field here and the same field in "Export JSON..." never say
// two different things; `json` on the top-level map carries that document
// whole. Deliberately short of every field ac3forge.hearth.media/1 carries -
// the deep per-object OAMD table and the per-frame EMDF/CRC dumps are "at
// play time" or "detail" questions (planning/hearth-reference-player.md,
// Media information) the Play page's own monitor and a future slice answer,
// not the Media page's summary.

[[nodiscard]] QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] QVariantMap media_container_to_map(const apps::ContainerFacts& facts) {
    QVariantMap map;
    if (facts.kind == apps::ContainerKind::kUnknown) {
        // Empty reads as "no container: an elementary stream" in QML -
        // Object.keys(inspectedMedia.container).length === 0.
        return map;
    }
    map[QStringLiteral("format")] = to_qstring(apps::container_token(facts.kind));
    map[QStringLiteral("track")] = static_cast<int>(facts.track);
    map[QStringLiteral("language")] = QString::fromStdString(facts.language);
    if (facts.sample_rate != 0) {
        map[QStringLiteral("sampleRate")] = facts.sample_rate;
    }
    if (facts.channels != 0) {
        map[QStringLiteral("channels")] = facts.channels;
    }
    if (facts.kind == apps::ContainerKind::kMp4) {
        map[QStringLiteral("edits")] = static_cast<qlonglong>(facts.edits);
        if (facts.codec_box) {
            const apps::CodecBox& box = *facts.codec_box;
            QVariantMap codec_box;
            codec_box[QStringLiteral("bsid")] = box.bsid;
            codec_box[QStringLiteral("bsmod")] = box.bsmod;
            codec_box[QStringLiteral("bsmodLabel")] =
                to_qstring(apps::probe_json::bsmod_label(box.bsmod, static_cast<Acmod>(box.acmod)));
            codec_box[QStringLiteral("lfeon")] = box.lfeon;
            codec_box[QStringLiteral("dataRateKbps")] = box.data_rate_kbps;
            codec_box[QStringLiteral("independentSubstreams")] = box.independent_substreams;
            codec_box[QStringLiteral("numDepSub")] = box.num_dep_sub;
            if (box.complexity_index) {
                codec_box[QStringLiteral("complexityIndex")] = *box.complexity_index;
            }
            map[QStringLiteral("codecBox")] = codec_box;
        }
    } else if (facts.kind == apps::ContainerKind::kMpegTs) {
        QVariantMap ts;
        ts[QStringLiteral("programNumber")] = static_cast<int>(facts.program_number);
        ts[QStringLiteral("pmtPid")] = static_cast<int>(facts.pmt_pid);
        ts[QStringLiteral("streamType")] = static_cast<int>(facts.stream_type);
        map[QStringLiteral("mpegts")] = ts;
    }
    return map;
}

[[nodiscard]] QVariantList media_programmes_to_list(
    const std::vector<ac3::hearth::MediaProgramme>& programmes) {
    QVariantList list;
    for (const ac3::hearth::MediaProgramme& programme : programmes) {
        QVariantMap row;
        row[QStringLiteral("substreamId")] = programme.substreamid;
        row[QStringLiteral("layoutLabel")] =
            to_qstring(analysis::layout_name(programme.acmod, programme.lfe));
        row[QStringLiteral("channels")] = programme.channels;
        row[QStringLiteral("bsid")] = programme.bsid;
        row[QStringLiteral("bsmodLabel")] =
            to_qstring(apps::probe_json::bsmod_label(programme.bsmod, programme.acmod));
        row[QStringLiteral("accessUnits")] = static_cast<qlonglong>(programme.access_units);
        if (programme.complexity_index) {
            row[QStringLiteral("complexityIndex")] = *programme.complexity_index;
        }
        list.push_back(row);
    }
    return list;
}

[[nodiscard]] QVariantMap media_bitstream_to_map(const ac3::hearth::MediaBitstream& bits) {
    QVariantMap map;
    if (bits.info) {
        const meta::BsiInfo& info = *bits.info;
        map[QStringLiteral("bsmodLabel")] =
            to_qstring(apps::probe_json::bsmod_label(static_cast<int>(info.bsmod), bits.acmod));
        map[QStringLiteral("dsurmodLabel")] = to_qstring(meta::describe(info.dsurmod));
        map[QStringLiteral("copyright")] = info.copyrightb;
        map[QStringLiteral("original")] = info.origbs;
        if (info.audprod) {
            map[QStringLiteral("mixLevelDbSpl")] = meta::mix_level_db_spl(info.audprod->mixlevel);
            map[QStringLiteral("roomTypeLabel")] = to_qstring(meta::describe(info.audprod->roomtyp));
        }
    }
    const MixLevels& levels = bits.levels;
    QVariantMap mix;
    mix[QStringLiteral("centreDb")] = 20.0 * std::log10(levels.loro_clev);
    mix[QStringLiteral("surroundDb")] = 20.0 * std::log10(levels.loro_slev);
    if (levels.lfe_mix_level_db) {
        mix[QStringLiteral("lfeDb")] = *levels.lfe_mix_level_db;
    }
    mix[QStringLiteral("preferredDownmixLabel")] = to_qstring(meta::describe(levels.preferred));
    map[QStringLiteral("mixLevels")] = mix;
    return map;
}

// dynrng/compr are carried as raw §7.7 words, not linear in dB across their
// full range (ac3::meta::dynrng_gain()'s own comment shows the sign-magnitude
// wrap at 0x80) - the same reason apps/common/probe_json.cpp's write_range()
// writes "compr"/"dynrng" as a raw word MinMax rather than a "_db" one the
// way it does for dialnorm. This follows that precedent rather than
// inventing its own dB range over a MinMax that may span the wrap.
[[nodiscard]] QVariantMap media_probe_to_map(const io::ProbeReport& report) {
    QVariantMap map;
    map[QStringLiteral("measuredBitrateKbps")] = report.bitrate_kbps;
    if (report.nominal_bitrate_kbps) {
        map[QStringLiteral("nominalBitrateKbps")] = static_cast<int>(*report.nominal_bitrate_kbps);
    }
    map[QStringLiteral("variableBitrate")] = report.variable_bitrate;
    map[QStringLiteral("accessUnits")] = static_cast<qlonglong>(report.access_units);
    map[QStringLiteral("syncframes")] = static_cast<qlonglong>(report.syncframes);
    map[QStringLiteral("crcFailures")] = static_cast<qlonglong>(report.crc_failures);
    map[QStringLiteral("parseFailures")] = static_cast<qlonglong>(report.parse_failures);

    if (report.dialnorm.seen) {
        map[QStringLiteral("dialnormDb")] = apps::probe_json::dialnorm_db(report.dialnorm.min);
        map[QStringLiteral("dialnormConstant")] = report.dialnorm.constant();
        if (!report.dialnorm.constant()) {
            map[QStringLiteral("dialnormMaxDb")] = apps::probe_json::dialnorm_db(report.dialnorm.max);
        }
    }
    map[QStringLiteral("comprSeen")] = report.compr.seen;
    map[QStringLiteral("dynrngSeen")] = report.dynrng.seen;

    map[QStringLiteral("blocksParsed")] = static_cast<qlonglong>(report.tools.blocks);
    map[QStringLiteral("blockSwitchBlocks")] = static_cast<qlonglong>(report.tools.block_switch);
    map[QStringLiteral("couplingBlocks")] = static_cast<qlonglong>(report.tools.coupling);
    map[QStringLiteral("ahtFrames")] = static_cast<qlonglong>(report.tools.aht_frames);

    map[QStringLiteral("oamd")] = report.oamd;
    map[QStringLiteral("joc")] = report.joc;
    if (report.oba_complexity_index) {
        map[QStringLiteral("complexityIndex")] = *report.oba_complexity_index;
    }
    if (report.program) {
        map[QStringLiteral("objectCount")] = report.program->dynamic_objects;
        map[QStringLiteral("bedLabel")] =
            QString::fromStdString(apps::probe_json::bed_label(*report.program));
    }
    map[QStringLiteral("authenticityTaggedFrames")] =
        static_cast<qlonglong>(report.authenticity_tagged_frames);

    QVariantList payload_ids;
    for (const int id : report.emdf_payload_ids) {
        payload_ids.push_back(id);
    }
    map[QStringLiteral("emdfPayloadIds")] = payload_ids;
    return map;
}

[[nodiscard]] QVariantMap media_ac4_to_map(const apps::probe_json::Ac4Summary& summary) {
    QVariantMap map;
    map[QStringLiteral("syncFrames")] = static_cast<qlonglong>(summary.sync_frames);
    map[QStringLiteral("bytes")] = static_cast<qlonglong>(summary.bytes);
    map[QStringLiteral("crcFailures")] = static_cast<qlonglong>(summary.crc_failures);
    if (summary.parse_error) {
        map[QStringLiteral("parseError")] = to_qstring(apps::probe_json::ac4_error_token(*summary.parse_error));
    }
    if (!summary.first_frame) {
        return map;
    }
    const ac4::Toc& toc = summary.first_frame->toc;
    map[QStringLiteral("bitstreamVersion")] = toc.bitstream_version;
    map[QStringLiteral("sampleRate")] = toc.sample_rate_hz;
    map[QStringLiteral("presentationCount")] = toc.n_presentations;
    map[QStringLiteral("substreamCount")] = toc.n_substreams;

    QVariantList presentations;
    if (!toc.presentations_v0.empty()) {
        int index = 0;
        for (const ac4::PresentationInfoV0& presentation : toc.presentations_v0) {
            QVariantMap row;
            row[QStringLiteral("index")] = index++;
            if (presentation.presentation_id) {
                row[QStringLiteral("id")] = *presentation.presentation_id;
            }
            QVariantList substreams;
            for (const auto& role_and_info : presentation.substreams) {
                QVariantMap sub;
                sub[QStringLiteral("role")] = QString::fromStdString(role_and_info.first);
                sub[QStringLiteral("channelMode")] =
                    QString::fromStdString(role_and_info.second.channel_mode_name);
                substreams.push_back(sub);
            }
            row[QStringLiteral("substreams")] = substreams;
            presentations.push_back(row);
        }
    } else {
        int index = 0;
        for (const ac4::PresentationInfoV1& presentation : toc.presentations_v1) {
            QVariantMap row;
            row[QStringLiteral("index")] = index++;
            if (presentation.presentation_id) {
                row[QStringLiteral("id")] = *presentation.presentation_id;
            }
            QVariantList group_refs;
            for (const int ref : presentation.group_refs) {
                group_refs.push_back(ref);
            }
            row[QStringLiteral("groupRefs")] = group_refs;
            presentations.push_back(row);
        }
    }
    map[QStringLiteral("presentations")] = presentations;

    QVariantList groups;
    int group_index = 0;
    for (const ac4::SubstreamGroupInfo& group : toc.substream_groups) {
        QVariantMap row;
        row[QStringLiteral("index")] = group_index++;
        row[QStringLiteral("channelCoded")] = group.b_channel_coded;
        QVariantList substreams;
        for (const ac4::GroupSubstream& sub : group.substreams) {
            substreams.push_back(QString::fromStdString(apps::probe_json::describe_group_substream(sub)));
        }
        row[QStringLiteral("substreams")] = substreams;
        groups.push_back(row);
    }
    map[QStringLiteral("substreamGroups")] = groups;
    return map;
}

[[nodiscard]] QVariantMap media_info_to_map(const ac3::hearth::MediaInfo& info) {
    QVariantMap map;
    map[QStringLiteral("path")] = QString::fromStdString(info.path);
    if (info.codec) {
        map[QStringLiteral("codec")] = to_qstring(ac3::hearth::codec_token(*info.codec));
    }
    if (!info.error.empty()) {
        map[QStringLiteral("error")] = QString::fromStdString(info.error);
    }
    if (!info.note.empty()) {
        map[QStringLiteral("note")] = QString::fromStdString(info.note);
    }
    map[QStringLiteral("container")] = media_container_to_map(info.container);
    if (info.sample_rate != 0) {
        map[QStringLiteral("sampleRate")] = info.sample_rate;
        map[QStringLiteral("durationSeconds")] =
            static_cast<double>(info.played_samples()) / static_cast<double>(info.sample_rate);
    }
    map[QStringLiteral("streamSamples")] = static_cast<qlonglong>(info.stream_samples);
    map[QStringLiteral("programmes")] = media_programmes_to_list(info.programmes);
    if (info.bitstream) {
        map[QStringLiteral("bitstream")] = media_bitstream_to_map(*info.bitstream);
    }
    if (info.probe) {
        map[QStringLiteral("probe")] = media_probe_to_map(*info.probe);
    }
    if (info.ac4) {
        map[QStringLiteral("ac4")] = media_ac4_to_map(*info.ac4);
    }
    map[QStringLiteral("json")] = QString::fromStdString(media_info_json(info));
    return map;
}

// --- DecoderSettings <-> QVariantMap, field by field (decoder_settings.hpp) ---
// Only the controls the Decoder page can set without stream-dependent
// information: not mix_levels (the design's "From the stream/Set here"
// choice needs the stream's own levels, which this controller does not read
// yet) and not programme (Session's choice of units, not part of
// DecoderSettings at all).

[[nodiscard]] QString mode_name(ac3::OperatingMode mode) {
    switch (mode) {
        case ac3::OperatingMode::kLine:
            return QStringLiteral("line");
        case ac3::OperatingMode::kRf:
            return QStringLiteral("rf");
        case ac3::OperatingMode::kCustom:
        default:
            return QStringLiteral("custom");
    }
}

[[nodiscard]] ac3::OperatingMode mode_from_name(const QString& name) {
    if (name == QLatin1String("line")) {
        return ac3::OperatingMode::kLine;
    }
    if (name == QLatin1String("rf")) {
        return ac3::OperatingMode::kRf;
    }
    return ac3::OperatingMode::kCustom;
}

[[nodiscard]] QString downmix_name(ac3::DownmixTarget target) {
    return target == ac3::DownmixTarget::kLtRt ? QStringLiteral("ltrt") : QStringLiteral("loro");
}

[[nodiscard]] ac3::DownmixTarget downmix_from_name(const QString& name) {
    return name == QLatin1String("ltrt") ? ac3::DownmixTarget::kLtRt : ac3::DownmixTarget::kLoRo;
}

[[nodiscard]] QString dual_mono_name(ac3::hearth::DualMonoChoice choice) {
    switch (choice) {
        case ac3::hearth::DualMonoChoice::kFirst:
            return QStringLiteral("first");
        case ac3::hearth::DualMonoChoice::kSecond:
            return QStringLiteral("second");
        case ac3::hearth::DualMonoChoice::kBoth:
        default:
            return QStringLiteral("both");
    }
}

[[nodiscard]] ac3::hearth::DualMonoChoice dual_mono_from_name(const QString& name) {
    if (name == QLatin1String("first")) {
        return ac3::hearth::DualMonoChoice::kFirst;
    }
    if (name == QLatin1String("second")) {
        return ac3::hearth::DualMonoChoice::kSecond;
    }
    return ac3::hearth::DualMonoChoice::kBoth;
}

[[nodiscard]] QString objects_policy_name(ac3::render::ObjectsPolicy policy) {
    switch (policy) {
        case ac3::render::ObjectsPolicy::kNever:
            return QStringLiteral("never");
        case ac3::render::ObjectsPolicy::kAlways:
            return QStringLiteral("always");
        case ac3::render::ObjectsPolicy::kAuto:
        default:
            return QStringLiteral("auto");
    }
}

[[nodiscard]] ac3::render::ObjectsPolicy objects_policy_from_name(const QString& name) {
    if (name == QLatin1String("never")) {
        return ac3::render::ObjectsPolicy::kNever;
    }
    if (name == QLatin1String("always")) {
        return ac3::render::ObjectsPolicy::kAlways;
    }
    return ac3::render::ObjectsPolicy::kAuto;
}

[[nodiscard]] QString joc_domain_name(ac3::oba::joc::Domain domain) {
    return domain == ac3::oba::joc::Domain::kMdctBand ? QStringLiteral("mdct") : QStringLiteral("qmf");
}

[[nodiscard]] ac3::oba::joc::Domain joc_domain_from_name(const QString& name) {
    return name == QLatin1String("mdct") ? ac3::oba::joc::Domain::kMdctBand
                                          : ac3::oba::joc::Domain::kQmf;
}

[[nodiscard]] QString concealment_name(ac3::ConcealmentPolicy policy) {
    switch (policy) {
        case ac3::ConcealmentPolicy::kNone:
            return QStringLiteral("stop");
        case ac3::ConcealmentPolicy::kMute:
            return QStringLiteral("mute");
        case ac3::ConcealmentPolicy::kRepeatFade:
        default:
            return QStringLiteral("repeatFade");
    }
}

[[nodiscard]] ac3::ConcealmentPolicy concealment_from_name(const QString& name) {
    if (name == QLatin1String("stop")) {
        return ac3::ConcealmentPolicy::kNone;
    }
    if (name == QLatin1String("mute")) {
        return ac3::ConcealmentPolicy::kMute;
    }
    return ac3::ConcealmentPolicy::kRepeatFade;
}

[[nodiscard]] QVariantMap decoder_settings_to_map(const ac3::hearth::DecoderSettings& settings) {
    QVariantMap map;
    map[QStringLiteral("mode")] = mode_name(settings.mode);
    map[QStringLiteral("rfCeilingDb")] = settings.rf_ceiling_db;
    map[QStringLiteral("drcCut")] = settings.drc_cut;
    map[QStringLiteral("drcBoost")] = settings.drc_boost;
    map[QStringLiteral("heavyCompression")] = settings.heavy_compression;
    map[QStringLiteral("normaliseDialogue")] = settings.normalise_dialogue;
    map[QStringLiteral("stereoFold")] = downmix_name(settings.stereo_fold);
    map[QStringLiteral("ltrtPhaseShift")] = settings.ltrt_phase_shift;
    map[QStringLiteral("mixLfe")] = settings.mix_lfe;
    map[QStringLiteral("dualMono")] = dual_mono_name(settings.dual_mono);
    map[QStringLiteral("objects")] = objects_policy_name(settings.objects);
    map[QStringLiteral("jocDomain")] = joc_domain_name(settings.joc_domain);
    map[QStringLiteral("concealment")] = concealment_name(settings.concealment);
    map[QStringLiteral("fastInverseTransform")] = settings.fast_inverse_transform;
    return map;
}

// Starts from `base` (the engine's last-known settings) so a key this map
// does not carry - or a caller that reads decoderSettings(), changes one
// key and writes the rest back unmodified - keeps its value rather than
// resetting to DecoderSettings{}'s defaults.
[[nodiscard]] ac3::hearth::DecoderSettings decoder_settings_from_map(
    const QVariantMap& map, const ac3::hearth::DecoderSettings& base) {
    ac3::hearth::DecoderSettings out = base;
    if (map.contains(QStringLiteral("mode"))) {
        out.mode = mode_from_name(map[QStringLiteral("mode")].toString());
    }
    if (map.contains(QStringLiteral("rfCeilingDb"))) {
        out.rf_ceiling_db = map[QStringLiteral("rfCeilingDb")].toDouble();
    }
    if (map.contains(QStringLiteral("drcCut"))) {
        out.drc_cut = map[QStringLiteral("drcCut")].toDouble();
    }
    if (map.contains(QStringLiteral("drcBoost"))) {
        out.drc_boost = map[QStringLiteral("drcBoost")].toDouble();
    }
    if (map.contains(QStringLiteral("heavyCompression"))) {
        out.heavy_compression = map[QStringLiteral("heavyCompression")].toBool();
    }
    if (map.contains(QStringLiteral("normaliseDialogue"))) {
        out.normalise_dialogue = map[QStringLiteral("normaliseDialogue")].toBool();
    }
    if (map.contains(QStringLiteral("stereoFold"))) {
        out.stereo_fold = downmix_from_name(map[QStringLiteral("stereoFold")].toString());
    }
    if (map.contains(QStringLiteral("ltrtPhaseShift"))) {
        out.ltrt_phase_shift = map[QStringLiteral("ltrtPhaseShift")].toBool();
    }
    if (map.contains(QStringLiteral("mixLfe"))) {
        out.mix_lfe = map[QStringLiteral("mixLfe")].toBool();
    }
    if (map.contains(QStringLiteral("dualMono"))) {
        out.dual_mono = dual_mono_from_name(map[QStringLiteral("dualMono")].toString());
    }
    if (map.contains(QStringLiteral("objects"))) {
        out.objects = objects_policy_from_name(map[QStringLiteral("objects")].toString());
    }
    if (map.contains(QStringLiteral("jocDomain"))) {
        out.joc_domain = joc_domain_from_name(map[QStringLiteral("jocDomain")].toString());
    }
    if (map.contains(QStringLiteral("concealment"))) {
        out.concealment = concealment_from_name(map[QStringLiteral("concealment")].toString());
    }
    if (map.contains(QStringLiteral("fastInverseTransform"))) {
        out.fast_inverse_transform = map[QStringLiteral("fastInverseTransform")].toBool();
    }
    return out;
}

// --- the play monitor: MeterSnapshot/UnitReport <-> QVariant, field by ----
// field (play_meters.hpp, stream_decoder.hpp) --------------------------

[[nodiscard]] QVariantMap channel_level_to_map(const ac3::analysis::ChannelLevel& level) {
    QVariantMap map;
    map[QStringLiteral("peakDb")] = level.peak_db;
    map[QStringLiteral("holdDb")] = level.hold_db;
    map[QStringLiteral("rmsDb")] = level.rms_db;
    map[QStringLiteral("clipped")] = level.clipped;
    return map;
}

[[nodiscard]] QString output_mode_name(ac3::hearth::OutputMode mode) {
    switch (mode) {
        case ac3::hearth::OutputMode::kBitstream:
            return QStringLiteral("bitstream");
        case ac3::hearth::OutputMode::kBitstreamAsAc3:
            return QStringLiteral("bitstreamAsAc3");
        case ac3::hearth::OutputMode::kLocalPcm:
            return QStringLiteral("pcm");
        case ac3::hearth::OutputMode::kNetworkGroup:
            return QStringLiteral("networkGroup");
        case ac3::hearth::OutputMode::kNone:
        default:
            return QStringLiteral("none");
    }
}

[[nodiscard]] QVariantMap output_format_to_map(const ac3::hearth::OpenOutputFormat& format) {
    QVariantMap map;
    map[QStringLiteral("sampleRate")] = format.sample_rate;
    map[QStringLiteral("channels")] = format.channels;
    map[QStringLiteral("mode")] = output_mode_name(format.mode);
    return map;
}

// oba::describe_objects()'s DisplayObject, as QML reads it - the same shape
// apps/gui's ObjectDecodeController already settled on for its own room-plan
// view (object_decode_controller.cpp), so the two applications' object
// markers read the same fields the same way.
[[nodiscard]] QVariantMap display_object_to_map(const ac3::oba::DisplayObject& object) {
    QVariantMap map;
    map[QStringLiteral("x")] = object.position.x;
    map[QStringLiteral("y")] = object.position.y;
    map[QStringLiteral("z")] = object.position.z;
    map[QStringLiteral("gainDb")] = object.gain_db;
    map[QStringLiteral("snap")] = object.snap;
    map[QStringLiteral("active")] = object.active;
    map[QStringLiteral("label")] =
        QString::fromUtf8(object.label.data(), static_cast<qsizetype>(object.label.size()));
    // Above the bed plane: the room z axis every oba::Position shares (-1
    // floor, 0 ear height, +1 ceiling) - there is no separate "is this a
    // height channel" flag on the wire, so a threshold on z is what the
    // Objects panel's "raised" marker means, for a bed speaker (Table 12
    // heights sit well above 0) and a dynamic object alike.
    map[QStringLiteral("raised")] = object.position.z > 0.0;
    return map;
}

[[nodiscard]] QString realization_name(ac3::render::Speaker::Realization realization) {
    switch (realization) {
        case ac3::render::Speaker::Realization::kTop:
            return QStringLiteral("ceiling");
        case ac3::render::Speaker::Realization::kUpFiring:
            return QStringLiteral("upfiring");
        case ac3::render::Speaker::Realization::kDefault:
        case ac3::render::Speaker::Realization::kHeight:
        default:
            // kHeight (a wall-mounted, angled speaker) reads the same as
            // kDefault: layout.hpp's own header comment says the two render
            // identically, and "wall" is what setHeights("wall") writes -
            // kDefault, never kHeight - so a round trip through this control
            // is stable.
            return QStringLiteral("wall");
    }
}

[[nodiscard]] ac3::render::Speaker::Realization realization_from_name(const QString& name) {
    if (name == QLatin1String("ceiling")) {
        return ac3::render::Speaker::Realization::kTop;
    }
    if (name == QLatin1String("upfiring")) {
        return ac3::render::Speaker::Realization::kUpFiring;
    }
    return ac3::render::Speaker::Realization::kDefault;
}

[[nodiscard]] ac3::hearth::QueueItem queue_item_from_path(const QString& path) {
    ac3::hearth::QueueItem item;
    item.path = path.toStdString();
    item.title = QFileInfo(path).fileName().toStdString();
    return item;
}

[[nodiscard]] QVariantMap queue_row(const ac3::hearth::QueueItem& item, bool current) {
    QVariantMap row;
    row[QStringLiteral("path")] = QString::fromStdString(item.path);
    row[QStringLiteral("title")] = QString::fromStdString(item.title);
    row[QStringLiteral("playable")] = item.playable();
    row[QStringLiteral("note")] = QString::fromStdString(
        item.playable() ? item.facts.note : item.facts.unplayable_because);
    row[QStringLiteral("durationMs")] =
        item.facts.duration.has_value() ? static_cast<qlonglong>(item.facts.duration->count()) : qlonglong{0};
    row[QStringLiteral("channels")] = item.facts.channels;
    row[QStringLiteral("sampleRate")] = item.facts.sample_rate;
    row[QStringLiteral("hasObjects")] = item.facts.has_objects;
    row[QStringLiteral("streamKind")] = stream_kind_name(item.facts);
    row[QStringLiteral("codecBadge")] = codec_badge(item.facts);
    if (item.facts.bitrate_kbps) {
        row[QStringLiteral("bitrateKbps")] = *item.facts.bitrate_kbps;
    }
    row[QStringLiteral("current")] = current;
    return row;
}

// One row of the output picker's "this computer" list: OutputPicker.qml
// reads channels/speakers/sampleRates for the PCM section and
// supportsAc3/supportsEac3 for the passthrough section, from the one
// enumeration both come from - a device that cannot bitstream simply has
// both flags false, which is why the passthrough section shows only some of
// these rows rather than needing a second list.
[[nodiscard]] QVariantMap output_device_row(const ac3::audio::RenderDeviceInfo& device) {
    QVariantMap row;
    row[QStringLiteral("id")] = QString::fromStdString(device.id);
    row[QStringLiteral("name")] = QString::fromStdString(device.name);
    row[QStringLiteral("isDefault")] = device.is_default;
    // 0 is "not reported", not "no channels" (RenderDeviceInfo's own
    // comment) - QML reads a zero channel count that way too.
    row[QStringLiteral("channels")] = device.channels;
    row[QStringLiteral("speakers")] = QString::fromStdString(ac3::audio::describe_speakers(device.speakers));
    QVariantList rates;
    rates.reserve(static_cast<qsizetype>(device.sample_rates.size()));
    for (const std::uint32_t rate : device.sample_rates) {
        rates.push_back(static_cast<uint>(rate));
    }
    row[QStringLiteral("sampleRates")] = rates;
    row[QStringLiteral("supportsAc3")] = device.supports_ac3_passthrough;
    row[QStringLiteral("supportsEac3")] = device.supports_eac3_passthrough;
    return row;
}

// --- settings (the Settings page) ------------------------------------------

// ac3::hearth::SettingsStore over QSettings (planning/hearth-reference-
// player.md, A5: "it stores its settings through QSettings"). The key
// strings settings_model.cpp and pairing_store.cpp already compose
// ("playback/gapless", "queue/1/path", "pairing/1/client", ...) are plain
// QSettings keys, so this is a thin pass-through that does not have to know
// what any of them mean - MemorySettingsStore (settings_model.cpp) is the
// test suites' own version of the same three methods.
class QSettingsStore final : public ac3::hearth::SettingsStore {
public:
    explicit QSettingsStore(QSettings& settings) : settings_(settings) {}

    [[nodiscard]] std::optional<std::string> value(std::string_view key) const override {
        const QVariant found =
            settings_.value(QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size())));
        if (!found.isValid()) {
            return std::nullopt;
        }
        return found.toString().toStdString();
    }

    void set_value(std::string_view key, std::string_view value) override {
        settings_.setValue(QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size())),
                           QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
    }

    void remove_group(std::string_view group) override {
        // QSettings::remove() already removes the key itself and everything
        // under it; MemorySettingsStore::remove_group() only says so in a
        // comment because it has to do that walk by hand.
        settings_.remove(QString::fromUtf8(group.data(), static_cast<qsizetype>(group.size())));
    }

    [[nodiscard]] bool sync() override {
        settings_.sync();
        return settings_.status() == QSettings::NoError;
    }

private:
    QSettings& settings_;
};

[[nodiscard]] QString failure_policy_name(ac3::hearth::FailurePolicy policy) {
    return policy == ac3::hearth::FailurePolicy::kStop ? QStringLiteral("stop") : QStringLiteral("skip");
}

[[nodiscard]] ac3::hearth::FailurePolicy failure_policy_from_name(const QString& name) {
    return name == QLatin1String("stop") ? ac3::hearth::FailurePolicy::kStop
                                         : ac3::hearth::FailurePolicy::kSkip;
}

// Read fresh rather than cached, every time - hearth_controller.hpp's own
// comment on why EngineSettings is never a member here applies the same way
// to a local variable that would outlive one call.
[[nodiscard]] ac3::hearth::EngineSettings current_settings(const ac3::hearth::SettingsStore& store) {
    return ac3::hearth::load_settings(store, QSysInfo::machineHostName().toStdString());
}

// sync() is [[nodiscard]] (settings_model.hpp: "False when that failed: what
// was kept before is what a later start reads") - noted rather than silently
// dropped, since a full disk or a read-only settings folder is exactly the
// kind of thing a diagnostics export exists to have caught.
void sync_store(ac3::hearth::SettingsStore& store, ac3::hearth::DiagnosticLog& log) {
    if (!store.sync()) {
        log.note("settings: could not save to disk");
    }
}

}  // namespace

HearthController::HearthController(QObject* parent)
    : QObject(parent),
      log_(ac3::hearth::process_diagnostics()),
      settings_(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("ac3forge"),
               QStringLiteral("Hearth")),
      store_(std::make_unique<QSettingsStore>(settings_)),
      pairing_(std::make_unique<ac3::hearth::PairingStore>(
          *store_, [] { return QDate::currentDate().toString(Qt::ISODate).toStdString(); })) {
    poll_timer_.setInterval(kPollMs);
    connect(&poll_timer_, &QTimer::timeout, this, &HearthController::poll);
    // A window close runs this; a session logout or a killed process does
    // not, which is the same trade every setter below already makes by
    // syncing only on a change rather than continuously (save_on_quit()'s
    // own comment in the header).
    if (auto* application = QCoreApplication::instance()) {
        connect(application, &QCoreApplication::aboutToQuit, this, &HearthController::save_on_quit);
    }
}

HearthController::~HearthController() = default;

QString HearthController::versionDetails() const {
    return QString::fromStdString(ac3::version_details());
}

QString HearthController::licenceNotices() const {
    // The same file the package installs, embedded by
    // apps/hearth/notices/notices.cmake once ac3hearth exists for it to
    // embed into, so the dialog cannot say something the package does not.
    // A binary built without the embedding gets a sentence that says so
    // rather than an empty view - the same fallback
    // CrucibleController::licenceNotices() uses.
    QFile file(QStringLiteral(":/notices/NOTICES.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return tr("This build carries no embedded notices file (:/notices/NOTICES.txt was not "
                  "compiled in); the NOTICES.txt beside the application and the repository's "
                  "LICENSE say what it ships.");
    }
    return QString::fromUtf8(file.readAll());
}

void HearthController::start() {
    if (engine_) {
        return;
    }
    // "2.0" until the Speakers page makes the layout a setting; a literal
    // this application writes always parses.
    const std::optional<ac3::render::OutputLayout> layout = ac3::render::OutputLayout::parse("2.0");
    // EngineOutputs, not a bare PcmSink: given every render endpoint
    // (device_endpoints(), output_selector.hpp), the engine decides each
    // item's output itself and set_output_preferences() - the output
    // picker's "Play here" - has something to act on. No bitstream sink yet,
    // so OutputSelector::endpoints() reads every endpoint's passthrough
    // flags as false regardless of what the device reports
    // (bitstream_output=false in engine_thread.cpp's EngineOutputs
    // constructor) and every item still decodes to PCM - the same outcome
    // the old bare-PcmSink construction gave, on the same default device
    // (best_for_pcm() picks it the same way DeviceSink::open() did with an
    // empty device id), until a dialog row pins a different one.
    ac3::hearth::EngineOutputs outputs{.pcm = ac3::hearth::make_device_sink(std::string()),
                                       .bitstream = {},
                                       .endpoints = ac3::hearth::device_endpoints()};
    const ac3::hearth::EngineSettings loaded = current_settings(*store_);
    engine_ = std::make_unique<ac3::hearth::Engine>(
        std::move(outputs), ac3::hearth::ui::make_file_item_loader(), *layout,
        ac3::hearth::DecoderSettings{}, ac3::hearth::EngineTiming{}, &log_);
    // Each reads with its own loader instance (make_file_item_loader()
    // builds a fresh std::function every call, same as the engine's own
    // above) so the Media page's own pick never blocks on whatever
    // currentMedia is mid-reading, and vice versa (media_inspector.hpp).
    now_playing_inspector_ =
        std::make_unique<ac3::hearth::MediaInspector>(ac3::hearth::ui::make_file_item_loader());
    inspected_item_inspector_ =
        std::make_unique<ac3::hearth::MediaInspector>(ac3::hearth::ui::make_file_item_loader());
    engine_->set_gapless(loaded.playback.gapless);
    engine_->set_on_failure(loaded.playback.on_failure);
    if (loaded.playback.resume_queue) {
        const ac3::hearth::SavedQueue saved = ac3::hearth::load_queue(*store_);
        if (!saved.items.empty()) {
            engine_->restore(saved.items, saved.current, saved.position);
        }
    }
    poll_timer_.start();
    poll();
}

void HearthController::setGapless(bool on) {
    if (gapless_ == on) {
        return;
    }
    gapless_ = on;
    if (engine_) {
        engine_->set_gapless(on);
    }
    ac3::hearth::EngineSettings settings = current_settings(*store_);
    settings.playback.gapless = on;
    ac3::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    emit stateChanged();
}

void HearthController::play() {
    if (engine_) {
        engine_->play();
    }
}

void HearthController::pause() {
    if (engine_) {
        engine_->pause();
    }
}

void HearthController::stop() {
    if (engine_) {
        engine_->stop();
    }
}

void HearthController::next() {
    if (engine_) {
        engine_->next();
    }
}

void HearthController::previous() {
    if (engine_) {
        engine_->previous();
    }
}

void HearthController::seek(qlonglong ms) {
    if (engine_ && ms >= 0) {
        engine_->seek(std::chrono::milliseconds(ms));
    }
}

void HearthController::playItem(int index) {
    if (engine_ && index >= 0) {
        engine_->play_item(static_cast<std::size_t>(index));
    }
}

void HearthController::removeAt(int index) {
    if (engine_ && index >= 0) {
        engine_->remove(static_cast<std::size_t>(index));
    }
}

void HearthController::addFiles(const QStringList& paths) {
    if (!engine_ || paths.isEmpty()) {
        return;
    }
    std::vector<ac3::hearth::QueueItem> items;
    items.reserve(static_cast<std::size_t>(paths.size()));
    for (const QString& path : paths) {
        items.push_back(queue_item_from_path(path));
    }
    engine_->add(std::move(items));
}

void HearthController::addFolder(const QString& path) {
    if (!engine_ || path.isEmpty()) {
        return;
    }
    const std::vector<std::string> found = ac3::hearth::ui::list_folder_items(path.toStdString());
    if (found.empty()) {
        return;
    }
    std::vector<ac3::hearth::QueueItem> items;
    items.reserve(found.size());
    for (const std::string& item_path : found) {
        items.push_back(queue_item_from_path(QString::fromStdString(item_path)));
    }
    engine_->add(std::move(items));
}

void HearthController::setVolumeDb(double db) {
    if (engine_) {
        engine_->set_volume_db(db);
    }
}

void HearthController::inspectItem(int index) {
    if (index == inspected_index_) {
        return;
    }
    if (index >= 0 && index >= queue_.size()) {
        return;
    }
    inspected_index_ = index;
    // poll(), the next tick, sees inspected_index_ changed and issues the
    // request - the same "only act on a change" shape as everything else
    // here, so a page bound to inspectedIndex settles on the same tick
    // whether the pick came from inspectItem() or from currentIndex moving
    // under a "follow now playing" pick (index == -1).
    emit inspectedMediaChanged();
}

bool HearthController::exportInspectedMedia(const QUrl& fileUrl) {
    if (!inspected_media_.contains(QStringLiteral("json"))) {
        return false;
    }
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray json = inspected_media_[QStringLiteral("json")].toString().toUtf8();
    return file.write(json) == json.size();
}

void HearthController::poll() {
    if (!engine_) {
        return;
    }
    const ac3::hearth::EngineStatus status = engine_->status();

    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(status.queue.size()));
    for (std::size_t i = 0; i < status.queue.size(); ++i) {
        rows.push_back(queue_row(status.queue[i], i == status.current));
    }
    const bool queue_changed = rows != queue_;
    queue_ = std::move(rows);
    const int new_current =
        status.current == ac3::hearth::Queue::kNone ? -1 : static_cast<int>(status.current);
    if (queue_changed || new_current != current_index_) {
        current_index_ = new_current;
        emit queueChanged();
    }

    const QString new_state = transport_state_name(status.state);
    const QString new_output_reason = QString::fromStdString(status.output_reason);
    const QString new_note = QString::fromStdString(status.note);
    const QString new_error = QString::fromStdString(status.error);
    QVariantMap new_output_format = output_format_to_map(status.output);
    if (new_state != state_ || status.gapless != gapless_ || status.volume_db != volume_db_ ||
        new_output_reason != output_reason_ || new_note != note_ || new_error != error_ ||
        new_output_format != output_format_) {
        state_ = new_state;
        gapless_ = status.gapless;
        volume_db_ = status.volume_db;
        output_reason_ = new_output_reason;
        note_ = new_note;
        error_ = new_error;
        output_format_ = std::move(new_output_format);
        emit stateChanged();
    }

    // --- media information ------------------------------------------------
    // currentMedia follows the item playing now.
    QString new_now_playing_path;
    if (status.current != ac3::hearth::Queue::kNone && status.current < status.queue.size()) {
        new_now_playing_path = QString::fromStdString(status.queue[status.current].path);
    }
    if (new_now_playing_path != now_playing_path_) {
        now_playing_path_ = new_now_playing_path;
        current_media_.clear();
        emit currentMediaChanged();
        if (!now_playing_path_.isEmpty()) {
            now_playing_inspector_->request(now_playing_path_.toStdString());
        }
    }
    if (const std::optional<ac3::hearth::MediaInfo> latest = now_playing_inspector_->latest();
        latest && QString::fromStdString(latest->path) == now_playing_path_ &&
        current_media_.value(QStringLiteral("path")).toString() != now_playing_path_) {
        current_media_ = media_info_to_map(*latest);
        emit currentMediaChanged();
    }

    // inspectedMedia follows inspectItem()'s pick (the Media page's own
    // "Showing" picker), defaulting to currentMedia's own item - the only
    // way an AC-4 item, never playing in this build, is ever reached.
    if (inspected_index_ >= 0 &&
        static_cast<std::size_t>(inspected_index_) >= status.queue.size()) {
        // The queue shrank under the picked index: follow now playing again
        // rather than keep pointing at nothing.
        inspected_index_ = -1;
    }
    const QString new_inspected_path =
        inspected_index_ < 0
            ? new_now_playing_path
            : QString::fromStdString(
                  status.queue[static_cast<std::size_t>(inspected_index_)].path);
    if (new_inspected_path != inspected_path_) {
        inspected_path_ = new_inspected_path;
        inspected_media_.clear();
        emit inspectedMediaChanged();
        if (!inspected_path_.isEmpty()) {
            inspected_item_inspector_->request(inspected_path_.toStdString());
        }
    }
    if (const std::optional<ac3::hearth::MediaInfo> latest = inspected_item_inspector_->latest();
        latest && QString::fromStdString(latest->path) == inspected_path_ &&
        inspected_media_.value(QStringLiteral("path")).toString() != inspected_path_) {
        inspected_media_ = media_info_to_map(*latest);
        emit inspectedMediaChanged();
    }

    // Read apart from status() - Engine::position()'s own comment says why -
    // and on its own signal, so the scrubber does not have to sit through
    // queue-row rebuilding sixty times a second just to hear it move.
    const ac3::hearth::PlayPosition position = engine_->position();
    const qlonglong new_position_ms = static_cast<qlonglong>(position.heard.count());
    const qlonglong new_duration_ms = static_cast<qlonglong>(position.duration.count());
    if (new_position_ms != position_ms_ || new_duration_ms != duration_ms_) {
        position_ms_ = new_position_ms;
        duration_ms_ = new_duration_ms;
        emit positionChanged();
    }

    const QVariantMap new_decoder_settings = decoder_settings_to_map(status.settings);
    if (new_decoder_settings != decoder_settings_) {
        decoder_settings_ = new_decoder_settings;
        emit decoderSettingsChanged();
    }

    const std::size_t slots = status.layout.slots();
    // The layout can change now (setLayoutText()/setHeights()/
    // setSpeakerSmall()), not just appear once, so everything keyed by slot
    // index - the labels, which are small, which is LFE, the Heights
    // reading - is recomputed whenever the text says it changed, not only
    // the first time slots appear.
    const QString new_layout_text = QString::fromStdString(std::string(status.layout.text()));
    const bool layout_changed = new_layout_text != layout_text_;
    QStringList new_labels;
    QVariantList new_small;
    QVariantList new_is_lfe;
    bool new_has_height = layout_has_height_;
    QString new_heights = heights_realization_;
    if (layout_changed) {
        new_labels.reserve(static_cast<qsizetype>(slots));
        new_small.reserve(static_cast<qsizetype>(slots));
        new_is_lfe.reserve(static_cast<qsizetype>(slots));
        bool has_height = false;
        bool heights_mixed = false;
        std::optional<ac3::render::Speaker::Realization> shared_realization;
        for (std::size_t slot = 0; slot < slots; ++slot) {
            std::array<char, 32> name{};
            status.layout.slot_name(slot, name);
            const ac3::render::Speaker& speaker = status.layout.slot(slot);
            new_labels.push_back(QString::fromLatin1(name.data()));
            new_small.push_back(speaker.small);
            new_is_lfe.push_back(speaker.kind == ac3::render::Speaker::Kind::kLfe);
            if (speaker.location.has_value() &&
                ac3::render::OutputLayout::is_realizable_height(*speaker.location)) {
                has_height = true;
                if (!shared_realization) {
                    shared_realization = speaker.realization;
                } else if (*shared_realization != speaker.realization) {
                    heights_mixed = true;
                }
            }
        }
        new_has_height = has_height;
        new_heights =
            (has_height && !heights_mixed) ? realization_name(*shared_realization) : QString();
    }

    QVariantList new_trim_db;
    QVariantList new_delay_ms;
    new_trim_db.reserve(static_cast<qsizetype>(status.trim_db.size()));
    new_delay_ms.reserve(static_cast<qsizetype>(status.delay_ms.size()));
    for (const double db : status.trim_db) {
        new_trim_db.push_back(db);
    }
    for (const double ms : status.delay_ms) {
        new_delay_ms.push_back(ms);
    }
    QVariantList new_routing;
    new_routing.reserve(static_cast<qsizetype>(slots));
    for (std::size_t slot = 0; slot < slots; ++slot) {
        new_routing.push_back(status.routing.output_of(slot));
    }
    const auto new_routing_outputs = static_cast<std::uint16_t>(status.routing.outputs());
    QStringList new_output_names;
    for (const std::string& name : ac3::audio::output_names(status.speaker_mask, new_routing_outputs)) {
        new_output_names.push_back(QString::fromStdString(name));
    }
    const QString new_device_name = QString::fromStdString(status.device_name);
    const QString new_device_id = QString::fromStdString(status.device_id);
    const bool new_has_lfe = status.layout.lfe_count() > 0;
    const int new_identify_slot = status.identify_slot == ac3::hearth::Queue::kNone
                                      ? -1
                                      : static_cast<int>(status.identify_slot);
    if (layout_changed || new_trim_db != trim_db_ || new_delay_ms != delay_ms_ ||
        status.crossover_hz != crossover_hz_ || new_routing != routing_ ||
        static_cast<int>(new_routing_outputs) != routing_outputs_ ||
        new_output_names != output_names_ || new_device_name != device_name_ ||
        new_device_id != device_id_ || new_has_lfe != layout_has_lfe_ ||
        status.identify_level_db != identify_level_db_ || new_identify_slot != identify_slot_) {
        trim_db_ = std::move(new_trim_db);
        delay_ms_ = std::move(new_delay_ms);
        crossover_hz_ = status.crossover_hz;
        routing_ = std::move(new_routing);
        routing_outputs_ = static_cast<int>(new_routing_outputs);
        output_names_ = std::move(new_output_names);
        device_name_ = new_device_name;
        device_id_ = new_device_id;
        layout_has_lfe_ = new_has_lfe;
        if (layout_changed) {
            layout_text_ = new_layout_text;
            speaker_labels_ = new_labels;
            speaker_small_ = new_small;
            speaker_is_lfe_ = new_is_lfe;
            layout_has_height_ = new_has_height;
            heights_realization_ = new_heights;
        }
        identify_level_db_ = status.identify_level_db;
        identify_slot_ = new_identify_slot;
        emit speakerSetupChanged();
    }

    // --- the play monitor: Engine::meters() and Engine::unit_report(), ----
    // read every tick the same as status() above - both calls have existed
    // on Engine since A3 (slices 6 and 8); this is the first place in the
    // tree that polls them (planning/hearth-reference-player.md, Monitor).
    bool monitor_changed = false;

    QVariantList new_levels;
    QVariantMap new_loudness;
    if (const std::optional<ac3::hearth::MeterSnapshot> snapshot = engine_->meters()) {
        new_levels.reserve(static_cast<qsizetype>(snapshot->levels.size()));
        for (const auto& level : snapshot->levels) {
            new_levels.push_back(channel_level_to_map(level));
        }
        if (snapshot->momentary_lkfs) {
            new_loudness[QStringLiteral("momentary")] = *snapshot->momentary_lkfs;
        }
        if (snapshot->short_term_lkfs) {
            new_loudness[QStringLiteral("shortTerm")] = *snapshot->short_term_lkfs;
        }
        if (snapshot->integrated_lkfs) {
            new_loudness[QStringLiteral("integrated")] = *snapshot->integrated_lkfs;
        }
        if (snapshot->loudness_range) {
            new_loudness[QStringLiteral("range")] = *snapshot->loudness_range;
        }
        if (snapshot->true_peak_dbtp) {
            new_loudness[QStringLiteral("truePeak")] = *snapshot->true_peak_dbtp;
        }
    }
    if (new_levels != levels_ || new_loudness != loudness_) {
        levels_ = std::move(new_levels);
        loudness_ = std::move(new_loudness);
        monitor_changed = true;
    }

    QVariantMap new_this_frame;
    QVariantList new_objects;
    int new_objects_placed = 0;
    bool new_has_object_metadata = false;
    if (const std::optional<ac3::hearth::UnitReport> report = engine_->unit_report()) {
        new_this_frame[QStringLiteral("dialnorm")] = report->dialnorm;
        if (report->compr) {
            new_this_frame[QStringLiteral("comprDb")] =
                20.0 * std::log10(ac3::meta::compr_gain(*report->compr));
        }
        if (report->blocks > 0) {
            double dynrng_min_db = std::numeric_limits<double>::infinity();
            double dynrng_max_db = -std::numeric_limits<double>::infinity();
            for (int i = 0; i < report->blocks; ++i) {
                const double db = 20.0 * std::log10(
                    ac3::meta::dynrng_gain(report->dynrng[static_cast<std::size_t>(i)]));
                dynrng_min_db = std::min(dynrng_min_db, db);
                dynrng_max_db = std::max(dynrng_max_db, db);
            }
            new_this_frame[QStringLiteral("dynrngMinDb")] = dynrng_min_db;
            new_this_frame[QStringLiteral("dynrngMaxDb")] = dynrng_max_db;
        }
        if (report->short_blocks) {
            new_this_frame[QStringLiteral("shortBlocks")] = *report->short_blocks;
        }
        new_this_frame[QStringLiteral("blocks")] = report->blocks;
        if (report->bitrate_kbps) {
            new_this_frame[QStringLiteral("bitrateKbps")] = *report->bitrate_kbps;
        }
        new_this_frame[QStringLiteral("sequence")] = static_cast<qlonglong>(report->sequence);

        if (report->objects) {
            new_has_object_metadata = true;
            const std::vector<ac3::oba::DisplayObject> described =
                ac3::oba::describe_objects(*report->objects);
            new_objects.reserve(static_cast<qsizetype>(described.size()));
            for (const auto& object : described) {
                new_objects.push_back(display_object_to_map(object));
                if (object.label.empty() && object.active) {
                    ++new_objects_placed;
                }
            }
        }
    }
    if (new_this_frame != this_frame_ || new_objects != objects_ ||
        new_objects_placed != objects_placed_ || new_has_object_metadata != has_object_metadata_) {
        this_frame_ = std::move(new_this_frame);
        objects_ = std::move(new_objects);
        objects_placed_ = new_objects_placed;
        has_object_metadata_ = new_has_object_metadata;
        monitor_changed = true;
    }

    if (monitor_changed) {
        emit monitorChanged();
    }
}

void HearthController::setDecoderSettings(const QVariantMap& settings) {
    if (!engine_) {
        return;
    }
    // A fresh read rather than a cached struct: engine_->status() is already
    // a cheap, synchronous snapshot (poll() calls it every tick), and this
    // avoids hearth_controller.hpp itself needing ac3::hearth::DecoderSettings
    // by value, which would pull in ac3::render::OutputLayout (through
    // decoder_settings.hpp) ahead of this header's own Qt includes and
    // reintroduce the slots-macro collision hearth_controller.cpp's own
    // #undef only guards its own translation unit against.
    engine_->set_decoder_settings(decoder_settings_from_map(settings, engine_->status().settings));
}

void HearthController::setTrimDb(int slot, double db) {
    if (engine_ && slot >= 0) {
        engine_->set_trim_db(static_cast<std::size_t>(slot), db);
    }
}

void HearthController::setDelayMs(int slot, double ms) {
    if (engine_ && slot >= 0) {
        engine_->set_delay_ms(static_cast<std::size_t>(slot), ms);
    }
}

void HearthController::setCrossoverHz(double hz) {
    if (engine_) {
        engine_->set_crossover_hz(hz);
    }
}

void HearthController::setRoutingAssignment(int slot, int output) {
    if (!engine_ || slot < 0) {
        return;
    }
    ac3::render::Routing patch = ac3::render::Routing::identity(
                                     static_cast<std::size_t>(routing_.size()),
                                     static_cast<std::size_t>(routing_outputs_))
                                     .value_or(ac3::render::Routing{});
    for (qsizetype i = 0; i < routing_.size(); ++i) {
        patch.assign(static_cast<std::size_t>(i), routing_[i].toInt());
    }
    patch.assign(static_cast<std::size_t>(slot), output);
    engine_->set_routing(patch);
}

void HearthController::clearRouting() {
    if (!engine_) {
        return;
    }
    const auto patch = ac3::render::Routing::identity(static_cast<std::size_t>(routing_.size()), 0);
    engine_->set_routing(patch.value_or(ac3::render::Routing{}));
}

void HearthController::useDeviceOrder() {
    if (!engine_) {
        return;
    }
    const auto patch = ac3::render::Routing::identity(static_cast<std::size_t>(routing_.size()),
                                                       static_cast<std::size_t>(routing_outputs_));
    if (patch) {
        engine_->set_routing(*patch);
    }
}

void HearthController::refreshOutputDevices() {
    QVariantList rows;
    const auto devices = ac3::audio::enumerate_render_devices();
    if (devices.has_value()) {
        rows.reserve(static_cast<qsizetype>(devices->size()));
        for (const auto& device : *devices) {
            rows.push_back(output_device_row(device));
        }
    }
    // A failed enumeration (kNoBackend, say) empties the list rather than
    // keeping whatever an earlier, working refresh found - a row from a
    // probe this machine can no longer repeat should not sit there
    // clickable as if it still could.
    output_devices_ = std::move(rows);
    emit outputDevicesChanged();
}

void HearthController::selectOutputDevice(const QString& deviceId) {
    if (!engine_ || deviceId.isEmpty()) {
        return;
    }
    engine_->set_output_preferences(
        ac3::hearth::OutputPreferences{.pinned = ac3::hearth::OutputMode::kLocalPcm,
                                       .endpoint_id = deviceId.toStdString(),
                                       .follow_sink = true});
}

void HearthController::setLayoutText(const QString& text) {
    if (!engine_) {
        return;
    }
    const auto layout = ac3::render::OutputLayout::parse(text.toStdString());
    if (layout) {
        engine_->set_layout(*layout);
    }
}

void HearthController::setHeights(const QString& realization) {
    if (!engine_) {
        return;
    }
    // A fresh read, not the (possibly one poll stale) layoutText property -
    // same reasoning as setDecoderSettings()'s own comment.
    const ac3::render::OutputLayout layout = engine_->status().layout;
    engine_->set_layout(layout.with_realization(realization_from_name(realization)));
}

void HearthController::setSpeakerSmall(int slot, bool small) {
    if (!engine_ || slot < 0) {
        return;
    }
    const ac3::render::OutputLayout layout = engine_->status().layout;
    const auto changed = layout.with_small(static_cast<std::size_t>(slot), small);
    if (changed) {
        engine_->set_layout(*changed);
    }
}

void HearthController::setIdentifyLevelDb(double db) {
    if (engine_) {
        engine_->set_identify_level_db(db);
    }
}

void HearthController::startIdentify(int slot) {
    if (engine_ && slot >= 0) {
        engine_->identify_start(static_cast<std::size_t>(slot));
    }
}

void HearthController::stopIdentify() {
    if (engine_) {
        engine_->identify_stop();
    }
}

// --- first run ---------------------------------------------------------

bool HearthController::firstRunSeen() const {
    return settings_.value(QStringLiteral("firstRun/seen"), false).toBool();
}

void HearthController::setFirstRunSeen(bool seen) {
    if (seen == firstRunSeen()) {
        return;
    }
    if (seen) {
        settings_.setValue(QStringLiteral("firstRun/seen"), true);
    } else {
        settings_.remove(QStringLiteral("firstRun/seen"));
    }
    settings_.sync();  // survive a hard exit
    emit firstRunSeenChanged();
}

// --- settings (the Settings page) ------------------------------------------

bool HearthController::resumeQueue() const {
    return current_settings(*store_).playback.resume_queue;
}

void HearthController::setResumeQueue(bool on) {
    ac3::hearth::EngineSettings settings = current_settings(*store_);
    if (settings.playback.resume_queue == on) {
        return;
    }
    settings.playback.resume_queue = on;
    ac3::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    emit settingsChanged();
}

QString HearthController::onFailure() const {
    return failure_policy_name(current_settings(*store_).playback.on_failure);
}

void HearthController::setOnFailure(const QString& policy) {
    const ac3::hearth::FailurePolicy wanted = failure_policy_from_name(policy);
    ac3::hearth::EngineSettings settings = current_settings(*store_);
    if (settings.playback.on_failure == wanted) {
        return;
    }
    settings.playback.on_failure = wanted;
    ac3::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    if (engine_) {
        engine_->set_on_failure(wanted);
    }
    emit settingsChanged();
}

QString HearthController::networkName() const {
    return QString::fromStdString(current_settings(*store_).network.name);
}

void HearthController::setNetworkName(const QString& name) {
    ac3::hearth::EngineSettings settings = current_settings(*store_);
    const std::string wanted = name.toStdString();
    if (settings.network.name == wanted) {
        return;
    }
    settings.network.name = wanted;
    // settings_rows() (called from save_settings()) runs this through
    // ac3::hearth::network_name() itself before it is written, so a name
    // typed with leading/trailing spaces or past the 63-byte mDNS label
    // limit is stored trimmed either way; load_settings() re-derives the
    // same trim on every read, so the round trip agrees with what is shown.
    ac3::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    emit settingsChanged();
}

bool HearthController::networkDiscover() const {
    return current_settings(*store_).network.discover;
}

void HearthController::setNetworkDiscover(bool on) {
    ac3::hearth::EngineSettings settings = current_settings(*store_);
    if (settings.network.discover == on) {
        return;
    }
    settings.network.discover = on;
    ac3::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    emit settingsChanged();
}

QVariantList HearthController::pairingRecords() const {
    QVariantList rows;
    if (!pairing_) {
        return rows;
    }
    for (const ac3::hearth::PairingRecordView& record : pairing_->records()) {
        QVariantMap row;
        row[QStringLiteral("id")] =
            QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(record.client_key.data()),
                                           static_cast<qsizetype>(record.client_key.size()))
                                    .toHex());
        row[QStringLiteral("name")] = QString::fromStdString(record.name);
        row[QStringLiteral("pairedOn")] = QString::fromStdString(record.paired_on);
        rows.push_back(row);
    }
    return rows;
}

void HearthController::forgetPairing(const QString& id) {
    if (!pairing_) {
        return;
    }
    const QByteArray bytes = QByteArray::fromHex(id.toLatin1());
    ac3::sendspin::crypto::Key32 key{};
    if (static_cast<std::size_t>(bytes.size()) != key.size()) {
        return;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(bytes[static_cast<qsizetype>(i)]);
    }
    pairing_->forget(key);
    emit pairingChanged();
}

// --- appearance --------------------------------------------------------

QString HearthController::theme() const {
    return settings_.value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString();
}

void HearthController::setTheme(const QString& theme) {
    if (theme == this->theme()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/theme"), theme);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

QString HearthController::palette() const {
    return settings_.value(QStringLiteral("appearance/palette"), QStringLiteral("signal")).toString();
}

void HearthController::setPalette(const QString& palette) {
    if (palette == this->palette()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/palette"), palette);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

QString HearthController::textScale() const {
    const auto stored = settings_.value(QStringLiteral("appearance/textScale"), QStringLiteral("100")).toString();
    static const QStringList known{QStringLiteral("system"), QStringLiteral("100"), QStringLiteral("125"),
                                   QStringLiteral("150"), QStringLiteral("175")};
    return known.contains(stored) ? stored : QStringLiteral("100");
}

void HearthController::setTextScale(const QString& scale) {
    if (scale == textScale()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/textScale"), scale);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

// --- diagnostics -------------------------------------------------------

QString HearthController::diagnosticsReport() const {
    const ac3::hearth::EngineStatus status = engine_ ? engine_->status() : ac3::hearth::EngineStatus{};

    ac3::hearth::ReportFacts facts;
    facts.written_at = QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toStdString();
    const auto started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 log_.started_at().time_since_epoch())
                                .count();
    facts.log_started_at =
        QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(started_ms)).toString(Qt::ISODateWithMs).toStdString();
    facts.version = ac3::version_details();

    auto platform_row = [&facts](const char* name, const QString& value) {
        facts.platform.emplace_back(name, value.toStdString());
    };
    platform_row("os", QSysInfo::prettyProductName());
    platform_row("kernel", QSysInfo::kernelType() + QLatin1Char(' ') + QSysInfo::kernelVersion());
    platform_row("cpu", QSysInfo::currentCpuArchitecture());
    platform_row("qt", QString::fromLatin1(qVersion()) + QStringLiteral(" (built against ") +
                           QString::fromLatin1(QT_VERSION_STR) + QLatin1Char(')'));
    platform_row("qpa", QGuiApplication::platformName());

    facts.output_name = status.device_name;
    facts.output_reason = status.output_reason;
    facts.settings = ac3::hearth::settings_rows(current_settings(*store_));

    // The caller's own secrets, beyond what render_report() already knows to
    // withhold from the engine snapshot (an item's path, kWithheldSettings):
    // where this computer's settings live, and the person's own home folder,
    // each in every spelling withhold_path()/scrub() might meet.
    ac3::hearth::Secrets secrets;
    auto add_secret = [&secrets](const QString& path) {
        if (path.isEmpty()) {
            return;
        }
        secrets.strings.push_back(path.toStdString());
        secrets.strings.push_back(QDir::fromNativeSeparators(path).toStdString());
        secrets.strings.push_back(QDir::toNativeSeparators(path).toStdString());
    };
    add_secret(QFileInfo(settings_.fileName()).absolutePath());
    add_secret(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));

    return QString::fromStdString(ac3::hearth::render_report(facts, status, log_, secrets));
}

QString HearthController::suggestedDiagnosticsFile() const {
    QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (folder.isEmpty()) {
        folder = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    const QString name = QStringLiteral("hearth-diagnostics-") +
                         QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
                         QStringLiteral(".txt");
    return QUrl::fromLocalFile(QDir(folder).filePath(name)).toString();
}

bool HearthController::exportDiagnostics(const QString& fileUrl) {
    // The GUI's rule for a dialog's answer: a file: URL becomes a local
    // path, anything else is taken as one already.
    const QUrl url(fileUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : fileUrl;
    const QString shown = QDir::toNativeSeparators(path);
    // UTF-8 with LF line endings on every platform: written as bytes, not
    // through a text-mode translation.
    const QByteArray report = diagnosticsReport().toUtf8();
    QSaveFile file(path);
    bool ok = file.open(QIODevice::WriteOnly);
    if (ok) {
        ok = file.write(report) == static_cast<qint64>(report.size()) && file.commit();
    }
    if (ok) {
        diagnostics_message_ = tr("saved to %1").arg(shown);
        log_.note("diagnostics saved");
    } else {
        diagnostics_message_ = tr("could not write %1: %2").arg(shown, file.errorString());
    }
    emit diagnosticsChanged();
    return ok;
}

void HearthController::save_on_quit() {
    if (!engine_) {
        return;
    }
    const ac3::hearth::EngineSettings settings = current_settings(*store_);
    if (settings.playback.resume_queue) {
        ac3::hearth::save_queue(ac3::hearth::saved_queue(engine_->status(), engine_->position()), *store_);
    }
    sync_store(*store_, log_);
}

}  // namespace ac3::hearth::ui
