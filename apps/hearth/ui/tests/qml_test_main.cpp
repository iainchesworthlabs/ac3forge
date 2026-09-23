#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QSettings>
#include <QTemporaryDir>

#include <memory>
#include <optional>

#include "language_manager.hpp"

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
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        // Same URI and reasoning as main.cpp's own registration: its own,
        // not the module's, so registering a type by hand cannot mark
        // Ac3ForgeHearth registered and stop HearthController registering.
        language_manager_ = std::make_unique<LanguageManager>(
            *qGuiApp, *engine, QStringLiteral("ac3hearth"));
        qmlRegisterSingletonInstance("Ac3ForgeHearthLanguage", 1, 0, "LanguageManager",
                                     language_manager_.get());
    }

private:
    std::optional<QTemporaryDir> scratch_;
    std::unique_ptr<LanguageManager> language_manager_;
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(ac3hearth, HearthTestIsolation)

#include "qml_test_main.moc"
