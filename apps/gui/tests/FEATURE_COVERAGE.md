# forge-gui feature coverage (Qt Quick Test)

What every user-facing feature of the Forge GUI (`apps/gui/qml`, driven by
`EncoderController`, `QcController`, `ObjectDecodeController`,
`StreamPlayerController`, `LanguageManager`) is exercised by. Two sets of tests
hold it: the Qt Quick suites in `apps/gui/tests/qml/tst_*.qml` (36 suites, 215
test functions), and the Qt-free C++ cases in `tests/gui/*.cpp` (four files, 45
cases, tagged `[gui]` and built into `iclforge-tests`). All the QML suites run the
real controllers, not mocks.

Columns: **Before** is the status before the tst_e2e_* suites were added
(commit `8ee7eb7f2`, 2026-09-24). **Now** is the current status. Status key:

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
| A loaded WAV sets `sourceReady` and the window's source label | logic | logic | SourceLoading::test_loadingAFileUpdatesSourceReadyAndTheDisplayedName |
| Mismatched-rate source resampled and labelled | logic | logic | SourceLoading::test_addingAMismatchedRateSourceResamplesAndLabelsTheRow |
| Per-source level pips (sourceLevels) | logic | logic | SourceLoading::test_sourceLevelsIsAPerSourceLookupSeparateFromSourceModel |
| Source offset spin box / timeline length | logic | logic | TimelineTimeModel::test_timelineLengthDerivesFromSourcesAndOffsets, test_encodeWithASourceOffsetProducesADoneRun |
| Drag-and-drop / `forge-gui <file>` dispatch: a WAV becomes a source, a second WAV adds one, an `.ec3` opens the player, the window has a drop area for file URLs | logic | logic | DesktopIntegration::* (calls `openDroppedFile`, since an OS drag cannot be synthesised) |
| A dropped or launched `.ac4` | new | none | No case. `openDroppedFile` routes on the `.ac3`/`.ec3` suffix alone, so an `.ac4` goes to the WAV source loader. |
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
| Codec combo (AC-3 ↔ E-AC-3; AC-4 is under its own heading below) | logic | UI | E2eEncode::test_expertCodecComboSwitchesToEac3AndEncodesAccessUnits |
| Container combo → .mkv written, QC reads it back | logic | UI | E2eSettings::test_containerComboWritesAMatroskaFileQcCanReadBack |
| Other containers (Matroska, S/PDIF, MP4, fMP4 folder, TS) CLI line | logic | logic | SweepConformance::test_matroskaIsHonestlyTwoCommands, test_spdifIsHonestlyTwoCommandsToo, test_mp4IsHonestlyTwoCommands, test_fmp4IsHonestlyTwoCommandsAndNeedsAFolder, test_mpegTsIsHonestlyTwoCommands |
| Channel presets 5.1/7.1/5.1.4/7.1.4/7.2.4 | logic | logic | ChannelCounts::test_presetsProduceTheExpectedChannelCounts; the buttons' disabled-under-Atmos state: E2eObjects |
| Bed chips / LFE count / extras checkboxes | logic | logic | Keyboard::test_theBedChipsNameFollowsTheChoiceItDraws, test_theExtrasCheckboxTakesItsNameFromTheModel, ChannelCounts::* |
| Extras rows carry their channel tokens (`wide`: Lw Rw) | logic | logic | SweepConformance::test_extrasRowsCarryChannelTokens |
| Extra promotes AC-3 → E-AC-3 | UI | UI | FormatChannels::test_tickingAnExtraUnderAc3PromotesTheCodec |
| Codec-change warning dialog | UI | UI | TiersAndFlows::test_codecChangeWarningGatesThePromotion |
| Bit-rate floor advisory | logic | logic | SweepConformance::test_bitrateFloorAdvisoryTracksCodedChannelsAndFloor, GuidedWizard::test_wizardBitrateFloorAdvisoryShowsForAWideRoomOnGood |
| Bit-rate ladder (E-AC-3 768, low-rate rungs) | logic | logic | SweepConformance::test_eac3Gains768AndAc3ClampsBack, test_lowRateSourceDropsUnframableEac3Rungs |
| Dual mono (LFE clear, extras lock with the reason "not part of dual mono", DRC2, dialnorm per programme) | logic | logic | DualMono::*, SweepConformance::test_dualMonoLockReasonIsNotPartOfDualMono |
| Guided hides the tab bar and shows the wizard; choosing Advanced hides the wizard | UI | UI | FormatChannels::test_guidedHidesTheTabBarAndShowsTheWizard |

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
| Live session tab appears for a live source and goes with it | logic | logic | SweepConformance::test_liveSessionTabExistsForALiveSource |
| The tab pages follow the current page's height | logic | logic | SweepConformance::test_tabPagesFollowTheCurrentPagesHeight |

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
| A finished encode adds a done run to the strip | logic | logic | RunHistory::test_encodingAddsADoneRunToTheStrip |
| A run snapshots its CLI line, codec and play device, and the next run starts clean | logic | logic | RunHistory::test_startingARunSnapshotsCliLineEac3AndPlayDeviceIndex |
| Name pattern preference → planned name | logic | logic | TiersAndFlows::test_fileNamePatternDrivesThePlannedName |
| Run chip → details popover with snapshotted CLI line | UI | UI | RunHistory::test_clickingARunChipOpensItsDetailsPopoverWithTheSnapshottedCliLine |
| Failed-run details, pre-run refusal banner | logic | logic | RunHistory::test_detailsPopoverShowsTheFailureTextForAFailedRun, SweepConformance::test_preRunRefusalRaisesTheBanner |
| Run chip "More…" → QC / Inspect for this run | logic | logic | StreamPlayer::test_runChipMoreMenuOpensQcAndInspectForThisRunsFile (MenuItems are not findable Items; calls `openRunInQc`/`openRunInInspector`) |
| Run history persisted / restored | logic | logic | RunHistory::test_restoreRuns*, test_saveSession* |
| Cancel a running encode (run-chip Cancel) | none | none | Needs a run long enough to press Cancel mid-flight without a fixed wait. Not deterministic on the small fixtures. |
| Keep partial output on failure | none | none | Needs a mid-run failure or cancel |
| Run chip Play to receiver / Show in folder | logic | logic | RunHistory::test_playFileToReceiverIsANoOpForAnInvalidDeviceOrEmptyPath, test_outputDeviceSupportsFormatIsFalseOutOfRange. Needs a bitstream-capable endpoint. |
| Run chip Play on a finished AC-4 run | new | none | No case. The chip reports the run as AC-3, so the button follows the device's AC-3 passthrough, and `playFileToReceiver` finds no AC-3 or E-AC-3 stream in the file. |
| Command bar CLI chip → popover, Copy | UI | UI | SweepConformance::test_cliChipOpensThePopoverWithTheLiveLine |
| CLI line content (src map, meta tokens, containers) | logic | logic | SweepConformance::test_cliLine*, test_*IsHonestlyTwoCommands |

## AC-4 (encode, QC, player, object page)

These controls arrived with AC-4 (planning/ac4.md, I3), after the tst_e2e_*
suites, so **Before** reads "new".

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Codec combo → AC-4; the AC-4 tab takes the place of Coding tools and Metadata | new | UI | E2eAc4::test_rawStreamFromThePageEqualsTheEchoedLine, test_mp4FileFromThePageEqualsTheEchoedLine |
| Container combo → MP4 for AC-4 (`out.mp4` on the echoed line) | new | UI | E2eAc4::test_mp4FileFromThePageEqualsTheEchoedLine |
| Frame rate combo | new | UI | E2eAc4::test_rawStreamFromThePageEqualsTheEchoedLine |
| Rate mode combo | new | UI | E2eAc4::test_rawStream… (average), test_mp4File… (variable) |
| Codec mode combo | new | UI | E2eAc4::test_mp4FileFromThePageEqualsTheEchoedLine |
| I-frame interval spin box | new | UI | E2eAc4::test_rawStreamFromThePageEqualsTheEchoedLine |
| CRC checkbox (disabled for MP4) | new | UI | E2eAc4::test_rawStream…, test_mp4File… |
| dialnorm spin box (quarter-dB steps) | new | UI | E2eAc4::test_mp4FileFromThePageEqualsTheEchoedLine |
| Measure dialnorm checkbox | new | UI | E2eAc4::test_rawStreamFromThePageEqualsTheEchoedLine |
| Loudness values combo | new | UI | E2eAc4::test_rawStreamFromThePageEqualsTheEchoedLine |
| DRC profile combo | new | UI | E2eAc4::test_rawStreamFromThePageEqualsTheEchoedLine |
| Stereo downmix: centre, surround and preferred combos (5.1 source; disabled for stereo) | new | UI | Ac4Encode::test_surroundDownmixFromThePageEqualsTheEchoedLine, test_stereoSourceLeavesTheDownmixDisabled |
| Dialogue enhancement: L and R checkboxes | new | UI | E2eAc4::test_rawStreamFromThePageEqualsTheEchoedLine |
| Dialogue enhancement: C and Mid checkboxes, largest boost combo; tab badge | new | UI | Ac4Encode::test_dialogueCentreMidAndLargestBoostReachTheEchoedLine |
| The echoed `forge ac4-encode` line, run through forge, writes the page's bytes (raw, MP4, 5.1 raw) | new | UI | E2eAc4::test_rawStream…, test_mp4File…; Ac4Encode::test_surroundDownmix… (qml_test_main.cpp's cliRunner) |
| A container other than raw or MP4 is refused before the run | new | UI | Ac4Encode::test_ac4RefusesAContainerOtherThanRawOrMp4 |
| A preset that needs an extra moves AC-4 to E-AC-3 | new | UI | Ac4Encode::test_aPresetNeedingAnExtraMovesAc4ToEac3 |
| Player: an AC-4 file decoded through iclforge::ac4::Decoder | new | UI | E2eAc4::test_rawStreamFromThePageEqualsTheEchoedLine |
| Player: presentation picker | new | UI | Ac4Decode::test_playerPlaysThePresentationItsPickerChooses |
| QC of AC-4: dialnorm and stated loudness, raw and in MP4 | new | UI | E2eAc4::test_rawStream…, test_mp4File… |
| QC: presentation picker | new | UI | Ac4Decode::test_qcMeasuresThePresentationItsPickerChooses |
| Object page: presentations, bed and dynamic objects, the note that it exports nothing | new | UI | Ac4Decode::test_objectPageListsWhatTheDecoderReports, E2eAc4::test_rawStream… |
| The AC-4 name filter of the QC, Inspect objects and Open stream file pickers | new | none | No case. The tests set the picker's `selectedFile` and accept it, so a name filter is not exercised. |
| AC-4 page settings → ac4-encode tokens and iclforge::ac4::EncoderConfig; presentation labels | new | logic | iclforge-tests [gui]: tests/gui/test_ac4_encode_settings.cpp (Qt-free) |

## AC-4 objects

The Objects tab's switch under the AC-4 codec (planning/ac4.md, I5b), which writes the
objects `forge atmos-encode … codec=ac4` writes, so **Before** reads "new".

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Object mode keeps AC-4 as the codec; the AC-4 tab, the plan strip, the summary and the Encode button say AC-4 objects; the bit rate is left alone | new | UI | Ac4Objects::test_objectModeKeepsAc4AndOffersItsTabBesideTheObjectsTab |
| Codec combo in object mode: E-AC-3 and AC-4, never AC-3 (its entry is greyed out; the setter refuses it) | new | UI | Ac4Objects::test_codecPickerInObjectModeTakesEac3AndAc4ButNotAc3 |
| AC-4 tab in object mode: coding, dialnorm and CRC are controls; frame rate (shown native), rate mode, codec mode, I-frame interval, loudness and DRC are off; downmix and dialogue cards hidden | new | UI | Ac4Objects::test_ac4TabCarriesTheObjectControlsAndSwitchesOffWhatDescribesChannels |
| Object coding combo (A-JOC or direct-coded), dialnorm in whole dB and CRC reach the echoed line; the CRC token goes for MP4 | new | UI | Ac4Objects::test_codingDialnormAndCrcReachTheEchoedLine |
| The writer's limits in the Objects tab's text (2 048 samples a frame, 64 objects, the scene file's name); the count line reads of 64; the LFE send is off | new | UI | Ac4Objects::test_theWritersLimitsAreOnTheObjectsTabAndTheLfeSendIsOff |
| The count line counts a channel assigned to a speaker as an object held there ("assigned to speakers"), against 64; E-AC-3's "pinned to the bed" and its fifteen are as they were | new | UI | Ac4Objects::test_theCountLineCountsAChannelAssignedToASpeakerAsAnObjectNotAsABed |
| A container that is not raw or MP4 is refused beside the controls and before Encode | new | UI | Ac4Objects::test_aContainerAnObjectStreamCannotBeIsRefusedBesideTheControlsAndBeforeEncode |
| One LFE object is taken, two are refused, an LFE alone is no object stream | new | logic | Ac4Objects::test_oneLfeObjectIsTakenAndTwoAreRefused |
| A dialnorm off the whole-dB grid, and a measured one, are refused | new | logic | Ac4Objects::test_aDialnormOffTheWholeDbGridIsRefusedAndMeasuringIsToo |
| Export paths… suggests `<source>-paths.json`, the scene the echoed command reads | new | UI | Ac4Objects::test_exportPathsSuggestsTheNameTheEchoedCommandReads |
| Guided's Movement card writes E-AC-3 objects even where AC-4 was the codec | new | UI | GuidedWizard::test_movementCardKeepsGuidedsObjectsEac3WhenAc4WasTheCodec |
| The echoed `forge atmos-encode … codec=ac4` line, run through forge, writes the page's bytes (raw A-JOC; MP4 direct-coded of two sources with an assignment, an offset, a trim, a fold, a speaker and an LFE) | new | UI | E2eAc4Objects::test_rawStreamOfOneSource…, test_mp4OfSeveralSources… (qml_test_main.cpp's cliRunner) |
| The object page reads the page's AC-4 objects: their count, movement and gain | new | UI | E2eAc4Objects::test_rawStreamOfOneSource…, test_mp4OfSeveralSources… |
| An ADM master's scene (two bed channels, one object that jumps) authored on the page decodes with its objects within 0.06 per axis and 2 dB | new | logic | E2eAc4Objects::test_anAdmMasterAuthoredOnThePageDecodesWithItsObjectsInTolerance (controller-level; the page reads audio, not ADM) |
| Object slots, pinned places, flat planes, object planes, the encode; AC-4 object settings → tokens and parameters | new | logic | iclforge-tests [gui]: tests/gui/test_ac4_objects_core.cpp, test_ac4_encode_settings.cpp (Qt-free); [cli][atmos][ac4]: tests/cli/test_cli_atmos_encode_ac4.cpp |

## Stream player (decode)

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| "Open stream…" → Choose file → decode summary, meters, soundfield | logic | UI | E2eInspect::test_playerPlayScrubAndPauseFromItsOwnControls, E2eEncode::decodeInPlayer (every encode case) |
| Scrub slider / position label | logic | UI | E2eInspect::test_playerPlayScrubAndPauseFromItsOwnControls |
| An Atmos stream decodes asynchronously and reports its objects | logic | logic | StreamPlayer::test_openingRealAtmosStreamDecodesAsyncAndReportsObjects |
| The dialog draws one meter row per channel and offers Export objects | logic | logic | StreamPlayer::test_dialogRendersOneMeterRowPerChannelAndOffersObjectExport |
| Seeking while paused moves the position at once, clamped to the stream | logic | logic | StreamPlayer::test_seekWhilePausedMovesPositionImmediately |
| Play / Pause button | none | UI (hardware) | E2eInspect::test_playerPlayScrubAndPauseFromItsOwnControls. With no output: error line shown. With an output: plays, then pauses. |
| Export decoded WAV (loads back as a source) | none | UI | E2eEncode::test_playerExportsDecodedWavThatLoadsBackAsASource |
| Export objects (one WAV per object) | none | UI | E2eInspect::test_playerExportsOneWavPerObjectFromAnAtmosStream |
| Export objects, AC-4 (A-JOC or direct-coded) | new | UI | E2eInspect::test_playerExportsOneWavPerObjectFromAnAc4Stream |
| Closing the dialog stops playback | none | UI | E2eInspect::test_playerPlayScrubAndPauseFromItsOwnControls |
| Quitting while a decode, QC measurement, object inspection or encode is running | new | logic | Ac4Decode::cleanupTestCase (the first three) and Teardown::test_anEncodeStartedAsTheSuiteEnds leave the work running as the suite ends; the process must exit with code 0 |

## QC panel and gate meters

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| "QC a stream…" → Choose file → per-preset PASS/FAIL that matches the measured numbers (words and colour) | logic | UI | E2eInspect::test_qcFromTheHeaderButtonShowsAVerdictPerPresetThatMatchesTheNumbers |
| Delivery-preset segmented control narrows rows, meter band | logic | UI | E2eInspect::test_qcFromTheHeaderButton… ; QcPanel::test_presetControlOffersEveryPresetAndSelectsWhatItNames |
| QC of a non-stream shows the error | none | UI | E2eInspect::test_qcOnAFileThatIsNotAStreamSaysSoInTheDialog |
| QC report data / preset constants | logic | logic | QcPanel::test_measuringRealFileIsAsyncAndReportsRealData, test_presetSelectionNarrowsToTheChosenPresetsRealNumbers |
| The dialog shows the measurement: the summary line and one programme card | logic | logic | QcPanel::test_dialogRendersTheRealMeasurement |
| Gate meter pass/fail visuals and accessibility | logic | logic | QcPanel::test_gateMeter*, Accessibility::test_qcGateMeter* |

## Objects (authoring) and object inspector

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Object-mode switch → objects, codec forced, presets locked | logic | UI | E2eObjects::test_objectSwitchAuthoringAndEncodeRoundTripThroughTheInspector |
| Turning object mode on raises the rate to 384 kbps and keeps the codec | logic | logic | SweepConformance::test_atmosEnableFloorsTheRate |
| Add key / Delete key buttons | logic | UI | E2eObjects::test_objectSwitchAuthoring…, test_deleteKeyButtonRemovesTheSelectedKey. The key selection is set on the tab, because the diamonds are drag targets. |
| A hand-added key seeds the 0.7/√n gain | logic | logic | SweepConformance::test_handAddedKeySeedsTheInverseRootGain |
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
| Inspector: the decode is asynchronous and reports every frame's motion; scrubbing moves to another frame | logic | logic | ObjectInspector::test_inspectingRealObjectStreamIsAsyncAndReportsRealMotion, test_scrubbingMovesToADifferentDecodedFrame |
| Inspector audition | none | UI (hardware) | E2eInspect::test_inspectorChosenFromItsButton… (error line, or a real audition that the button stops); test_auditionButtonsFitInsideTheInspectorDialog (each row's Audition button lies inside the dialog at the window's 1280x900 minimum, and a mouse click on it reaches it) |
| Soundfield view | logic | UI | E2eInspect (spSoundfield shown for a decode); DualMono::test_dualMonoHasNoSoundstage (logic) |

## Live capture (multi-device)

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Input mode File / Live | UI | UI | TiersAndFlows::test_firstRunCaptureSwitchesToTheLiveBranch |
| Device rows, add/remove, cap, totals, channel labels | logic (hardware) | logic (hardware) | LiveMultiDevice::* (with whatever devices the machine has; `addCaptureDeviceButton` clicked) |
| Start/Stop session gating, safety copy, OSC toggle, receiver combo | UI/logic (hardware) | UI/logic (hardware) | LiveSession::* (checkboxes clicked; no session actually started) |
| Running a live session, recording, reconnect banner, layout switch mid-session, live objects | none (hardware) | none (hardware) | Needs a real capture device. The no-op paths are covered in LiveSession/LiveMultiDevice. |
| A live session or the rail's Monitor with AC-4 as the codec is refused with a status line | new | none | No case. `startLiveSession` refuses before it looks at the capture device, so no device is needed to reach it. |
| The take a live session or Record… writes: raw stream, MPEG-TS, IEC 61937 WAV, Matroska, fragmented-MP4 folder, and each one's reporting of a bad destination, a full disk and an empty take | logic | logic | iclforge-tests [gui]: tests/gui/test_recording_sink.cpp (19 cases, Qt-free; `RecordingSink` is shared with the CLI's `record`) |
| The same take with AC-4 frames: raw, MPEG-TS, IEC 61937-14 bursts, fragments at I-frames; Matroska refused | new | logic | iclforge-tests [gui]: tests/gui/test_recording_sink.cpp (4 cases) |
| Receiver passthrough (Play to receiver) | none (hardware) | none (hardware) | Needs an S/PDIF/HDMI endpoint |

## Guided wizard

| Feature | Before | Now | Test case(s) |
|---|---|---|---|
| Step navigation, setup/room/quality/movement/destination cards | UI | UI | GuidedWizard::* (20 cases) |
| Amp destination encodes directly | UI | UI | GuidedWizard::test_ampDestinationEncodesDirectlyAndThreadsItsDevicePick |
| The wizard's Next button stays on screen and its scroll area exists | logic | logic | SweepConformance::test_guidedFooterStaysOnScreen |

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
| Diagnostics report content | logic | logic | Diagnostics::*; iclforge-tests [gui]: tests/gui/test_gui_diagnostics.cpp (5 cases, Qt-free: the message ring, notes, the named facts, no signing value, the scrub) |
| Output folder chooser / Reset | none | none | FolderDialog. Same seam would work, but no case. |
| Meters show (mode), monitor button, clip latch | UI/logic | UI/logic | ClipLatch::test_clipLatchStaysLitUntilClickedOrANewTransportStarts (UI); meter mode: none |
| About dialog shows version, closes | none | UI | E2eSettings::test_aboutDialogShowsTheBuildsVersionAndCloses |
| Window floor, header accessibility | logic | logic | MainShell::* |
| Keyboard tab chain / focus ring / names | logic | logic | Keyboard::*, Accessibility::* |
| Themes / palettes | logic | logic | ThemePalettes::* |

## Counts

| | Before | Now |
|---|---|---|
| Rows (features) | 115 | 160 |
| Driven from the UI | 16 | 85 |
| Logic only (controller or QML function called directly) | 80 | 62 |
| Not covered | 19 | 13 |

"UI (hardware)" and "UI/logic" rows count as UI. The 45 rows whose **Before**
reads "new" arrived after the tst_e2e_* suites, so **Before** counts only the
115 rows that existed then.

C++ line coverage of `apps/gui/*.cpp|hpp` as it stood on 2026-09-24 (commit
`8ee7eb7f2`), from the QML suites plus the C++ unit tests that also compile
`gui_diagnostics.cpp`. It was measured with `/opt/gui-cov.sh`, a script kept
outside this repository. Before that change `forge_gui_qmltests` was not
instrumented at all; `iclforge::coverage` is now linked into it. The figures have
not been measured again. The six suites added since (the AC-4 ones and
`Teardown`) and the AC-4 code they reach are not in them.

| File | Before (26 suites) | After (30 suites) |
|---|---|---|
| encoder_controller.cpp | 52% (2291/4335) | 55% (2412/4335) |
| stream_player_controller.cpp | 38% (153/397) | 60% (240/397) |
| object_decode_controller.cpp | 68% (160/233) | 76% (177/233) |
| qc_controller.cpp | 76% (222/290) | 77% (226/290) |
| channel_geometry.cpp | 34% (22/63) | 49% (31/63) |
| Total | 56% (3261/5751) | 60% (3502/5751) |
