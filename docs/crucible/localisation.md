# Languages

Crucible's window ships in seven languages: English, plus French, German, Spanish, Arabic, Hebrew
and Yiddish. This page is what the chooser offers, how a language is picked at launch, what
changes in the window when the language reads right to left, the glossary the translations are
held to, and how the catalogues are regenerated and gated.

The pipeline itself — `LanguageManager`, the `.ts` and `.qm` files, the shared canonical language
set — belongs to [ac3gui's localisation page](../gui/localisation.md); Crucible reuses it rather
than carrying a second copy. What is below is Crucible's own half.

## What the chooser offers

**Settings → Appearance → Language** lists System first, then the seven languages by their own
names. Choosing one applies it immediately: nothing restarts, and every string in the window is
re-read from the catalogue.

| Code | Language | Catalogue |
| --- | --- | --- |
| `en` | English | none — the `qsTr()` source text |
| `fr` | Français | `apps/crucible/translations/ac3crucible_fr.ts` |
| `de` | Deutsch | `ac3crucible_de.ts` |
| `es` | Español | `ac3crucible_es.ts` |
| `ar` | العربية | `ac3crucible_ar.ts` |
| `he` | עברית | `ac3crucible_he.ts` |
| `yi` | יידיש | `ac3crucible_yi.ts` |

## How the language is chosen at launch

Three sources, in this order; the first that names a language the app ships wins.

1. `AC3GUI_LOCALE` in the environment. Nothing is written to the settings store when it is used,
   so a run under it leaves the person's own choice alone. `ac3crucible --language <code>` sets
   it for that run: it is how a screenshot in one language is captured.
2. The `language/code` setting, which the chooser writes. **System** removes it.
3. The system locale, mapped to one of the seven. A locale the app does not ship falls back to
   English rather than to an empty window.

## Right to left

Arabic, Hebrew and Yiddish set the application's layout direction, and the window root
(`apps/crucible/ui/qml/Main.qml`) mirrors on it: rows run right to left, anchors swap sides, the
header title sits at the right edge, and the combo-box chevrons move to the left of their
controls. Padding is outside that: neither a `Text`'s padding nor a `Control`'s `leftPadding` and
`rightPadding` swaps on its own, so a control padded differently on its two sides reads its own
`mirrored` property and swaps them itself.

Two things stay where they are:

- **The markers and the speakers in the room views.** The plan and the elevation are pictures of
  a room. L is the left speaker in every language, so anything placed at an explicit x keeps it.
  The rest of those views — their captions, their rows, their labels — mirrors like any other
  page; only the placements hold still.
- **Figures.** A coordinate, a bitrate or a channel count is written left to right inside a
  right-to-left line, which is what the bidirectional algorithm does with them.

Arabic and Hebrew script have no coverage in Archivo, the window's own face, so the two bundled
Noto Sans faces are swapped in for those three languages (`font_family_for()` in
`apps/gui/language_manager.cpp`, and `Theme.rtlFonts`, which carry the same pairing).

Two cases in `apps/crucible/ui/tests/qml/tst_shell.qml` hold this: the header title moves to the
right half of the window under Arabic and back under English, and the plan's L speaker stays in
the left third of the view under Hebrew.

## Writing a string that can be translated

Every string a person reads goes through `qsTr()`, and each one is a whole phrase rather than a
piece of one. A line assembled at run time from pieces — `qsTr("Restore ") + name` — cannot be
translated: the word order, the article and the agreement all depend on what is put in the gap.
So a value goes in as a placeholder:

```qml
text: qsTr("Restore %1").arg(CrucibleController.previousDefaultName)
```

and a line that reads differently in two states is two whole strings rather than one string with
a fragment glued on:

```qml
text: CrucibleController.objectsEnabled ? qsTr("apps → %1 · objects signed")
                                        : qsTr("apps → %1 · 5.1 bed only")
```

Four more rules the window follows:

- **No platform names in QML prose.** Windows, PipeWire, WirePlumber, a driver, a package — the
  words that are true on one platform and wrong on another come from `CrucibleController`
  properties that the platform seams fill (`nullSinkName`, `silentDeviceAdvice`,
  `silentDeviceFromPackage`, `movesDefault`). A sentence in QML says "the system default output",
  and the platform's own words arrive through a `%1`.
- **A short string carries a comment.** `lupdate` copies a `//:` line above a `qsTr()` into the
  catalogue as an `<extracomment>`, which is what the translator reads. "Centre", "Put", "point"
  and "PIN" are verbs in this window and nouns in most dictionaries, and the comment is what says
  which.
- **A column sizes to its own heading.** The endpoint table's fixed columns take the width of
  their translated head (`page.pcmColumn` and its neighbours in `OutputPage.qml`), because
  "PCM CH" is "PCM-KAN." in German and "قنوات PCM" in Arabic.
- **Anchor an overlay rather than placing it at an x.** Mirroring moves an anchor and leaves an
  `x` and a padding where they are, so an element written as `x: box.width - 22` sits on the
  right under Arabic too. Where a control's two paddings differ, choose them from
  `control.mirrored`. The combo-box chevrons in `OutputPage.qml` and `SettingsPage.qml` are the
  worked example: `anchors.right` with a margin, and a `contentItem` whose left and right padding
  are picked by `mirrored`.

## The glossary

The rule per term, applied consistently within a language. **The renderings below are
recommendations awaiting a reader fluent in the language**: they came out of an audit of the
mechanical translations, not from a native speaker, and the review pass is where each is
confirmed or replaced. What is not open to preference is consistency — one rendering per term
per language, whichever it turns out to be.

Never translated, in any of the six: AC3Forge, Crucible, Dolby Atmos, Atmos, Dolby Digital
(Plus), E-AC-3, AC-3, JOC, PCM, HDMI, PipeWire, WirePlumber, Windows, `ac3forge` (lower case: the
library), the speaker abbreviations L/R/C/Ls/Rs, the bitrate labels, and 5.1 / 7.1 / 7.1.4.
German may join a brand into a compound with hyphens — Dolby-Atmos-Szene — which is that
language's own orthography and correct. The three palette names (Signal, Ink, Console) are
product names and stay as they are. A dash in the table is a term the audit made no
recommendation for; the reviewer chooses it and it goes in here.

| Term | de | fr | es | ar | he | yi |
| --- | --- | --- | --- | --- | --- | --- |
| application | Anwendung | application | aplicación | تطبيق | יישום | אַפּליקאַציע |
| bed | Bett | lit | cama | الطبقة الأساسية | שכבת הבסיס | בעט |
| object | Objekt | objet | objeto | كائن | אובייקט | אָביעקט |
| slot | Platz | emplacement | ranura | فتحة | משבצת | שטעל |
| endpoint | Endpunkt | point de sortie | punto de salida | نقطة نهاية | נקודת קצה | ענדפּונקט |
| device | Gerät | périphérique | dispositivo | جهاز | התקן | מכשיר |
| silent device | stilles Gerät | périphérique silencieux | dispositivo silencioso | الجهاز الصامت | ההתקן השקט | דער שטילער מכשיר |
| default output | Standardausgabe | sortie par défaut | salida predeterminada | المخرج الافتراضي | פלט ברירת המחדל | דיפֿאָלט־אַרויסגאַנג |
| tap (verb, noun) | abgreifen / Abgriff | capter / prise | tomar / toma | التقاط | לכידה | אָפּנעמען / אָפּנעמער |
| pin (verb) | anheften | épingler | fijar | تثبيت | הצמדה | פֿעסטשטעלן |
| underrun | Underrun | underrun | underrun | انقطاع في المخزن المؤقت | underrun | underrun |
| receiver | Receiver | récepteur | receptor | جهاز الاستقبال | מקלט | רעסיווער |
| engine | Engine | moteur | motor | المحرك | מנוע | מאָטאָר |
| driver | Treiber | pilote | controlador | برنامج التشغيل | מנהל התקן | דרײַווער |
| signing key | Signaturschlüssel | clé de signature | clave de firma | مفتاح التوقيع | — | — |
| place (verb) | platzieren | placer | colocar | وضع | מיקום | שטעלן |
| room | Raum | pièce | sala | الغرفة | חדר | צימער |
| mode | Modus | mode | modo | نمط | מצב | מאָדוס |
| stream | Stream | flux | flujo | البث | זרם | — |
| address form | Sie | vous | tú | masculine singular imperative | plural imperative | איר |

What the audit found and the review has to settle, per language. The counts were re-read off the
filled catalogues on 2026-09-06, so they are what a reviewer will find in the files today:

- **fr** — "endpoint" is rendered `point de sortie` in nine entries and `point de terminaison` in
  two, and "receiver" `récepteur` in five and `ampli` in two. "underrun" became
  `sous-alimentation`, which is undernourishment.
- **es** — "endpoint" is rendered `punto de salida` in nine entries, `punto final` in one, and
  bare `salida` elsewhere. "underrun" became `subdesbordamiento`.
- **de** — "silent device" is `stilles Gerät`, declined to its case, in nineteen entries and
  `Stummes Gerät` in one: the Advanced row's label on the Settings page, which is also that
  field's accessible name. "pin" is two stems for one term — `anheften` / `Anheftung` in six
  entries, against `FESTLEGEN` on the kicker and `Festlegung` in a sentence.
- **ar** — "bed" is `القاعدة` in thirteen entries, `الطبقة الأساسية` in seven and `سرير` (a
  sleeping bed) in two. "mode" and "place" both became `وضع`, so "best mode" and "place an
  application" read alike. "underrun" became a feeding deficiency.
- **he** — "bed" is `מיטה`, a sleeping bed, in fifteen entries against `שכבת הבסיס` in seven.
  The address form is singular in some entries and plural in others. "Centre", the button, is
  `מרכז`, the noun, where the extracomment says it is a verb.
- **yi** — "device" is `מכשיר` in twenty-eight entries and `מיטל` (means, medium) in five.
  "endpoint" is `ענדפּונקט` in twelve and `ענדפּוינט` in five. Compounds join with an ASCII hyphen
  in some entries and a maqaf (U+05BE) in others; the maqaf is the one to keep. The address form
  is mostly `איר`, with `דו` in one entry.

## Regenerating the catalogues

`lupdate` reads every QML file the app lists and the `tr()` calls in `crucible_controller.cpp`,
and writes what it finds into the six `.ts` files:

```sh
cmake --build --preset <preset> --target ac3crucible_lupdate
```

A new or reworded string arrives as `<translation type="unfinished">`, holding the previous text
where there was one. `LUPDATE_OPTIONS -no-obsolete` on the `qt_add_translations()` call in
`apps/crucible/CMakeLists.txt` drops a string the source no longer has rather than keeping it as
translation memory: git history is the memory, and the dead-entry rule below wants the files
clean after every regeneration.

The six files were regenerated from the current source and filled on 2026-09-06. Each carries 385
messages, none marked `unfinished`, `vanished` or `obsolete`, and the six hold the same set of
(context, source) pairs. That is the state the gate below now enforces, so a regeneration that
adds a string leaves the tree red until the new entry is filled.

Filled is not reviewed. The renderings are machine-made and no speaker of any of the six languages
has read them; the window says so in its own language note under the chooser. The glossary above
is what a review holds them to.

Fill a newly extracted entry in **Qt Linguist**, or by editing the `<translation>` elements
directly; when editing by hand, remove the `type="unfinished"` attribute once the entry has a
rendering. Rebuild to recompile the `.qm` files, which are embedded as resources under `:/i18n`.

`lupdate` groups the messages by the component they came from, which is how to find a string in a
large file. The sixteen contexts in the files today are `AboutDialog`, `AppRow`, `BedChip`,
`CrucibleController` (the `tr()` calls in `crucible_controller.cpp`), `FirstRunDialog`,
`LicencesDialog`, `Main`, `OutputPage`, `QObject`, `Room3DView`, `RoomKeys`, `RoomPage`,
`RoomView`, `RoomWords`, `SettingsPage` and `SignalPath`. The position phrases live under
`RoomWords` since they moved out of `RoomPage.qml`; the renderings they had before that move are
in `apps/crucible/translations/ac3crucible_<code>.ts` at commit `c5c9df76`, under `RoomPage`.

## What the gate checks

`tests/crucible/test_translations.cpp` reads the six files and runs on every platform, in the
plain `ac3tests` binary, so a developer's own `ctest` sees it:

| Rule | State |
| --- | --- |
| Every finished translation is non-empty | live |
| The `%1`..`%9` placeholders of a source all appear in its translation | live |
| A brand term in a source appears in the translation | live |
| All six files hold the same set of (context, source) pairs | live |
| No entry is left `type="unfinished"` | live |
| No dead entry (`type="vanished"` or `type="obsolete"`) is left in a file | live |

The last two were written idle and armed on 2026-09-06, when the catalogues were filled. The
constant `kCatalogueRefilled` at the top of the file is the switch, and it is now `true`; the two
case names no longer carry the "(idle until refilled)" caveat they were given while they tolerated
what they found. What they assert is that every entry has a translation, which is a smaller claim
than that every translation is right — the review the glossary describes is what settles that, and
arming these rules does not stand in for it.

`.github/workflows/_build.yml` also runs `ac3crucible_lupdate` and then
`git diff --exit-code -- apps/crucible/translations`, the way it already does for `ac3gui`. It
runs on the `windows-msvc` leg alone, because the `ac3crucible_lupdate` target exists only where
Crucible is configured and the matrix build tree carries Crucible on Windows; one leg is enough,
since extraction does not depend on the compiler. It turns a forgotten regeneration into a red
check rather than a quietly stale catalogue.

**`lupdate` sees only the platform it runs on, and deletes what it cannot see.** The seams under
`ui/platform/<os>/` are one file per platform with one compiled, so `tray_support.cpp`'s `tr()`
reaches `lupdate` on exactly one of them. Running it on Linux drops the Windows sentence —
translated, in every catalogue — and replaces it with the Linux one, unfinished; running it on
macOS does the same with its own. Nothing warns. That the gate above runs on `windows-msvc`
alone is what settles it: the catalogues hold Windows' sentence, a regeneration there produces
no diff, and the other two platforms' tray sentences are simply not in them. So regenerate on
Windows, and if you regenerate anywhere else, read the diff before committing it — a `-` on a
translated `<source>` you did not touch is this, and the fix is to put the other platform's
message back rather than to accept it.

The rules that read a translation skip an entry marked unfinished. No entry is marked that way
today, so the exemption applies to nothing in the tree; it is there for the window between a
regeneration that extracts a string and the edit that fills it, where failing the placeholder rule
on an empty entry would say nothing about the pass.

## Screenshots in one language

```sh
QT_QPA_PLATFORM=offscreen ac3crucible --language ar --page settings --shot crucible-settings-ar.png
```

`--language` sets `AC3GUI_LOCALE` for that run only, so a capture never moves the language the
person chose. What to look at in the result: the header title at the right edge, the page switch
and the status pill on the left, the combo-box chevrons on the left of their controls, the room
plan's L speaker still on the left, the endpoint table's heads unclipped, and no missing-glyph
boxes in the Arabic or Hebrew text.
