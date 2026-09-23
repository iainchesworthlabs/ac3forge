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
// asked for it (Crucible's own main.cpp carries the identical shape for the
// identical reason), and now also runs HearthController against a scratch
// settings store instead of the real per-user one (registry key
// HKCU\Software\ac3forge\Hearth on Windows) - apps/gui/main.cpp's `--smoke`
// uses the identical recipe - so repeated captures on a shared machine
// neither inherit nor pollute anyone's real queue/device/pairing state
// (issue #884). `--open-output-picker` opens the output picker dialog
// (OutputPicker.qml) before the grab, since nothing else drives its mouse
// click headlessly. `--decoder-format ac4` switches the Decoder page's own
// AC-3/E-AC-3 vs AC-4 sub-tab (DecoderPage.qml's `format` property) before
// the grab - otherwise nothing reaches that sub-tab headlessly either
// (issue #901).
//
// Translations run through the family's own LanguageManager
// (apps/gui/language_manager.cpp, shared rather than copied), pointed at
// this app's own ac3hearth_<code>.qm catalogues under :/i18n/. The six
// languages are the same set ac3gui and Crucible ship. The catalogues carry
// every source string and no translations yet, so what a language change
// visibly does today is switch the layout direction and the typeface;
// filling them is a translator's task, not a build one.

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include "ac3/internal/profiling.hpp"

#include <optional>

#include "ac3/sendspin/firewall.hpp"
#include "language_manager.hpp"

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

// Ties Tracy's frame view to real Qt Quick presentation instead of leaving it
// empty - see apps/crucible/ui/main.cpp's identical helper for why this is a
// NAMED ("UI") frame mark rather than the bare AC3_FRAME_MARK(), and why the
// connection is direct rather than queued.
void mark_frames_for_tracy(QQuickWindow* window) {
    QObject::connect(window, &QQuickWindow::frameSwapped, window,
                     [] { AC3_FRAME_MARK_NAMED("UI"); }, Qt::DirectConnection);
}

}  // namespace

int main(int argc, char** argv) {
    // Elevated relaunch for a Windows Firewall rule NetworkController's mDNS browsing is about
    // to need (ac3/sendspin/firewall.hpp): std::exit()s before touching Qt when argv says this
    // is that relaunch, so an ordinary launch is the only one that reaches the window below.
    ac3::sendspin::firewall::maybe_run_as_firewall_helper_and_exit(argc, argv);

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
                             ":/fonts/Archivo-SemiBold.ttf", ":/fonts/Archivo-ExtraBold.ttf",
                             ":/fonts/MaterialSymbolsSharp-Regular.ttf",
                             ":/fonts/NotoSansArabic.ttf",
                             ":/fonts/NotoSansHebrew.ttf"}) {
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

    // A --shot capture must not touch the real, persistent per-user settings
    // store (issue #884): HearthController builds settings_ from
    // QSettings::defaultFormat() rather than a fixed native format
    // (hearth_controller.hpp's own comment explains why), so overriding the
    // process-wide default here - before the engine, and so
    // HearthController's QML singleton, exists - reaches it with no change
    // to HearthController itself, the same recipe apps/gui/main.cpp's
    // --smoke already uses. shot_settings_scratch has to outlive the run, so
    // it is kept in scope here rather than left as a temporary whose
    // directory would vanish immediately.
    std::optional<QTemporaryDir> shot_settings_scratch;
    if (!shot_path.isEmpty()) {
        shot_settings_scratch.emplace();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, shot_settings_scratch->path());
    }

    QQmlApplicationEngine engine;
    // The family's own language manager, pointed at this app's catalogues:
    // the system locale by default, a saved override once the person has
    // chosen one (docs/forge/gui/localisation.md). Constructed and applied
    // BEFORE the QML loads, so the first frame is already translated and
    // already mirrored where the language is written right to left.
    LanguageManager language_manager(app, engine, QStringLiteral("ac3hearth"));
    language_manager.applyInitialLanguage();
    // A singleton instance rather than a context property, and under its own
    // URI rather than this module's: registering a type into Ac3ForgeHearth
    // by hand marks that module registered, and its own types
    // (HearthController, NetworkController) then never register at load.
    // apps/crucible/ui/main.cpp carries the identical comment for the
    // identical reason.
    qmlRegisterSingletonInstance("Ac3ForgeHearthLanguage", 1, 0, "LanguageManager",
                                 &language_manager);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Ac3ForgeHearth", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    if (auto* root_window = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) {
        mark_frames_for_tracy(root_window);
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
