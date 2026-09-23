#include "network_controller.hpp"

#include <QDate>
#include <QSysInfo>

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

#include "network_sinks.hpp"
#include "network_view.hpp"
#include "pairing_store.hpp"
#include "qsettings_store.hpp"
#include "settings_model.hpp"

namespace ac3::hearth::ui {

namespace {

constexpr int kPollMs = 60;

[[nodiscard]] QVariantMap row_to_variant(const ac3::hearth::SinkRow& row) {
    QVariantMap map;
    map[QStringLiteral("id")] = QString::fromStdString(row.id);
    map[QStringLiteral("name")] = QString::fromStdString(row.name);
    map[QStringLiteral("icon")] = QString::fromStdString(row.icon);
    map[QStringLiteral("subtitle")] = QString::fromStdString(row.subtitle);
    map[QStringLiteral("badge")] = QString::fromStdString(row.badge);
    map[QStringLiteral("badgeText")] = QString::fromStdString(row.badge_text);
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

void NetworkController::poll() {
    if (!sinks_engine_) {
        return;
    }
    const ac3::hearth::NetworkStatus status = sinks_engine_->status();

    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(status.sinks.size()));
    QVariantMap selected;
    for (const ac3::hearth::SinkFacts& facts : status.sinks) {
        rows.push_back(row_to_variant(ac3::hearth::to_row(facts)));
        if (facts.id == status.selected_id) {
            selected = detail_to_variant(ac3::hearth::to_detail(facts));
        }
    }

    const QString new_selected_id = QString::fromStdString(status.selected_id);
    const QString new_pairing_error = QString::fromStdString(status.pairing_error);
    if (rows == sinks_ && new_selected_id == selected_id_ && selected == selected_sink_ &&
        new_pairing_error == pairing_error_) {
        return;
    }
    sinks_ = std::move(rows);
    selected_id_ = new_selected_id;
    selected_sink_ = std::move(selected);
    pairing_error_ = new_pairing_error;
    emit sinksChanged();
}

}  // namespace ac3::hearth::ui
