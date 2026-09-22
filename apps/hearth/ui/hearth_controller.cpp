#include "hearth_controller.hpp"

#include <QFileInfo>

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

#include "ac3/render/layout.hpp"
#include "decoder_settings.hpp"
#include "engine_thread.hpp"
#include "item_loader.hpp"
#include "pcm_sink.hpp"
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
        ac3::hearth::QueueItem item;
        item.path = path.toStdString();
        item.title = QFileInfo(path).fileName().toStdString();
        items.push_back(std::move(item));
    }
    engine_->add(std::move(items));
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
}

}  // namespace ac3::hearth::ui
