#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QSettings>
#include <QTemporaryDir>

#include <optional>

#include "../../../gui/language_manager.hpp"

// Qt Quick Test entry point for the Desktop Atmos Demo's window: runs every
// tst_*.qml under QUICK_TEST_SOURCE_DIR against the REAL DeskController the
// embedded Ac3ForgeDesk module registers - the same rule apps/gui/tests
// follows, and for the same reason: a parallel fake API is a second thing
// the real one can silently disagree with. The controller starts the
// engine only when a test (or Main.qml) calls start(), so suites that never
// do are hardware-free; the ones that do skip when start() refuses, which
// is what happens on a machine with no audio endpoint.
//
// The setup object mirrors the GUI harness's SettingsIsolation: real
// organisation and application names plus a QTemporaryDir settings path,
// so DeskController's QSettings (organisation "ac3forge", application
// "DesktopAtmos", the shipped app's own) read and write a store that is
// empty at start and gone at exit rather than the developer's own. The
// Basic style is what ui/main.cpp sets; the offscreen platform (set on the
// ctest entries) has no native theme to consult, and the QML customises
// contentItems that a native style would refuse.
class DeskIsolation : public QObject {
    Q_OBJECT

public slots:
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("ac3forge"));
        QCoreApplication::setApplicationName(QStringLiteral("DesktopAtmos"));
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        scratch_.emplace();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch_->path());
    }

    // Once per tst_*.qml: the same LanguageManager singleton ui/main.cpp
    // registers, under the same URI, pointed at this app's translations.
    void qmlEngineAvailable(QQmlEngine* engine) {
        language_manager_.emplace(*qGuiApp, *engine, QStringLiteral("ac3desk"));
        language_manager_->applyInitialLanguage();
        qmlRegisterSingletonInstance("Ac3ForgeDeskLanguage", 1, 0, "LanguageManager", &*language_manager_);
    }

private:
    std::optional<QTemporaryDir> scratch_;
    std::optional<LanguageManager> language_manager_;
};

QUICK_TEST_MAIN_WITH_SETUP(ac3desk, DeskIsolation)

#include "qml_test_main.moc"
