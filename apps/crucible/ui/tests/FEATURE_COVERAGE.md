# Crucible: feature coverage from the UI

This file maps each user-facing feature of the Crucible window (`ui/qml`) and
the `CrucibleController` surface it drives to the Qt Quick Test cases
(`ui/tests/qml/tst_*.qml`) that exercise it.

- **UI**: a case drives the feature through the window's own control (a
  click, a key, a drag, a combo-box pick, a dialog's OK) and checks the real
  outcome: the engine's answer through the controller's poll, the settings
  store on disk, or the page's own text.
- **Logic**: covered only by writing the controller property or calling the
  invokable or QML function directly, with no control pressed.
- **None**: not covered.

"Scripted machine" means the fake platform seams from `tests/crucible`
(sessions, devices, default device, full-screen, silent device) that
`ui/tests/qml_test_main.cpp` installs under the real controller and the real
engine. Because the default output there is a fake, the cases that move it
("Send applications…", Restore, first-run Send, the tray entries) are safe to
press. On a real machine they would change the developer's own sound settings.

Case names are given as `suite::case` (the suite name is the file name
without `tst_`).

## Summary

| | Before | After |
|---|---|---|
| Features in the inventory | 89 | 89 |
| Covered from the UI | 28 | 85 |
| Logic only | 24 | 1 |
| Not covered | 37 | 3 |
| `crucible_controller.cpp` line coverage (gcov, Qt Quick suites only) | 72.7 % (588/809) | 93.0 % (752/809) |
| Qt Quick suites / cases | 11 / 93 | 16 / 141 |
| Suite wall time (`ctest -j2`) | about 11 s | about 15 s |

The line-coverage and wall-time rows date from when the suites were added
(2026-09-24). `crucible_controller.cpp` and the suites have not changed since,
and the two figures have not been measured again.

## Window, header and status strip (`Main.qml`)

| # | Feature | Before | After | Cases |
|---|---|---|---|---|
| 1 | Window opens on the Room page | UI | UI | shell::test_windowOpensOnTheRoomAndSwitchesPages |
| 2 | Header page switch (Room / Signal path / Settings) | Logic | UI | shellworkflow::test_theHeaderSwitchesPagesAndThePillOpensTheSignalPath |
| 3 | Ctrl+1/2/3 page shortcuts | UI | UI | keyboard::test_ctrlNumberSwitchesPages (skips if the offscreen window never becomes active) |
| 4 | F1 (HelpContents) opens About | None | UI | keyboard::test_ctrlNumberSwitchesPages |
| 5 | Status pill opens Signal path | None | UI | shellworkflow::test_theHeaderSwitchesPagesAndThePillOpensTheSignalPath |
| 6 | Pill text: where applications play, bed or objects, what is heard | None | UI | shellworkflow::test_thePillSaysWhatIsHeardAndWhereApplicationsPlay |
| 7 | "?" button opens About | Logic (`openAbout()`) | UI | shellworkflow::test_aboutAndLicencesOpenAndCloseFromTheirButtons |
| 8 | About → Licences… stacks Licences, Close on each | Logic | UI | shellworkflow::test_aboutAndLicencesOpenAndCloseFromTheirButtons; about::test_licencesOpenFromAbout |
| 9 | Escape closes About | UI | UI | keyboard::test_escapeClosesTheAboutDialog |
| 10 | Licences text is this build's NOTICES.txt | UI | UI | about::* |
| 11 | Status strip Start / Stop button | None | UI | shellworkflow::test_theStatusStripStopsAndStartsTheEngine |
| 12 | Status strip shows the running state or the refusal reason | UI | UI | shell::test_statusStripReportsTheEngine, shell::test_statusStripSaysWhyTheEngineCouldNotStart |
| 13 | Theme and palette applied from the setting | Logic | UI | shell::test_themeFollowsTheSetting; settingsworkflow::test_appearanceChoicesAreStoredAndApplied |
| 14 | Text size scales the whole window | Logic | UI | shellworkflow::test_theTextSizeSettingScalesTheWholeWindow; settingsworkflow::test_appearanceChoicesAreStoredAndApplied; keyboard::test_textScaleGrowsTheControls |
| 15 | Close hides the window while it stays in the tray | UI | UI | shell::test_closingHidesWhileTrayResident |
| 16 | Close quits (and restores the default) without a tray | None | None | `quit()` ends the process the suite runs in |
| 17 | Screen-reader announcer (once per change, not per poll) | UI | UI | accessibility::test_announcerRelaysStateChangesOnceNotPerPoll |
| 18 | Right-to-left mirroring; the plan keeps L on the left | UI | UI | shell::test_rightToLeftMirrorsTheHeader, shell::test_roomPlanKeepsLeftOnTheLeft |
| 19 | Diagnostics ring carries what the engine and the window said | UI | UI | shell::test_diagnosticsReportCarriesTheEngine |

## Tray (`Main.qml`, Qt Labs Platform)

A headless session never shows the menu, so each entry is fired through its
`triggered()` signal, which runs the entry's own `onTriggered`.

| # | Feature | Before | After | Cases |
|---|---|---|---|---|
| 20 | Tray published only where the platform has one, with a reason otherwise | UI | UI | platform::test_theTraySeamNeverRefusesWithoutSayingWhy, platform::test_linuxFollowsTheDesktopsTray |
| 21 | Flat menu (no submenu: the Linux D-Bus crash) | UI | UI | platform::test_theTrayMenuNestsNoSubmenu |
| 22 | Pin entries set the pin; heading follows; the engine switches | None | UI | shellworkflow::test_theTrayMenusPinsFollowAndSetThePin |
| 23 | Headphones entry only where the object renderer is | None | UI | shellworkflow::test_theTrayMenusPinsFollowAndSetThePin |
| 24 | Move default output / Restore entries | None | UI | shellworkflow::test_theTrayMovesAndRestoresTheDefaultOutput (scripted machine) |
| 25 | Open the room / Settings… / About… entries | None | UI | shellworkflow::test_theTrayOpensTheRoomSettingsAndAbout |
| 26 | Clicking the tray icon brings a hidden window back | None | UI | shellworkflow::test_aHiddenWindowComesBackWhenTheTrayIconIsClicked |
| 27 | Quit entry | None | None | Ends the process. Its presence is asserted. |

## First run (`FirstRunDialog.qml`)

| # | Feature | Before | After | Cases |
|---|---|---|---|---|
| 28 | Opens once on a fresh store and never again | UI | UI | firstrun::test_opensOnceOnAFreshStore |
| 29 | Text names the platform's device, advice and blocker | UI | UI | firstrun::test_textNamesThePlatformsDevice, firstrun::test_withNoSilentDeviceSendIsGreyedAndSaysWhy |
| 30 | Send offered only where the default moves | UI | UI | firstrun::test_sendIsOfferedOnlyWhereTheDefaultMoves |
| 31 | Send moves the default and counts as seen | None (unsafe on a real machine) | UI | firstrun::test_sendMovesTheDefaultAndCountsAsSeen (scripted machine) |
| 32 | "Do this every time" check | UI (`toggled()`) | UI (click) | firstrun::test_moveOnLaunchCheckWritesTheSetting, firstrun::test_theMoveOnLaunchCheckThenSendAreBothKept |
| 33 | Not now / Open Settings / Escape all count as seen | UI | UI | firstrun::test_notNowLeavesTheDefaultAlone, firstrun::test_openSettingsSwitchesPage, firstrun::test_anyCloseCountsAsSeen |
| 34 | Launch-time move once acknowledged, and not otherwise | None | UI | firstrun::test_anAcknowledgedLaunchMovesTheDefaultWhenAsked, firstrun::test_anAcknowledgedLaunchLeavesTheDefaultAloneOtherwise, firstrun::test_migratedLaunchWaitsForTheDialog |
| 35 | Suppressed for `--shot` captures | UI | UI | firstrun::test_suppressedForCaptures |
| 36 | No silent device: Send greyed, "No silent device yet" | None | UI | firstrun::test_withNoSilentDeviceSendIsGreyedAndSaysWhy |

## Room (`RoomPage.qml`, `RoomView.qml`, `AppRow.qml`, `BedChip.qml`, `RoomKeys.qml`)

| # | Feature | Before | After | Cases |
|---|---|---|---|---|
| 37 | Applications appear in the rail (names, sound-first order, counts) and follow arrivals and exits | Logic (machine-dependent, skipped here) | UI | roomworkflow::test_applicationsAppearInTheListAndTheBed |
| 38 | Every unplaced application has a bed chip | None | UI | roomworkflow::test_applicationsAppearInTheListAndTheBed |
| 39 | Row badges (idle, full-screen, …) | Logic (fake entry) | UI | roomworkflow::test_applicationsAppearInTheListAndTheBed, roomworkflow::test_aFullScreenApplicationIsLockedInTheBed; accessibility::test_appRowExposesSelectionAndDetail |
| 40 | Click a row to select it | None | UI | roomworkflow::* (`select()`) |
| 41 | List Up/Down selects, Enter places and hands keys to the room | UI | UI | keyboard::test_listSelectsWithArrowsAndEnterPlacesThenTheRoomTakesTheKeys, keyboard::test_theListRowFollowsTheSelectionAndNotTheOtherWayAbout |
| 42 | Place in the room / Send to bed button | None | UI | roomworkflow::test_theCardPlacesMovesCentresAndReturnsAnApplication |
| 43 | Centre button | None | UI | roomworkflow::test_theCardPlacesMovesCentresAndReturnsAnApplication |
| 44 | Quick placements (in front … rear right, overhead) | None | UI | roomworkflow::test_theCardPlacesMovesCentresAndReturnsAnApplication (left, rear right, overhead; all nine share one delegate) |
| 45 | Plan marker follows the engine's position | None | UI | roomworkflow::test_theCardPlacesMovesCentresAndReturnsAnApplication; accessibility::test_markerDescribesItsPositionFromLiveData |
| 46 | Drag a marker in the plan (clamped at the walls) | None | UI | roomworkflow::test_aMarkerDraggedInThePlanMovesAndADoubleClickReturnsIt |
| 47 | Double-click a marker returns it to the bed | None | UI | roomworkflow::test_aMarkerDraggedInThePlanMovesAndADoubleClickReturnsIt (regression for defect 1, fixed). |
| 48 | Drag in the elevation (depth + height) | None | UI | roomworkflow::test_theElevationDragMovesDepthAndHeight |
| 49 | Drag a bed chip into the plan: placed where it lands and selected | None | UI | roomworkflow::test_aBedChipDraggedIntoThePlanIsPlacedWhereItLands |
| 50 | Enter / Space on a bed chip places it and hands keys to the room | Logic (fake entry, signal only) | UI | roomworkflow::test_enterOnABedChipPlacesItAndHandsTheKeysToTheRoom; keyboard::test_bedChipPlacesOnEnter |
| 51 | Split / Mono button | Logic (`setSplit`) | UI | roomworkflow::test_splitPairSidesDragOnTheirOwnAndStandardStereoResetsThem |
| 52 | Drag one object of a split pair; Standard stereo resets it | Logic (`positionSide`/`resetPair`) | UI | roomworkflow::test_splitPairSidesDragOnTheirOwnAndStandardStereoResetsThem |
| 53 | Size track: click and drag (clamped) | Logic (`setSize`) | UI | roomworkflow::test_theSizeTrackSetsTheObjectsExtent |
| 54 | Size track keys | UI | UI | keyboard::test_sizeSliderRespondsToKeys |
| 55 | Room keys: arrows, Shift/Ctrl steps, clamp, Home, Enter, Delete, ± | UI (fake entry) | UI (through the engine) | keyboard::test_roomKeys*; roomworkflow::test_roomKeysMoveHeightRecentreResizeAndReturnThroughTheEngine |
| 56 | Full-screen application locked in the bed (card, chip, drag, keys) | Logic (fake entry) | UI | roomworkflow::test_aFullScreenApplicationIsLockedInTheBed (scripted full-screen seam); keyboard::test_roomKeysRefuseAFullScreenApplicationAndSayWhy |
| 57 | Full-screen rule note / platform reason | UI | UI | room::test_fullscreenRuleNoteSaysWhetherTheRuleApplies (engine-dependent half skips here) |
| 58 | Silent and background applications follow their Settings checks | None | UI | roomworkflow::test_silentAndBackgroundApplicationsFollowTheirSettings |
| 59 | Listing rule in the platform's words | UI | UI | platform::test_theListingRuleIsThePlatformsOwn |
| 60 | Plan / 3D switch, remembered in the store | Logic (`page.threeD`) | UI | roomworkflow::test_theRoomViewSwitchPersistsAndTheThreeDViewTakesADrop |
| 61 | 3D view loads; reference layout follows the stream and the setting | Logic | UI | roomworkflow::test_theRoomViewSwitchPersistsAndTheThreeDViewTakesADrop; room::test_threeDToggleLoadsTheViewWhenBuilt |
| 62 | Drop a bed chip on the 3D room (ray to the floor) | None | UI | roomworkflow::test_theRoomViewSwitchPersistsAndTheThreeDViewTakesADrop. The camera's projection works under the software backend. |
| 63 | 3D: drag a card, Shift/right-drag for height, orbit, wheel zoom, hover names | None | None | Needs `View3D.pickAll`, which needs a rendered scene. Under `QT_QUICK_BACKEND=software` Qt Quick 3D does not render ("not functional in such an environment"). |
| 64 | Signal-path rail "Choose…" asks for the Output page; middle station counts placements | None | UI | roomworkflow::test_chooseOnTheSignalPathRailAsksForTheOutputPage |
| 65 | Tab order through the room page | UI | UI | keyboard::test_tabWalksTheRoomPageInReadingOrder |

## Signal path (`OutputPage.qml`, `SignalPath.qml`)

| # | Feature | Before | After | Cases |
|---|---|---|---|---|
| 66 | Endpoint table lists what the probe found; silent row cannot be heard; default cannot be sent to | Logic (empty list only) | UI | signalpath::test_theTableListsWhatTheProbeFound; accessibility::test_endpointRowsNameTheirEndpointInEveryButton |
| 67 | Pin chosen from the combo box changes the mode the engine settles on, and is stored | Logic (`pinned =`) | UI | signalpath::test_aPinChosenFromTheBoxChangesWhatIsHeard |
| 68 | Pin nobody can carry falls back and says why | None | UI | signalpath::test_aPinNoEndpointCanCarryFallsBackAndSaysWhy |
| 69 | Headphones pin only where the renderer is, with the reason | UI | UI | output::test_pinOffersHeadphonesOnlyWhereTheRendererIs |
| 70 | "Hear it here" chooses an endpoint; "Automatic" hands it back | None | UI | signalpath::test_hearItHereChoosesAnEndpointAndAutomaticHandsItBack |
| 71 | Station "Send applications to …" and Restore | None | UI | signalpath::test_sendApplicationsToTheSilentDeviceAndRestoreFromTheStation |
| 72 | Row "Send applications here" (any endpoint) | None | UI | signalpath::test_sendApplicationsHereMovesTheDefaultToAnyRow |
| 73 | Refused move or restore: message shown, sound settings opened | None | UI | signalpath::test_aRefusedMoveSaysWhyAndOpensTheSoundSettings, signalpath::test_aRefusedRestoreSaysWhyAndLeavesTheSilentDevice |
| 74 | Send creates the silent device first where the application makes it | None | UI | signalpath::test_sendCreatesTheSilentDeviceFirstWhereTheApplicationMakesIt |
| 75 | No silent device: move greyed, warning shown | None | UI | signalpath::test_aMissingSilentDeviceGreysTheMoveAndSaysSo |
| 76 | Open Sound settings / Re-probe buttons | None | UI | signalpath::test_openSoundSettingsAndReprobeButtons |
| 77 | Codec bypass check reaches the engine (`codecBypassed`) | UI (`toggled()`) | UI (click) | signalpath::test_theBypassCheckIsClickedThroughToTheEngine; output::test_bypassCheckDrivesTheController |

## Settings (`SettingsPage.qml`)

| # | Feature | Before | After | Cases |
|---|---|---|---|---|
| 78 | Silent-device status card and platform note | UI | UI | settings::test_pageShowsTheDriverStateAndFolder, platform::test_theSilentDeviceIsNamedAndExplainedByThePlatform |
| 79 | Create device / Remove device / Check again (the application's own device) | Logic | UI | settingsworkflow::test_createDeviceAndRemoveDeviceFromThePage (scripted silent device) |
| 80 | Install / Remove driver from a driver folder (Windows) | Logic | Logic | settings::test_driverButtonsRefuseWithoutAPackage. The folder row is hidden on Linux, and a real install is elevated. |
| 81 | Advanced disclosure; silent-device name typed and stored | UI (Space only) | UI | settingsworkflow::test_theSilentDeviceNameIsTypedIntoAdvanced; keyboard::test_advancedDisclosureTogglesWithSpace |
| 82 | Signing key: Browse… → dialog → remembered path; Clear | Logic (`loadKey`) | UI | settingsdialogs::test_aKeyChosenInTheDialogIsRememberedAndClearedFromThePage |
| 83 | Latency, bitrate, split-stereo; stored; the stream restarts and keeps running | Logic | UI | settingsworkflow::test_latencyAndBitrateAreStoredAndRestartTheStream |
| 84 | Theme, palette, text size, 3D layout segments; stored; a new page shows them | Logic | UI | settingsworkflow::test_appearanceChoicesAreStoredAndApplied |
| 85 | Language box switches and retranslates the window, RTL, back to System | Logic (LanguageManager API) | UI | settingsworkflow::test_theLanguageBoxSwitchesTheWindowAndHandsBackToTheSystem; language::* |
| 86 | Move on launch / Keep running (greyed without a tray) checks, stored | UI (keep-running state) | UI | settingsworkflow::test_behaviourChecksAreStored; platform::test_theKeepRunningRowFollowsTheTraySeam |
| 87 | Show silent apps / Show background processes checks | None | UI | roomworkflow::test_silentAndBackgroundApplicationsFollowTheirSettings |
| 88 | Save diagnostics… → dialog → file written or reason shown | Logic (`exportDiagnostics`) | UI | settingsdialogs::test_saveDiagnosticsWritesTheFileThePageSuggests; settings::test_exportWritesAFile, settings::test_diagnosticsReportWithholdsTheKeyPath |
| 89 | Settings persist to the store a relaunch reads | Logic (controller getters) | UI | every `settingsworkflow`/`signalpath`/`roomworkflow` case that reads `TestServices.storedSetting` |

## Cases without a feature row of their own

36 of the 141 cases are not named in a row above. They check a property that
sits under a feature (an accessible role, a default, a lookup rung, one
platform's answer) rather than a feature of its own. The `about` and `language`
suites are covered by their wildcards in rows 10 and 85. Cases that start the
engine skip when it does not start, and the ones that need an application
skip when the machine has no application with an audio session. A case that
belongs to one platform skips on the others.

| Case | What it checks |
|---|---|
| accessibility::test_bedChipIsAButtonNamedForItsApplication | A bed chip has the Button role and its name holds the application's name. Its description changes when the application is full-screen. |
| accessibility::test_stationsAreNamedGroupsAndCarryTheirWarning | Signal-path stations have the Grouping role. The name holds the station's kicker and title, the warning comes before the detail in the description, and the first station's warning follows where applications play. |
| accessibility::test_listAndControlsCarryNames | The applications list has the List role and a name. The pin, bitrate and language boxes are named. The driver folder field is named by its row's label, and the Advanced toggle is a Button whose description changes with its state. |
| accessibility::test_sizeSliderReportsARange | The size track has the Slider role, runs 0 to 1 in steps of 0.05, and with nothing selected is out of the tab chain and has no description. |
| accessibility::test_announceKeepsTheLastMessageAndTicksItsSerial | An empty announcement says nothing. Each announcement keeps the last message and moves the serial, including the same text twice. It is polite unless it is asked to interrupt. |
| accessibility::test_themePalettesMeetTheContrastFloors | For each of the three palettes in light and dark, text on the background is at least 7:1. Muted text, accent ink, the focus ring and control borders are at least 4.5:1, 4.5:1, 3:1 and 3:1 on both the background and the surface. The label on an accent fill is at least 3:1 and is the better of the palette's two ends. |
| accessibility::test_contrastCompositesTheAlphaItIsGiven | The contrast function composites a colour's alpha before it measures, so muted text scores lower than solid text. |
| icons::test_iconNameResolvesThroughTheTheme | An icon name resolves through the fixture theme (Linux only). |
| icons::test_desktopEntryByExec | With no icon name and no application id, the .desktop entry whose Exec names the binary supplies the icon, ahead of a NoDisplay entry with the same Exec (Linux only). |
| icons::test_desktopEntryByWmClass | A .desktop entry whose StartupWMClass equals the application's name supplies the icon (Linux only). |
| icons::test_desktopEntryByAppId | An application with only an application id finds the .desktop file of that name (Linux only). |
| icons::test_themeByBinaryName | With no entry naming the application, an icon in the theme under the binary's name is used (Linux only). |
| icons::test_unknownApplicationKeepsTheMonogram | An application that no lookup matches shows a two-letter monogram ("No" for "Nothing") and no icon. Runs on every platform. |
| icons::test_oneIdentityAtTwoSizes | One icon identity at 28 and 160 pixels reaches an image at both sizes, and each item keeps its size (Linux only). |
| keyboard::test_buttonsAndChecksActivateFromTheKeyboard | A button is in the tab chain and Space and Return each activate it. Disabled, it leaves the tab chain and ignores Space. Space toggles a check. |
| keyboard::test_segmentedControlMovesWithArrowHomeAndEnd | Left, Right, Home and End move the chosen segment, and both arrows wrap at the ends. |
| keyboard::test_focusRingShowsOnlyWithActiveFocus | Of two buttons, only the one holding the focus shows its ring. |
| keyboard::test_focusRingIsOnEveryKindOfControl | A check, a bed chip and the room's key scope each carry a ring that shows only while they hold the focus. |
| output::test_pinnedModeRoundTrips | The controller reads back each of the seven pin values (`atmos`, `ddplus`, `dd`, `pcm`, `headphones`, `stereo`, `auto`) as written. |
| output::test_endpointTableIsAListEvenWithNoEngine | Before the engine starts, the endpoint table is an empty list, the mode key is empty and the engine is not running. |
| output::test_pageRendersWithTheEngineStopped | The Signal path page renders at a non-zero size with the engine stopped. |
| platform::test_windowsFollowsTheNotificationArea | Windows only. The controller's tray answer matches Qt's, and where there is no tray the reason names the notification area. |
| platform::test_macosFollowsTheMenuBar | macOS only. The tray answer matches Qt's, and where there is none the reason names the menu bar and says that closing the window quits. |
| platform::test_macosNeedsNoSilentDeviceAndMovesNothing | macOS only. No silent device is needed, none comes from a package, none can be created, and the default output is not moved. The advice says there is nothing to install, mentions the tap, and never mentions a driver. |
| platform::test_macosCannotAnswerTheFullScreenQuestion | macOS only. The diagnostics report's "full-screen rule" row starts with "unavailable" and gives a reason. |
| platform::test_thePlatformSaysWhetherItsDeviceComesFromAPackage | A silent device is needed on every platform except macOS. On Linux it is not from a package and the application can create it. On Windows it is from a package and cannot be created. No driver action is in progress. |
| room::test_emptyRoomWithTheEngineStopped | With the engine stopped there are no applications, none placed and none in the bed, and the Room page renders. |
| room::test_runningRoomCountsAddUp | Placed plus bed equals the number of applications, and each has an id, a name and a slot of -1 or 0 to 9. Commands for an unknown application are ignored. |
| room::test_splitTakesTwoSlotsAndMonoGivesOneBack | Split gives an application a width of 2 and a slot, mono gives it a width of 1, and the placed count stays the same. An object's size reads back as set and is clamped to 1. |
| room::test_placementRoundTripsForALiveApplication | Placing an application at (0.25, 0.75) reads back within 0.02, and returning it puts it back in the bed. |
| room::test_pairSidesPlaceOnTheirOwnAndResetToTheSpread | A split pair starts at the standard spread. Placing one side makes the pair custom, and resetting the pair returns it to the standard spread. |
| settings::test_defaults | A fresh store gives pin `auto`, low latency off, codec bypass off, bitrate 0, theme `system`, palette `signal`, silent-device name "Crucible Silent Output", keep running on, move on launch off, no key path and the first-run dialog not yet seen. |
| settings::test_settingsRoundTripAndNotify | Written settings read back and each write emits `settingsChanged`. Writing the same value again does not. |
| settings::test_signingKeyPathIsRememberedNotTheKey | Loading a key file stores a local path (not a `file:` URL), and clearing the key empties it. |
| settings::test_driverBlockReadsTheMachine | The silent-device facts the page shows are the right types. Where no silent device is needed, none is packaged or creatable. A driver folder is set and no driver action is in progress. |
| settings::test_pageCarriesTheDiagnosticsButton | The "Save diagnostics…" button exists and is enabled. The outcome note is visible only once an export has run. |

## Still not covered, and why

1. **Close without a tray quits, and the tray's Quit** (rows 16 and 27).
   `CrucibleController::quit()` calls `QCoreApplication::quit()`, which ends
   the test process, so `quit()` and `restore_on_quit()` are the main
   uncovered controller lines. `stop()` never touching the default is
   asserted instead (shell::test_stopNeverTouchesTheDefault).
2. **3D card drag, height drag, orbit, zoom and hover** (row 63). These need
   Qt Quick 3D picking, which needs a rendered scene. The suites run with
   `QT_QUICK_BACKEND=software`, where Qt Quick 3D does not render. A drop onto
   the 3D room does work, because it only uses the camera's projection.
3. **Windows driver-folder install and remove** (row 80). The folder row is
   hidden on Linux, and a real install needs elevation. The Linux
   create/remove path is covered.
4. **The engine-dependent halves of `room.qml` on a host with no audio
   server**. The host the suites were written on had no PipeWire daemon, so
   `start()` over the machine refuses and those cases skip, as designed. The
   same flows run over the scripted machine in `roomworkflow`.
5. **`ui/main.cpp`** (0 %). This is the application entry point
   (`--shot`/`--page`, demo-store migration, tray wiring). It is not linked
   into the Qt Quick test binary.

## UI defects found (both fixed)

1. **Double-clicking a marker did not return the application to the bed**,
   although the bed tray's hint says it does. A double-click arrives as press,
   release, press, doubleClicked, release; the second press set
   `marker.dragging`, so the final release sent the marker's position again
   and the engine re-placed what it had just been asked to return.
   `RoomView.qml`'s `onDoubleClicked` now clears `dragging` first.
   Regression: `roomworkflow::test_aMarkerDraggedInThePlanMovesAndADoubleClickReturnsIt`
   (only the first click's release may send a move, and the application ends
   in the bed).
2. **Warning when the size track's application went away.** The size track
   left the tab chain (`activeFocusOnTab: page.selected !== null`) while it
   still held the focus, which Qt refuses with "Cannot set activeFocusOnTab to
   false once item is the active focus item". It now drops the focus first, as
   `BedChip.qml` does. Regression:
   `roomworkflow::test_theSizeTrackGivesUpTheFocusWhenItsApplicationLeaves`
   (fails on that warning).

## Harness and seams added

All of these are test-only.

- **New `TestServices` calls in `ui/tests/qml_test_main.cpp`**:
  - `scriptMachine(apps, endpoints)`: choose the endpoints, from `avr`,
    `realtek`, `null` and `headphones`.
  - `scriptMachineThatMakesItsSilentDevice(apps)`: uses a
    `ScriptedSilentDevice` whose install adds the silent endpoint and whose
    remove takes it away.
  - `setSessions(apps)`: applications arrive and leave while the engine runs.
  - `setFullscreen(app)`: sets the full-screen seam.
  - `refuseDefaultMoves(reason)`: makes the fake default device refuse moves.
  - `soundSettingsOpened()`: reports how many times the sound settings were
    opened.
  - `storedSetting(key)`: reads the store on disk through its own `QSettings`.
- **Per-application flags**: `window: false` scripts a background process,
  and `session: false` scripts an application with no audio session.
- **`LinkedDefaultDevice`**: a move made through the fake default device also
  updates the `is_default` flags in the fake device enumeration. On a real
  machine the two agree, and without this the endpoint table would contradict
  the sound settings.
- **Production change**: `objectName: "keyDialog"` and
  `objectName: "diagnosticsDialog"` on the two `FileDialog`s in
  `SettingsPage.qml`, so a suite can accept a choice on them. This follows the
  existing objectName-for-findChild pattern.
