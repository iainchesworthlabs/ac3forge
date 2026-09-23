#include "hearth_controller.hpp"

#include <QFile>
#include <QFileInfo>
#include <QIODevice>

#include <chrono>
#include <cmath>
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
#include "ac3/render/layout.hpp"
#include "ac3/version.hpp"
#include "decoder_settings.hpp"
#include "engine_thread.hpp"
#include "item_loader.hpp"
#include "media_inspector.hpp"
#include "pcm_sink.hpp"
#include "probe_json.hpp"
#include "queue.hpp"
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
    return facts.has_objects ? QStringLiteral("E-AC-3 JOC") : QStringLiteral("AC-3/E-AC-3");
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
    if (map.contains(QStringLiteral("concealment"))) {
        out.concealment = concealment_from_name(map[QStringLiteral("concealment")].toString());
    }
    if (map.contains(QStringLiteral("fastInverseTransform"))) {
        out.fast_inverse_transform = map[QStringLiteral("fastInverseTransform")].toBool();
    }
    return out;
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
    row[QStringLiteral("current")] = current;
    return row;
}

}  // namespace

HearthController::HearthController(QObject* parent) : QObject(parent) {
    poll_timer_.setInterval(kPollMs);
    connect(&poll_timer_, &QTimer::timeout, this, &HearthController::poll);
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
    // "2.0" until the Speakers page (A5, following this slice) makes the
    // layout a setting; a literal this application writes always parses.
    const std::optional<ac3::render::OutputLayout> layout = ac3::render::OutputLayout::parse("2.0");
    engine_ = std::make_unique<ac3::hearth::Engine>(
        ac3::hearth::make_device_sink(std::string()), ac3::hearth::ui::make_file_item_loader(), *layout,
        ac3::hearth::DecoderSettings{});
    // Each reads with its own loader instance (make_file_item_loader()
    // builds a fresh std::function every call, same as the engine's own
    // above) so the Media page's own pick never blocks on whatever
    // currentMedia is mid-reading, and vice versa (media_inspector.hpp).
    now_playing_inspector_ =
        std::make_unique<ac3::hearth::MediaInspector>(ac3::hearth::ui::make_file_item_loader());
    inspected_item_inspector_ =
        std::make_unique<ac3::hearth::MediaInspector>(ac3::hearth::ui::make_file_item_loader());
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
    if (new_state != state_ || status.gapless != gapless_ || new_output_reason != output_reason_ ||
        new_note != note_ || new_error != error_) {
        state_ = new_state;
        gapless_ = status.gapless;
        output_reason_ = new_output_reason;
        note_ = new_note;
        error_ = new_error;
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
    if (speaker_labels_.isEmpty() && slots > 0) {
        // Fixed for the engine's lifetime (Player::layout()'s own comment
        // says why), so computed only the first time slots appear - the
        // block below's speakerSetupChanged() still covers telling QML,
        // since trim_db/delay_ms/routing all go from empty to populated on
        // this same tick.
        QStringList labels;
        QVariantList small_flags;
        labels.reserve(static_cast<qsizetype>(slots));
        small_flags.reserve(static_cast<qsizetype>(slots));
        for (std::size_t slot = 0; slot < slots; ++slot) {
            std::array<char, 32> name{};
            status.layout.slot_name(slot, name);
            labels.push_back(QString::fromLatin1(name.data()));
            small_flags.push_back(status.layout.slot(slot).small);
        }
        speaker_labels_ = labels;
        speaker_small_ = small_flags;
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
    const QString new_device_name = QString::fromStdString(status.device_name);
    const int new_identify_slot = status.identify_slot == ac3::hearth::Queue::kNone
                                      ? -1
                                      : static_cast<int>(status.identify_slot);
    if (new_trim_db != trim_db_ || new_delay_ms != delay_ms_ || status.crossover_hz != crossover_hz_ ||
        new_routing != routing_ || static_cast<int>(status.routing.outputs()) != routing_outputs_ ||
        new_device_name != device_name_ || status.identify_level_db != identify_level_db_ ||
        new_identify_slot != identify_slot_) {
        trim_db_ = std::move(new_trim_db);
        delay_ms_ = std::move(new_delay_ms);
        crossover_hz_ = status.crossover_hz;
        routing_ = std::move(new_routing);
        routing_outputs_ = static_cast<int>(status.routing.outputs());
        device_name_ = new_device_name;
        identify_level_db_ = status.identify_level_db;
        identify_slot_ = new_identify_slot;
        emit speakerSetupChanged();
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

}  // namespace ac3::hearth::ui
