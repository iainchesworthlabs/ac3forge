#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QWindow>
#include <QQuickStyle>
#include <QClipboard>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QMimeData>
#include <QPointF>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hearth_controller.hpp"
#include "language_manager.hpp"
#include "network_controller.hpp"
#include "test_room.hpp"

// Qt Quick Test entry point for the Hearth window: runs every tst_*.qml under
// QUICK_TEST_SOURCE_DIR against the REAL HearthController the embedded
// Ac3ForgeHearth module registers - the same rule apps/gui/tests and
// apps/crucible/ui/tests follow, and for the same reason: a parallel fake API
// is a second thing the real one could silently disagree with. Unlike
// CrucibleController, HearthController is QML_SINGLETON, so no manual
// qmlRegisterSingletonInstance() is needed here to reach it - the embedded
// module registers it itself, the same as Main.qml gets it.
//
// Hearth has no tray, no icon provider and no scripted-machine TestServices
// double, so this file is still shorter than apps/crucible/ui/tests/
// qml_test_main.cpp's own. It DOES need a LanguageManager now: Settings.qml
// imports Ac3ForgeHearthLanguage, and unlike HearthController that singleton
// is not QML_SINGLETON-registered by the module - main.cpp registers the
// instance by hand, so the suite has to as well or Settings.qml will not
// load here.

namespace {

// The fakes a suite can hand the REAL controllers (test_room.hpp), registered
// as the Ac3ForgeHearthTest singleton - apps/crucible/ui/tests/qml_test_main.cpp's
// own TestServices is the pattern this follows, and for the same reason: a
// suite that wants to see the engine actually play needs somewhere to play
// into, and neither a CI container nor a developer's desk should be it.
//
// useFakeRoom() must run before the first HearthController.start() in the
// process (start() reads the seam once - HearthController::set_test_outputs()'s
// own comment), so a suite calls it from its initTestCase(). A suite that never
// calls it sees the machine exactly as before, which is what the four
// original suites were written against. One process per suite (one ctest
// entry each), so a fake room in one cannot reach another.
class TestServices : public QObject {
    Q_OBJECT

public:
    explicit TestServices(QQmlEngine& engine) : engine_(&engine) {}

    // Three endpoints: a default 5.1 set of speakers, a stereo pair of
    // headphones, and a 7.1 receiver that says it takes AC-3/E-AC-3
    // passthrough (the picker's second section). False when the controller
    // cannot be reached, or when start() has already run - too late for the
    // seam to matter, and a suite should know rather than silently get the
    // machine.
    Q_INVOKABLE bool useFakeRoom() {
        auto* controller = hearth();
        if (controller == nullptr) {
            return false;
        }
        if (room_) {
            return true;
        }
        room_ = ac3::hearth::uitest::make_room({
            {.id = "fake-speakers", .name = "Test speakers", .is_default = true, .channels = 6},
            {.id = "fake-headphones", .name = "Test headphones", .is_default = false, .channels = 2},
            {.id = "fake-receiver", .name = "Test receiver", .is_default = false, .channels = 8, .passthrough = true},
        });
        controller->set_test_outputs(ac3::hearth::uitest::outputs_for(room_));
        return true;
    }

    // The device clock's rate against real time: 1.0 by default, faster for
    // a suite that wants an item to play to its end without waiting it out.
    Q_INVOKABLE void setClockSpeed(double speed) {
        if (room_) {
            ac3::hearth::uitest::set_speed(*room_, speed);
        }
    }

    // What the fake device has been asked to do: { open, paused, endpoint,
    // sampleRate, channels, opens, framesHeard, peak } - empty before
    // useFakeRoom().
    Q_INVOKABLE QVariantMap device() const {
        if (!room_) {
            return {};
        }
        const ac3::hearth::uitest::RoomReading reading = ac3::hearth::uitest::read(*room_);
        return {{QStringLiteral("open"), reading.open},
                {QStringLiteral("paused"), reading.paused},
                {QStringLiteral("endpoint"), QString::fromStdString(reading.endpoint)},
                {QStringLiteral("sampleRate"), reading.sample_rate},
                {QStringLiteral("channels"), reading.channels},
                {QStringLiteral("opens"), reading.opens},
                {QStringLiteral("framesHeard"), static_cast<double>(reading.frames_heard)},
                {QStringLiteral("peak"), reading.peak}};
    }

    Q_INVOKABLE void resetPeak() {
        if (room_) {
            ac3::hearth::uitest::reset_peak(*room_);
        }
    }

    // Copies `source` (a path, or a file: URL) into this process's scratch
    // folder as `name`, so a suite can queue several distinctly named items
    // from the repository's small golden streams, and returns the copy's
    // path - empty if the copy failed. `folder` (optional) puts it in a
    // subfolder of its own, for Add folder.
    Q_INVOKABLE QString stageFixture(const QString& source, const QString& name, const QString& folder = {}) {
        if (!scratch_.isValid()) {
            return {};
        }
        const QString from = source.startsWith(QStringLiteral("file:")) ? QUrl(source).toLocalFile() : source;
        QDir dir(scratch_.path());
        if (!folder.isEmpty()) {
            dir.mkpath(folder);
            dir.cd(folder);
        }
        const QString to = dir.filePath(name);
        QFile::remove(to);
        return QFile::copy(from, to) ? to : QString();
    }

    // A fresh folder under the scratch folder, for a suite's own output files
    // (a diagnostics export, a media JSON export).
    Q_INVOKABLE QString scratchPath(const QString& name) const { return QDir(scratch_.path()).filePath(name); }

    Q_INVOKABLE bool removeFile(const QString& path) const {
        return QFile::remove(path.startsWith(QStringLiteral("file:")) ? QUrl(path).toLocalFile() : path);
    }

    Q_INVOKABLE QString readTextFile(const QString& path) const {
        QFile file(path.startsWith(QStringLiteral("file:")) ? QUrl(path).toLocalFile() : path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromUtf8(file.readAll());
    }

    // The first QObject descendant of `root` whose `property` equals
    // `value` - for a non-visual object such as a QtQuick.Dialogs FileDialog,
    // which a ScrollView-rooted page (Settings.qml) parents without listing
    // in any QML list property a suite could walk, and which carries no
    // objectName for TestCase.findChild().
    Q_INVOKABLE QObject* findByProperty(QObject* root, const QString& property, const QVariant& value) const {
        if (root == nullptr) {
            return nullptr;
        }
        const QByteArray name = property.toUtf8();
        for (QObject* child : root->findChildren<QObject*>()) {
            const QVariant found = child->property(name.constData());
            if (found.isValid() && found == value) {
                return child;
            }
        }
        return nullptr;
    }

    // The clipboard's text, for a suite checking a Copy button.
    Q_INVOKABLE QString clipboardText() const { return QGuiApplication::clipboard()->text(); }

    // Starts an in-process Hearth test sink (apps/hearth/testsink) named
    // `name` on loopback and hands it to NetworkController's own
    // NetworkSinks as a found service - starting NetworkController first if
    // no suite code has yet. `acceptSettings`: the sink lists the Settings
    // command and applies one (SinkOptions::accept_settings). Every sink
    // started stays up for the process; the code/log calls below read the
    // most recent one. Returns the empty string on success, otherwise why
    // not.
    Q_INVOKABLE QString startTestSink(const QString& name, bool acceptSettings = false) {
        auto* network = this->network();
        if (network == nullptr) {
            return QStringLiteral("NetworkController is not registered");
        }
        network->start();
        if (network->sinks_for_test() == nullptr) {
            return QStringLiteral("NetworkController did not start its Sendspin host");
        }
        std::string error;
        auto sink = ac3::hearth::uitest::start_test_sink(
            name.toStdString(),
            QDir(scratch_.path()).filePath(QStringLiteral("sink-%1").arg(sinks_.size())).toStdString(),
            acceptSettings, &error);
        if (!sink) {
            return QString::fromStdString(error);
        }
        ac3::hearth::uitest::announce(*network->sinks_for_test(), *sink);
        sinks_.push_back(std::move(sink));
        return {};
    }

    // The code the test sink printed for the attempt under way, digits only -
    // what a person would read off the sink's console and type into the
    // Network page's code boxes. Empty until the sink has printed one.
    Q_INVOKABLE QString testSinkCode() const {
        return sinks_.empty() ? QString()
                              : QString::fromStdString(ac3::hearth::uitest::pairing_code(*sinks_.back()));
    }

    Q_INVOKABLE QStringList testSinkLog() const {
        QStringList lines;
        if (!sinks_.empty()) {
            for (const std::string& line : ac3::hearth::uitest::log_lines(*sinks_.back())) {
                lines.push_back(QString::fromStdString(line));
            }
        }
        return lines;
    }

    // A key press and release delivered to `target`'s own window (an item,
    // or a window itself). TestCase.keyClick() always sends to the test
    // case's own window, never to a second top-level one - so a suite that
    // creates Main.qml's ApplicationWindow needs this to reach that
    // window's Shortcuts (Ctrl+1..6, F1, Escape) or a text field inside it.
    // QTest::keyClick() on a QWindow sends the ShortcutOverride first, the
    // way a real key press arrives, so a Shortcut in that window fires
    // exactly as it would for a person - provided the window is the focus
    // window (Qt Quick's WindowShortcut context requires it), which the
    // caller arranges with requestActivate().
    Q_INVOKABLE bool keyClick(QObject* target, int key, int modifiers = 0) {
        QWindow* window = window_of(target);
        if (window == nullptr) {
            return false;
        }
        QTest::keyClick(window, static_cast<Qt::Key>(key), Qt::KeyboardModifiers(modifiers));
        return true;
    }

    // Each character of `text` typed into `target`'s window in turn.
    Q_INVOKABLE bool keyClicks(QObject* target, const QString& text) {
        QWindow* window = window_of(target);
        if (window == nullptr) {
            return false;
        }
        for (const QChar c : text) {
            QTest::keyClick(window, c.toLatin1());
        }
        return true;
    }

    // `paths` dropped on the middle of `target`, the way a file manager
    // drops them: a DragEnter, a DragMove and a Drop, each carrying the
    // files as file: URLs (text/uri-list), sent to `target`'s window, which
    // delivers them to whichever DropArea is under that point exactly as it
    // would a real drag. QtTest has no drag-and-drop call of its own, and an
    // internal Drag.active drag carries no URLs, so this is the only way to
    // reach PlayPage.qml's DropArea headlessly with real files. True when a
    // drop target under that point took the drag (accepted the DragEnter) -
    // PlayPage's own onDropped does not accept the Drop itself, which a
    // file manager does not need either.
    Q_INVOKABLE bool dropFiles(QObject* target, const QStringList& paths) {
        auto* item = qobject_cast<QQuickItem*>(target);
        if (item == nullptr || item->window() == nullptr) {
            return false;
        }
        QMimeData mime;
        QList<QUrl> urls;
        for (const QString& path : paths) {
            urls.push_back(QUrl::fromLocalFile(path));
        }
        mime.setUrls(urls);
        const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        QWindow* window = item->window();
        QDragEnterEvent enter(at.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &enter);
        QDragMoveEvent move(at.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &move);
        QDropEvent drop(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &drop);
        return enter.isAccepted();
    }

private:
    [[nodiscard]] static QWindow* window_of(QObject* target) {
        if (auto* window = qobject_cast<QWindow*>(target)) {
            return window;
        }
        if (auto* item = qobject_cast<QQuickItem*>(target)) {
            return item->window();
        }
        return nullptr;
    }

    [[nodiscard]] ac3::hearth::ui::HearthController* hearth() const {
        return engine_->singletonInstance<ac3::hearth::ui::HearthController*>(QStringLiteral("Ac3ForgeHearth"),
                                                                              QStringLiteral("HearthController"));
    }
    [[nodiscard]] ac3::hearth::ui::NetworkController* network() const {
        return engine_->singletonInstance<ac3::hearth::ui::NetworkController*>(QStringLiteral("Ac3ForgeHearth"),
                                                                               QStringLiteral("NetworkController"));
    }

    QQmlEngine* engine_ = nullptr;
    QTemporaryDir scratch_;
    std::shared_ptr<ac3::hearth::uitest::FakeRoom> room_;
    std::vector<std::shared_ptr<ac3::hearth::uitest::TestSinkHost>> sinks_;
};

// Mirrors DeskIsolation (apps/crucible/ui/tests/qml_test_main.cpp) and
// apps/gui/tests: real organisation/application names plus a QTemporaryDir
// settings path, so HearthController's QSettings (organisation "ac3forge",
// application "Hearth", the shipped app's own - hearth_controller.cpp's
// constructor) read and write a store that is empty at start and gone at
// exit rather than the developer's own. HearthController::start() is called
// explicitly by whichever test needs a live engine (there is no
// Component.onCompleted running it here, unlike Main.qml), so a suite that
// never calls it never opens a device at all.
class HearthTestIsolation : public QObject {
    Q_OBJECT

public slots:
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("ac3forge"));
        QCoreApplication::setApplicationName(QStringLiteral("Hearth"));
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        scratch_.emplace();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch_->path());
        // Documents (where Save diagnostics... suggests writing) and every
        // other standard location under ~/.qttest rather than the
        // developer's own folders; created, since a save into a missing
        // folder would fail for a reason that has nothing to do with the UI.
        QStandardPaths::setTestModeEnabled(true);
        QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
        // Before any suite starts the network (Main.qml's own
        // Component.onCompleted does, in every suite that opens it): a
        // suite's sinks are the loopback test sinks startTestSink() hands
        // NetworkSinks itself. Browsing too, every suite would dial the real
        // sinks on whatever network it runs on - CI's self-hosted runners sit
        // on someone's home network - and ask for a firewall exception this
        // binary cannot finish (NetworkController::set_network_discovery()'s
        // own comment).
        ac3::hearth::ui::NetworkController::set_network_discovery(false);
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        // Same URI and reasoning as main.cpp's own registration: its own,
        // not the module's, so registering a type by hand cannot mark
        // Ac3ForgeHearth registered and stop HearthController registering.
        language_manager_ = std::make_unique<LanguageManager>(
            *qGuiApp, *engine, QStringLiteral("ac3hearth"));
        qmlRegisterSingletonInstance("Ac3ForgeHearthLanguage", 1, 0, "LanguageManager",
                                     language_manager_.get());
        test_services_ = std::make_unique<TestServices>(*engine);
        qmlRegisterSingletonInstance("Ac3ForgeHearthTest", 1, 0, "TestServices", test_services_.get());
    }

private:
    std::optional<QTemporaryDir> scratch_;
    std::unique_ptr<LanguageManager> language_manager_;
    std::unique_ptr<TestServices> test_services_;
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(ac3hearth, HearthTestIsolation)

#include "qml_test_main.moc"
