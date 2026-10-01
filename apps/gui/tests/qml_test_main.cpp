#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QObject>
#include <QProcess>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <optional>

#include "../language_manager.hpp"
#include "iclforge/ac3/version.hpp"

// Standard Qt Quick Test entry point: discovers and runs every tst_*.qml
// file under QUICK_TEST_SOURCE_DIR (set in CMakeLists.txt), exercising the
// real EncoderController the qmldir already embedded into this binary
// resolves - see CMakeLists.txt for why that is a second embedding of the
// module rather than a shared library with forge-gui.

// The seam the byte-equality suites need and QML does not have: running the
// command line a page echoes through the forge this build made, in a folder
// of the test's choosing, and comparing what it wrote with what the page
// wrote. ICLFORGE_GUI_TEST_CLI is the build's own forge, or empty where the
// build has none (apps/gui/tests/CMakeLists.txt); then available() is false
// and a suite skips.
class CliRunner : public QObject {
    Q_OBJECT

public:
    [[nodiscard]] Q_INVOKABLE bool available() const {
        return !program().isEmpty() && QFile::exists(program());
    }

    // Runs `line` ("forge <command> <args>", quoted as the command bar quotes
    // a path with a space) with `folder` as the working directory; the exit
    // code, or -1 where it did not start or finish within two minutes.
    [[nodiscard]] Q_INVOKABLE int run(const QString& line, const QUrl& folder) {
        QStringList args = QProcess::splitCommand(line);
        if (!available() || args.isEmpty() || args.front() != QStringLiteral("forge")) {
            return -1;
        }
        args.removeFirst();
        QProcess process;
        process.setWorkingDirectory(folder.toLocalFile());
        process.setProcessChannelMode(QProcess::ForwardedChannels);
        process.start(program(), args);
        if (!process.waitForFinished(120000) || process.exitStatus() != QProcess::NormalExit) {
            return -1;
        }
        return process.exitCode();
    }

    // A fresh, empty folder at `folder`, and `source` copied into it under its
    // own name - the working directory an echoed line names its files in.
    [[nodiscard]] Q_INVOKABLE bool prepare(const QUrl& folder, const QUrl& source) {
        QDir dir(folder.toLocalFile());
        if (dir.exists() && !dir.removeRecursively()) {
            return false;
        }
        if (!QDir().mkpath(dir.path())) {
            return false;
        }
        const QString from = source.toLocalFile();
        return QFile::copy(from, dir.filePath(QFileInfo(from).fileName()));
    }

    // `file` copied into `folder`, which prepare() made, under its own name: a
    // further input an echoed line names beside the first (a second source, the
    // scene file an AC-4 object encode writes).
    [[nodiscard]] Q_INVOKABLE bool copyInto(const QUrl& folder, const QUrl& file) {
        const QString from = file.toLocalFile();
        const QString to = QDir(folder.toLocalFile()).filePath(QFileInfo(from).fileName());
        QFile::remove(to);
        return QFile::copy(from, to);
    }

    // Whether the two files hold the same bytes, both present and non-empty.
    [[nodiscard]] Q_INVOKABLE bool sameBytes(const QUrl& a, const QUrl& b) const {
        QFile first(a.toLocalFile());
        QFile second(b.toLocalFile());
        if (!first.open(QIODevice::ReadOnly) || !second.open(QIODevice::ReadOnly)) {
            return false;
        }
        const QByteArray left = first.readAll();
        return !left.isEmpty() && left == second.readAll();
    }

    [[nodiscard]] Q_INVOKABLE qint64 size(const QUrl& file) const {
        return QFileInfo(file.toLocalFile()).size();
    }

private:
    [[nodiscard]] static QString program() { return QStringLiteral(ICLFORGE_GUI_TEST_CLI); }
};

// The setup object exists for one reason: Main.qml's QML Settings must be
// HERMETIC here. With no organization/application identifiers, Qt 6.8's
// Settings failed to initialise and every window saw in-memory defaults -
// accidental but perfect isolation. Qt 6.10's fallback store PERSISTS
// instead, across windows and across runs, so every freshly created test
// window restored the previous window's saved session on top of the one
// live controller they all share - one duplicated source per test, and
// assignments reappearing from runs that finished days earlier. Real
// identifiers plus a QTemporaryDir settings path make the store behave
// normally and evaporate with the process; session restore itself is
// seeded OFF because restoring is a fresh-process feature no test wants
// firing under a shared controller.
class SettingsIsolation : public QObject {
    Q_OBJECT

public slots:
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("iclforge-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("forge_gui_qmltests"));

        // main.cpp forces Fusion before its engine loads any QML - see that
        // file's own comment: it renders identically on every platform,
        // where the native style would restyle controls at runtime. This
        // binary never did, which matters for more than looks: without it,
        // Qt Quick Controls resolves to the platform's native style, whose
        // native-theme queries are the documented cause of a real,
        // reproducible hang under the offscreen QPA platform (see
        // apps/gui/qml/Main.qml's "native Button inside a Repeater fed real
        // data" comment for the first occurrence, on Windows). That
        // occurrence was worked around locally in QML; a second one surfaced
        // here on macOS - not in the Repeater that fix already covers, but
        // in addDeviceBox (Main.qml), a plain native ComboBox populated from
        // EncoderController.captureDevices once a live capture session with
        // a real device selects it. Matching main.cpp's style here removes
        // the native-theme code path this binary was the only place still
        // exercising, rather than chasing each control it happens to affect
        // one at a time - and it is also more correct on its own terms: the
        // QML under test customizes several controls' contentItem (e.g.
        // Main.qml:1584, the exact line the "current style does not support
        // customization" QWARN below names), which only works under a
        // non-native style - Fusion, same as the shipped app - so this
        // binary had been exercising a style forge-gui never actually ships
        // with.
        QQuickStyle::setStyle(QStringLiteral("Fusion"));

        scratch_.emplace();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch_->path());

        QSettings settings;
        settings.beginGroup(QStringLiteral("workbench"));
        settings.setValue(QStringLiteral("restoreSession"), false);
    }

    // Quick Test's real per-suite hook (confirmed against CountdownSolver's
    // own tests/qml/tst_qml.cpp, which uses the identical mechanism): fires
    // once per tst_*.qml file, after applicationAvailable() above has already
    // pointed QSettings at this process's own scratch directory, so
    // LanguageManager::applyInitialLanguage() reads that isolated store
    // rather than a developer's real settings. Registers the exact
    // "languageManager" context property main.cpp installs for the real
    // app, so every suite can drive Preferences' language picker and
    // ICLFORGE_GUI_LOCALE-forced suites (tst_localisation_pipeline.qml) see the
    // same object the shipped app does.
    void qmlEngineAvailable(QQmlEngine* engine) {
        // Constructed lazily rather than as a plain member: LanguageManager
        // holds reference members (to the application and the engine),
        // neither of which exists yet when SettingsIsolation itself is
        // constructed.
        language_manager_.emplace(*qGuiApp, *engine);
        language_manager_->applyInitialLanguage();
        engine->rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                                   &*language_manager_);
        // Same context property, same value, main.cpp installs: without it
        // AboutDialog's version line is a ReferenceError here and the About
        // dialog could only ever be tested showing nothing.
        engine->rootContext()->setContextProperty(
            QStringLiteral("appVersionDetails"),
            QString::fromStdString(iclforge::ac3::version_details()));
        engine->rootContext()->setContextProperty(QStringLiteral("cliRunner"), &cli_runner_);
    }

private:
    std::optional<QTemporaryDir> scratch_;
    std::optional<LanguageManager> language_manager_;
    CliRunner cli_runner_;
};

QUICK_TEST_MAIN_WITH_SETUP(forge-gui, SettingsIsolation)

#include "qml_test_main.moc"
