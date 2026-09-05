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

What the audit found and the review has to settle, per language:

- **fr** — "endpoint" is rendered two ways (`point de sortie` and `point de terminaison`), and
  "receiver" two ways (`récepteur` and `ampli`). "underrun" became `sous-alimentation`, which is
  undernourishment.
- **es** — "endpoint" is rendered three ways (`punto de salida`, `punto final`, bare `salida`).
  "underrun" became `subdesbordamiento`.
- **de** — "silent device" is `stilles Gerät` in most entries and `stummes Gerät` in five. "pin"
  appears as `FESTLEGEN`, `Festlegung` and the bare loan `Pin`.
- **ar** — "bed" is `القاعدة` in nineteen entries and `سرير` (a sleeping bed) in two. "mode" and
  "place" both became `وضع`, so "best mode" and "place an application" read alike. "underrun"
  became a feeding deficiency.
- **he** — "bed" is `מיטה`, a sleeping bed, in fourteen entries. "pin" is both `הצמדה` and
  `נעיצה`. The address form is singular in some entries and plural in others. "Centre", the
  button, was translated as the noun.
- **yi** — "device" is `מכשיר` in sixteen entries and `מיטל` (means, medium) in eleven.
  "endpoint" is both `ענדפּונקט` and `ענדפּוינט`. Compounds join with an ASCII hyphen in some
  entries and a maqaf (U+05BE) in others; the maqaf is the one to keep. The address form is both
  `דו` and `איר`.

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

Two things to expect from the first regeneration after the source pass:

- The position phrases moved from `RoomPage.qml` into `RoomWords.qml` after the last extraction,
  and `lupdate` matches an entry on its context and its source. They arrive under a new
  `RoomWords` context, unfinished, even where the English did not change, and `-no-obsolete`
  drops the `RoomPage` originals. The previous renderings are in
  `apps/crucible/translations/ac3crucible_<code>.ts` at commit `c5c9df76`, under `RoomPage`, and
  can be pasted across before the review reads them.
- Around forty entries per language are newly unfinished because the English was reworded into
  whole sentences, on top of the fifteen strings the catalogues never had.

Fill the unfinished entries in **Qt Linguist**, or by editing the `<translation>` elements
directly; when editing by hand, remove the `type="unfinished"` attribute once the entry has
been read by someone who reads the language. Rebuild to recompile the `.qm` files, which
are embedded as resources under `:/i18n`.

`lupdate` groups the messages by the QML component they came from — `Main`, `OutputPage`,
`RoomPage`, `RoomWords`, `SettingsPage`, `SignalPath` — which is how to find a string in a large
file.

## What the gate checks

`tests/crucible/test_translations.cpp` reads the six files and runs on every platform, in the
plain `ac3tests` binary, so a developer's own `ctest` sees it:

| Rule | State |
| --- | --- |
| Every finished translation is non-empty | live |
| The `%1`..`%9` placeholders of a source all appear in its translation | live |
| A brand term in a source appears in the translation | live |
| All six files hold the same set of (context, source) pairs | live |
| No entry is left `type="unfinished"` | written, off |
| No dead entry (`type="vanished"` or `type="obsolete"`) is left in a file | written, off |

The last two are the rules the regeneration turns on. Until the catalogues have been regenerated
from the current source and the review has filled them, an unfinished entry is the expected state
of a freshly extracted string; the two cases say how many they are tolerating and pass, and their
names carry "(idle until refilled)" so a green run is not read as a guarantee. The constant
`kCatalogueRefilled` at the top of the file is what turns them on, in the commit that lands the
refilled catalogues; the names lose the parenthesis with it.

One check is not written yet: a CI step that runs `ac3crucible_lupdate` and then
`git diff --exit-code -- apps/crucible/translations`, the way `.github/workflows/_build.yml`
already does for `ac3gui`. It belongs in the same commit as the refilled catalogues, because
against the tree as it stands it would be red on the first run. It is what turns a forgotten
regeneration into a red check rather than a quietly stale catalogue.

The rules that read a translation skip an entry marked unfinished: an entry nobody has translated
yet is empty or holds the previous language's text, and failing the placeholder rule on it would
say nothing about the pass.

## Screenshots in one language

```sh
QT_QPA_PLATFORM=offscreen ac3crucible --language ar --page settings --shot crucible-settings-ar.png
```

`--language` sets `AC3GUI_LOCALE` for that run only, so a capture never moves the language the
person chose. What to look at in the result: the header title at the right edge, the page switch
and the status pill on the left, the combo-box chevrons on the left of their controls, the room
plan's L speaker still on the left, the endpoint table's heads unclipped, and no missing-glyph
boxes in the Arabic or Hebrew text.
