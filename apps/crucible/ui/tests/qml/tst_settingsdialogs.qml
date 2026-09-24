import QtQuick
import QtTest

import Ac3ForgeCrucible
import Ac3ForgeCrucibleTest

// The Settings page's two file dialogs - the signing key's Browse… and Save
// diagnostics… - pressed from the page. Both are Qt Quick Dialogs'
// FileDialog; a native picker cannot be driven headless, and Qt's own
// stand-in opens on whatever file it was given and returns what is selected
// in it when accepted. So the case selects the file a person would pick,
// presses the page's button (which opens the dialog), and accepts: the
// page's own onAccepted is what runs. The dialogs are found by the object
// names SettingsPage.qml gives them for this suite.
//
// A process of its own (tst_settingsworkflow.qml's header says why).
TestCase {
    id: testCase
    name: "SettingsDialogs"
    when: windowShown
    width: 1480
    height: 820

    Component { id: settingsPage; SettingsPage { width: 1480; height: 780 } }

    function init() {
        CrucibleController.stop();
        CrucibleController.clearKey();
    }

    function cleanup() {
        CrucibleController.clearKey();
    }

    function stored(key) {
        return String(TestServices.storedSetting(key));
    }

    function makePage() {
        const page = createTemporaryObject(settingsPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        return page;
    }

    function reveal(item) {
        // Layouts place their children at the next polish, so a control that
        // has just appeared (the card, on a selection) is where it will be
        // only once every layout above it has been polished.
        for (let up = item; up; up = up.parent) waitForItemPolished(up);
        let flick = item.parent;
        while (flick && flick.contentY === undefined) flick = flick.parent;
        if (!flick) return;
        const top = item.mapToItem(flick.contentItem, 0, 0).y;
        const wanted = Math.max(0, Math.min(flick.contentHeight - flick.height, top - flick.height / 3));
        // Only a scroll draws a new frame, and waitForRendering waits for one.
        if (Math.abs(flick.contentY - wanted) > 0.5) {
            flick.contentY = wanted;
            waitForRendering(item);
        }
    }

    function click(item) {
        reveal(item);
        mouseClick(item);
    }

    function byText(root, text) {
        if (root.text === text && (root.toggled !== undefined || root.clicked !== undefined)) return root;
        const kids = root.children || [];
        for (let i = 0; i < kids.length; ++i) {
            const found = byText(kids[i], text);
            if (found) return found;
        }
        if (root.contentItem && root.contentItem !== root) return byText(root.contentItem, text);
        return null;
    }

    // --- the signing key ------------------------------------------------------------

    function test_aKeyChosenInTheDialogIsRememberedAndClearedFromThePage() {
        const page = makePage();
        const keyDialog = findChild(page, "keyDialog");
        verify(keyDialog, "the page owns a key dialog");
        const browse = byText(page, "Browse…");
        const clear = byText(page, "Clear");
        verify(browse && clear);
        compare(clear.enabled, false, "nothing to clear yet");
        // Qt's own picker (the headless stand-in for the platform's) opens
        // on the file it was given and returns whatever is selected in it
        // when accepted, so the file a person would pick is selected first;
        // Browse then opens the dialog and accept() is the person's OK,
        // which runs the page's own handler.
        keyDialog.selectedFile = Qt.resolvedUrl("tst_settingsworkflow.qml");
        click(browse);
        tryCompare(keyDialog, "visible", true);
        keyDialog.accept();
        tryCompare(keyDialog, "visible", false);
        tryVerify(function() { return CrucibleController.keyPath.length > 0; }, 3000, "the chosen path is remembered");
        verify(CrucibleController.keyPath.indexOf("tst_settingsworkflow.qml") >= 0, CrucibleController.keyPath);
        compare(stored("signing/keyPath"), CrucibleController.keyPath);
        tryCompare(clear, "enabled", true);
        click(clear);
        compare(CrucibleController.keyPath, "");
        compare(TestServices.storedSetting("signing/keyPath"), undefined, "cleared from the store, not stored empty");
    }

    // --- diagnostics ----------------------------------------------------------------

    function test_saveDiagnosticsWritesTheFileThePageSuggests() {
        const page = makePage();
        const save = findChild(page, "exportDiagnosticsButton");
        const saveDialog = findChild(page, "diagnosticsDialog");
        verify(save && saveDialog);
        click(save);
        // The page fills in the suggestion before it opens the dialog.
        verify(String(saveDialog.selectedFile).indexOf("crucible-diagnostics-") > 0, saveDialog.selectedFile);
        tryCompare(saveDialog, "visible", true);
        // OK on the suggestion as it stands. The suggestion is the Documents
        // folder, which a headless runner's home may not have; either way the
        // page reports the outcome, naming the file it was asked for.
        const suggested = String(saveDialog.selectedFile).replace("file://", "");
        saveDialog.accept();
        tryCompare(saveDialog, "visible", false);
        tryVerify(function() { return CrucibleController.diagnosticsMessage.length > 0; }, 3000, "an outcome is reported");
        verify(CrucibleController.diagnosticsMessage.indexOf("saved to") === 0
               || CrucibleController.diagnosticsMessage.indexOf("could not write") === 0,
               CrucibleController.diagnosticsMessage);
        verify(CrucibleController.diagnosticsMessage.indexOf(suggested.split("/").pop()) >= 0,
               CrucibleController.diagnosticsMessage + " names " + suggested);
        const message = findChild(page, "diagnosticsMessage");
        tryCompare(message, "visible", true);
        compare(message.text, CrucibleController.diagnosticsMessage);
    }
}
