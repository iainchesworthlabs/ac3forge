import QtQuick
import QtTest

import Ac3Forge

// The settings a person reaches from the header and the Expert tabs, each
// followed through to what it actually changes: the container combo to the
// file that gets written, the DRC combo and Heavy compression box to the compr word
// QC finds in that file, the service combo to the metadata token, and the header's About and
// Preferences dialogs to what they show and write.
//
// Same seams as tst_e2e_encode.qml: pickers are found by objectName and
// their accepted() signal emitted; plain Controls with no objectName are found by the accessible
// name a screen reader would announce and driven from the keyboard (a combo
// steps with Down, exactly as it does for a person without a mouse).
TestCase {
    id: testCase
    name: "E2eSettings"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url wavUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url mkvOutUrl: Qt.resolvedUrl("_test_output/tst_e2e_settings.mkv")
    readonly property url drcOutUrl: Qt.resolvedUrl("_test_output/tst_e2e_settings_drc.ac3")
    readonly property url diagnosticsUrl:
        Qt.resolvedUrl("_test_output/tst_e2e_settings_diagnostics.txt")

    function cleanup() {
        while (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        EncoderController.codecIndex = 0;
        EncoderController.containerIndex = 0;
        EncoderController.drcIndex = 0;
        EncoderController.heavy = false;
        EncoderController.bsmodIndex = 0;
        EncoderController.loudnessTouched = false;
        EncoderController.formatDefaultsTouched = false;
        if (languageManager) {
            languageManager.setLanguage("en");
        }
    }

    // ---- helpers (same shape as tst_e2e_encode.qml's) --------------------

    function findByName(root, name) {
        let found = null;
        tryVerify(() => {
            found = findChild(root, name);
            return found !== null;
        }, 5000, "no object called " + name);
        return found;
    }

    function findWhere(item, pred) {
        if (pred(item)) {
            return item;
        }
        const kids = item.children;
        for (let i = 0; i < kids.length; ++i) {
            const hit = findWhere(kids[i], pred);
            if (hit) {
                return hit;
            }
        }
        return null;
    }

    function findAccessible(item, name) {
        return findWhere(item, (n) => n.Accessible && n.Accessible.name === name && n.visible);
    }

    // Key events from keyClick() go to the application's FOCUS window, not
    // to the window an item lives in - and the TestCase's own window starts
    // out as that. So the Main window is activated first, then the control
    // takes focus, exactly as clicking into the window would do.
    function focusIn(item) {
        const w = item.Window.window;
        w.requestActivate();
        tryVerify(() => w.active, 5000, "window never became active");
        item.forceActiveFocus();
        tryVerify(() => item.activeFocus, 5000, "control never took focus");
    }

    function settle(item) {
        waitForRendering(item);
        let last = "";
        tryVerify(() => {
            const p = item.mapToItem(null, 0, 0);
            const key = p.x + "," + p.y + "," + item.width + "," + item.height;
            const stable = item.width > 0 && key === last;
            last = key;
            return stable;
        }, 5000, "item never settled");
    }

    function click(item) {
        settle(item);
        mouseClick(item);
    }

    function pickFile(root, dialogName, url) {
        const dialog = findByName(root, dialogName);
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = url;
        dialog.accepted();  // see pickFile
        dialog.close();
        tryCompare(dialog, "visible", false, 5000);
        return dialog;
    }

    function loadWav(win) {
        click(findByName(win.contentItem, "firstRun-file"));
        pickFile(win, "openDialog", wavUrl);
        tryCompare(EncoderController, "sourceReady", true, 10000);
    }

    function expertTab(win, key) {
        click(findByName(win.contentItem, "seg-expert"));
        compare(win.tier, "expert");
        click(findByName(win.contentItem, "tab-" + key));
        compare(win.currentTab, key);
    }

    function stepCombo(combo, steps) {
        focusIn(combo);
        for (let i = 0; i < steps; ++i) {
            keyClick(Qt.Key_Down);
        }
    }

    function encodeTo(win, url) {
        const button = findByName(win.contentItem, "encodeButton");
        tryCompare(button, "enabled", true);
        click(button);
        pickFile(win, "saveDialog", url);
        tryCompare(EncoderController, "busy", false, 30000);
        const run = EncoderController.runs[0];
        compare(run.status, "done", run.detail);
        return run;
    }

    function qcRun(win, run) {
        win.openRunInQc(run.path);
        tryVerify(() => win.qcDialogRef.opened);
        tryCompare(QcController, "busy", false, 15000);
        compare(QcController.error, "");
    }

    // ---- cases ----------------------------------------------------------

    function test_containerComboWritesAMatroskaFileQcCanReadBack() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        loadWav(win);
        expertTab(win, "format");

        const container = findAccessible(win.contentItem, "Container");
        verify(container !== null, "no Container combo");
        compare(container.currentIndex, 0);
        stepCombo(container, 1);
        compare(EncoderController.containerIndex, 1);
        compare(EncoderController.formatDefaultsTouched, true);
        compare(EncoderController.outputSuffix(), "mkv");
        tryCompare(findByName(win.contentItem, "encodeButton"), "text", "Encode to .mkv");
        // The command bar owns up to the two-step CLI equivalent.
        verify(win.cliLine.indexOf("&& ac3cli mkv") > 0, win.cliLine);

        const run = encodeTo(win, mkvOutUrl);
        verify(run.path.endsWith(".mkv"), run.path);
        qcRun(win, run);
        // QC sniffs the container and measures the AC-3 inside it: same
        // codec, rate and 32-frame length the elementary encode has.
        verify(QcController.summaryLine.indexOf("AC-3") === 0, QcController.summaryLine);
        verify(QcController.summaryLine.indexOf("48000 Hz") > 0, QcController.summaryLine);
        verify(QcController.summaryLine.indexOf("32 ") > 0, QcController.summaryLine);
        win.qcDialogRef.close();
    }

    function test_drcAndHeavyCompressionFromTheMetadataTabPutComprIntoTheStream() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        loadWav(win);
        expertTab(win, "meta");

        const drc = findAccessible(win.contentItem, "DRC profile");
        verify(drc !== null, "no DRC combo");
        compare(drc.currentIndex, 0);
        stepCombo(drc, 1);  // "none" -> the first real profile (film standard)
        compare(EncoderController.drcIndex, 1);
        compare(EncoderController.loudnessTouched, true);
        verify(EncoderController.metaTokens.indexOf("drc=") >= 0, EncoderController.metaTokens);
        // The Metadata tab badge now counts it.
        compare(win.visibleTabs.filter((t) => t.key === "meta")[0].badge, "1");

        // dynrng (the DRC profile) has no readout in QC; compr does. It is
        // the Heavy compression card's word, so tick that too.
        const heavy = findWhere(win.contentItem,
                                (n) => n.text === "Heavy compression" && n.checkable === true);
        verify(heavy !== null, "no Heavy compression checkbox");
        focusIn(heavy);
        keyClick(Qt.Key_Space);
        compare(EncoderController.heavy, true);
        verify(EncoderController.metaTokens.indexOf("heavy") >= 0, EncoderController.metaTokens);
        compare(win.visibleTabs.filter((t) => t.key === "meta")[0].badge, "2");

        const run = encodeTo(win, drcOutUrl);
        qcRun(win, run);
        compare(QcController.programmes[0].hasCompr, true);
        const block = findByName(win.qcDialogRef.contentItem, "qcDialnormBlock");
        const texts = [];
        (function walk(n) {
            if (typeof n.text === "string") texts.push(n.text);
            for (let i = 0; i < n.children.length; ++i) walk(n.children[i]);
        })(block);
        verify(texts.some((t) => t.indexOf("compr present") === 0), texts.join(" | "));
        win.qcDialogRef.close();
    }

    function test_serviceComboSetsBsmodAndTheMetadataBadge() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        loadWav(win);
        expertTab(win, "meta");

        const bsmod = findByName(win.contentItem, "bsmodCombo");
        compare(bsmod.currentIndex, 0);
        stepCombo(bsmod, 4);  // "dialogue"
        compare(EncoderController.bsmodIndex, 4);
        compare(bsmod.displayText, EncoderController.bsmodNames[4]);
        verify(EncoderController.metaTokens.indexOf("bsmod=dialogue") >= 0, EncoderController.metaTokens);
        compare(win.visibleTabs.filter((t) => t.key === "meta")[0].badge, "1");
    }

    function test_aboutDialogShowsTheBuildsVersionAndCloses() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        click(findByName(win.contentItem, "aboutOpenButton"));
        const version = findByName(win.contentItem, "aboutVersionText");
        tryCompare(version, "visible", true);
        verify(version.text.indexOf("ac3forge") === 0, version.text);
        click(findByName(win.contentItem, "aboutCloseButton"));
        tryCompare(version, "visible", false);
    }

    function test_preferencesSaveDiagnosticsWritesTheSupportFile() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const prefsButton = findWhere(win.contentItem,
                                      (n) => n.text === "Preferences" && n.checkable !== undefined);
        verify(prefsButton !== null);
        click(prefsButton);
        tryVerify(() => win.prefsDialog.opened);

        const saveDiag = findByName(win.prefsDialog.contentItem, "prefsDiagnosticsButton");
        // The Diagnostics section sits at the foot of a scrolling page, so
        // press it from the keyboard rather than at a coordinate that may be
        // scrolled out of view.
        focusIn(saveDiag);
        keyClick(Qt.Key_Space);
        const dialog = findByName(win.prefsDialog, "diagnosticsDialog");
        tryCompare(dialog, "visible", true);
        // The suggested name is filled in before the picker shows.
        verify(dialog.selectedFile.toString().endsWith(".txt"), dialog.selectedFile);
        dialog.selectedFile = diagnosticsUrl;
        dialog.accepted();  // see pickFile
        dialog.close();
        tryVerify(() => EncoderController.diagnosticsMessage.length > 0);
        verify(EncoderController.diagnosticsMessage.indexOf("tst_e2e_settings_diagnostics.txt") >= 0,
               EncoderController.diagnosticsMessage);
        tryCompare(findByName(win.prefsDialog.contentItem, "prefsDiagnosticsMessage"),
                   "visible", true);
        click(findByName(win.prefsDialog.contentItem, "prefsCancelButton"));
        tryVerify(() => !win.prefsDialog.visible);
    }

    function test_preferencesLanguageComboSwitchesTheWindowImmediately() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        verify(languageManager !== null);
        compare(languageManager.currentLanguage, "en");
        const englishTier = findByName(win.contentItem, "seg-guided").Accessible.name;

        win.prefsDialog.open();
        tryVerify(() => win.prefsDialog.opened);
        const combo = findByName(win.prefsDialog.contentItem, "prefsLanguage");
        compare(combo.currentValue, "en");
        stepCombo(combo, 1);
        const picked = combo.currentValue;
        verify(picked !== "en");
        // Applied on the pick, not behind Save.
        compare(languageManager.currentLanguage, picked);
        tryVerify(() => findChild(win.contentItem, "seg-guided").Accessible.name !== englishTier,
                  5000, "header did not retranslate");
        click(findByName(win.prefsDialog.contentItem, "prefsCancelButton"));
        tryVerify(() => !win.prefsDialog.visible);
        languageManager.setLanguage("en");
        tryVerify(() => findChild(win.contentItem, "seg-guided").Accessible.name === englishTier);
    }
}
