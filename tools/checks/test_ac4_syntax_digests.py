"""The AC-4 reference parser against the committed syntax digests.

tools/references/ac4_syntax.py reads the syntax of every AC-4 substream a
frame holds, transcribed from ETSI TS 103 190-1 and -2 separately from the
decoder in src/ac4dec, and summarises what it read as one digest line per
frame and substream: how many syntax elements, where the last one ended, and a
CRC-32 over every element's (bit offset, width, value). The digests of the
committed DEE streams are under tests/golden/ac4dec/, and
tests/ac4dec/test_ac4dec_syntax.cpp requires the decoder to produce the same
lines. This test is the other half: it requires the reference parser still to
produce them, so neither transcription can change alone.

stdlib `unittest`, run by ci.yml's script-lint job with the other oracle
tests; the reference parser is stdlib-only for the same reason.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
GOLDEN = REPO / "tests" / "golden" / "ac4dec"
STREAMS = REPO / "tests" / "golden" / "external-baseline"

sys.path.insert(0, str(REPO / "tools" / "references"))

import ac4_syntax  # noqa: E402  (the path above has to come first)


def golden_files():
    return sorted(GOLDEN.glob("*.tsv"))


class SyntaxDigests(unittest.TestCase):
    def test_there_are_digests(self):
        self.assertTrue(golden_files(), f"no digest files under {GOLDEN}")

    def test_reference_parser_reproduces_every_digest(self):
        for golden in golden_files():
            lines = golden.read_text(encoding="utf-8").splitlines()
            header = lines[0]
            prefix = "# ac4-syntax-digest/1 "
            self.assertTrue(header.startswith(prefix), f"{golden.name}: header {header!r}")
            stream = header[len(prefix):]
            with self.subTest(stream=stream):
                data = (STREAMS / stream).read_bytes()
                diagnostics = []
                produced = ac4_syntax.digest_lines(data, stream, diagnostics=diagnostics)
                self.assertEqual(produced, lines)
                # Every committed stream is channel-coded 2.0, 5.1 or
                # immersive stereo, which the parser reads to the end: no
                # refusal, and every invariant holds.
                self.assertEqual(diagnostics, [])


if __name__ == "__main__":
    unittest.main()
