"""The AC-4 reference's presentation names against the committed name table.

tests/golden/ac4dec/presentations/presentation-names.tsv lists sequences of frames'
presentation_name bytes and the name each leaves (ETSI TS 103 190-2 clause 6.3.3.1.4, read as
src/ac4dec/ERRATA.md's "A presentation name in chunks" reads it).
tests/ac4dec/test_ac4dec_api.cpp decodes each sequence through the decoder in hand-built frames;
this test holds tools/references/ac4_presentations.py's PresentationName, written separately, to
the same table.

stdlib `unittest`, run by ci.yml's script-lint job with the other oracle tests.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
TABLE = REPO / "tests" / "golden" / "ac4dec" / "presentations" / "presentation-names.tsv"

sys.path.insert(0, str(REPO / "tools" / "references"))

import ac4_presentations  # noqa: E402  (the path above has to come first)


def cases():
    for line in TABLE.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        name, frames = fields[0], fields[1]
        expected = fields[2] if len(fields) > 2 else ""
        yield name, [None if f == "-" else bytes.fromhex(f) for f in frames.split()], expected


class PresentationNames(unittest.TestCase):
    def test_table(self):
        seen = 0
        for name, frames, expected in cases():
            with self.subTest(case=name):
                assembled = ac4_presentations.PresentationName()
                for data in frames:
                    assembled.frame(data)
                self.assertEqual(assembled.name, expected)
            seen += 1
        self.assertGreaterEqual(seen, 10)

    def test_change_of_source_forgets_the_name(self):
        assembled = ac4_presentations.PresentationName()
        assembled.frame(b"Main\x00")
        assembled.forget()
        self.assertEqual(assembled.name, "")


if __name__ == "__main__":
    unittest.main()
