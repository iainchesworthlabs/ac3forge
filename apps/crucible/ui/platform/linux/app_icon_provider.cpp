#include "app_icon_provider.hpp"

#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QLatin1Char>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPixmap>
#include <QSemaphore>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "desktop_entries.hpp"

// The Linux AppIconProvider: an application's icon from the freedesktop
// icon theme and its .desktop entry (docs/crucible/promotion.md, Phase 4).
//
// Linux has no single call for "the icon this executable has", so the
// answer is assembled from what the platform does give, in four rungs, the
// first that yields a picture winning:
//
//   1. application.icon-name, which the application set on its own PipeWire
//      client and the session monitor read off the stream's info - a theme
//      icon name, resolved by QIcon::fromTheme.
//   2. A .desktop entry, matched in order by application id (a Flatpak's
//      portal app id is its .desktop file id), TryExec, Exec, StartupWMClass
//      against the name or the binary, and Name; its Icon= is a theme name
//      or an absolute path (ui/desktop_entries.hpp has the rules).
//   3. The theme by the binary's own name: firefox, mpv, vlc and most
//      packaged applications name their icon after their executable.
//   4. Nothing: a null image, and the monogram stays. A script or an
//      interpreter (python3, sh) lands here, or on the interpreter's icon
//      if the theme has one, and the monogram is the right answer for it.
//
// Where it runs. The constructor, on the GUI thread, seeds the icon theme
// (below). Every request arrives on Qt Quick's pixmap-reader thread, because
// the provider asks for ForceAsynchronousImageLoading; the id is parsed and
// the .desktop index built and searched there, and none of that touches
// Qt's GUI state. The theme lookups themselves - QIcon::fromTheme and the
// pixmap it yields - run on the GUI thread, one queued call per new
// identity, which the reader thread waits on. QIconLoader is one process-
// wide set of caches with no lock, and the GUI thread uses it too (a file
// dialog's icons, a widget style's standard icons), so two threads in it at
// once would be a race; it is only ever entered from the thread that owns
// it. The wait is bounded rather than a blocking connection: the reader
// thread's own destructor, at engine teardown, runs on the GUI thread and
// waits for the reader, and a reader blocked on that same GUI thread would
// never return. On a timeout the answer is null and is not cached, so the
// next request asks again. The frame thread never touches any of it.
//
// The offscreen platform - the Qt Quick test suite, and `--shot` - has no
// icon theme: its platform theme is a bare QPlatformTheme whose hints are
// the base class's empty defaults, so QIcon::fromTheme finds nothing whatever
// XDG_DATA_DIRS says. seed_icon_theme() fills the search paths from the same
// variables a desktop theme reads and falls back to hicolor, which is also
// what lets the test suite point XDG_DATA_DIRS at a fixture theme.

namespace ac3::crucible::ui {

namespace {

// Off by default. QT_LOGGING_RULES="ac3crucible.icons.debug=true" prints
// the theme the provider seeded and the rung each application resolved on,
// which is what the Pi record quotes.
Q_LOGGING_CATEGORY(lcIcons, "ac3crucible.icons", QtWarningMsg)

// How long the reader thread waits for the GUI thread's answer (the file
// header). A GUI thread that has not served an event in this long is
// shutting down or stuck, and a monogram meanwhile costs nothing.
constexpr int kGuiThreadWaitMs = 2000;

// The .desktop index, built on first use and shared by every request, and
// the mutex that serialises the index build against the lookups.
struct Resolver {
    QMutex mutex;
    std::vector<DesktopEntry> entries;
    bool loaded = false;
    std::chrono::steady_clock::time_point loaded_at;
};

Resolver& resolver() {
    static Resolver instance;
    return instance;
}

std::vector<std::filesystem::path> data_dirs() {
    return xdg_data_dirs(std::getenv("XDG_DATA_HOME"), std::getenv("HOME"),
                         std::getenv("XDG_DATA_DIRS"));
}

// See the file header. A no-op, apart from an empty fallback theme, under
// a platform theme that already supplies the paths and a theme name.
void seed_icon_theme() {
    const QStringList existing = QIcon::themeSearchPaths();
    const bool bare = std::ranges::all_of(
        existing, [](const QString& entry) { return entry.startsWith(QLatin1Char(':')); });
    if (bare) {
        QStringList theme_dirs;
        QStringList pixmap_dirs;
        for (const auto& dir : data_dirs()) {
            const QString base = QString::fromStdString(dir.string());
            theme_dirs.append(base + QStringLiteral("/icons"));
            pixmap_dirs.append(base + QStringLiteral("/pixmaps"));
        }
        if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
            theme_dirs.append(QString::fromUtf8(home) + QStringLiteral("/.icons"));
        }
        theme_dirs.append(QStringLiteral(":/icons"));
        QIcon::setThemeSearchPaths(theme_dirs);
        QIcon::setFallbackSearchPaths(pixmap_dirs);
    }
    if (QIcon::themeName().isEmpty()) {
        QIcon::setThemeName(QStringLiteral("hicolor"));
    }
    if (QIcon::fallbackThemeName().isEmpty()) {
        QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
    }
    qCDebug(lcIcons) << "icon theme" << QIcon::themeName() << "fallback"
                     << QIcon::fallbackThemeName() << "search paths" << QIcon::themeSearchPaths()
                     << "pixmap paths" << QIcon::fallbackSearchPaths();
}

// A theme icon as an image no larger than px on a side. pixmap() hands
// back the closest theme entry not above the size - a 48-pixel-only icon
// comes back at 48 - and the caller scales it to the request, which is
// what the Windows shell icons get too. GUI thread only (the file header).
QImage theme_image(const QString& icon_name, int px) {
    if (icon_name.isEmpty()) {
        return {};
    }
    const QIcon icon = QIcon::fromTheme(icon_name);
    if (icon.isNull()) {
        return {};
    }
    return icon.pixmap(QSize(px, px)).toImage();
}

// An Icon= value as an image: an absolute path is read as a file, a name
// goes to the theme without any extension it carries. GUI thread only.
QImage desktop_image(const std::string& icon_value, int px) {
    if (icon_value.starts_with('/')) {
        return QImageReader(QString::fromStdString(icon_value)).read();
    }
    const std::string_view bare = icon_name_without_extension(icon_value);
    return theme_image(QString::fromUtf8(bare.data(), static_cast<qsizetype>(bare.size())), px);
}

QString basename_of(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    return QString::fromStdString(std::filesystem::path(path.toStdString()).filename().string());
}

// The .desktop rungs' candidate: the Icon= of the entry that fits, or
// nothing. The index is built on first use, and rebuilt once when a lookup
// misses on an index older than a minute, so an application installed
// mid-session gets its icon at the next miss without a timer. Reader
// thread, under the resolver mutex.
std::optional<std::string> desktop_icon(Resolver& r, const AppIdentity& who) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::steady_clock::now();
    if (!r.loaded) {
        r.entries = read_desktop_entries(data_dirs());
        r.loaded = true;
        r.loaded_at = now;
        qCDebug(lcIcons) << "desktop entries indexed:" << r.entries.size();
    }
    auto found = desktop_icon_for(r.entries, who);
    if (!found && now - r.loaded_at > 60s) {
        r.entries = read_desktop_entries(data_dirs());
        r.loaded_at = now;
        found = desktop_icon_for(r.entries, who);
    }
    return found;
}

// What one request resolves to, and the rung that answered.
struct Resolved {
    QImage image;
    const char* rung = "monogram";
};

// The three picture rungs, in order, from what the reader thread worked
// out. GUI thread only.
Resolved resolve_on_gui_thread(const QString& icon_name, const std::optional<std::string>& desktop,
                               const QString& binary, int px) {
    Resolved result;
    result.image = theme_image(icon_name, px);
    if (!result.image.isNull()) {
        result.rung = "icon-name";
        return result;
    }
    if (desktop) {
        result.image = desktop_image(*desktop, px);
        if (!result.image.isNull()) {
            result.rung = "desktop-entry";
            return result;
        }
    }
    result.image = theme_image(binary, px);
    if (!result.image.isNull()) {
        result.rung = "theme-by-binary";
    }
    return result;
}

// Runs resolve_on_gui_thread() where it must run and waits for the answer,
// or gives up after kGuiThreadWaitMs with `answered` false (the file header
// says why the wait is bounded). The posted call owns its inputs and the
// slot for its answer through a shared block, so a call that outlives this
// wait writes into memory nobody else reads any more.
Resolved on_gui_thread(QString icon_name, std::optional<std::string> desktop, QString binary,
                       int px, bool& answered) {
    if (qGuiApp == nullptr) {
        answered = false;
        return {};
    }
    if (QThread::currentThread() == qGuiApp->thread()) {
        answered = true;
        return resolve_on_gui_thread(icon_name, desktop, binary, px);
    }
    struct Shared {
        QSemaphore done;
        QMutex mutex;
        Resolved result;
    };
    const auto shared = std::make_shared<Shared>();
    // The captures carry their own names rather than the parameters': an
    // init-capture is a declaration, and one named after a variable already
    // in scope is what -Wshadow is for.
    QMetaObject::invokeMethod(
        qGuiApp,
        [shared, theme = std::move(icon_name), entry = std::move(desktop),
         program = std::move(binary), px] {
            Resolved resolved = resolve_on_gui_thread(theme, entry, program, px);
            const QMutexLocker lock(&shared->mutex);
            shared->result = std::move(resolved);
            shared->done.release();
        },
        Qt::QueuedConnection);
    answered = shared->done.tryAcquire(1, kGuiThreadWaitMs);
    if (!answered) {
        return {};
    }
    const QMutexLocker lock(&shared->mutex);
    return shared->result;
}

}  // namespace

AppIconProvider::AppIconProvider()
    : QQuickImageProvider(QQuickImageProvider::Image,
                          QQuickImageProvider::ForceAsynchronousImageLoading) {
    seed_icon_theme();
}

QImage AppIconProvider::requestImage(const QString& id, QSize* size, const QSize& requested_size) {
    const QString path = QUrl::fromPercentEncoding(id.section(QLatin1Char('?'), 0, 0).toUtf8());
    const QUrlQuery query(id.section(QLatin1Char('?'), 1));
    const QString icon_name = query.queryItemValue(QStringLiteral("icon"), QUrl::FullyDecoded);
    const QString app_id = query.queryItemValue(QStringLiteral("app"), QUrl::FullyDecoded);
    const QString name = query.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded);
    const bool large = requested_size.width() > 20 || requested_size.height() > 20 || !requested_size.isValid();
    // The whole id is the key: two processes of one application, and a
    // restarted one, share a lookup (the per-process cache is the session
    // monitor's).
    const QString key = id + (large ? QStringLiteral("|L") : QStringLiteral("|S"));
    {
        const QMutexLocker lock(&mutex_);
        if (const auto it = cache_.constFind(key); it != cache_.constEnd()) {
            if (size != nullptr) {
                *size = it->size();
            }
            return *it;
        }
    }

    const int px = large ? 256 : 32;
    const QString binary = basename_of(path);
    // The .desktop candidate first, here on the reader thread: it is plain
    // file reading, and the GUI thread then gets everything in one call.
    std::optional<std::string> desktop;
    if (!app_id.isEmpty() || !binary.isEmpty() || !name.isEmpty()) {
        Resolver& r = resolver();
        const QMutexLocker lock(&r.mutex);
        desktop = desktop_icon(r, {.app_id = app_id.toStdString(),
                                   .binary = binary.toStdString(),
                                   .name = name.toStdString()});
    }
    bool answered = false;
    Resolved resolved = on_gui_thread(icon_name, desktop, binary, px, answered);
    QImage image = std::move(resolved.image);
    if (!image.isNull() && requested_size.isValid()) {
        image = image.scaled(requested_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (size != nullptr) {
        *size = image.size();
    }
    qCDebug(lcIcons) << "icon for" << name << "path" << path << "icon-name" << icon_name << "app"
                     << app_id << "->" << (answered ? resolved.rung : "no answer from the GUI thread");
    if (answered) {
        // A null is cached too, as Windows caches one: the monogram is the
        // answer for this identity for the life of the process.
        const QMutexLocker lock(&mutex_);
        cache_.insert(key, image);
    }
    return image;
}

}  // namespace ac3::crucible::ui
