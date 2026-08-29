#pragma once

#include <QGuiApplication>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QTranslator>
#include <QVariantList>

// Owns ac3gui's QTranslators (Qt's own bundled strings plus this app's own
// ac3gui_<code>.qm catalog), the persisted language preference, and
// everything a language switch touches: installed translators, the
// application's layout direction (LTR/RTL) and QQmlEngine::retranslate().
// Modelled directly on CountdownSolver's own language_manager.{hpp,cpp}
// (R:\CountdownSolver\src\app) - same shape, same canonical language set
// (roadmap UX3: build to the set already shipped there rather than invent a
// second one), ported to this app's plain-global-namespace controller
// convention (SystemTheme, EncoderController, QcController).
//
// Not a QML_SINGLETON: like its model, it takes QGuiApplication&/QQmlEngine&
// constructor arguments a singleton factory can't supply, so it is
// constructed once (main.cpp for the real app, qml_test_main.cpp's
// qmlEngineAvailable() hook for the QML test suite) and exposed as a plain
// "languageManager" context property instead.
class LanguageManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentLanguage READ currentLanguage NOTIFY currentLanguageChanged)

public:
    // `app` and `engine` must outlive this LanguageManager.
    explicit LanguageManager(QGuiApplication& app, QQmlEngine& engine, QObject* parent = nullptr);

    // Installs the translators for the initial language - an AC3GUI_LOCALE
    // environment override first (the pseudo-locale QA fixture and the
    // deterministic test suites use this - see docs/gui/localisation.md),
    // then the persisted preference, then the system locale, then "en" - and
    // sets the initial layout direction. Called once, before the QML loads,
    // so the first frame already renders translated and RTL-mirrored where
    // appropriate.
    void applyInitialLanguage();

    [[nodiscard]] QString currentLanguage() const noexcept { return current_language_; }

    // One { "code": QString, "name": QString } entry per supported REAL
    // language - "en" (no .qm - falls back to the untranslated source
    // strings) first, then every language apps/gui/translations/ ships.
    // Deliberately excludes the "xx" pseudo-locale fixture: it exists to
    // prove the extraction/compile/load pipeline and to catch a qsTr()
    // bypass, not as something a user would ever pick from Preferences.
    Q_INVOKABLE QVariantList availableLanguages() const;

    // Persists `code`, swaps the installed translators, updates the
    // application's layout direction, and retranslates the running QML.
    // Returns false (no change made) if `code` isn't one of
    // availableLanguages()'s codes - the pseudo-locale included, since it is
    // reached only through AC3GUI_LOCALE, never through this entry point.
    Q_INVOKABLE bool setLanguage(const QString& code);

signals:
    void currentLanguageChanged();

private:
    void installTranslators(const QString& code);
    void updateFontFamily(const QString& code);

    QGuiApplication& app_;
    QQmlEngine& engine_;
    QTranslator qt_translator_;
    QTranslator app_translator_;
    QString current_language_ = QStringLiteral("en");
};
