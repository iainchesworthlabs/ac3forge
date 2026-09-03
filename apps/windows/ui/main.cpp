// ac3desk: the Desktop Atmos Demo's window (docs/platforms/windows-demo.md,
// "UI"). Everything that is not the window lives in ../engine; this file
// only stands the QML up, applies the language, and offers one debugging
// aid: `--shot <path.png>` grabs the window after it has settled and quits,
// the way ac3gui's smoke modes do, so a headless check can see the screen.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QTimer>
#include <QUrl>

#include "language_manager.hpp"

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Desktop Atmos"));
    QGuiApplication::setOrganizationName(QStringLiteral("ac3forge"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Desktop Atmos"));
    // Every control is drawn by the QML in this module, on the Theme's
    // tokens; the Basic style is the one that gets out of the way.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    // The tray keeps the engine alive after the window closes, so the app
    // must not quit with its last window.
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QString shot_path;
    const QStringList args = QCoreApplication::arguments();
    for (qsizetype i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == QLatin1String("--shot")) {
            shot_path = args[i + 1];
        }
    }

    QQmlApplicationEngine engine;
    // The GUI's own language manager, pointed at this app's translation
    // files: system locale by default, a saved override when the user chose
    // one (docs/gui/localisation.md).
    LanguageManager language_manager(app, engine, QStringLiteral("ac3desk"));
    language_manager.applyInitialLanguage();
    // A singleton instance rather than a context property: QML compiled
    // ahead of time resolves a registered type, where an unqualified
    // context name came back null at first evaluation. Under its own URI,
    // not the module's: registering a type into Ac3ForgeDesk by hand marks
    // that module as registered and the module's own types (DeskController)
    // then never get registered at load.
    qmlRegisterSingletonInstance("Ac3ForgeDeskLanguage", 1, 0, "LanguageManager",
                                 &language_manager);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Ac3ForgeDesk", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    if (!shot_path.isEmpty()) {
        QTimer::singleShot(3000, &app, [&engine, shot_path] {
            auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            int code = 1;
            if (window != nullptr) {
                code = window->grabWindow().save(shot_path) ? 0 : 2;
            }
            QCoreApplication::exit(code);
        });
    }
    return QGuiApplication::exec();
}
