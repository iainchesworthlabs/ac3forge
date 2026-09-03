// ac3desk: the Desktop Atmos Demo's window (docs/platforms/windows-demo.md,
// "UI"). Everything that is not the window lives in ../engine; this file
// only stands the QML up, applies the language, and offers one debugging
// aid: `--shot <path.png>` grabs the window after it has settled and quits,
// the way ac3gui's smoke modes do, so a headless check can see the screen;
// `--page settings` (or output, room) picks the page it shows first, and
// `--place Name=x,y,z` positions a listed application before the capture.

#include <QGuiApplication>
#include <QIcon>
#include <QVariant>
#include <QVariantMap>

#include "desk_controller.hpp"
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QIcon>
#include <QTimer>

#include "app_entry.hpp"
#include "app_icon_provider.hpp"

#include <memory>
#include <QUrl>

#include "language_manager.hpp"

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Desktop Atmos"));
    QGuiApplication::setOrganizationName(QStringLiteral("ac3forge"));
    // The window and taskbar icon; the .exe's own icon comes from the
    // resource script CMake generates.
    QIcon app_icon;
    app_icon.addFile(QStringLiteral(":/icons/ac3forge-32.png"));
    app_icon.addFile(QStringLiteral(":/icons/ac3forge-256.png"));
    QGuiApplication::setWindowIcon(app_icon);
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Desktop Atmos"));
    // Every control is drawn by the QML in this module, on the Theme's
    // tokens; the Basic style is the one that gets out of the way.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    // The tray keeps the engine alive after the window closes, so the app
    // must not quit with its last window.
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QString shot_path;
    QString page;
    QStringList placements;
    const QStringList args = QCoreApplication::arguments();
    for (qsizetype i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == QLatin1String("--shot")) {
            shot_path = args[i + 1];
        } else if (args[i] == QLatin1String("--page")) {
            page = args[i + 1];  // room, output or settings
        } else if (args[i] == QLatin1String("--place")) {
            placements.push_back(args[i + 1]);  // name=x,y,z, applied before the shot
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
    engine.addImageProvider(QStringLiteral("appicon"), new ac3::desk::AppIconProvider);
    engine.loadFromModule("Ac3ForgeDesk", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    if (page == QLatin1String("room3d")) {  // the room with its 3D view up
        engine.rootObjects().first()->setProperty("roomThreeD", true);
        page = QStringLiteral("room");
    }
    if (!page.isEmpty()) {
        engine.rootObjects().first()->setProperty("page", page);
    }

    // `--place Name=x,y,z`, once the engine has listed the sessions: the
    // application whose display name matches is positioned there, so a
    // capture can show a populated room without a hand on the mouse. The
    // list arrives when the engine's first refresh does, which is not at a
    // fixed time, so this polls until every named application has been
    // placed (or gives up after six seconds) rather than guessing once.
    auto* poll = new QTimer(&app);
    auto pending = std::make_shared<QStringList>(placements);
    auto tries = std::make_shared<int>(0);
    auto take_shot = [&engine, shot_path] {
        if (shot_path.isEmpty()) {
            return;
        }
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        int code = 1;
        if (window != nullptr) {
            qInfo("--shot: page %s", qPrintable(window->property("page").toString()));
            code = window->grabWindow().save(shot_path) ? 0 : 2;
        }
        QCoreApplication::exit(code);
    };
    QObject::connect(poll, &QTimer::timeout, &app, [&engine, &app, poll, pending, tries, take_shot, shot_path] {
        auto* controller = engine.singletonInstance<DeskController*>("Ac3ForgeDesk", "DeskController");
        if (controller != nullptr) {
            for (auto it = pending->begin(); it != pending->end();) {
                const QString& spec = *it;
                const auto eq = spec.indexOf(QLatin1Char('='));
                // A trailing ",split" asks for the pair.
                const bool split = spec.endsWith(QLatin1String(",split"), Qt::CaseInsensitive);
                QString coords = spec.mid(eq + 1);
                if (split) {
                    coords.chop(6);
                }
                const QStringList xyz = coords.split(QLatin1Char(','));
                bool done = eq < 0 || xyz.size() != 3;  // malformed: drop it
                if (!done) {
                    for (QObject* object : controller->apps()) {
                        auto* entry = qobject_cast<ac3::desk::AppEntry*>(object);
                        if (entry != nullptr && entry->name().compare(spec.left(eq), Qt::CaseInsensitive) == 0) {
                            const int app = entry->app();
                            if (split) {
                                controller->setSplit(app, true);
                            }
                            controller->position(app, xyz[0].toDouble(), xyz[1].toDouble(), xyz[2].toDouble());
                            qInfo("--place: %s -> app %d at (%s)", qPrintable(spec.left(eq)), app, qPrintable(coords));
                            done = true;
                        }
                    }
                }
                it = done ? pending->erase(it) : it + 1;
            }
        }
        ++*tries;
        if (pending->isEmpty() || *tries >= 24) {
            for (const QString& spec : *pending) {
                qWarning("--place: no application named %s after %d polls", qPrintable(spec.left(spec.indexOf(QLatin1Char('=')))), *tries);
            }
            poll->stop();
            // Let the placement glide and the views settle before the grab.
            QTimer::singleShot(shot_path.isEmpty() ? 0 : 1500, &app, take_shot);
        }
    });
    if (!placements.isEmpty() || !shot_path.isEmpty()) {
        poll->start(250);
    }

    return QGuiApplication::exec();
}
