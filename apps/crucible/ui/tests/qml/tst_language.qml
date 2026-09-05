import QtQuick
import QtTest

import Ac3ForgeCrucible
import Ac3ForgeCrucibleLanguage

// The shared LanguageManager under this app's translation basename: the
// six languages plus English are offered, each loads (a translated string
// differs from the source), the two RTL ones flip the layout direction,
// and the override persists and can be handed back to the system.
TestCase {
    id: testCase
    name: "Language"

    function init() {
        LanguageManager.setLanguage("en");
    }

    function test_offersEnglishAndSixLanguages() {
        const languages = LanguageManager.availableLanguages();
        compare(languages.length, 7);
        compare(languages[0].code, "en");
        const codes = languages.map(function (l) { return l.code; });
        for (const code of ["fr", "de", "es", "ar", "he", "yi"]) {
            verify(codes.indexOf(code) >= 0, code + " missing");
        }
    }

    function test_eachLanguageTranslatesAndSetsDirection() {
        const source = "Install driver";
        for (const code of ["fr", "de", "es", "ar", "he", "yi"]) {
            verify(LanguageManager.setLanguage(code), "setLanguage " + code);
            compare(LanguageManager.currentLanguage, code);
            const translated = qsTranslate("SettingsPage", source);
            verify(translated !== source, code + ": '" + source + "' came back untranslated");
            const rtl = code === "ar" || code === "he" || code === "yi";
            compare(Qt.application.layoutDirection === Qt.RightToLeft, rtl, code + " layout direction");
        }
        verify(LanguageManager.setLanguage("en"));
        compare(qsTranslate("SettingsPage", source), source);
        compare(Qt.application.layoutDirection, Qt.LeftToRight);
    }

    function test_unknownCodeIsRefused() {
        verify(!LanguageManager.setLanguage("xx-no-such"));
        compare(LanguageManager.currentLanguage, "en");
    }

    function test_overrideCanBeHandedBackToTheSystem() {
        verify(LanguageManager.setLanguage("de"));
        verify(LanguageManager.hasOverride());
        LanguageManager.useSystemLanguage();
        verify(!LanguageManager.hasOverride());
    }
}
