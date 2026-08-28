# ac3gui — localisation

The app's UI chrome (menus, buttons, Preferences) is translated via Qt Linguist, the same
mechanism and the same canonical language set as the sibling CountdownSolver project. This page
covers what is translated today, how to update or extend it, and the pseudo-locale QA fixture the
pipeline itself is tested against — see [Loading a source](loading-a-source.md) and the rest of
this guide for what the untranslated majority of the app still looks like.

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
`qt_add_translations()` call wires these in: it scans every QML file `AC3_QML_FILES` lists (the
only source of `qsTr()` calls in this app — no `.cpp` file has one) for translatable strings, then
compiles each `.ts` to a `.qm` and embeds it as a resource under `:/i18n` at build time.
`LanguageManager` (`apps/gui/language_manager.{hpp,cpp}`) loads the matching `.qm` for the active
language and applies right-to-left layout mirroring for Arabic, Hebrew and Yiddish
(`Main.qml`'s `LayoutMirroring` root, and the bundled Noto Sans Arabic/Hebrew faces those three
scripts need — Latin has no glyph coverage for either).

Preferences → Appearance → **Language** switches it live, no restart needed
(`languageManager.setLanguage(code)`, a plain context property `main.cpp` installs the same way
`EncoderController` is installed — not a `QML_SINGLETON`, since `LanguageManager` takes
constructor arguments a singleton factory cannot supply).

## What's translated today

Coverage is real but **partial by design**, tracked here rather than silently incomplete: 98 of
758 extracted messages (roughly the app's window chrome, header buttons, tab names, the Guided
wizard's step titles, and the entire Preferences dialog) are finished per language, identically
across all six. The remainder — mostly the longer explanatory `PrefsNote`-style paragraphs
scattered through the Format/Objects/Coding tools/Metadata tabs, and most of the `Accessible.*`
descriptions the roadmap UX3 accessibility pass added alongside this — stay in English rather
than showing blank, exactly the fallback a real partial translation gives everywhere else in this
app. Completing the rest is tracked as follow-on work, not a hidden gap: search any `.ts` file for
`type="unfinished"` to see exactly what remains.

## Updating an existing translation

1. Regenerate the `.ts` files from current source strings:

   ```sh
   cmake --build --preset <preset> --target ac3gui_lupdate
   ```

   Any new or changed `qsTr()` string shows up as a `<translation type="unfinished">` entry
   (empty, or holding the last-known text) in the relevant `.ts` file(s). CI's own "Check
   translations are up to date" step (`.github/workflows/_build.yml`) reruns this same target and
   fails the build if it produces a diff nobody committed — the same drift this step exists to
   catch.
2. Open the `.ts` file in **Qt Linguist** (ships with Qt), or edit the `<translation>` elements
   directly, and fill in the unfinished entries. Editing by hand, remove the `type="unfinished"`
   attribute yourself once an entry is genuinely reviewed.
3. Rebuild normally to recompile the `.qm` and pick up the change.

### Finding a string

`lupdate` groups each `.ts` file's messages into a `<context><name>` block named after the QML
component it came from (`Main`, `PreferencesDialog`, `GuidedWizard`, `ChannelMeter`, ...) — use
that to jump straight to the right area of a large `.ts` file.

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

`apps/gui/translations/ac3gui_xx.ts` is not a real language — "xx" is not an ISO 639 code, and it
never appears in `LanguageManager::availableLanguages()` or Preferences' picker. It exists purely
to prove the extraction → compile → load pipeline works end to end without depending on any real
language's translation being complete, and to catch a string that bypasses `qsTr()` entirely.

`tools/generators/gen_pseudo_locale.py` reads a real `lupdate` extraction and mechanically
decorates **every** message — accented characters, a bracketed and length-padded wrapper
(`[Àccéntéd téxt ~~~~]`) — so the fixture is 100% "complete" by construction, unlike the six real
languages. A visible string that reaches the screen *without* that decoration under the
pseudo-locale means it never went through `qsTr()` in the first place.

Regenerate it after `ac3gui_lupdate` picks up new source strings:

```sh
cmake --build --preset <preset> --target ac3gui_lupdate
python tools/generators/gen_pseudo_locale.py
```

It is loaded only through an `AC3GUI_LOCALE=xx` environment override
(`LanguageManager::applyInitialLanguage()`, checked ahead of the persisted setting and the system
locale) — `apps/gui/tests/CMakeLists.txt` sets this for `tst_localisation_pipeline.qml`'s ctest
entry alone, and it is embedded only into `ac3gui_qmltests`, never into the shipped `ac3gui`
binary (`apps/gui/CMakeLists.txt`'s own comment on `AC3_PSEUDO_TS_FILE` says why).
