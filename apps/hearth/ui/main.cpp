// ac3hearth: the desktop reference player's window
// (planning/hearth-reference-player.md, A5). Everything that is not the
// window lives in ../engine and hearth_controller.hpp; this file stands the
// QML up and offers Crucible's own debugging aid: `--shot <path.png>` grabs
// the window after it has settled and quits, so a headless check (or the
// screenshot script, later) can see it; `--page <name>` picks the page it
// opens on first - play, media, speakers, decoder, network or settings, or
// opens the "Before you play anything" dialog (firstrun), the
// keyboard-shortcuts reference (shortcuts, issue #830), the About dialog
// (about) or its Licences view (licences) over the Play page, the same
// special values apps/crucible/ui/main.cpp's own `--page` accepts. A
// `--shot` run never shows the first-run dialog unless `--page firstrun`
// asked for it, so a capture against a fresh settings store is clean
// (Crucible's own main.cpp carries the identical shape for the identical
// reason). `--open-output-picker` opens the output picker dialog
// (OutputPicker.qml) before the grab, since nothing else drives its mouse
// click headlessly. `--decoder-format ac4` switches the Decoder page's own
// AC-3/E-AC-3 vs AC-4 sub-tab (DecoderPage.qml's `format` property) before
// the grab - otherwise nothing reaches that sub-tab headlessly either
// (issue #901).
//
// Translations are not wired up yet: this slice is the shell and the Play
// page over the real engine, with the other five pages as placeholders, and
// follows once there is more of the window for it to cover.

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

namespace {

bool save_window(QQmlApplicationEngine& engine, const QString& path) {
    if (engine.rootObjects().isEmpty()) {
        return false;
    }
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (window == nullptr) {
        return false;
    }
    const QImage shot = window->grabWindow();
    return !shot.isNull() && shot.save(path);
}

}  // namespace

int main(int argc, char** argv) {
    // Render on the GUI thread, as Crucible's window does and for the same
    // reason: the threaded loop's render thread paints a frame behind a
    // window drag on Windows.
    if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP")) {
        qputenv("QSG_RENDER_LOOP", QByteArrayLiteral("basic"));
    }
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Hearth"));
    QGuiApplication::setOrganizationName(QStringLiteral("ac3forge"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Hearth"));

    QIcon app_icon;
    app_icon.addFile(QStringLiteral(":/icons/ac3forge-32.png"));
    app_icon.addFile(QStringLiteral(":/icons/ac3forge-256.png"));
    QGuiApplication::setWindowIcon(app_icon);

    // The family's own faces (apps/gui/fonts), registered before the engine
    // loads so the Theme's font probe finds them.
    for (const auto* face : {":/fonts/Archivo-Regular.ttf", ":/fonts/Archivo-Medium.ttf",
                             ":/fonts/Archivo-SemiBold.ttf", ":/fonts/Archivo-ExtraBold.ttf"}) {
        if (QFontDatabase::addApplicationFont(QLatin1String(face)) < 0) {
            qWarning("could not register bundled font %s", face);
        }
    }
    QFont default_font = QGuiApplication::font();
    default_font.setFamily(QStringLiteral("Archivo"));
    QGuiApplication::setFont(default_font);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QString shot_path;
    QString page;
    QString decoder_format;
    const QStringList args = QCoreApplication::arguments();
    // A bare flag, not a "--name value" pair: checked separately so it can
    // be the last argument with nothing following it.
    const bool open_output_picker = args.contains(QLatin1String("--open-output-picker"));
    for (qsizetype i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == QLatin1String("--shot")) {
            shot_path = args[i + 1];
        } else if (args[i] == QLatin1String("--page")) {
            // play, media, speakers, decoder, network, settings, firstrun,
            // shortcuts, about or licences
            page = args[i + 1];
        } else if (args[i] == QLatin1String("--decoder-format")) {
            // "eac3" or "ac4" - DecoderPage.qml's own `format` values.
            decoder_format = args[i + 1];
        }
    }

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Ac3ForgeHearth", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    // A capture never shows the first-run dialog it did not ask for:
    // Main.qml reads this one event-loop turn later, after main() has had
    // its say. `--page firstrun`/`shortcuts`/`about`/`licences` open their
    // dialog over the Play page, for a capture.
    if (!shot_path.isEmpty()) {
        engine.rootObjects().first()->setProperty("suppressFirstRun", true);
    }
    if (page == QLatin1String("firstrun")) {
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openFirstRun");
    } else if (page == QLatin1String("shortcuts")) {
        engine.rootObjects().first()->setProperty("page", QStringLiteral("play"));
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openShortcuts");
    } else if (page == QLatin1String("about")) {
        engine.rootObjects().first()->setProperty("page", QStringLiteral("play"));
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openAbout");
    } else if (page == QLatin1String("licences")) {
        engine.rootObjects().first()->setProperty("page", QStringLiteral("play"));
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openLicences");
    } else if (!page.isEmpty()) {
        engine.rootObjects().first()->setProperty("page", page);
    }
    if (!decoder_format.isEmpty()) {
        engine.rootObjects().first()->setProperty("decoderFormat", decoder_format);
    }
    if (open_output_picker) {
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openOutputPicker");
    }

    if (!shot_path.isEmpty()) {
        // Let the page switch and the layout settle before the grab.
        QTimer::singleShot(500, &app, [&engine, shot_path] {
            const int code = save_window(engine, shot_path) ? 0 : 2;
            QCoreApplication::exit(code);
        });
    }

    return QGuiApplication::exec();
}
