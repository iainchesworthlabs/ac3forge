// ac3crucible: the AC3Forge Crucible's window (docs/platforms/windows-demo.md,
// "UI"). Everything that is not the window lives in ../engine; this file
// only stands the QML up, applies the language, and offers one debugging
// aid: `--shot <path.png>` grabs the window after it has settled and quits,
// the way ac3gui's smoke modes do, so a headless check can see the screen;
// `--page settings` (or output, room, room3d, about, firstrun) picks the
// page it shows first, and `--place Name=x,y,z` positions a listed
// application before the capture. A `--shot` run never shows the first-run
// dialog unless `--page firstrun` asked for it, so a capture against a
// fresh settings store is clean.

#include <QFont>
#include <QFontDatabase>
#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QMessageLogContext>
#include <QVariant>
#include <QVariantMap>

#include "crucible_controller.hpp"
#include "diagnostics.hpp"
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QString>
#include <QIcon>
#include <QTimer>

#include "app_entry.hpp"
#include "app_icon_provider.hpp"

#include <memory>
#include <QUrl>

#include "language_manager.hpp"

namespace {

// The demo stored its settings under ac3forge/DesktopAtmos; the product
// stores them under ac3forge/Crucible (roadmap UX12, Phase 1). Copy the old
// tree across the first time the new one is empty, so a machine that ran the
// demo keeps its signing-key path, endpoint choice and appearance. The old
// tree is left where it is rather than deleted: nothing here is large enough
// to be worth removing, and a person who goes back to the demo build should
// still find their settings.
//
// Both use the four-argument constructor for the reason CrucibleController
// does: the two-argument one always takes the native store whatever
// QSettings::setDefaultFormat says, which would make this read and write the
// developer's real registry from a test process. With the format honoured,
// the QML tests' INI-in-a-temporary-directory isolation holds and this is a
// no-op there. Runs before anything constructs a controller.
void migrate_demo_settings() {
    QSettings current(QSettings::defaultFormat(), QSettings::UserScope,
                      QStringLiteral("ac3forge"), QStringLiteral("Crucible"));
    if (!current.allKeys().isEmpty()) {
        return;
    }
    QSettings previous(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("ac3forge"), QStringLiteral("DesktopAtmos"));
    const auto keys = previous.allKeys();
    if (keys.isEmpty()) {
        return;
    }
    for (const auto& key : keys) {
        current.setValue(key, previous.value(key));
    }
    // A marker that the copy happened, for the first-run dialog's one
    // sentence that says so. The dialog's own acknowledgement cannot have
    // been copied: the demo never wrote one, so a migrated machine sees the
    // explanation once too.
    current.setValue(QStringLiteral("migration/fromDesktopAtmos"), true);
    current.sync();
}

// Qt's own messages - this file's --shot and --place lines, a QML warning,
// a font that failed to register - into the diagnostics ring
// (engine/diagnostics.hpp), so a saved report carries them: on a
// WIN32_EXECUTABLE they otherwise reach only an attached debugger. Debug
// output is left out; it is Qt's own chatter and the ring holds 512 lines.
// The handler runs on whatever thread emitted the message, possibly during
// teardown, so it converts the string, hands it to note() (which takes the
// ring's lock and returns) and chains to the handler that was there before;
// nothing in it may itself log, since that would recurse.
QtMessageHandler g_previous_handler = nullptr;

void forward_to_diagnostics(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (type != QtDebugMsg) {
        const char* level = type == QtInfoMsg       ? "info: "
                            : type == QtWarningMsg  ? "warning: "
                            : type == QtCriticalMsg ? "critical: "
                                                    : "fatal: ";
        ac3::crucible::process_diagnostics().note(level + message.toStdString());
    }
    if (g_previous_handler != nullptr) {
        g_previous_handler(type, context, message);
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Render on the GUI thread. With the threaded loop the window's frames
    // are produced on a render thread that paints while Windows moves the
    // window, so a drag translates the frame a step behind the cursor and
    // the motion judders. The basic loop renders in step with the event
    // loop, which Windows' move loop drives, so the window follows the
    // cursor smoothly. A person can still choose another loop with the
    // environment variable.
    if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP")) {
        qputenv("QSG_RENDER_LOOP", QByteArrayLiteral("basic"));
    }
    // QApplication, not QGuiApplication: the tray icon (Qt.labs.platform,
    // Main.qml) is QSystemTrayIcon underneath, a Widgets class. Windows
    // tolerated the narrower application object because its native menu
    // path needs no widgets; Linux refused it and the tray was absent.
    QApplication app(argc, argv);
    // The ring exists from here, so its clock starts with the window and
    // every later message lands in it.
    ac3::crucible::process_diagnostics().note("ac3crucible started");
    g_previous_handler = qInstallMessageHandler(forward_to_diagnostics);
    QGuiApplication::setApplicationName(QStringLiteral("Crucible"));
    QGuiApplication::setOrganizationName(QStringLiteral("ac3forge"));
    migrate_demo_settings();
    // The window and taskbar icon; the .exe's own icon comes from the
    // resource script CMake generates.
    QIcon app_icon;
    app_icon.addFile(QStringLiteral(":/icons/ac3forge-32.png"));
    app_icon.addFile(QStringLiteral(":/icons/ac3forge-256.png"));
    QGuiApplication::setWindowIcon(app_icon);
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Crucible"));
    // The GUI app's faces (Archivo, and Noto Sans for the scripts it does
    // not cover), registered before the engine loads so the Theme's
    // font probe finds them, Archivo made the application default so every
    // control uses it; the language manager swaps the family for Arabic,
    // Hebrew and Yiddish.
    for (const auto* face : {":/fonts/Archivo-Regular.ttf", ":/fonts/Archivo-Medium.ttf",
                             ":/fonts/Archivo-SemiBold.ttf", ":/fonts/Archivo-ExtraBold.ttf",
                             ":/fonts/NotoSansArabic.ttf", ":/fonts/NotoSansHebrew.ttf"}) {
        if (QFontDatabase::addApplicationFont(QLatin1String(face)) < 0) {
            qWarning("could not register bundled font %s", face);
        }
    }
    QFont default_font = QGuiApplication::font();
    default_font.setFamily(QStringLiteral("Archivo"));
    QGuiApplication::setFont(default_font);
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
    LanguageManager language_manager(app, engine, QStringLiteral("ac3crucible"));
    language_manager.applyInitialLanguage();
    // A singleton instance rather than a context property: QML compiled
    // ahead of time resolves a registered type, where an unqualified
    // context name came back null at first evaluation. Under its own URI,
    // not the module's: registering a type into Ac3ForgeCrucible by hand marks
    // that module as registered and the module's own types (CrucibleController)
    // then never get registered at load.
    qmlRegisterSingletonInstance("Ac3ForgeCrucibleLanguage", 1, 0, "LanguageManager",
                                 &language_manager);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.addImageProvider(QStringLiteral("appicon"), new ac3::crucible::ui::AppIconProvider);
    engine.loadFromModule("Ac3ForgeCrucible", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    // A capture never shows the first-run dialog it did not ask for:
    // Main.qml reads this one event-loop turn later, after main() has had
    // its say. `--page firstrun` opens the dialog over the Room page.
    if (!shot_path.isEmpty()) {
        engine.rootObjects().first()->setProperty("suppressFirstRun", true);
    }
    if (page == QLatin1String("firstrun")) {
        engine.rootObjects().first()->setProperty("page", QStringLiteral("room"));
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openFirstRun");
        page.clear();
    }
    if (page == QLatin1String("room3d")) {  // the room with its 3D view up
        engine.rootObjects().first()->setProperty("roomThreeD", true);
        page = QStringLiteral("room");
    }
    // `--page about` opens the About box over the Room page, for a capture.
    if (page == QLatin1String("about")) {
        engine.rootObjects().first()->setProperty("page", QStringLiteral("room"));
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openAbout");
    } else if (!page.isEmpty()) {
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
            if (auto* loader = window->findChild<QObject*>(QStringLiteral("room3dLoader"))) {
                if (auto* room = loader->property("item").value<QObject*>()) {
                    QMetaObject::invokeMethod(room, "debugPick");
                }
            }
            code = window->grabWindow().save(shot_path) ? 0 : 2;
        }
        QCoreApplication::exit(code);
    };
    QObject::connect(poll, &QTimer::timeout, &app, [&engine, &app, poll, pending, tries, take_shot, shot_path] {
        auto* controller = engine.singletonInstance<CrucibleController*>("Ac3ForgeCrucible", "CrucibleController");
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
                        auto* entry = qobject_cast<ac3::crucible::ui::AppEntry*>(object);
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
