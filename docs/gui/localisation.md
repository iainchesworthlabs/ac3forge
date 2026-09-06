# ac3gui — localisation

The app's own text is translated via Qt Linguist, the same mechanism and the same canonical
language set as the sibling CountdownSolver project. This page covers what is in the catalogues
today and what that does and does not promise, how to update or extend them, and the pseudo-locale
QA fixture the pipeline itself is tested against.

## How it fits together

Translation source files live at `apps/gui/translations/ac3gui_<code>.ts` (Qt Linguist XML), one
per language:

| Code | Language | File |
| --- | --- | --- |
| `fr` | Français | `ac3gui_fr.ts` |
| `de` | Deutsch | `ac3gui_de.ts` |
| `es` | Español | `ac3gui_es.ts` |
| `ar` | العربية | `ac3gui_ar.ts` |
| `he` | עברית | `ac3gui_he.ts` |
| `yi` | יידיש | `ac3gui_yi.ts` |

English has no `.ts` file — it is the literal `qsTr()` source text. `apps/gui/CMakeLists.txt`'s
`qt_add_translations()` call wires these in: it scans the target's sources — every QML file
`AC3_QML_FILES` lists, which is where nearly all of the marked strings are, and the `tr()` calls in
`encoder_controller.cpp` — for translatable strings, then compiles each `.ts` to a `.qm` and embeds
it as a resource under `:/i18n` at build time.
`LanguageManager` (`apps/gui/language_manager.{hpp,cpp}`) loads the matching `.qm` for the active
language and applies right-to-left layout mirroring for Arabic, Hebrew and Yiddish
(`Main.qml`'s `LayoutMirroring` root, and the bundled Noto Sans Arabic/Hebrew faces those three
scripts need — Latin has no glyph coverage for either).

Preferences → Appearance → **Language** switches it live, no restart needed
(`languageManager.setLanguage(code)`, a plain context property `main.cpp` installs the same way
`EncoderController` is installed — not a `QML_SINGLETON`, since `LanguageManager` takes
constructor arguments a singleton factory cannot supply).

## What's translated today

Every catalogue is complete. The six files carry 795 messages each — the window chrome, the header
buttons, the tab names, the Guided wizard, the whole Preferences dialog, the longer explanatory
`PrefsNote` paragraphs through the Format/Objects/Coding tools/Metadata tabs, and the
`Accessible.*` names and descriptions — and none of them is left `type="unfinished"`. That
includes the strings the keyboard and text-size pass of 2026-09-06 added
([Keyboard & text size](accessibility.md)): the text-size setting, the diagnostics section, and
the two `tr()` calls in `encoder_controller.cpp`. Nothing in the app falls back to English for
want of a catalogue entry.

Complete is not the same as reviewed. The renderings are machine-made and no speaker of any of the
six languages has read them, so a term can be filled in and still be the wrong word, or two words
for one thing. [Crucible's languages page](../crucible/localisation.md) carries the glossary the
shared six are held to and what an audit of the mechanical output found; the review that confirms
or replaces each rendering has not run for either app.

Crucible's window says that in its own note under the language chooser. `ac3gui`'s Preferences note
has not caught up: it still describes the six as partially translated and untranslated text as
staying in English, which the refill made wrong. Correcting it edits a `qsTr()` string, so it lands
together with a catalogue regeneration.

Searching an `apps/gui` `.ts` file for `type="unfinished"` finds nothing today, and nothing in the
suite holds it that way: `tests/crucible/test_translations.cpp` reads Crucible's six files only, so
its no-unfinished and no-dead-entry rules do not cover these. What CI does check for `ac3gui` is
drift — that the committed catalogues match what `lupdate` extracts. Extending that Crucible gate
over `apps/gui/translations` is the step that would keep the completeness above from quietly
lapsing.

## Crucible shares this pipeline

Everything above is `ac3gui`, half of [Forge](../forge/index.md).
[Crucible](../crucible/index.md) (`apps/crucible/`, roadmap UX11/UX12) is the family's other Qt
application, and reuses `LanguageManager` rather than copying it: the class takes a translation
basename (`"ac3gui"` by default, `"ac3crucible"` for Crucible) that names the `.qm` files it
loads from `:/i18n/`, and `useSystemLanguage()` forgets a saved override so the app follows the
system locale again. Crucible ships the same six languages (`apps/crucible/translations/`), has
its own `ac3crucible_lupdate` target, and honours the same `AC3GUI_LOCALE` override for smoke
checks. What is Crucible's own — the glossary its six languages are held to, the window's
right-to-left half, and the gate over its catalogues — is on
[Crucible's languages page](../crucible/localisation.md).

## Updating an existing translation

1. Regenerate the `.ts` files from current source strings:

   ```sh
   cmake --build --preset <preset> --target ac3gui_lupdate
   ```

   Any new or changed `qsTr()` string shows up as a `<translation type="unfinished">` entry
   (empty, or holding the last-known text) in the relevant `.ts` file(s). CI's own "Check
   translations are up to date" step (`.github/workflows/_build.yml`, on the Linux GCC leg) reruns
   this same target and fails the build if it produces a diff nobody committed. Extraction does not
   depend on the compiler, so one leg is enough; Crucible's six get the same check on the
   `windows-msvc` leg.
2. Open the `.ts` file in **Qt Linguist** (ships with Qt), or edit the `<translation>` elements
   directly, and fill in the unfinished entries. Editing by hand, remove the `type="unfinished"`
   attribute yourself once an entry has a rendering you are willing to ship.
3. Rebuild normally to recompile the `.qm` and pick up the change.

### Finding a string

`lupdate` groups each `.ts` file's messages into a `<context><name>` block named after the
component it came from — `AboutDialog`, `AssignmentPanel`, `ChannelMeter`, `EncoderController`
(the `tr()` calls in the C++), `FirstRunScreen`, `GuidedWizard`, `LoudnessGroup`, `Main`,
`ObjectInspectorDialog`, `PreferencesDialog`, `QcDialog`, `QcGateMeter`, `SoundfieldView`,
`StreamPlayerDialog` and `VbrPanel` — which is how to jump straight to the right area of a large
`.ts` file.

## Adding a new language

1. Add `translations/ac3gui_<code>.ts` to the `TS_FILES` list in `qt_add_translations()`
   (`apps/gui/CMakeLists.txt`'s `AC3_TS_FILES`), then run `ac3gui_lupdate` to generate the initial
   file and translate it as above.
2. Add `{code, "Native name"}` to the `kLanguages` array in `apps/gui/language_manager.cpp`. Miss
   this and `LanguageManager::setLanguage()` rejects the code as unsupported — the language never
   appears in Preferences' picker even with a fully-translated `.ts`/`.qm`.
3. If the script is right-to-left, `LanguageManager` already derives layout direction from
   `QLocale(code).textDirection()` automatically — no extra code needed there. If it needs a font
   `Theme.qml`'s Archivo doesn't cover (as Arabic and Hebrew do, via the bundled Noto Sans faces),
   add the pairing to `Theme.qml`'s `rtlFonts` map **and** `language_manager.cpp`'s
   `font_family_for()` — the two must agree, since `Theme.rtlFonts` is documentation for the
   pairing and `font_family_for()` is what actually swaps the application-wide default font
   `LanguageManager::updateFontFamily()` applies on every switch.

## The pseudo-locale QA fixture

`apps/gui/translations/ac3gui_xx.ts` is not a language — "xx" is not an ISO 639 code, and it
never appears in `LanguageManager::availableLanguages()` or Preferences' picker. It exists to prove
the extraction → compile → load pipeline works end to end without depending on any one language's
catalogue, and to catch a string that bypasses `qsTr()` entirely.

`tools/generators/gen_pseudo_locale.py` reads `ac3gui_fr.ts` for the message set and mechanically
decorates **every** message it finds there — accented characters, a bracketed and length-padded
wrapper (`[Àccéntéd téxt ~~~~]`) — so what it writes is complete for the extraction it was run
against. A visible string that reaches the screen *without* that decoration under the
pseudo-locale either never went through `qsTr()`, or was added after the fixture was last
generated.

The fixture is not in `AC3_TS_FILES`, so `ac3gui_lupdate` does not touch it and CI's drift check
cannot see it going stale. It is stale now: 766 messages against the six languages' 795, missing
the keyboard and text-size pass. Regenerate it after `ac3gui_lupdate` picks up new source
strings:

```sh
cmake --build --preset <preset> --target ac3gui_lupdate
python tools/generators/gen_pseudo_locale.py
```

It is loaded only through an `AC3GUI_LOCALE=xx` environment override
(`LanguageManager::applyInitialLanguage()`, checked ahead of the persisted setting and the system
locale) — `apps/gui/tests/CMakeLists.txt` sets this for `tst_localisation_pipeline.qml`'s ctest
entry alone, and it is embedded only into `ac3gui_qmltests`, never into the shipped `ac3gui`
binary (`apps/gui/CMakeLists.txt`'s own comment on `AC3_PSEUDO_TS_FILE` says why).
