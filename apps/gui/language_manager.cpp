#include "language_manager.hpp"

#include <QFont>
#include <QLibraryInfo>
#include <QLocale>
#include <QProcessEnvironment>
#include <QSettings>
#include <QVariantMap>

#include <array>
#include <utility>

namespace {

struct LanguageInfo {
    const char* code;
    const char* nativeName;
};

// "en" first (no .qm - the untranslated source strings are already English),
// then every language apps/gui/translations/ ships - the same set, same
// order, as CountdownSolver's own kLanguages (roadmap UX3: build to that
// project's already-shipped canonical set rather than invent a second one).
// Real translation coverage is partial today (see docs/gui/localisation.md);
// a language appears here once its .ts exists, whether or not every string
// in it is finished yet.
constexpr std::array<LanguageInfo, 7> kLanguages{{
    {"en", "English"},
    {"fr", "Français"},
    {"de", "Deutsch"},
    {"es", "Español"},
    {"ar", "العربية"},
    {"he", "עברית"},
    {"yi", "יידיש"},
}};

// The pseudo-locale QA fixture (tools/generators/gen_pseudo_locale.py,
// apps/gui/translations/ac3gui_xx.ts) - reachable only through the
// AC3GUI_LOCALE override below, never through availableLanguages()/
// setLanguage(), since it exists to prove the pipeline and catch a qsTr()
// bypass rather than to be user-selectable.
constexpr auto kPseudoLocaleCode = "xx";

constexpr auto kSettingsKey = "language/code";
constexpr auto kLocaleEnvOverride = "AC3GUI_LOCALE";

// The handoff's default (main.cpp registers it as the application font
// before the engine ever loads) and the two bundled faces with actual
// Arabic/Hebrew glyph coverage Archivo lacks - Theme.qml's rtlFonts carries
// the same pairing for anything that wants to read it directly, but this is
// the map that actually takes effect: most of ac3gui's Text/Control
// elements take their font from the QGuiApplication-wide default rather
// than an explicit per-Text binding, so switching that default here is what
// makes the whole window follow a language switch instead of only the
// handful of elements that bind Theme.headingFamily explicitly.
constexpr auto kLatinFontFamily = "Archivo";

[[nodiscard]] const char* font_family_for(const QString& code) {
    if (code == QLatin1String("ar")) {
        return "Noto Sans Arabic";
    }
    if (code == QLatin1String("he") || code == QLatin1String("yi")) {
        return "Noto Sans Hebrew";
    }
    return kLatinFontFamily;
}

[[nodiscard]] bool is_supported(const QString& code) {
    for (const LanguageInfo& info : kLanguages) {
        if (code == QLatin1String(info.code)) {
            return true;
        }
    }
    return false;
}

// Maps a system locale to one of kLanguages, or "en" if unsupported - so an
// unrecognised system locale falls back to the untranslated UI rather than
// failing to find any translator at all.
[[nodiscard]] QString language_for_locale(const QLocale& locale) {
    const QString name = QLocale::languageToCode(locale.language());
    return is_supported(name) ? name : QStringLiteral("en");
}

}  // namespace

LanguageManager::LanguageManager(QGuiApplication& app, QQmlEngine& engine,
                                 QString translation_basename, QObject* parent)
    : QObject(parent),
      app_(app),
      engine_(engine),
      translation_basename_(std::move(translation_basename)) {}

void LanguageManager::useSystemLanguage() {
    QSettings settings;
    settings.remove(QLatin1String(kSettingsKey));
    const QString system = language_for_locale(QLocale::system());
    if (system == current_language_) {
        return;
    }
    installTranslators(system);
    app_.setLayoutDirection(QLocale(system).textDirection());
    updateFontFamily(system);
    current_language_ = system;
    engine_.retranslate();
    emit currentLanguageChanged();
}

bool LanguageManager::hasOverride() const {
    const QSettings settings;
    return is_supported(settings.value(QLatin1String(kSettingsKey)).toString());
}

void LanguageManager::applyInitialLanguage() {
    const QString env_override =
        QProcessEnvironment::systemEnvironment().value(QLatin1String(kLocaleEnvOverride));
    QString initial;
    if (env_override == QLatin1String(kPseudoLocaleCode) || is_supported(env_override)) {
        initial = env_override;
    } else {
        const QSettings settings;
        const QString saved = settings.value(QLatin1String(kSettingsKey)).toString();
        initial = is_supported(saved) ? saved : language_for_locale(QLocale::system());
    }

    installTranslators(initial);
    app_.setLayoutDirection(QLocale(initial).textDirection());
    updateFontFamily(initial);
    current_language_ = initial;
}

QVariantList LanguageManager::availableLanguages() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(kLanguages.size()));
    for (const LanguageInfo& info : kLanguages) {
        QVariantMap entry;
        entry["code"] = QString::fromLatin1(info.code);
        entry["name"] = QString::fromUtf8(info.nativeName);
        result << entry;
    }
    return result;
}

bool LanguageManager::setLanguage(const QString& code) {
    if (!is_supported(code)) {
        qWarning("LanguageManager::setLanguage: unsupported language code %s",
                 qUtf8Printable(code));
        return false;
    }
    if (code == current_language_) {
        return true;
    }

    installTranslators(code);
    app_.setLayoutDirection(QLocale(code).textDirection());
    updateFontFamily(code);
    // current_language_ must be updated before retranslate(): retranslate()
    // synchronously re-evaluates every live qsTr()-using binding, and any of
    // those that also read currentLanguage() must see the new code, not the
    // language being switched away from (CountdownSolver's own
    // LanguageManager carries the same ordering, fixed there after a real
    // reproduced race - see tst_LanguageSwitching.qml's header comment).
    current_language_ = code;
    engine_.retranslate();

    QSettings settings;
    settings.setValue(QLatin1String(kSettingsKey), code);

    emit currentLanguageChanged();
    return true;
}

void LanguageManager::updateFontFamily(const QString& code) {
    QFont font = QGuiApplication::font();
    font.setFamily(QString::fromLatin1(font_family_for(code)));
    QGuiApplication::setFont(font);
}

void LanguageManager::installTranslators(const QString& code) {
    // Each translator's previous language (if any) is replaced in place via
    // QTranslator::load() rather than swapped for a fresh instance, so a
    // failed load below cleanly leaves the old translator removed instead of
    // half-installed.
    app_.removeTranslator(&qt_translator_);
    app_.removeTranslator(&app_translator_);

    if (code == QLatin1String("en")) {
        return;  // No translator needed - source strings are already English.
    }

    // "xx" is not a real ISO 639 code, so QLocale("xx") does not resolve to
    // it - QTranslator::load(QLocale, ...) would then try filenames built
    // from whatever QLocale falls back to instead (observed: it silently
    // finds nothing), never "ac3gui_xx". Loading the resource path directly
    // sidesteps QLocale matching entirely, which is fine here specifically
    // because the pseudo-locale is reached only through the exact resource
    // name this app itself generated (gen_pseudo_locale.py), never through
    // a system locale someone's OS actually reports.
    if (code == QLatin1String(kPseudoLocaleCode)) {
        if (app_translator_.load(QStringLiteral(":/i18n/") + translation_basename_ + QStringLiteral("_xx.qm"))) {
            app_.installTranslator(&app_translator_);
        } else {
            qWarning("LanguageManager::installTranslators: no %s_xx.qm resource found",
                     qUtf8Printable(translation_basename_));
        }
        return;
    }

    // Qt 6 consolidated its own translation catalogs (formerly qtbase_xx,
    // qtdeclarative_xx, ...) into a single qt_xx.qm per language.
    if (qt_translator_.load(QLocale(code), QStringLiteral("qt"), QStringLiteral("_"),
                             QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        app_.installTranslator(&qt_translator_);
    }

    if (app_translator_.load(QLocale(code), translation_basename_, QStringLiteral("_"),
                              QStringLiteral(":/i18n"))) {
        app_.installTranslator(&app_translator_);
    } else {
        qWarning("LanguageManager::installTranslators: no %s_%s.qm resource found",
                 qUtf8Printable(translation_basename_), qUtf8Printable(code));
    }
}
