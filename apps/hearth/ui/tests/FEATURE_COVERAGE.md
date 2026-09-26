# Hearth window: feature coverage from the UI

Every user-facing feature of `apps/hearth/ui/qml` and the
`HearthController`/`NetworkController` surface it drives, mapped to the Qt
Quick Test cases (`apps/hearth/ui/tests/qml/tst_*.qml`) that exercise it.
Case names are `Suite::test_function` (the suite is the TestCase `name`).

Status:

- **UI**: driven from the UI (click, key, typed text, drop, dialog) against the real controller, with the outcome asserted.
- **logic**: exercised only by calling the controller, or by rendering a page without driving it.
- The four defects this work found are fixed; each now has a normal regression case. See "UI bugs found (fixed)".
- **none**: not exercised.

## How the suites stand in for hardware

- **Audio device**: `TestServices.useFakeRoom()` (`tests/test_room.cpp`) hands `HearthController` a fake room of three endpoints ("Test speakers" 6 ch default, "Test headphones" 2 ch, "Test receiver" 8 ch with AC-3/E-AC-3 passthrough) through `HearthController::set_test_outputs()` - a seam of the same shape as `CrucibleController::set_test_services()`. The REAL engine decodes real golden streams (`tests/golden/external-baseline/...`, `esp-idf/.../objects-mdct.ec3`) into it; the fake device has its own clock and records frames heard, peak level, the endpoint opened and how many times. So "it plays" is asserted on what the device received.
- **Network sink**: `TestServices.startTestSink()` runs `apps/hearth/testsink`'s real `Sink` in-process on loopback and hands it to `NetworkController`'s own `NetworkSinks` as a found service (`NetworkController::sinks_for_test()` + `NetworkSinks::on_found()`), which is the one step mDNS multicast would otherwise do. After that it is real: WebSocket dial, pairing with the code the sink prints, groups, player commands.
- **File dialogs**: under `-platform offscreen`, QtQuick.Dialogs shows its non-native dialog. The suites click the page's own button, which opens it, and then `accept()` it. The file is set as the dialog's selection before it opens, because the non-native dialog drops a selection made after it has opened.
- **Drag and drop**: `TestServices.dropFiles()` sends real DragEnter/DragMove/Drop events that carry file URLs to the window, so the page's own DropArea receives them.
- **Keys into Main.qml's window**: `TestServices.keyClick()` (`QTest::keyClick` on that `QWindow`). `TestCase.keyClick()` only ever reaches the test case's own window.

## Coverage

| # | Area | Feature | Before | After | Cases (after) |
|---|------|---------|--------|-------|---------------|
| 1 | Shell | Page switch (Play/Media/Speakers/Decoder/Network/Settings) | none | UI | Shell::test_headerPageSwitchShowsEachPage |
| 2 | Shell | Ctrl+1..Ctrl+6 page shortcuts | none | UI | Shell::test_ctrlDigitShortcutsSwitchPages |
| 3 | Shell | F1 opens Keyboard shortcuts | none | UI | Shell::test_f1AndTheHelpButtonOpenKeyboardShortcuts |
| 4 | Shell | `?` header button opens Keyboard shortcuts | none | UI | Shell::test_f1AndTheHelpButton..., Dialogs::test_shortcutsAboutLicencesChain |
| 5 | Shell | Escape stops the identify tone | none | UI | Shell::test_escapeStopsTheIdentifyTone |
| 6 | Shell | Header output summary (click / Space) opens the output picker | none | UI | OutputPicker::test_headerSummary..., Shell::test_outputSummaryOpensThePickerFromTheKeyboard |
| 7 | Shell | Header summary shows the output decision's reason | none | UI | OutputPicker::test_headerSummaryShowsTheOutputDecisionAndOpensThePicker |
| 8 | Shell | Theme/palette/text size applied at start and on change | none | UI | Shell::test_themePaletteAndTextSizeFollowTheSettings, SettingsPage::test_appearanceThemePaletteAndTextSize |
| 9 | Play | Add files... button (file dialog) queues files | logic | UI | Playback::test_addFilesDialogQueuesTheChosenFileWithItsProbedFacts |
| 10 | Play | Add folder... button (folder dialog) queues a folder | none | UI | Playback::test_addFolderDialogQueuesEveryStreamInTheFolder |
| 11 | Play | Drop files on the page | none | UI | Playback::test_droppedFilesAreQueuedAndAnUnplayableOneIsSkipped (and most Playback cases) |
| 12 | Play | Queue list reflects the queue, with probed facts per row | logic | UI | Playback::test_addFilesDialog... |
| 13 | Play | Click a queue row to play it | logic | UI | Playback::test_clickingAQueueRowPlaysThatItem |
| 14 | Play | Unplayable item: "not playable" pill + reason, skipped | none | UI | Playback::test_droppedFilesAreQueuedAndAnUnplayableOneIsSkipped |
| 15 | Play | Remove from queue (`removeAt`) | logic | logic | PlayQueue::test_removeAtShrinksTheQueue. **The page has no control for it**, so it cannot be driven from the UI |
| 16 | Play | Now playing card (title, "next: ..." hint) | none | UI | Playback::test_playButtonPlaysIntoTheDeviceAndTheMonitorFollows |
| 17 | Play | Levels meters at play time | none | UI | Playback::test_playButton... (one meter per slot, off the floor) |
| 18 | Play | Loudness card | none | UI | Playback::test_playButton... (momentary LUFS present while playing) |
| 19 | Play | This frame card | none | UI | Playback::test_playButton... ("access unit" shown) |
| 20 | Play | Objects card | none | UI | MediaPage::test_objectMetadataShowsOnTheMediaPageAndWhilePlaying |
| 21 | Play | Signal path card (you hear it on: the device) | none | UI | Playback::test_playButton... |
| 22 | Play | Signal path "Choose..." opens the output picker | none | UI | OutputPicker::test_signalPathChooseOpensThePicker |
| 23 | Transport | Play / Pause | none | UI | Playback::test_playButton..., Playback::test_pauseButtonPausesTheDeviceAndPlayResumesIt |
| 24 | Transport | Stop | none | UI | Playback::test_nextPreviousAndStopButtons |
| 25 | Transport | Next / Previous (and their enabled states) | none | UI | Playback::test_nextPreviousAndStopButtons |
| 26 | Transport | Position scrubber seeks (press pauses, release seeks and resumes) | none | UI | Playback::test_scrubberSeeksToWhereItIsReleased |
| 27 | Transport | Elapsed / duration readouts | none | UI | Playback::test_playButton... |
| 28 | Transport | Gapless toggle (click, Space) | none | UI | Playback::test_gaplessToggleInTheTransportBar |
| 29 | Transport | Volume slider and dB readout | none | UI | Playback::test_volumeSliderTurnsTheDeviceDown (device peak drops >26 dB) |
| 30 | Transport | Note / error text | none | UI | Playback::test_droppedFilesAreQueuedAndAnUnplayableOneIsSkipped |
| 31 | Engine | Item actually plays: device opened at the item's rate, frames heard, non-silent | none | UI | Playback::test_playButton..., OutputPicker::test_playHere... |
| 32 | Engine | Queue advances by itself at the end of an item, gaplessly (one output open) | none | UI | Playback::test_theQueueAdvancesByItselfAtTheEndOfAnItem |
| 33 | Output picker | Lists this computer's endpoints (default marked, summary line) | none | UI | OutputPicker::test_headerSummary... |
| 34 | Output picker | Select a row + Play here moves playback there | none | UI | OutputPicker::test_playHereMovesPlaybackToThePickedEndpoint |
| 35 | Output picker | Passthrough section lists passthrough-capable endpoints | none | UI | OutputPicker::test_headerSummary... |
| 36 | Output picker | "playing here" pill on the open endpoint | none | UI | OutputPicker::test_headerSummary..., test_playHere... |
| 37 | Output picker | Cancel closes without changing the output | none | UI | OutputPicker::test_cancelLeavesTheOutputWhereItWas |
| 38 | Output picker | Network page... switches to Network | none | UI | OutputPicker::test_networkPageButtonSwitchesToTheNetworkPage |
| 39 | Media | Showing picker inspects another queue item | none | UI | MediaPage::test_showingPickerInspectsAnotherItem |
| 40 | Media | Stream / bitstream / objects cards show the file | none | UI | MediaPage::test_showingPicker..., test_objectMetadata... |
| 41 | Media | Copy JSON | none | UI | MediaPage::test_copyAndExportJson (clipboard read back) |
| 42 | Media | Export JSON... writes the file | none | UI | MediaPage::test_copyAndExportJson (file parsed) |
| 43 | Speakers | Layout presets | none | UI | SpeakersPage::test_layoutPresetsAndTheAsTextField |
| 44 | Speakers | Layout "as text" field (and refusal of a bad one) | none | UI | SpeakersPage::test_layoutPresetsAndTheAsTextField |
| 45 | Speakers | Height speaker realisation | none | UI | SpeakersPage::test_heightsRealizationNeedsAHeightLayout |
| 46 | Speakers | Routing grid: arrow keys, Space commits | UI | UI | SpeakersRouting::test_arrowKeys..., test_spaceCommits... |
| 47 | Speakers | Routing grid: click a cell to patch (6 real output columns) | logic | UI | SpeakersPage::test_routingGridClicksPatchSpeakersToDeviceOutputs |
| 48 | Speakers | "Use the device's order" / "Clear" buttons | logic | UI | SpeakersPage::test_routingGridClicksPatchSpeakersToDeviceOutputs, SpeakersPage::test_clearRoutingButtonUnpatchesEverySlot |
| 49 | Speakers | Size large / small per speaker | none | UI | SpeakersPage::test_sizeTrimAndDelayPerSpeaker |
| 50 | Speakers | Trim / delay fields: a refused value reverts to the one in effect | none | UI | SpeakersPage::test_sizeTrimAndDelayPerSpeaker |
| 51 | Speakers | Delay field | none | UI | SpeakersPage::test_sizeTrimAndDelayPerSpeaker |
| 52 | Speakers | Identify button per speaker (Identify / Stop) | none | UI | SpeakersPage::test_identifyButtonAndLevel, Shell::test_escapeStops... |
| 53 | Speakers | Crossover presets and exact field | none | UI | SpeakersPage::test_crossoverPresetAndExactField |
| 54 | Speakers | Identify level | none | UI | SpeakersPage::test_identifyButtonAndLevel |
| 55 | Decoder | E-AC-3 / AC-4 sub-page switch | UI | UI | DecoderSettings::test_formatSwitch..., DecoderApplied::test_ac4SubPageSharesTheDownmixAndNotTheMode, DecoderAc4::test_presentationDialogue... (the page turns to AC-4 when an AC-4 item plays) |
| 56 | Decoder | Dynamic range mode | UI | UI | DecoderSettings::test_modeSegmentedControl..., DecoderApplied::* |
| 57 | Decoder | DRC cut / boost sliders | logic | UI | DecoderApplied::test_customModeSlidersAndCheckboxes |
| 58 | Decoder | Heavy compression / dialogue normalisation checkboxes | logic | UI | DecoderApplied::test_customModeSlidersAndCheckboxes |
| 59 | Decoder | RF ceiling field | logic | UI | DecoderApplied::test_rfCeilingFieldTakesATypedValueInRfMode |
| 60 | Decoder | Stereo fold (Lo/Ro, Lt/Rt) | UI | UI | DecoderSettings::test_stereoFold... |
| 61 | Decoder | Lt/Rt phase shift / mix LFE checkboxes | logic | UI | DecoderApplied::test_stereoAndTransformCheckboxes |
| 62 | Decoder | Dual mono, objects, JOC domain, concealment | UI | UI | DecoderSettings::test_dualMono... |
| 63 | Decoder | Fast inverse transform checkbox | logic | UI | DecoderApplied::test_stereoAndTransformCheckboxes |
| 64 | Decoder | This stream / Programme cards show the playing stream | none | UI | DecoderApplied::test_settingsReachWhatIsPlaying |
| 65 | Decoder | AC-4 sub-page's shared downmix / bad-frame controls (no operating mode: decision 12) | none | UI | DecoderApplied::test_ac4SubPageSharesTheDownmixAndNotTheMode, DecoderAc4::test_eachControlWritesItsSetting |
| 66 | Decoder | Settings applied to what is playing | none | UI | DecoderApplied::test_settingsReachWhatIsPlaying (RF is louder than Line at the device) |
| 67 | Network | Sink list shows a discovered sink | none | UI | NetworkPairing::test_discoveredSinkIsListedAndPairsWithTheCodeItShows (discovery injected, not mDNS) |
| 68 | Network | Look again (rescan) | none | UI | NetworkPairing::test_discoveredSink... (clicked; mDNS itself not exercised) |
| 69 | Network | Select a sink: pairing view; "Pair with this computer" starts the attempt (selecting alone does not) | none | UI | NetworkPairing::test_discoveredSink..., NetworkPairing::test_cancelEndsThePairingAttempt |
| 70 | Network | Type the sink's code, Pair: sink becomes paired | none | UI | NetworkPairing::test_discoveredSink... (the sink logs "paired with server") |
| 71 | Network | Cancel pairing | none | UI | NetworkPairing::test_cancelEndsThePairingAttempt |
| 72 | Network | Paired Hearth sink settings view (its outputs, crossover range) | logic | UI | NetworkPairing::test_pairedSinkSpeakersTabShowsTheSinkAndEditsReachIt |
| 73 | Network | Sink Speakers tab: disabled with the reason when the sink takes no settings; an edit reaches a sink that does | logic | UI | NetworkPairing::test_pairedSinkSpeakersTabIsDisabledWhenTheSinkTakesNoSettings, NetworkPairing::test_sinkThatTakesSettingsAppliesAnEditFromTheSpeakersTab (report shows "revision N · applied") |
| 74 | Network | Sink Decoder tab offers only what the sink lists | logic | UI | NetworkPairing::test_pairedSinkDecoderTabFollowsWhatTheSinkAccepts (test sink lists none, so all disabled) |
| 75 | Network | Sink report panel | logic | UI | NetworkPairing::test_pairedSinkDecoderTab... ("Nothing playing.", "0 bursts") |
| 76 | Network | New group, rename, delete | none | UI | NetworkPairing::test_groupsCreateRenameAddMemberAndDelete |
| 77 | Network | Group member: add, volume (heard by the sink), remove | none | UI | NetworkPairing::test_groupsCreateRenameAddMemberAndDelete |
| 78 | Settings | Gapless checkbox | none | UI | SettingsPage::test_playbackCheckboxesAndFailurePolicy |
| 79 | Settings | Resume queue checkbox | none | UI | SettingsPage::test_playbackCheckboxesAndFailurePolicy |
| 80 | Settings | When an item fails (skip / stop) | none | UI | SettingsPage::test_playbackCheckboxesAndFailurePolicy |
| 81 | Settings | Network name field | none | UI | SettingsPage::test_networkNameAndDiscovery |
| 82 | Settings | Look for Sendspin players checkbox | none | UI | SettingsPage::test_networkNameAndDiscovery |
| 83 | Settings | Pairing records list and Forget | none | UI | NetworkPairing::test_pairingRecordShowsInSettingsAndForgetRemovesIt (after a real pairing) |
| 84 | Settings | Theme / palette / text size | none | UI | SettingsPage::test_appearanceThemePaletteAndTextSize |
| 85 | Settings | Language | none | UI | SettingsPage::test_languageChoice |
| 86 | Settings | Save diagnostics... | none | UI | SettingsPage::test_saveDiagnosticsWritesTheReport (file read back) |
| 87 | Dialogs | Keyboard shortcuts dialog, About... chain, Close | none | UI | Dialogs::test_shortcutsAboutLicencesChain |
| 88 | Dialogs | About: version, Licences... chain, Close | none | UI | Dialogs::test_shortcutsAboutLicencesChain |
| 89 | Dialogs | Licences: notices text, Close | none | UI | Dialogs::test_shortcutsAboutLicencesChain |
| 90 | Dialogs | First run shows once; Not now / Open Speakers | none | UI | Dialogs::test_firstRunShowsOnceAndNotNowRemembersIt, test_firstRunOpenSpeakersGoesToTheSpeakersPage |
| 91 | Output | Play to a network group (`selectOutputGroup`) | none | UI | OutputPicker::test_groupRowPinsPlaybackToTheGroup (the picker's group row, Play here, then a device row moves it back) |
| 92 | Decoder | AC-4 presentation picker and table (the one playing in bold) | none | UI | DecoderAc4::test_presentationDialogueAndDescriptionAreHeard (picked by keyboard; the description goes with presentation 1) |
| 93 | Decoder | AC-4 dialogue enhancement slider | none | UI | DecoderAc4::test_eachControlWritesItsSetting, DecoderAc4::test_levelAndEnhancementAreHeardAsTheirFormulasSay (C up by the gain, to the stream's 9 dB cap, measured at the device) |
| 94 | Decoder | AC-4 dialogue level slider | none | UI | DecoderAc4::test_eachControlWritesItsSetting, DecoderAc4::test_presentationDialogue... (the dialogue alone, to the stream's 6 dB maximum) |
| 95 | Decoder | AC-4 audio description checkbox and its level | none | UI | DecoderAc4::test_eachControlWritesItsSetting, DecoderAc4::test_presentationDialogue... (the presentation carrying it plays; its level heard) |
| 96 | Decoder | AC-4 device list (the DRC decoder mode) | none | UI | DecoderAc4::test_eachControlWritesItsSetting (all six by keyboard; each mode's compression is held against the decoder's own by the [hearth][ac4] cases) |
| 97 | Decoder | AC-4 output level slider and dialogue normalisation checkbox | none | UI | DecoderAc4::test_eachControlWritesItsSetting, DecoderAc4::test_levelAndEnhancement... (2^((Lout - dialnorm) / 6) at -31 and -17 dBFS) |
| 98 | Decoder | AC-4 downmix, LFE in the fold, and the stream's preferred downmix | none | UI | DecoderAc4::test_downmixIsHeardWithTheStreamsGains (Lo/Ro, Lt/Rt and the LFE at the stream's gains, each tone against its coded level) |
| 99 | Decoder | AC-4 page readouts (dB values beside the sliders, "Nothing AC-4 is playing.") | none | UI | DecoderAc4::test_aTheBannerIsGoneAndEveryCardIsLive, DecoderAc4::test_eachControlWritesItsSetting |
| 100 | Media | AC-4 item: Stream, Presentations and Metadata cards, and it plays | none | UI | MediaPage::test_ac4ItemIsDescribedAndPlays |

### Totals

| | Before (4 suites, 18 cases) | After (15 suites, 80 cases) |
|---|---|---|
| UI | 5 | 99 |
| logic only | 15 | 1 (row 15: no UI control exists) |
| none | 80 | 0 |

Gaps that remain inside covered rows:

- The sink's own trim, delay, routing and identify edits are not driven: even the accept-settings test sink manages none of them (`management.routing` false, trim range 0..0, `identify` false), so only the layout edit is shown reaching a sink.
- The group mute and group volume sliders are not driven.
- The Only-on-sink panel is rendered but its text is not read.

## UI bugs found (fixed)

Each was first shown failing on the unfixed code, then fixed.

1. **ac3hearth segfaulted on start (Qt 6.9.3, qmlcachegen-compiled QML).**
   - Cause: `Network.qml`'s `Loader.sourceComponent` read members off a local (`const group = NetworkController.selectedGroup; if (group && group.id ...)`). qmlcachegen compiled that as a value-type lookup on QVariant, passing `QMetaType::fromName("QVariant").metaObject()` (null) to `AOTCompiledContext::initGetValueLookup()`, which dereferenced it.
   - Fix: the binding reads `NetworkController.selectedGroup.id`, `.selectedSink.id` and `.selectedSink.badge` straight off the singleton. The generated `Network_qml.cpp` now has no QVariant value lookups. The behaviour is unchanged, because an empty map's `.id` is undefined. A comment at the binding says why it must not be "simplified" back.
   - Check: `QT_QPA_PLATFORM=offscreen build/gui-cov/bin/ac3hearth` under `timeout 5` now exits 124 (still running); before the fix it exited 139.
   - The `QML_DISABLE_DISK_CACHE=1` workaround is gone from `tests/CMakeLists.txt`, so every suite runs the compiled QML.
   - Regression: `NetworkPageAot::test_networkPageBuildsWithNothingSelected` and `test_networkPageBuildsWithAGroupSelected` (unskipped; both segfaulted before the fix, as did `shell`).
2. **Speakers "Clear" did nothing while a device was open.**
   - Cause: `HearthController::clearRouting()` posted a patch sized for zero outputs, which `PcmOutput::set_routing()` refuses.
   - Fix: it now posts every slot unassigned for the open device's own output count (`Routing::from_outputs(all -1, routingOutputs)`).
   - Regression: `SpeakersPage::test_clearRoutingButtonUnpatchesEverySlot`.
3. **The sink Speakers tab offered edits that were silently dropped.**
   - Cause: the sink's state did not list the Settings command, but the controls were enabled anyway.
   - Fix, in NetworkController:
     - `sinkSpeakerSettings.settingsAccepted` says whether the sink's `ac3forge_state` lists the Settings command.
     - The report says "The sink does not take settings from Hearth." instead of "Nothing sent yet.".
     - A push that is refused anyway is recorded (`note_push()`) and reported as "not sent: ...".
   - Fix, in QML: `NetworkSinkSpeakers.qml` disables every control and shows the reason (`networkSinkSettingsBlocked`). `NetworkSinkDecoder.qml`'s `accepts()` also requires the command.
   - The test sink gained `SinkOptions::accept_settings` (off by default), so a sink that does take settings can be shown receiving an edit.
   - Regressions: `NetworkPairing::test_pairedSinkSpeakersTabIsDisabledWhenTheSinkTakesNoSettings` and `NetworkPairing::test_sinkThatTakesSettingsAppliesAnEditFromTheSpeakersTab`.
4. **A refused trim or delay value stayed displayed after focus left.**
   - Fix: both fields now carry the same `Binding on text { when: !activeFocus }` restore as the page's layout and crossover fields. The page reverts rather than clamps, so this is consistent with it.
   - Regression: `SpeakersPage::test_sizeTrimAndDelayPerSpeaker`.

Not bugs, but worth knowing:

- An item's facts (codec, channels, duration) are read when the engine opens it, not when it is added. A freshly queued row shows only its file name until it has played or been prepared as the next item.
- In the non-native dialog, `FileDialog.accept()` keeps only a selection that was made before `open()`.

## C++ coverage (secondary measure)

These are line figures from `build/gui-cov`, with the Hearth `.gcda` files cleared before each run:

| | Before | After |
|---|---|---|
| `apps/hearth/engine` | 835 / 5184 lines, 16.1% (4 original suites) | 3105 / 5184 lines, 59.9% (all 14 suites) |

- `apps/hearth/ui/*.cpp` (the controllers) is not instrumented in this tree, because `ac3::coverage` is not applied to the Qt app or test targets. So the controllers have no gcov figure.
- `/opt/gui-cov.sh` searches the whole repository for `.gcda` files, including other build trees. Its figure for `apps/hearth` (80%+) mostly comes from `ac3tests` runs elsewhere. To count only these suites, add `build/gui-cov/apps/hearth` as gcovr's search path.

## Running

```
/opt/gui-build.sh ac3hearth_qmltests
ctest --test-dir build/gui-cov -R ac3hearth_qml_tests_ -j2 --output-on-failure
```

All 14 suites take about 11 s wall-clock at `-j2`. The slowest is Playback, at about 6 s.
