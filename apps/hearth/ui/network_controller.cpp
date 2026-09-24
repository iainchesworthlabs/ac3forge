#include "network_controller.hpp"

#include <QDate>
#include <QSysInfo>

#include <algorithm>
#include <map>
#include <utility>

// Qt's <QObject> headers define `slots` as a macro unless QT_NO_KEYWORDS is
// set, which this project's Qt targets do not
// (hearth-ui-qt-slots-macro-collides-with-render-layout): ac3::render::
// OutputLayout::slots() is a real method name elsewhere in this engine, and
// left alone the macro would rewrite it into nonsense the moment a header
// pulling it in joins a Qt header in one translation unit. Nothing below
// spells the word, but network_sinks.hpp's own includes reach ac3::render
// nowhere - this line is precautionary, matching hearth_controller.cpp's own,
// for whichever future include first makes the two meet here too.
#undef slots

#include <array>
#include <cstdint>
#include <vector>

#include "ac3/render/layout.hpp"
#include "ac3/render/routing.hpp"
#include "ac3/sendspin/ac3forge_player.hpp"
#include "network_output_status.hpp"
#include "network_sinks.hpp"
#include "network_view.hpp"
#include "pairing_store.hpp"
#include "qsettings_store.hpp"
#include "settings_model.hpp"

namespace ac3::hearth::ui {

namespace {

namespace forge = ac3::sendspin::ac3forge;

constexpr int kPollMs = 60;

// --- a Hearth sink's own settings pages ---------------------------------
// Field names and the "whole struct, apply what changed" idiom deliberately
// mirror HearthController's own decoder_settings_to_map()/from_map() and
// Speakers.qml's own property names (hearth_controller.cpp) - not shared
// code with that file, because it is one of the hottest files in this
// initiative's own concurrent-session swarm right now (several other Hearth
// UI sessions edit it at once); a small, self-contained duplicate here
// avoids taking on that file's own merge risk for a handful of lines.

[[nodiscard]] QString mode_name(forge::DecoderMode mode) {
    switch (mode) {
        case forge::DecoderMode::kRf: return QStringLiteral("rf");
        case forge::DecoderMode::kCustom: return QStringLiteral("custom");
        case forge::DecoderMode::kLine: default: return QStringLiteral("line");
    }
}
[[nodiscard]] forge::DecoderMode mode_from_name(const QString& name) {
    if (name == QStringLiteral("rf")) return forge::DecoderMode::kRf;
    if (name == QStringLiteral("custom")) return forge::DecoderMode::kCustom;
    return forge::DecoderMode::kLine;
}
[[nodiscard]] QString downmix_name(forge::Downmix downmix) {
    return downmix == forge::Downmix::kLtRt ? QStringLiteral("ltrt") : QStringLiteral("loro");
}
[[nodiscard]] forge::Downmix downmix_from_name(const QString& name) {
    return name == QStringLiteral("ltrt") ? forge::Downmix::kLtRt : forge::Downmix::kLoRo;
}
[[nodiscard]] QString objects_name(forge::ObjectsPolicy policy) {
    switch (policy) {
        case forge::ObjectsPolicy::kAlways: return QStringLiteral("always");
        case forge::ObjectsPolicy::kNever: return QStringLiteral("never");
        case forge::ObjectsPolicy::kAuto: default: return QStringLiteral("auto");
    }
}
[[nodiscard]] forge::ObjectsPolicy objects_from_name(const QString& name) {
    if (name == QStringLiteral("always")) return forge::ObjectsPolicy::kAlways;
    if (name == QStringLiteral("never")) return forge::ObjectsPolicy::kNever;
    return forge::ObjectsPolicy::kAuto;
}
[[nodiscard]] QString concealment_name(forge::Concealment concealment) {
    switch (concealment) {
        case forge::Concealment::kNone: return QStringLiteral("none");
        case forge::Concealment::kMute: return QStringLiteral("mute");
        case forge::Concealment::kRepeatFade: default: return QStringLiteral("repeatFade");
    }
}
[[nodiscard]] forge::Concealment concealment_from_name(const QString& name) {
    if (name == QStringLiteral("none")) return forge::Concealment::kNone;
    if (name == QStringLiteral("mute")) return forge::Concealment::kMute;
    return forge::Concealment::kRepeatFade;
}
// "wall"/"ceiling"/"upfiring" - Speakers.qml's own Heights SegmentedControl
// values. kDefault reads as "wall" too: Speaker::Realization's own comment
// says kHeight is "numerically the same as kDefault", i.e. a height slot
// nobody has re-tiered yet is already wall-mounted in effect.
[[nodiscard]] QString realization_name(ac3::render::Speaker::Realization realization) {
    switch (realization) {
        case ac3::render::Speaker::Realization::kTop: return QStringLiteral("ceiling");
        case ac3::render::Speaker::Realization::kUpFiring: return QStringLiteral("upfiring");
        case ac3::render::Speaker::Realization::kHeight:
        case ac3::render::Speaker::Realization::kDefault:
        default:
            return QStringLiteral("wall");
    }
}
[[nodiscard]] ac3::render::Speaker::Realization realization_from_name(const QString& name) {
    if (name == QStringLiteral("ceiling")) return ac3::render::Speaker::Realization::kTop;
    if (name == QStringLiteral("upfiring")) return ac3::render::Speaker::Realization::kUpFiring;
    return ac3::render::Speaker::Realization::kHeight;
}

// A/52 Table 5.8, plus "+ LFE" appended by the caller - "3/2" for acmod 7,
// and so on. Empty for an acmod this table does not cover (only 0-7 are
// defined; DecoderReport::acmod's own comment says the range is checked
// on the wire already).
[[nodiscard]] QString acmod_text(std::int32_t acmod) {
    static constexpr std::array<const char*, 8> kNames{
        "1/0", "1+1", "2/0", "3/0", "2/1", "3/1", "2/2", "3/2",
    };
    return (acmod >= 0 && acmod < static_cast<std::int32_t>(kNames.size()))
               ? QString::fromLatin1(kNames[static_cast<std::size_t>(acmod)])
               : QString();
}

// The layout this sink's settings page treats as "current" before this run
// has pushed anything of its own: HearthController::start()'s own comment
// says the LOCAL engine opens at "2.0 the first time" too, for the same
// reason - there is no sink-reported default to read instead (SinkFacts::
// intended_settings's own comment), so this is a starting point for editing,
// never asserted as what the sink is actually running.
[[nodiscard]] ac3::render::OutputLayout draft_layout(const std::optional<std::string>& text) {
    if (text) {
        if (const std::optional<ac3::render::OutputLayout> parsed = ac3::render::OutputLayout::parse(*text)) {
            return *parsed;
        }
    }
    return ac3::render::OutputLayout::stereo();
}

// This sink's own draft/cached Settings, ALWAYS fully populated in every
// top-level field (never leaving one std::nullopt so the sink would fall
// back to its own default unannounced) except `decoder`, `routing` (only
// meaningful when the sink's own management object offers it) and
// `programme` (this page has no control for it - see network_view.hpp's own
// comment on why the mockups don't need one). `decoder`'s own fields follow
// DecoderEac3.qml's own "?? default" convention instead, applied in
// sink_decoder_settings_to_map() below, since every decoder key is
// independently optional on the wire in a way the speaker fields are not.
[[nodiscard]] forge::Settings sink_settings_base(const ac3::hearth::SinkFacts& facts) {
    if (facts.intended_settings) {
        return *facts.intended_settings;
    }
    forge::Settings settings;
    const ac3::render::OutputLayout layout = draft_layout(std::nullopt);
    settings.layout = std::string(layout.text());
    const auto outputs = static_cast<std::size_t>(facts.ac3forge_support ? facts.ac3forge_support->outputs.count : 0);
    if (facts.ac3forge_support && facts.ac3forge_support->management.routing) {
        std::array<char, ac3::render::Routing::kTextBytes> text{};
        if (const std::optional<ac3::render::Routing> identity =
                ac3::render::Routing::identity(layout.slots(), outputs)) {
            if (identity->format(text) > 0) {
                settings.routing = std::string(text.data());
            }
        }
    }
    settings.trim_db = std::vector<double>(outputs, 0.0);
    settings.delay_ms = std::vector<double>(outputs, 0.0);
    settings.crossover_hz = 80.0;
    return settings;
}

struct LayoutFields {
    QStringList labels;
    QVariantList small;
    QVariantList is_lfe;
    bool has_height = false;
    bool has_lfe = false;
    QString heights_realization;
};

// Mirrors hearth_controller.cpp's own HearthController::poll() layout block
// (labels/small/isLfe/hasHeight/heightsRealization from an OutputLayout) -
// deliberately duplicated rather than shared, for the same reason the enum
// name tables above are: that file is this initiative's own hottest file
// today.
[[nodiscard]] LayoutFields layout_fields(const ac3::render::OutputLayout& layout) {
    LayoutFields fields;
    const std::size_t slots = layout.slots();
    fields.labels.reserve(static_cast<qsizetype>(slots));
    fields.small.reserve(static_cast<qsizetype>(slots));
    fields.is_lfe.reserve(static_cast<qsizetype>(slots));
    bool heights_mixed = false;
    std::optional<ac3::render::Speaker::Realization> shared_realization;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        std::array<char, 32> name{};
        layout.slot_name(slot, name);
        const ac3::render::Speaker& speaker = layout.slot(slot);
        fields.labels.push_back(QString::fromLatin1(name.data()));
        fields.small.push_back(speaker.small);
        const bool is_lfe = speaker.kind == ac3::render::Speaker::Kind::kLfe;
        fields.is_lfe.push_back(is_lfe);
        fields.has_lfe = fields.has_lfe || is_lfe;
        if (speaker.location.has_value() && ac3::render::OutputLayout::is_realizable_height(*speaker.location)) {
            fields.has_height = true;
            if (!shared_realization) {
                shared_realization = speaker.realization;
            } else if (*shared_realization != speaker.realization) {
                heights_mixed = true;
            }
        }
    }
    fields.heights_realization =
        (fields.has_height && !heights_mixed) ? realization_name(*shared_realization) : QString();
    return fields;
}

[[nodiscard]] QVariantMap sink_speaker_settings_to_map(const ac3::hearth::SinkFacts& facts) {
    QVariantMap map;
    if (!facts.ac3forge_support) {
        return map;
    }
    const forge::Settings settings = sink_settings_base(facts);
    const ac3::render::OutputLayout layout = draft_layout(settings.layout);
    const LayoutFields fields = layout_fields(layout);
    const auto outputs = static_cast<std::size_t>(facts.ac3forge_support->outputs.count);

    map[QStringLiteral("layoutText")] = QString::fromStdString(std::string(layout.text()));
    map[QStringLiteral("labels")] = fields.labels;
    map[QStringLiteral("small")] = fields.small;
    map[QStringLiteral("isLfe")] = fields.is_lfe;
    map[QStringLiteral("hasHeight")] = fields.has_height;
    map[QStringLiteral("hasLfe")] = fields.has_lfe;
    map[QStringLiteral("heightsRealization")] = fields.heights_realization;
    map[QStringLiteral("outputs")] = static_cast<int>(outputs);
    map[QStringLiteral("outputBitDepth")] = facts.ac3forge_support->outputs.bit_depth;
    map[QStringLiteral("crossoverHz")] = settings.crossover_hz.value_or(80.0);

    QVariantList trim_db;
    QVariantList delay_ms;
    trim_db.reserve(static_cast<qsizetype>(outputs));
    delay_ms.reserve(static_cast<qsizetype>(outputs));
    for (std::size_t i = 0; i < outputs; ++i) {
        trim_db.push_back(settings.trim_db && i < settings.trim_db->size() ? (*settings.trim_db)[i] : 0.0);
        delay_ms.push_back(settings.delay_ms && i < settings.delay_ms->size() ? (*settings.delay_ms)[i] : 0.0);
    }
    map[QStringLiteral("trimDb")] = trim_db;
    map[QStringLiteral("delayMs")] = delay_ms;

    QVariantList routing;
    routing.reserve(static_cast<qsizetype>(fields.labels.size()));
    const std::optional<ac3::render::Routing> parsed_routing =
        settings.routing ? ac3::render::Routing::parse(*settings.routing, outputs) : std::nullopt;
    for (std::size_t slot = 0; slot < static_cast<std::size_t>(fields.labels.size()); ++slot) {
        routing.push_back(parsed_routing ? parsed_routing->output_of(slot) : ac3::render::Routing::kUnassigned);
    }
    map[QStringLiteral("routing")] = routing;

    const forge::Management& management = facts.ac3forge_support->management;
    QVariantMap management_map;
    management_map[QStringLiteral("routing")] = management.routing;
    management_map[QStringLiteral("trimMinDb")] = management.trim_db[0];
    management_map[QStringLiteral("trimMaxDb")] = management.trim_db[1];
    management_map[QStringLiteral("maxDelayMs")] = management.max_delay_ms;
    management_map[QStringLiteral("crossoverMinHz")] = management.crossover_hz[0];
    management_map[QStringLiteral("crossoverMaxHz")] = management.crossover_hz[1];
    management_map[QStringLiteral("identify")] = management.identify;
    map[QStringLiteral("management")] = management_map;

    map[QStringLiteral("identifySlot")] = facts.identify_slot.value_or(-1);
    return map;
}

[[nodiscard]] QVariantMap sink_decoder_settings_to_map(const ac3::hearth::SinkFacts& facts) {
    QVariantMap map;
    if (!facts.ac3forge_support) {
        return map;
    }
    const forge::DecoderSettings& decoder = sink_settings_base(facts).decoder;
    map[QStringLiteral("mode")] = mode_name(decoder.mode.value_or(forge::DecoderMode::kLine));
    map[QStringLiteral("drcCut")] = decoder.drc_cut.value_or(1.0);
    map[QStringLiteral("drcBoost")] = decoder.drc_boost.value_or(1.0);
    map[QStringLiteral("heavyCompression")] = decoder.heavy_compression.value_or(false);
    map[QStringLiteral("normaliseDialogue")] = decoder.dialnorm.value_or(true);
    map[QStringLiteral("downmix")] = downmix_name(decoder.downmix.value_or(forge::Downmix::kLoRo));
    map[QStringLiteral("mixLfe")] = decoder.mix_lfe.value_or(false);
    map[QStringLiteral("objects")] = objects_name(decoder.objects.value_or(forge::ObjectsPolicy::kAuto));
    map[QStringLiteral("concealment")] = concealment_name(decoder.concealment.value_or(forge::Concealment::kRepeatFade));

    QStringList accepted;
    for (const std::string& name : facts.ac3forge_support->decoder_settings) {
        accepted.push_back(QString::fromStdString(name));
    }
    map[QStringLiteral("acceptedKeys")] = accepted;
    return map;
}

[[nodiscard]] QVariantMap sink_report_to_map(const ac3::hearth::SinkFacts& facts) {
    QVariantMap map;
    if (!facts.ac3forge_support) {
        return map;
    }
    QString settings_text = QStringLiteral("Nothing sent yet.");
    if (facts.intended_settings) {
        const std::int64_t sent = facts.intended_settings->revision;
        if (!facts.ac3forge_state) {
            settings_text = QObject::tr("revision %1 sent, not reported yet").arg(sent);
        } else if (facts.ac3forge_state->settings_error && facts.ac3forge_state->settings_error->revision == sent) {
            settings_text = QObject::tr("revision %1 refused: %2")
                                .arg(sent)
                                .arg(QString::fromStdString(facts.ac3forge_state->settings_error->why));
        } else if (facts.ac3forge_state->settings_revision >= sent) {
            settings_text = QObject::tr("revision %1 · applied").arg(sent);
        } else {
            settings_text = QObject::tr("revision %1 sent · sink on %2").arg(sent).arg(facts.ac3forge_state->settings_revision);
        }
    }
    map[QStringLiteral("settingsText")] = settings_text;

    QString stream_text = QObject::tr("Nothing playing.");
    QString objects_text = QObject::tr("none");
    QString dialogue_text = QObject::tr("not reported");
    if (facts.ac3forge_state && facts.ac3forge_state->decoder) {
        const forge::DecoderReport& decoder = *facts.ac3forge_state->decoder;
        const QString data_type = QString::fromStdString(std::string(forge::data_type_name(decoder.data_type)))
                                       .toUpper();
        stream_text = QObject::tr("%1 · %2%3 · %4 substream%5")
                          .arg(data_type == QStringLiteral("EAC3") ? QStringLiteral("E-AC-3") : QStringLiteral("AC-3"))
                          .arg(acmod_text(decoder.acmod))
                          .arg(decoder.lfe ? QStringLiteral(" + LFE") : QString())
                          .arg(decoder.substreams)
                          .arg(decoder.substreams == 1 ? QString() : QStringLiteral("s"));
        objects_text = decoder.objects == 0
                           ? QObject::tr("none")
                           : QObject::tr("%1 carried · %2").arg(decoder.objects).arg(decoder.objects_placed
                                                                                          ? QObject::tr("placed")
                                                                                          : QObject::tr("not placed"));
        dialogue_text = QObject::tr("dialnorm %1").arg(decoder.dialnorm, 0, 'f', 0);
    }
    map[QStringLiteral("streamText")] = stream_text;
    map[QStringLiteral("objectsText")] = objects_text;
    map[QStringLiteral("dialogueText")] = dialogue_text;

    const forge::Counters counters = facts.ac3forge_state ? facts.ac3forge_state->counters : forge::Counters{};
    map[QStringLiteral("playedText")] = QObject::tr("%1 bursts").arg(counters.bursts_played);
    map[QStringLiteral("problemsText")] = QObject::tr("%1 underruns · %2 late · %3 dropped · %4 invalid")
                                               .arg(counters.underruns)
                                               .arg(counters.late_chunks)
                                               .arg(counters.dropped_chunks)
                                               .arg(counters.invalid_chunks);
    return map;
}

[[nodiscard]] QVariantMap sink_only_on_sink_to_map(const ac3::hearth::SinkFacts& facts) {
    QVariantMap map;
    if (!facts.ac3forge_support) {
        return map;
    }
    map[QStringLiteral("name")] = QString::fromStdString(facts.name);
    map[QStringLiteral("slotsText")] =
        facts.output_slots
            ? QObject::tr("%1-bit · %2 slots").arg(facts.output_bit_depth.value_or(0)).arg(*facts.output_slots)
            : QObject::tr("not reported");
    // No RSSI or interface (Wi-Fi/Ethernet) field exists anywhere on the
    // wire (ac3forge_player.hpp, messages.hpp) - the mockup's own note says
    // this whole panel is "set on the sink's page", and this row shows the
    // one network fact this app actually has: how it reached the sink.
    map[QStringLiteral("network")] =
        facts.address.empty() ? QObject::tr("not reported") : QString::fromStdString(facts.address);
    map[QStringLiteral("firmware")] =
        facts.firmware.empty() ? QObject::tr("not reported") : QString::fromStdString(facts.firmware);
    map[QStringLiteral("url")] = facts.address.empty() ? QString() : QStringLiteral("http://%1/").arg(QString::fromStdString(facts.address));
    return map;
}

[[nodiscard]] QVariantMap row_to_variant(const ac3::hearth::SinkRow& row) {
    QVariantMap map;
    map[QStringLiteral("id")] = QString::fromStdString(row.id);
    map[QStringLiteral("name")] = QString::fromStdString(row.name);
    map[QStringLiteral("icon")] = QString::fromStdString(row.icon);
    map[QStringLiteral("subtitle")] = QString::fromStdString(row.subtitle);
    map[QStringLiteral("badge")] = QString::fromStdString(row.badge);
    map[QStringLiteral("badgeText")] = QString::fromStdString(row.badge_text);
    map[QStringLiteral("notice")] = QString::fromStdString(row.notice);
    return map;
}

[[nodiscard]] QVariantMap detail_to_variant(const ac3::hearth::SinkDetail& detail) {
    QVariantMap map;
    map[QStringLiteral("id")] = QString::fromStdString(detail.id);
    map[QStringLiteral("name")] = QString::fromStdString(detail.name);
    map[QStringLiteral("badge")] = QString::fromStdString(detail.badge);
    map[QStringLiteral("kindText")] = QString::fromStdString(detail.kind_text);
    map[QStringLiteral("address")] = QString::fromStdString(detail.address);
    map[QStringLiteral("rolesText")] = QString::fromStdString(detail.roles_text);
    map[QStringLiteral("takesText")] = QString::fromStdString(detail.takes_text);
    map[QStringLiteral("outputsText")] = QString::fromStdString(detail.outputs_text);
    map[QStringLiteral("latencyText")] = QString::fromStdString(detail.latency_text);
    map[QStringLiteral("clockText")] = QString::fromStdString(detail.clock_text);
    map[QStringLiteral("pairedOnText")] = QString::fromStdString(detail.paired_on_text);
    map[QStringLiteral("notice")] = QString::fromStdString(detail.notice);
    return map;
}

// At least one member connected right now - NetworkOutputStatus::Entry::ready's
// own comment says why this does not require every member.
[[nodiscard]] bool group_ready(const ac3::hearth::GroupFacts& facts) {
    return std::any_of(facts.members.begin(), facts.members.end(),
                       [](const ac3::hearth::GroupMemberFacts& member) { return member.connected; });
}

[[nodiscard]] QVariantMap group_row_to_variant(const ac3::hearth::GroupRow& row) {
    QVariantMap map;
    map[QStringLiteral("id")] = QString::fromStdString(row.id);
    map[QStringLiteral("name")] = QString::fromStdString(row.name);
    map[QStringLiteral("icon")] = QString::fromStdString(row.icon);
    map[QStringLiteral("subtitle")] = QString::fromStdString(row.subtitle);
    map[QStringLiteral("badge")] = QString::fromStdString(row.badge);
    map[QStringLiteral("badgeText")] = QString::fromStdString(row.badge_text);
    return map;
}

[[nodiscard]] QVariantMap group_member_to_variant(const ac3::hearth::GroupMemberRow& member) {
    QVariantMap map;
    map[QStringLiteral("sinkId")] = QString::fromStdString(member.sink_id);
    map[QStringLiteral("name")] = QString::fromStdString(member.name);
    map[QStringLiteral("getsText")] = QString::fromStdString(member.gets_text);
    map[QStringLiteral("volume")] = member.volume;
    map[QStringLiteral("muted")] = member.muted;
    map[QStringLiteral("volumeSupported")] = member.volume_supported;
    map[QStringLiteral("muteSupported")] = member.mute_supported;
    map[QStringLiteral("connected")] = member.connected;
    return map;
}

[[nodiscard]] QVariantMap group_detail_to_variant(const ac3::hearth::GroupDetail& detail) {
    QVariantMap map;
    map[QStringLiteral("id")] = QString::fromStdString(detail.id);
    map[QStringLiteral("name")] = QString::fromStdString(detail.name);
    QVariantList members;
    members.reserve(static_cast<qsizetype>(detail.members.size()));
    for (const ac3::hearth::GroupMemberRow& member : detail.members) {
        members.push_back(group_member_to_variant(member));
    }
    map[QStringLiteral("members")] = members;
    map[QStringLiteral("groupVolume")] = detail.group_volume;
    map[QStringLiteral("groupMuted")] = detail.group_muted;
    map[QStringLiteral("membersConnectedText")] = QString::fromStdString(detail.members_connected_text);
    map[QStringLiteral("leadTimeText")] = QString::fromStdString(detail.lead_time_text);
    return map;
}

}  // namespace

NetworkController::NetworkController(QObject* parent)
    : QObject(parent),
      // The four-argument constructor: the two-argument one always uses the
      // native store (the registry here) whatever QSettings::setDefaultFormat
      // says, which would let a QML test suite - or a --shot capture - read
      // and write the developer's own settings. hearth_controller.cpp's own
      // constructor carries the identical comment for the identical reason.
      settings_(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("ac3forge"),
                QStringLiteral("Hearth")),
      settings_store_(std::make_unique<ac3::hearth::ui::QSettingsStore>(settings_)),
      pairing_store_(std::make_unique<ac3::hearth::PairingStore>(
          *settings_store_, [] { return QDate::currentDate().toString(Qt::ISODate).toStdString(); })) {
    poll_timer_.setInterval(kPollMs);
    connect(&poll_timer_, &QTimer::timeout, this, &NetworkController::poll);
}

NetworkController::~NetworkController() = default;

void NetworkController::start() {
    if (sinks_engine_) {
        return;
    }
    const std::optional<ac3::sendspin::noise::KeyPair> identity = ac3::sendspin::noise::KeyPair::generate();
    if (!identity.has_value()) {
        return;
    }
    const std::string name =
        ac3::hearth::default_network_name(QSysInfo::machineHostName().toStdString());
    sinks_engine_ = std::make_unique<ac3::hearth::NetworkSinks>(*identity, name, *pairing_store_);
    poll_timer_.start();
    poll();
}

void NetworkController::rescan() {
    if (sinks_engine_) {
        sinks_engine_->rescan();
    }
}

void NetworkController::selectSink(const QString& id) {
    if (sinks_engine_) {
        sinks_engine_->select_sink(id.toStdString());
    }
}

void NetworkController::submitPairingCode(const QString& id, const QString& code) {
    if (sinks_engine_) {
        sinks_engine_->submit_pairing_code(id.toStdString(), code.toStdString());
    }
}

void NetworkController::cancelPairing(const QString& id) {
    if (sinks_engine_) {
        sinks_engine_->cancel_pairing(id.toStdString());
    }
}

QString NetworkController::createGroup(const QString& name) {
    if (!sinks_engine_) {
        return {};
    }
    const QString id = QString::fromStdString(sinks_engine_->create_group(name.toStdString()));
    poll();
    return id;
}

void NetworkController::renameGroup(const QString& groupId, const QString& name) {
    if (sinks_engine_) {
        sinks_engine_->rename_group(groupId.toStdString(), name.toStdString());
    }
}

void NetworkController::deleteGroup(const QString& groupId) {
    if (sinks_engine_) {
        sinks_engine_->delete_group(groupId.toStdString());
    }
}

void NetworkController::selectGroup(const QString& groupId) {
    if (sinks_engine_) {
        sinks_engine_->select_group(groupId.toStdString());
    }
}

void NetworkController::addGroupMember(const QString& groupId, const QString& sinkId) {
    if (sinks_engine_) {
        sinks_engine_->add_group_member(groupId.toStdString(), sinkId.toStdString());
    }
}

void NetworkController::removeGroupMember(const QString& groupId, const QString& sinkId) {
    if (sinks_engine_) {
        sinks_engine_->remove_group_member(groupId.toStdString(), sinkId.toStdString());
    }
}

void NetworkController::setGroupVolume(const QString& groupId, int volume) {
    if (sinks_engine_) {
        sinks_engine_->set_group_volume(groupId.toStdString(), volume);
    }
}

void NetworkController::setGroupMuted(const QString& groupId, bool muted) {
    if (sinks_engine_) {
        sinks_engine_->set_group_muted(groupId.toStdString(), muted);
    }
}

void NetworkController::setMemberVolume(const QString& groupId, const QString& sinkId, int volume) {
    if (sinks_engine_) {
        sinks_engine_->set_member_volume(groupId.toStdString(), sinkId.toStdString(), volume);
    }
}

void NetworkController::setMemberMuted(const QString& groupId, const QString& sinkId, bool muted) {
    if (sinks_engine_) {
        sinks_engine_->set_member_muted(groupId.toStdString(), sinkId.toStdString(), muted);
    }
}

void NetworkController::poll() {
    if (!sinks_engine_) {
        return;
    }
    const ac3::hearth::NetworkStatus status = sinks_engine_->status();

    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(status.sinks.size()));
    QVariantMap selected;
    bool selected_settable = false;
    QVariantMap speaker_settings;
    QVariantMap decoder_settings;
    QVariantMap report;
    QVariantMap only_on_sink;
    for (const ac3::hearth::SinkFacts& facts : status.sinks) {
        rows.push_back(row_to_variant(ac3::hearth::to_row(facts)));
        if (facts.id == status.selected_id) {
            selected = detail_to_variant(ac3::hearth::to_detail(facts));
            selected_settable = facts.pair_state == ac3::hearth::PairState::kPaired &&
                                 facts.ac3forge_support.has_value();
            if (selected_settable) {
                speaker_settings = sink_speaker_settings_to_map(facts);
                decoder_settings = sink_decoder_settings_to_map(facts);
                report = sink_report_to_map(facts);
                only_on_sink = sink_only_on_sink_to_map(facts);
            }
        }
    }

    QVariantList group_rows;
    group_rows.reserve(static_cast<qsizetype>(status.groups.size()));
    QVariantMap selected_group;
    // Published whether or not anything below actually changed (unlike
    // sinks_/groups_ and friends, which only emit sinksChanged() on a real
    // difference): NetworkOutputStatus::Entry::ready is live, per-member
    // state (connected can flip without the group's own row text changing),
    // and HearthController's own poll() needs to see that promptly rather
    // than only when this controller's own UI-facing fields happen to.
    std::map<std::string, ac3::hearth::ui::NetworkOutputStatus::Entry> output_status;
    for (const ac3::hearth::GroupFacts& facts : status.groups) {
        group_rows.push_back(group_row_to_variant(ac3::hearth::to_group_row(facts)));
        if (facts.id == status.selected_group_id) {
            selected_group = group_detail_to_variant(ac3::hearth::to_group_detail(facts));
        }
        output_status.emplace(facts.id, ac3::hearth::ui::NetworkOutputStatus::Entry{
                                            .ready = group_ready(facts),
                                            .group = sinks_engine_->group(facts.id)});
    }
    ac3::hearth::ui::NetworkOutputStatus::instance().set_groups(std::move(output_status));

    const QString new_selected_id = QString::fromStdString(status.selected_id);
    const QString new_pairing_error = QString::fromStdString(status.pairing_error);
    const QString new_selected_group_id = QString::fromStdString(status.selected_group_id);
    if (rows == sinks_ && new_selected_id == selected_id_ && selected == selected_sink_ &&
        new_pairing_error == pairing_error_ && selected_settable == selected_sink_settable_ &&
        speaker_settings == sink_speaker_settings_ && decoder_settings == sink_decoder_settings_ &&
        report == sink_report_ && only_on_sink == sink_only_on_sink_ &&
        group_rows == groups_ && new_selected_group_id == selected_group_id_ &&
        selected_group == selected_group_) {
        return;
    }
    sinks_ = std::move(rows);
    selected_id_ = new_selected_id;
    selected_sink_ = std::move(selected);
    pairing_error_ = new_pairing_error;
    selected_sink_settable_ = selected_settable;
    sink_speaker_settings_ = std::move(speaker_settings);
    sink_decoder_settings_ = std::move(decoder_settings);
    sink_report_ = std::move(report);
    sink_only_on_sink_ = std::move(only_on_sink);
    groups_ = std::move(group_rows);
    selected_group_id_ = new_selected_group_id;
    selected_group_ = std::move(selected_group);
    emit sinksChanged();
}

namespace {
[[nodiscard]] std::optional<ac3::hearth::SinkFacts> selected_sink_facts(const ac3::hearth::NetworkSinks& sinks) {
    const ac3::hearth::NetworkStatus status = sinks.status();
    for (const ac3::hearth::SinkFacts& facts : status.sinks) {
        if (facts.id == status.selected_id) {
            return facts;
        }
    }
    return std::nullopt;
}
}  // namespace

void NetworkController::setSinkLayoutText(const QString& text) {
    if (!sinks_engine_) {
        return;
    }
    const std::optional<ac3::hearth::SinkFacts> facts = selected_sink_facts(*sinks_engine_);
    if (!facts || !facts->ac3forge_support) {
        return;
    }
    if (!ac3::render::OutputLayout::parse(text.toStdString())) {
        return;
    }
    forge::Settings settings = sink_settings_base(*facts);
    settings.layout = text.toStdString();
    // A layout change can change the slot count: re-size trim_db/delay_ms to
    // the sink's own OUTPUT count (unaffected by slots) is already right as
    // is, and routing is re-checked against the new slot count next poll -
    // an assignment past the new layout's own slot count simply stops
    // showing in the grid, the same "narrower layout drops the tail" rule
    // HearthController::setLayoutText() follows locally.
    sinks_engine_->push_sink_settings(facts->id, settings);
}

void NetworkController::setSinkHeights(const QString& realization) {
    if (!sinks_engine_) {
        return;
    }
    const std::optional<ac3::hearth::SinkFacts> facts = selected_sink_facts(*sinks_engine_);
    if (!facts || !facts->ac3forge_support) {
        return;
    }
    forge::Settings settings = sink_settings_base(*facts);
    const ac3::render::OutputLayout layout = draft_layout(settings.layout);
    settings.layout = std::string(layout.with_realization(realization_from_name(realization)).text());
    sinks_engine_->push_sink_settings(facts->id, settings);
}

void NetworkController::setSinkSpeakerSmall(int slot, bool small) {
    if (!sinks_engine_ || slot < 0) {
        return;
    }
    const std::optional<ac3::hearth::SinkFacts> facts = selected_sink_facts(*sinks_engine_);
    if (!facts || !facts->ac3forge_support) {
        return;
    }
    forge::Settings settings = sink_settings_base(*facts);
    const ac3::render::OutputLayout layout = draft_layout(settings.layout);
    const std::optional<ac3::render::OutputLayout> changed = layout.with_small(static_cast<std::size_t>(slot), small);
    if (!changed) {
        return;
    }
    settings.layout = std::string(changed->text());
    sinks_engine_->push_sink_settings(facts->id, settings);
}

void NetworkController::setSinkTrimDb(int output, double db) {
    if (!sinks_engine_ || output < 0) {
        return;
    }
    const std::optional<ac3::hearth::SinkFacts> facts = selected_sink_facts(*sinks_engine_);
    if (!facts || !facts->ac3forge_support) {
        return;
    }
    forge::Settings settings = sink_settings_base(*facts);
    if (!settings.trim_db || static_cast<std::size_t>(output) >= settings.trim_db->size()) {
        return;
    }
    (*settings.trim_db)[static_cast<std::size_t>(output)] = db;
    sinks_engine_->push_sink_settings(facts->id, settings);
}

void NetworkController::setSinkDelayMs(int output, double ms) {
    if (!sinks_engine_ || output < 0) {
        return;
    }
    const std::optional<ac3::hearth::SinkFacts> facts = selected_sink_facts(*sinks_engine_);
    if (!facts || !facts->ac3forge_support) {
        return;
    }
    forge::Settings settings = sink_settings_base(*facts);
    if (!settings.delay_ms || static_cast<std::size_t>(output) >= settings.delay_ms->size()) {
        return;
    }
    (*settings.delay_ms)[static_cast<std::size_t>(output)] = ms;
    sinks_engine_->push_sink_settings(facts->id, settings);
}

void NetworkController::setSinkCrossoverHz(double hz) {
    if (!sinks_engine_) {
        return;
    }
    const std::optional<ac3::hearth::SinkFacts> facts = selected_sink_facts(*sinks_engine_);
    if (!facts || !facts->ac3forge_support) {
        return;
    }
    forge::Settings settings = sink_settings_base(*facts);
    settings.crossover_hz = hz;
    sinks_engine_->push_sink_settings(facts->id, settings);
}

void NetworkController::setSinkRoutingAssignment(int slot, int output) {
    if (!sinks_engine_ || slot < 0) {
        return;
    }
    const std::optional<ac3::hearth::SinkFacts> facts = selected_sink_facts(*sinks_engine_);
    if (!facts || !facts->ac3forge_support || !facts->ac3forge_support->management.routing) {
        return;
    }
    forge::Settings settings = sink_settings_base(*facts);
    const ac3::render::OutputLayout layout = draft_layout(settings.layout);
    const auto outputs = static_cast<std::size_t>(facts->ac3forge_support->outputs.count);
    std::optional<ac3::render::Routing> patch =
        settings.routing ? ac3::render::Routing::parse(*settings.routing, outputs)
                          : ac3::render::Routing::identity(layout.slots(), outputs);
    if (!patch || !patch->assign(static_cast<std::size_t>(slot), output)) {
        return;
    }
    std::array<char, ac3::render::Routing::kTextBytes> text{};
    if (patch->format(text) == 0 && patch->channels() > 0) {
        return;
    }
    settings.routing = std::string(text.data());
    sinks_engine_->push_sink_settings(facts->id, settings);
}

void NetworkController::setSinkDecoderSettings(const QVariantMap& settings_map) {
    if (!sinks_engine_) {
        return;
    }
    const std::optional<ac3::hearth::SinkFacts> facts = selected_sink_facts(*sinks_engine_);
    if (!facts || !facts->ac3forge_support) {
        return;
    }
    forge::Settings settings = sink_settings_base(*facts);
    // Starts from the sink's own last-known decoder settings (sink_settings_
    // base() above, which already applies "?? default" for any key never
    // pushed) and, like HearthController::decoder_settings_from_map()'s own
    // comment says, only overwrites a key this map actually carries - so a
    // caller that reads sinkDecoderSettings(), changes one key and writes
    // the rest back unmodified keeps every other field's value.
    forge::DecoderSettings& decoder = settings.decoder;
    if (settings_map.contains(QStringLiteral("mode"))) {
        decoder.mode = mode_from_name(settings_map.value(QStringLiteral("mode")).toString());
    }
    if (settings_map.contains(QStringLiteral("drcCut"))) {
        decoder.drc_cut = settings_map.value(QStringLiteral("drcCut")).toDouble();
    }
    if (settings_map.contains(QStringLiteral("drcBoost"))) {
        decoder.drc_boost = settings_map.value(QStringLiteral("drcBoost")).toDouble();
    }
    if (settings_map.contains(QStringLiteral("heavyCompression"))) {
        decoder.heavy_compression = settings_map.value(QStringLiteral("heavyCompression")).toBool();
    }
    if (settings_map.contains(QStringLiteral("normaliseDialogue"))) {
        decoder.dialnorm = settings_map.value(QStringLiteral("normaliseDialogue")).toBool();
    }
    if (settings_map.contains(QStringLiteral("downmix"))) {
        decoder.downmix = downmix_from_name(settings_map.value(QStringLiteral("downmix")).toString());
    }
    if (settings_map.contains(QStringLiteral("mixLfe"))) {
        decoder.mix_lfe = settings_map.value(QStringLiteral("mixLfe")).toBool();
    }
    if (settings_map.contains(QStringLiteral("objects"))) {
        decoder.objects = objects_from_name(settings_map.value(QStringLiteral("objects")).toString());
    }
    if (settings_map.contains(QStringLiteral("concealment"))) {
        decoder.concealment = concealment_from_name(settings_map.value(QStringLiteral("concealment")).toString());
    }
    sinks_engine_->push_sink_settings(facts->id, settings);
}

void NetworkController::startSinkIdentify(int slot) {
    if (!sinks_engine_ || slot < 0) {
        return;
    }
    const std::optional<ac3::hearth::SinkFacts> facts = selected_sink_facts(*sinks_engine_);
    if (!facts || !facts->ac3forge_support || !facts->ac3forge_support->management.identify) {
        return;
    }
    sinks_engine_->push_sink_identify(facts->id, forge::Identify{.output = slot});
}

void NetworkController::stopSinkIdentify() {
    if (!sinks_engine_) {
        return;
    }
    const std::optional<ac3::hearth::SinkFacts> facts = selected_sink_facts(*sinks_engine_);
    if (!facts) {
        return;
    }
    sinks_engine_->push_sink_identify(facts->id, std::nullopt);
}

}  // namespace ac3::hearth::ui
