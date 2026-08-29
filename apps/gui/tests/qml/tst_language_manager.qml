import QtQuick
import QtTest

// Exercises the `languageManager` context property qml_test_main.cpp's
// SettingsIsolation::qmlEngineAvailable() registers - the same LanguageManager
// object main.cpp installs for the real app (language_manager.hpp/.cpp).
// Modelled directly on CountdownSolver's own tst_LanguageManager.qml
// (R:\CountdownSolver\tests\qml), same shape, ported to this app's seven-
// language set.
TestCase {
    id: testCase
    name: "LanguageManager"

    function init() {
        // Every suite in this binary shares the process-wide QGuiApplication
        // languageManager mutates (installTranslators()/setLayoutDirection()
        // are static/application-wide state) - reset to "en" before each
        // test so an earlier failure elsewhere cannot leave this one reading
        // a language it never selected itself.
        languageManager.setLanguage("en");
    }

    function test_availableLanguagesListsEnPlusSixRealLanguagesNotThePseudoLocale() {
        const languages = languageManager.availableLanguages();
        compare(languages.length, 7);
        compare(languages[0].code, "en");
        const codes = languages.map(function (l) { return l.code; });
        for (const code of ["fr", "de", "es", "ar", "he", "yi"]) {
            verify(codes.indexOf(code) >= 0, code + " missing from availableLanguages()");
        }
        // The pseudo-locale QA fixture (apps/gui/translations/ac3gui_xx.ts)
        // is reachable only through the AC3GUI_LOCALE environment override -
        // never a real, user-selectable entry.
        verify(codes.indexOf("xx") < 0);
    }

    function test_setLanguageSwitchesRejectsUnknownCodesAndPersists() {
        compare(languageManager.currentLanguage, "en");

        verify(languageManager.setLanguage("fr"));
        compare(languageManager.currentLanguage, "fr");

        // Re-selecting the already-active language is a no-op success, not
        // a translator reload.
        verify(languageManager.setLanguage("fr"));
        compare(languageManager.currentLanguage, "fr");

        verify(!languageManager.setLanguage("not-a-real-language"));
        compare(languageManager.currentLanguage, "fr");

        // The pseudo-locale fixture is deliberately not one of
        // availableLanguages()'s codes - setLanguage() must refuse it the
        // same way it refuses any other unsupported code.
        verify(!languageManager.setLanguage("xx"));
        compare(languageManager.currentLanguage, "fr");

        languageManager.setLanguage("en");
        compare(languageManager.currentLanguage, "en");
    }

    function test_rtlLanguagesSetRightToLeftLayoutDirection() {
        for (const code of ["ar", "he", "yi"]) {
            verify(languageManager.setLanguage(code));
            compare(Qt.application.layoutDirection, Qt.RightToLeft, code + " should be RTL");
        }
        for (const code of ["en", "fr", "de", "es"]) {
            verify(languageManager.setLanguage(code));
            compare(Qt.application.layoutDirection, Qt.LeftToRight, code + " should be LTR");
        }
    }
}
