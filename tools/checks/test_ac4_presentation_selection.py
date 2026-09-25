"""The AC-4 reference parser's presentation selection against the committed selection table.

tests/ac4dec/test_ac4dec_presentations.cpp builds a table of tables of contents, each with a
choice (presentation_id, position, language, associated audio, headphones), a decoder level and
the presentation ETSI TS 103 190-2 clause 4.8.2 and src/ac4dec/ERRATA.md's readings select, and
holds the decoder to it; the table is committed as
tests/golden/ac4dec/presentations/presentation-selection.tsv. This test holds
tools/references/ac4_presentations.py, the selection transcribed separately in Python over
ac4_parse.py's reading of each table of contents, to the same table, so neither can change a
reading alone.

stdlib `unittest`, run by ci.yml's script-lint job with the other oracle tests.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
TABLE = REPO / "tests" / "golden" / "ac4dec" / "presentations" / "presentation-selection.tsv"

sys.path.insert(0, str(REPO / "tools" / "references"))

import ac4_parse  # noqa: E402  (the path above has to come first)
import ac4_presentations  # noqa: E402


def optional_int(field):
    return None if field == "-" else int(field)


def cases():
    lines = TABLE.read_text(encoding="utf-8").splitlines()
    for line in lines:
        if not line or line.startswith("#"):
            continue
        (name, frame, presentation_id, index, language, associated, associated_type, headphones,
         level, expected) = line.split("\t")
        choice = {"presentation_id": optional_int(presentation_id), "index": optional_int(index),
                  "language": "" if language == "-" else language,
                  "associated": optional_int(associated), "associated_type": associated_type,
                  "headphones": headphones == "1"}
        yield name, bytes.fromhex(frame), choice, int(level), optional_int(expected)


class PresentationSelection(unittest.TestCase):
    def test_there_are_cases(self):
        self.assertGreater(len(list(cases())), 20)

    def test_reference_selects_as_the_table_says(self):
        for name, frame, choice, level, expected in cases():
            with self.subTest(case=name):
                toc, _ = ac4_parse.parse_raw_frame(frame)
                selected = ac4_presentations.select_presentation(toc, choice, level)
                self.assertEqual(selected, expected)


if __name__ == "__main__":
    unittest.main()
