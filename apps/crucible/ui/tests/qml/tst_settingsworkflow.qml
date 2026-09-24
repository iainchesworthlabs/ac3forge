import QtQuick
import QtTest

import Ac3ForgeCrucible
import Ac3ForgeCrucibleLanguage
import Ac3ForgeCrucibleTest

// The Settings page, pressed rather than written to: each case clicks the
// page's own segment, check, combo box or field, then reads the choice back
// twice - from the controller, and from the settings store ON DISK through a
// QSettings of its own (TestServices.storedSetting), which is what the next
// launch reads. A page built afterwards is shown the stored choice, which is
// the other half of "it persists".
//
// The page's two file dialogs are tst_settingsdialogs.qml's, in a process of
// their own: once Qt's headless picker has been opened in a window, a later
// click in that window no longer gives a text field the focus, which would
// make the typing case here depend on the order the cases run in.
TestCase {
    id: testCase
    name: "SettingsWorkflow"
    when: windowShown
    width: 1480
    height: 820

    Component { id: settingsPage; SettingsPage { width: 1480; height: 780 } }

    function init() {
        CrucibleController.stop();
        CrucibleController.lowLatency = false;
        CrucibleController.bitrate = 0;
        CrucibleController.splitStereo = false;
        CrucibleController.theme = "system";
        CrucibleController.palette = "signal";
        CrucibleController.textScale = "100";
        CrucibleController.roomLayout = "auto";
        CrucibleController.moveDefaultOnLaunch = false;
        CrucibleController.nullSinkName = "Desktop Atmos";
        CrucibleController.clearKey();
        LanguageManager.setLanguage("en");
    }

    function cleanup() {
        CrucibleController.stop();
        TestServices.clear();
        CrucibleController.textScale = "100";
        CrucibleController.nullSinkName = "Desktop Atmos";
        CrucibleController.clearKey();
        LanguageManager.setLanguage("en");
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

    // The SegmentedControl whose accessible name is `name`, and a segment of it.
    function segments(root, name) {
        if (root.accessibleName === name && root.model !== undefined && root.selected !== undefined) return root;
        const kids = root.children || [];
        for (let i = 0; i < kids.length; ++i) {
            const found = segments(kids[i], name);
            if (found) return found;
        }
        if (root.contentItem && root.contentItem !== root) return segments(root.contentItem, name);
        return null;
    }
    function pick(page, groupName, value) {
        const group = segments(page, groupName);
        verify(group, "the " + groupName + " control");
        const segment = findChild(group, "seg-" + value);
        verify(segment, groupName + " offers " + value);
        click(segment);
        compare(group.currentValue, value, groupName + " shows the choice");
    }

    // A control by its label (checks and buttons carry no object names).
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

    function chooseFromCombo(box, value) {
        reveal(box);
        mouseClick(box);
        tryCompare(box.popup, "opened", true);
        const index = box.indexOfValue(value);
        verify(index >= 0, box.objectName + " offers " + value);
        const list = box.popup.contentItem;
        list.positionViewAtIndex(index, ListView.Contain);
        let delegate = null;
        tryVerify(function() { delegate = list.itemAtIndex(index); return delegate !== null; }, 2000);
        waitForRendering(list);
        mouseClick(delegate);
        tryCompare(box.popup, "visible", false);
    }

    // --- sound --------------------------------------------------------------------

    function test_latencyAndBitrateAreStoredAndRestartTheStream() {
        if (!TestServices.scriptSessions([{ app: 900, name: "chrome", active: true }])) {
            skip("the scripted machine is not available in this harness");
        }
        CrucibleController.start();
        if (!CrucibleController.running) {
            skip("the engine did not run over the scripted machine: " + CrucibleController.lastError);
        }
        const page = makePage();
        pick(page, "Latency", "low");
        compare(CrucibleController.lowLatency, true);
        compare(stored("codec/lowLatency"), "true");
        // The restart the note promises, and the engine is back afterwards.
        compare(CrucibleController.running, true, "the stream restarted rather than stopped");

        const bitrate = findChild(page, "bitrateBox");
        verify(bitrate);
        chooseFromCombo(bitrate, 640);
        compare(CrucibleController.bitrate, 640);
        compare(stored("codec/bitrate"), "640");
        compare(CrucibleController.running, true);

        click(findChild(page, "splitCheck"));
        compare(CrucibleController.splitStereo, true);
        compare(stored("codec/splitStereo"), "true");

        // A fresh page reads the store.
        const again = makePage();
        compare(segments(again, "Latency").currentValue, "low");
        compare(findChild(again, "bitrateBox").currentValue, 640);
        compare(findChild(again, "splitCheck").checked, true);
    }

    // --- appearance ---------------------------------------------------------------

    function test_appearanceChoicesAreStoredAndApplied() {
        const page = makePage();
        pick(page, "Theme", "dark");
        compare(CrucibleController.theme, "dark");
        compare(stored("appearance/theme"), "dark");
        pick(page, "Palette", "console");
        compare(CrucibleController.palette, "console");
        compare(stored("appearance/palette"), "console");
        pick(page, "Text size", "150");
        compare(CrucibleController.textScale, "150");
        compare(stored("appearance/textScale"), "150");
        pick(page, "3D reference layout", "7.1.4");
        compare(CrucibleController.roomLayout, "7.1.4");
        compare(stored("appearance/roomLayout"), "7.1.4");

        const again = makePage();
        compare(segments(again, "Theme").currentValue, "dark");
        compare(segments(again, "Palette").currentValue, "console");
        compare(segments(again, "Text size").currentValue, "150");
        compare(segments(again, "3D reference layout").currentValue, "7.1.4");

        // The newer page is the one on top, so it is the one clicked.
        pick(again, "Theme", "light");
        compare(stored("appearance/theme"), "light");
    }

    // --- language -----------------------------------------------------------------

    function test_theLanguageBoxSwitchesTheWindowAndHandsBackToTheSystem() {
        const page = makePage();
        const box = findChild(page, "languageBox");
        verify(box);
        const install = findChild(page, "installDriverButton");
        const english = install.text;
        chooseFromCombo(box, "fr");
        compare(LanguageManager.currentLanguage, "fr");
        verify(LanguageManager.hasOverride());
        // The page retranslates in place.
        tryVerify(function() { return install.text !== english; }, 3000, "the button's label was retranslated: " + install.text);
        compare(install.text, qsTranslate("SettingsPage", english));
        compare(box.currentValue, "fr", "the box still shows the chosen language after the retranslate");

        chooseFromCombo(box, "he");
        compare(LanguageManager.currentLanguage, "he");
        compare(Qt.application.layoutDirection, Qt.RightToLeft, "Hebrew mirrors the window");

        chooseFromCombo(box, "");
        verify(!LanguageManager.hasOverride(), "System hands the choice back");
        compare(box.currentIndex, 0);
    }

    // --- behaviour ----------------------------------------------------------------

    function test_behaviourChecksAreStored() {
        const page = makePage();
        const move = byText(page, "Move the default output to the silent device on launch");
        verify(move);
        click(move);
        compare(CrucibleController.moveDefaultOnLaunch, true);
        compare(stored("behaviour/moveDefaultOnLaunch"), "true");
        click(move);
        compare(stored("behaviour/moveDefaultOnLaunch"), "false");

        // Keep running is offered only where there is a tray to keep it in;
        // a click on the greyed row changes nothing.
        const keep = findChild(page, "keepRunningCheck");
        const before = CrucibleController.keepRunningWhenClosed;
        click(keep);
        if (CrucibleController.trayAvailable) {
            compare(CrucibleController.keepRunningWhenClosed, !before);
            click(keep);
        } else {
            compare(CrucibleController.keepRunningWhenClosed, before, "a greyed row does not toggle");
        }
    }

    // --- the silent device's name -------------------------------------------------------

    function test_theSilentDeviceNameIsTypedIntoAdvanced() {
        if (!TestServices.scriptSessions([{ app: 900, name: "chrome", active: true }])) {
            skip("the scripted machine is not available in this harness");
        }
        const page = makePage();
        compare(CrucibleController.nullSinkPresent, true, "the scripted machine has a Desktop Atmos device");
        const toggle = findChild(page, "advancedToggle");
        click(toggle);
        compare(findChild(page, "advancedSection").open, true, "a click opens Advanced");
        const input = findChild(page, "silentDeviceInput");
        verify(input);
        waitForRendering(page);
        click(input);
        verify(input.activeFocus, "the field takes the keys");
        input.selectAll();
        keyClick(Qt.Key_Delete);
        for (const ch of "Nowhere") keyClick(ch);
        keyClick(Qt.Key_Return);
        compare(CrucibleController.nullSinkName, "Nowhere");
        compare(stored("output/nullSinkName"), "Nowhere");
        // No endpoint on this machine is called that, so there is no silent
        // device any more, and the page's status says so.
        compare(CrucibleController.nullSinkPresent, false);
        click(toggle);
        compare(findChild(page, "advancedSection").open, false);
    }


    // --- the silent device, made by the application --------------------------------

    function test_createDeviceAndRemoveDeviceFromThePage() {
        if (!TestServices.scriptMachineThatMakesItsSilentDevice([{ app: 900, name: "chrome", active: true }])) {
            skip("the scripted machine is not available in this harness");
        }
        const page = makePage();
        compare(CrucibleController.nullSinkPresent, false);
        compare(CrucibleController.silentDeviceFromPackage, false);
        const note = findChild(page, "driverPackageNote");
        verify(note.visible && note.text.indexOf("Nothing to install") === 0, note.text);
        const create = findChild(page, "installDriverButton");
        compare(create.text, "Create device");
        verify(create.enabled, "the application can make its own device here");
        click(create);
        tryVerify(function() { return CrucibleController.driverMessage.indexOf("installed") === 0; }, 3000,
                  CrucibleController.driverMessage);
        verify(CrucibleController.driverMessage.indexOf("created Desktop Atmos") > 0, CrucibleController.driverMessage);
        compare(CrucibleController.driverBusy, false);
        tryCompare(CrucibleController, "nullSinkPresent", true);
        tryCompare(create, "visible", false);

        // And away again, from Advanced.
        click(findChild(page, "advancedToggle"));
        const remove = findChild(page, "removeDriverButton");
        compare(remove.text, "Remove device");
        click(remove);
        tryVerify(function() { return CrucibleController.driverMessage.indexOf("removed") === 0; }, 3000,
                  CrucibleController.driverMessage);
        tryCompare(CrucibleController, "nullSinkPresent", false);
        // Check again reads the machine and finds it still gone.
        click(byText(page, "Check again"));
        compare(CrucibleController.nullSinkPresent, false);
    }
}
