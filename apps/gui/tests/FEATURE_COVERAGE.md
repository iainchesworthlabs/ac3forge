# ac3gui feature coverage (Qt Quick Test)

What every user-facing feature of the Forge GUI (`apps/gui/qml`, driven by
`EncoderController`, `QcController`, `ObjectDecodeController`,
`StreamPlayerController`, `LanguageManager`) is exercised by, in
`apps/gui/tests/qml/tst_*.qml`. All suites run the real controllers, not mocks.

Columns: **Before** is the status before the tst_e2e_* suites were added.
**Now** is the current status. Status key:

- **UI**: a test drives the feature through the window. That means a mouse
  click, a key press, or a picker the real button opened and the test then
  accepted. The test asserts the outcome.
- **logic**: a test covers the feature, but it calls the controller or the
  window's function directly, or sets a QML property. No control is pressed.
- **none**: nothing covers it.

Picker seam: every `FileDialog` has an `objectName`. A test presses the real
button, checks that the picker it opened is showing, fills in
`selectedFile`/`selectedFolder` and calls `accept()`. This runs the same
`onAccepted` handler that a real pick runs.

Hardware: no platform output exists under the offscreen harness, and this
repo has no null sink. The Play and Audition cases assert whichever honest
outcome the machine gives: the dialog shows an error naming the output it
could not open, or playback really starts and the same button stops it. Live
capture, recording and receiver passthrough need a real capture device or an
S/PDIF/HDMI endpoint. They are covered only at the level the existing suites
reach without a device.

## Source loading

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| First-run "Choose a file" card → WAV picker → source loaded | logic | UI | E2eEncode::test_firstRunChooseFileEncodeAc3AndDecodeWhatWasWritten (and every tst_e2e_* setup) |
| First-run "Open the bundled test signal" | UI | UI | TiersAndFlows::test_firstRunBundledTestSignalLoadsARealSource |
| First-run "Capture live" | UI | UI | TiersAndFlows::test_firstRunCaptureSwitchesToTheLiveBranch |
| Rail "+ Add files…" → add picker → second source | logic | UI | E2eEncode::test_playerExportsDecodedWavThatLoadsBackAsASource |
| Rail per-source remove | logic | UI | E2eEncode::test_playerExportsDecodedWavThatLoadsBackAsASource |
| Mismatched-rate source resampled and labelled | logic | logic | SourceLoading::test_addingAMismatchedRateSourceResamplesAndLabelsTheRow |
| Per-source level pips (sourceLevels) | logic | logic | SourceLoading::test_sourceLevelsIsAPerSourceLookupSeparateFromSourceModel |
| Source offset spin box / timeline length | logic | logic | TimelineTimeModel::test_timelineLengthDerivesFromSourcesAndOffsets, test_encodeWithASourceOffsetProducesADoneRun |
| Drag-and-drop / `ac3gui <file>` dispatch | logic | logic | DesktopIntegration::* (calls `openDroppedFile`, since an OS drag cannot be synthesised) |
| Session restore of sources and assignments | logic | logic | TiersAndFlows::test_sessionSaveAndRestoreRoundTripsSourcesAndAssignments |

## Multi-source assignment

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Unassigned "goes nowhere" warnings | logic | logic | MultiSource::test_addingASecondSourceNeedsAnAssignment |
| Assignment destination / trim | logic | logic | MultiSource::test_explicitAssignmentClearsTheGoesNowhereWarnings, test_trimRoundTripsThroughSetAssignmentTrimAndMapToken |
| "Auto-assign by name" | logic | logic | MultiSource::test_autoAssignByNameFillsChannelsTheirOwnLayoutNames |
| Encode with explicit assignment | logic | logic | MultiSource::test_encodingWithAnExplicitAssignmentProducesADoneRun |
| Guided "open assignments" round trip | UI | UI | TiersAndFlows::test_goAssignFromGuidedRoundTripsLosslessly |

## Format and channel layout

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Codec combo (AC-3 ↔ E-AC-3) | logic | UI | E2eEncode::test_expertCodecComboSwitchesToEac3AndEncodesAccessUnits |
| Container combo → .mkv written, QC reads it back | logic | UI | E2eSettings::test_containerComboWritesAMatroskaFileQcCanReadBack |
| Other containers (S/PDIF, MP4, fMP4 folder, TS) CLI line | logic | logic | SweepConformance::test_*IsHonestlyTwoCommands |
| Channel presets 5.1/7.1/5.1.4/7.1.4/7.2.4 | logic | logic | ChannelCounts::test_presetsProduceTheExpectedChannelCounts; the buttons' disabled-under-Atmos state: E2eObjects |
| Bed chips / LFE count / extras checkboxes | logic | logic | Keyboard::test_theBedChipsNameFollowsTheChoiceItDraws, test_theExtrasCheckboxTakesItsNameFromTheModel, ChannelCounts::* |
| Extra promotes AC-3 → E-AC-3 | UI | UI | FormatChannels::test_tickingAnExtraUnderAc3PromotesTheCodec |
| Codec-change warning dialog | UI | UI | TiersAndFlows::test_codecChangeWarningGatesThePromotion |
| Bit-rate floor advisory | logic | logic | SweepConformance::test_bitrateFloorAdvisoryTracksCodedChannelsAndFloor, GuidedWizard::test_wizardBitrateFloorAdvisoryShowsForAWideRoomOnGood |
| Bit-rate ladder (E-AC-3 768, low-rate rungs) | logic | logic | SweepConformance::test_eac3Gains768AndAc3ClampsBack, test_lowRateSourceDropsUnframableEac3Rungs |
| Dual mono (LFE clear, extras lock, DRC2, dialnorm per programme) | logic | logic | DualMono::* |

## Bitrate / VBR

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| VBR availability, token, encode reports what it spent | logic | logic | Vbr::* |
| Rate-mode control / quality slider / min-max spins | logic | logic | Vbr::test_vbrTokenMatchesTheCliGrammar (properties set directly) |

## Tiers and coding tools

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Guided / Advanced / Expert tier switch | UI | UI | FormatChannels::test_clickingExpertRevealsTheHiddenTabsAndTheTabBar, every tst_e2e_* |
| Tab bar and badges | logic | UI | E2eEncode::test_codingToolsTabDrivesTheToolsTokenAndTheEncode, E2eSettings (meta badge); TiersAndFlows::test_tabBadgesCountTheHiddenNonDefaults (logic) |
| Coupling / SPX checkboxes → tools token → encode | logic | UI | E2eEncode::test_codingToolsTabDrivesTheToolsTokenAndTheEncode |
| Coupling begin-band spin box | none | UI | E2eEncode::test_codingToolsTabDrivesTheToolsTokenAndTheEncode |
| AHT / GAQ / SPX begin band / SPX attenuation | none | none | Same pattern as coupling. No separate case. |
| Leaving Expert on an Expert-only tab | logic | logic | TiersAndFlows::test_leavingExpertOnAnExpertOnlyTabFallsBackToFormat |

## Metadata and loudness

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| dialnorm spin box → written into the stream (QC reads it back) | logic | UI | E2eEncode::test_metadataDialnormSetFromTheUiIsWhatQcReadsBackFromTheFile |
| DRC profile combo → drc token, Metadata badge | logic | UI | E2eSettings::test_drcAndHeavyCompressionFromTheMetadataTabPutComprIntoTheStream |
| Measure dialnorm / guided loudness contract | logic | logic | GuidedWizard::test_guidedContract* , DualMono::test_dialnormAuto* |
| Service (bsmod) combo → meta token, tab badge | none | UI | E2eSettings::test_serviceComboSetsBsmodAndTheMetadataBadge |
| Mix level, room type, dsurmod, dheadphon, dsurex, A/D converter, copyright, original, Annex D | none | none | Same combo/checkbox pattern as bsmod. No case. |
| Heavy compression checkbox → compr in the stream (QC reads "compr present") | logic | UI | E2eSettings::test_drcAndHeavyCompressionFromTheMetadataTabPutComprIntoTheStream; ceiling/dialogue and programme 2: DualMono::test_metaTokensEmitDrc2AndHeavy2OnlyUnderDualMono (logic) |
| Downmix (cmix/surmix/dmixmod), mixing metadata, lfemix | none | none | Only the tab-badge count (TiersAndFlows) |

## Encode runs and results

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Encode button → save picker with planned name → AC-3 file; frames, KB, rate, duration on status and chip; file decodes to the right shape | logic | UI | E2eEncode::test_firstRunChooseFileEncodeAc3AndDecodeWhatWasWritten |
| Same for E-AC-3 (access units) | logic | UI | E2eEncode::test_expertCodecComboSwitchesToEac3AndEncodesAccessUnits |
| Name pattern preference → planned name | logic | logic | TiersAndFlows::test_fileNamePatternDrivesThePlannedName |
| Run chip → details popover with snapshotted CLI line | UI | UI | RunHistory::test_clickingARunChipOpensItsDetailsPopoverWithTheSnapshottedCliLine |
| Failed-run details, pre-run refusal banner | logic | logic | RunHistory::test_detailsPopoverShowsTheFailureTextForAFailedRun, SweepConformance::test_preRunRefusalRaisesTheBanner |
| Run chip "More…" → QC / Inspect for this run | logic | logic | StreamPlayer::test_runChipMoreMenuOpensQcAndInspectForThisRunsFile (MenuItems are not findable Items; calls `openRunInQc`/`openRunInInspector`) |
| Run history persisted / restored | logic | logic | RunHistory::test_restoreRuns*, test_saveSession* |
| Cancel a running encode (run-chip Cancel) | none | none | Needs a run long enough to press Cancel mid-flight without a fixed wait. Not deterministic on the small fixtures. |
| Keep partial output on failure | none | none | Needs a mid-run failure or cancel |
| Run chip Play to receiver / Show in folder | logic | logic | RunHistory::test_playFileToReceiverIsANoOpForAnInvalidDeviceOrEmptyPath. Needs a bitstream-capable endpoint. |
| Command bar CLI chip → popover, Copy | UI | UI | SweepConformance::test_cliChipOpensThePopoverWithTheLiveLine |
| CLI line content (src map, meta tokens, containers) | logic | logic | SweepConformance::test_cliLine*, test_*IsHonestlyTwoCommands |

## Stream player (decode)

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| "Open stream…" → Choose file → decode summary, meters, soundfield | logic | UI | E2eInspect::test_playerPlayScrubAndPauseFromItsOwnControls, E2eEncode::decodeInPlayer (every encode case) |
| Scrub slider / position label | logic | UI | E2eInspect::test_playerPlayScrubAndPauseFromItsOwnControls |
| Play / Pause button | none | UI (hardware) | E2eInspect::test_playerPlayScrubAndPauseFromItsOwnControls. With no output: error line shown. With an output: plays, then pauses. |
| Export decoded WAV (loads back as a source) | none | UI | E2eEncode::test_playerExportsDecodedWavThatLoadsBackAsASource |
| Export objects (one WAV per object) | none | UI | E2eInspect::test_playerExportsOneWavPerObjectFromAnAtmosStream |
| Closing the dialog stops playback | none | UI | E2eInspect::test_playerPlayScrubAndPauseFromItsOwnControls |

## QC panel and gate meters

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| "QC a stream…" → Choose file → per-preset PASS/FAIL that matches the measured numbers (words and colour) | logic | UI | E2eInspect::test_qcFromTheHeaderButtonShowsAVerdictPerPresetThatMatchesTheNumbers |
| Delivery-preset segmented control narrows rows, meter band | logic | UI | E2eInspect::test_qcFromTheHeaderButton… ; QcPanel::test_presetControlOffersEveryPresetAndSelectsWhatItNames |
| QC of a non-stream shows the error | none | UI | E2eInspect::test_qcOnAFileThatIsNotAStreamSaysSoInTheDialog |
| QC report data / preset constants | logic | logic | QcPanel::test_measuringRealFileIsAsyncAndReportsRealData, test_presetSelectionNarrowsToTheChosenPresetsRealNumbers |
| Gate meter pass/fail visuals and accessibility | logic | logic | QcPanel::test_gateMeter*, Accessibility::test_qcGateMeter* |

## Objects (authoring) and object inspector

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Object-mode switch → objects, codec forced, presets locked | logic | UI | E2eObjects::test_objectSwitchAuthoringAndEncodeRoundTripThroughTheInspector |
| Add key / Delete key buttons | logic | UI | E2eObjects::test_objectSwitchAuthoring…, test_deleteKeyButtonRemovesTheSelectedKey. The key selection is set on the tab, because the diamonds are drag targets. |
| Zoom in / Fit / readout | logic | UI | E2eObjects::test_objectSwitchAuthoring… |
| Motion preview start/stop | none | UI | E2eObjects::test_objectSwitchAuthoring… |
| Export paths… picker → file, CLI line quotes it | logic | UI | E2eObjects::test_objectSwitchAuthoring… |
| Atmos encode → inspector decodes the objects back | logic | UI | E2eObjects::test_objectSwitchAuthoring… |
| Trajectory presets, what-moves (guided) | UI | UI | GuidedWizard::test_trajectoryPresets*, test_whatMoves* |
| Keyframe retime/shift, paths grammar, zoom snap tiers | logic | logic | TimelineTimeModel::*, SweepConformance::test_moveObjectKeyframeRetimesTheCue |
| Per-source objects, objm pairs | logic | logic | ObjectsPerSource::* |
| Timeline drag/double-click/right-click on keys, pan strip, clip band shift-drag | none | none | Pointer-gesture authoring on a Canvas-like timeline. Only the controller calls behind it are covered (logic). |
| Inspector: Choose file → object rows, scrub to last frame updates rows | logic | UI | E2eInspect::test_inspectorChosenFromItsButtonListsObjectsAndScrubsFrames |
| Inspector plan/elevation markers | logic | logic | ObjectInspector::test_dialogRendersOneMarkerPerObject |
| Inspector audition | none | UI (hardware) | E2eInspect::test_inspectorChosenFromItsButton… (error line, or a real audition that the button stops) |
| Soundfield view | logic | UI | E2eInspect (spSoundfield shown for a decode); DualMono::test_dualMonoHasNoSoundstage (logic) |

## Live capture (multi-device)

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Input mode File / Live | UI | UI | TiersAndFlows::test_firstRunCaptureSwitchesToTheLiveBranch |
| Device rows, add/remove, cap, totals, channel labels | logic (hardware) | logic (hardware) | LiveMultidevice::* (with whatever devices the machine has; `addCaptureDeviceButton` clicked) |
| Start/Stop session gating, safety copy, OSC toggle, receiver combo | UI/logic (hardware) | UI/logic (hardware) | LiveSession::* (checkboxes clicked; no session actually started) |
| Running a live session, recording, reconnect banner, layout switch mid-session, live objects | none (hardware) | none (hardware) | Needs a real capture device. The no-op paths are covered in LiveSession/LiveMultidevice. |
| Receiver passthrough (Play to receiver) | none (hardware) | none (hardware) | Needs an S/PDIF/HDMI endpoint |

## Guided wizard

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Step navigation, setup/room/quality/movement/destination cards | UI | UI | GuidedWizard::* (19 cases) |
| Amp destination encodes directly | UI | UI | GuidedWizard::test_ampDestinationEncodesDirectlyAndThreadsItsDevicePick |

## Preferences, about, first run, shell

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Preferences Save / Cancel | UI | UI | TiersAndFlows::test_preferencesDialogSavesOnSaveAndDiscardsOnCancel |
| Palette / theme | logic | logic | ThemePalettes::* (sets the dialog's choice, then presses Save) |
| Text size | logic | logic | Keyboard::test_textSizeSettingReachesTheTheme |
| Explanations toggle, CLI visible, codec warning, defaults | logic | logic | TiersAndFlows::test_explanationsToggle…, test_preferencesDefaultsApply… |
| Language combo switches the window immediately | logic | UI | E2eSettings::test_preferencesLanguageComboSwitchesTheWindowImmediately |
| Language manager (available, persist, RTL) | logic | logic | LanguageManager::* |
| Pseudo-locale pipeline | logic | logic | LocalisationPipeline::* |
| Save diagnostics… → file written, message shown | logic | UI | E2eSettings::test_preferencesSaveDiagnosticsWritesTheSupportFile |
| Diagnostics report content | logic | logic | Diagnostics::* |
| Output folder chooser / Reset | none | none | FolderDialog. Same seam would work, but no case. |
| Meters show (mode), monitor button, clip latch | UI/logic | UI/logic | ClipLatch::test_clipLatchStaysLitUntilClickedOrANewTransportStarts (UI); meter mode: none |
| About dialog shows version, closes | none | UI | E2eSettings::test_aboutDialogShowsTheBuildsVersionAndCloses |
| Window floor, header accessibility | logic | logic | MainShell::* |
| Keyboard tab chain / focus ring / names | logic | logic | Keyboard::*, Accessibility::* |
| Themes / palettes | logic | logic | ThemePalettes::* |

## Counts

| | Before | Now |
|---|---|---|
| Rows (features) | 99 | 99 |
| Driven from the UI | 15 | 50 |
| Logic only (controller or QML function called directly) | 65 | 40 |
| Not covered | 19 | 9 |

"UI (hardware)" and "UI/logic" rows count as UI.

C++ line coverage of `apps/gui/*.cpp|hpp`, from the QML suites plus the
C++ unit tests that also compile `gui_diagnostics.cpp`. It was measured with
`/opt/gui-cov.sh`. Before this change `ac3gui_qmltests` was not instrumented
at all; `ac3::coverage` is now linked into it.

| File | Before (26 suites) | After (30 suites) |
|---|---|---|
| encoder_controller.cpp | 52% (2291/4335) | 55% (2412/4335) |
| stream_player_controller.cpp | 38% (153/397) | 60% (240/397) |
| object_decode_controller.cpp | 68% (160/233) | 76% (177/233) |
| qc_controller.cpp | 76% (222/290) | 77% (226/290) |
| channel_geometry.cpp | 34% (22/63) | 49% (31/63) |
| Total | 56% (3261/5751) | 60% (3502/5751) |

## Known UI bug (exposed, test skipped)

`ObjectInspectorDialog.qml`: the dialog is 900 px wide. Its content needs
about 1300 px: the 340 px room plan, beside object rows of fixed-width
columns and an Audition button. At the window's 1280x900 minimum, each
row's Audition button runs about 60 px past the dialog's right edge. The
centre of the button is on the modal dimmer, so a mouse click there does
nothing. The keyboard still works. The check is
`E2eInspect::test_auditionButtonsFitInsideTheInspectorDialog`, which calls
`skip()` while the bug stands.
