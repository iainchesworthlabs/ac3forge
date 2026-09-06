import QtQuick
import QtTest

import Ac3Forge

// The support file the window offers under Preferences > Diagnostics.
//
// apps/gui/gui_diagnostics.hpp holds the RULE (no signing key, no
// environment value, no byte of a loaded source) and tests/gui/
// test_gui_diagnostics.cpp holds it on every CI leg, window or not. This
// suite covers the other half: that the controller fills the report's named
// fields from what the window is actually showing, and that a source names
// itself the way the input rail names it rather than by the folder it came
// from.
TestCase {
    id: testCase
    name: "Diagnostics"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    // The same fixture tst_source_loading.qml and the --smoke harness use,
    // so the three cannot disagree about what a known-good WAV looks like.
    readonly property url fixtureUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")

    // The controller singleton is shared across the cases in this binary -
    // leaving a source loaded would change what a later case reports.
    function cleanup() {
        while (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
    }

    function test_theReportCarriesEverySectionEvenWithNothingLoaded() {
        const report = EncoderController.diagnosticsReport();
        verify(report.length > 0);
        verify(report.indexOf("AC3Forge ac3gui diagnostics") === 0, report.substring(0, 60));
        for (const heading of ["# version", "# platform", "# signing", "# sources", "# plan",
                               "# settings", "# runs", "# last errors", "# recent messages"]) {
            verify(report.indexOf(heading) >= 0, "no " + heading + " section");
        }
        // An empty section reads as empty rather than as a missing heading,
        // which is the difference between "nothing was loaded" and "the
        // report is broken".
        verify(report.indexOf("(none)") >= 0);
    }

    function test_theReportSaysWhetherTheSigningVariablesAreSetAndNeverWhatTheyHold() {
        const report = EncoderController.diagnosticsReport();
        // One of the two forms, whichever this machine is in - the point is
        // that the row is a yes/no about the variable's existence and has no
        // room for a value.
        const fileRow = report.indexOf("AC3FORGE_SIGNING_KEY_FILE: set") >= 0
                        || report.indexOf("AC3FORGE_SIGNING_KEY_FILE: not set") >= 0;
        const inlineRow = report.indexOf("AC3FORGE_SIGNING_KEY: set") >= 0
                          || report.indexOf("AC3FORGE_SIGNING_KEY: not set") >= 0;
        verify(fileRow, "no AC3FORGE_SIGNING_KEY_FILE row");
        verify(inlineRow, "no AC3FORGE_SIGNING_KEY row");
    }

    function test_theSettingsSectionIsAFixedListNotTheWholeStore() {
        const report = EncoderController.diagnosticsReport();
        // A setting the test harness itself writes (qml_test_main.cpp), so
        // this proves the section reads the store on disk rather than printing
        // placeholders.
        verify(report.indexOf("workbench/restoreSession = false") >= 0, report);
        verify(report.indexOf("workbench/textScale =") >= 0);
        // ...and the session blobs are NOT in it: they are JSON lists of
        // source paths and past runs, which the sources and runs sections
        // above already say in the form the report is allowed to say it in.
        compare(report.indexOf("workbench/sessionSources"), -1);
        compare(report.indexOf("workbench/sessionAssignments"), -1);
    }

    function test_aLoadedSourceIsNamedTheWayTheRailNamesItAndNeverByItsFolder() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.loadSourceFile(fixtureUrl);
        tryCompare(EncoderController, "sourceReady", true);

        const report = EncoderController.diagnosticsReport();
        const row = EncoderController.sourceModel[0];
        // The base name, its channel count and its rate - the same three the
        // rail draws, read from the same model rather than recomputed.
        verify(report.indexOf(row.label) >= 0, "the report does not name the loaded source");
        verify(report.indexOf(String(row.channels) + " ch") >= 0);
        verify(report.indexOf(String(row.rate) + " Hz") >= 0);

        // And not one component of the folder it came from. "fuzz_wav_read"
        // is a directory in the fixture's path and appears nowhere in its
        // base name, so finding it would mean a path had leaked in - through
        // this section or through a message in the ring.
        compare(report.indexOf("fuzz_wav_read"), -1);
    }

    function test_theSuggestedFileIsATextFileUnderAWritableFolder() {
        const suggestion = EncoderController.suggestedDiagnosticsFile();
        verify(suggestion.indexOf("file:") === 0, suggestion);
        verify(suggestion.indexOf("ac3gui-diagnostics-") > 0, suggestion);
        verify(suggestion.lastIndexOf(".txt") === suggestion.length - 4, suggestion);
    }

    function test_aRefusedExportSaysSoRatherThanFailingSilently() {
        // A folder that does not exist and that QSaveFile will not create:
        // the one refusal a test can produce without writing a file anywhere.
        // The message is what Preferences shows under its button, so an
        // export that went nowhere is never mistaken for one that landed.
        compare(EncoderController.exportDiagnostics(
                    "file:///no-such-folder-for-ac3gui/diagnostics.txt"), false);
        verify(EncoderController.diagnosticsMessage.length > 0);
        verify(EncoderController.diagnosticsMessage.indexOf("diagnostics.txt") > 0,
               EncoderController.diagnosticsMessage);
    }
}
