"""Generate ac3gui's pseudo-locale QA fixture from a real lupdate extraction.

Roadmap UX3 needs a "stub locale that proves the [translation] pipeline works
end to end" without requiring an actual translation. A hand-picked handful of
decorated strings would only prove the mechanism for the strings someone
remembered to decorate; this generator instead pseudo-localizes EVERY message
lupdate finds, mechanically, so the fixture is complete by construction and
catches something a partial real translation cannot: a user-visible string
that bypasses qsTr() entirely never gets decorated, so it stands out in a
screenshot or a test assertion exactly because it looks unmodified.

The transform (per <source>, applied to every <translation>, including ones
already marked type="unfinished" - unfinished is a completeness state that
means nothing for a fixture defined to be 100% synthetic):
  1. Accent every vowel (a->à, e->é, ...) so the string reads as
     "foreign" at a glance without changing its meaning or length.
  2. Pad the end with a bracketed run of filler characters sized to ~40% of
     the original length, the standard pseudo-localization technique for
     surfacing truncation/overflow in fixed-width UI - exactly the kind of
     bug a real, shorter translation would hide until it happened to land on
     a long phrase.
  3. Wrap the whole thing in [...] so a decorated string is unmistakable even
     before reading it closely.
Qt placeholders (%1, %2, ..., %L1) and the numerus %n are left untouched -
mangling one would break the very string it is meant to protect, and a real
translation would leave them alone too.

Run after regenerating the real catalog (regenerating this fixture is a
separate, explicit step - see docs/gui/localisation.md - not part of the
default build, since apps/gui/translations/ac3gui_xx.ts is committed, static
input to ac3gui_qmltests):

    cmake --build <preset> --target ac3gui_lupdate
    python tools/generators/gen_pseudo_locale.py

It reads apps/gui/translations/ac3gui_fr.ts purely as a source of TRUTH for
which messages currently exist (any one of the six real .ts files would do -
lupdate scans the identical QML sources for all of them) and writes
apps/gui/translations/ac3gui_xx.ts from scratch with the same message set.
Deterministic: same input always produces the same output byte for byte.
"""

from __future__ import annotations

import re
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE_TS = REPO_ROOT / "apps" / "gui" / "translations" / "ac3gui_fr.ts"
OUTPUT_TS = REPO_ROOT / "apps" / "gui" / "translations" / "ac3gui_xx.ts"

_VOWEL_ACCENTS = str.maketrans(
    "aeiouAEIOU",
    "àéîõûÀÉÎÕÛ",
)

# %1.. %99, %L1.. %L99, the plural placeholder %n, and any HTML/rich-text tag
# (AboutDialog's licence text embeds <a href="...">) - split out so the
# accenting/padding pass below only ever touches literal, user-facing text.
# A decorated tag name or attribute (e.g. "hréf") does not merely look odd -
# Qt's rich-text renderer does not recognise it and the markup breaks, which
# a real translation would never do either.
_PLACEHOLDER_RE = re.compile(r"%L?\d+|%n|<[^>]*>")


def pseudo_localize(source: str) -> str:
    """Decorate one <source> string. Empty input stays empty (a real
    translation would leave a blank source blank too, and accenting nothing
    then padding it would fabricate content out of an empty string)."""
    if not source:
        return source

    pieces = _PLACEHOLDER_RE.split(source)
    placeholders = _PLACEHOLDER_RE.findall(source)
    decorated_pieces = [piece.translate(_VOWEL_ACCENTS) for piece in pieces]

    decorated = decorated_pieces[0]
    # strict=True documents the real invariant here, not just satisfies the
    # linter: re.split() on a pattern with N matches always yields N + 1
    # pieces, so placeholders and decorated_pieces[1:] are the same length
    # by construction - a mismatch would mean that invariant broke.
    for placeholder, piece in zip(placeholders, decorated_pieces[1:], strict=True):
        decorated += placeholder + piece

    padding_len = max(1, round(len(source) * 0.4))
    filler = "~" * padding_len
    return f"[{decorated} {filler}]"


def build_pseudo_locale() -> ET.ElementTree:
    source_tree = ET.parse(SOURCE_TS)
    source_root = source_tree.getroot()

    root = ET.Element("TS", attrib={"version": source_root.get("version", "2.1"), "language": "xx"})
    for context in source_root.findall("context"):
        name_el = context.find("name")
        out_context = ET.SubElement(root, "context")
        ET.SubElement(out_context, "name").text = name_el.text if name_el is not None else ""

        for message in context.findall("message"):
            out_message = ET.SubElement(out_context, "message")
            source_el = message.find("source")
            source_text = source_el.text or "" if source_el is not None else ""
            ET.SubElement(out_message, "source").text = source_text

            comment_el = message.find("comment")
            if comment_el is not None:
                ET.SubElement(out_message, "comment").text = comment_el.text

            translation_el = message.find("translation")
            numerusforms = [] if translation_el is None else translation_el.findall("numerusform")
            out_translation = ET.SubElement(out_message, "translation")
            if numerusforms:
                # No %n usage exists in apps/gui/qml today (see this script's
                # own docstring), but handled rather than silently dropped in
                # case one is ever added: every numerus form gets the same
                # treatment as a plain source string.
                for form in numerusforms:
                    ET.SubElement(out_translation, "numerusform").text = pseudo_localize(
                        form.text or ""
                    )
            else:
                out_translation.text = pseudo_localize(source_text)

    ET.indent(root, space="    ")
    return ET.ElementTree(root)


def main() -> None:
    if not SOURCE_TS.exists():
        raise SystemExit(
            f"{SOURCE_TS} does not exist yet - run the ac3gui_lupdate build target first "
            "(see this script's own docstring)."
        )
    tree = build_pseudo_locale()
    OUTPUT_TS.parent.mkdir(parents=True, exist_ok=True)
    tree.write(OUTPUT_TS, encoding="utf-8", xml_declaration=True)
    # ElementTree has no DOCTYPE support, so it's spliced in by hand - lupdate
    # always emits one (<!DOCTYPE TS>) right after the XML declaration, and
    # matching that exactly keeps this fixture indistinguishable from a real
    # lupdate/Linguist-edited file for any tool that inspects it.
    text = OUTPUT_TS.read_text(encoding="utf-8")
    declaration, _, rest = text.partition("\n")
    OUTPUT_TS.write_text(f"{declaration}\n<!DOCTYPE TS>\n{rest}", encoding="utf-8")
    print(f"wrote {OUTPUT_TS}")


if __name__ == "__main__":
    main()
