"""Unit tests for check_corpus.py, the fixture-corpus drift gate.

Every published quality series is only comparable across commits because the
fixtures under it never move. The gate must FAIL on: a fixture whose bytes
changed, one named in the manifest but missing, one present but unregistered,
a structural mismatch (channels / rate / bits / duration), and an unreadable
audio header - and pass on a corpus that matches.

A synthetic corpus (a WAV, a hand-built FLAC STREAMINFO and a "bitstream")
is built in a temp dir and the module's AUDIO/MANIFEST are pointed at it.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
import wave
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_corpus as cc


def flac_bytes(channels=2, rate=48000, bits=16, frames=96000, first_block_type=0):
    packed = (rate << 44) | ((channels - 1) << 41) | ((bits - 1) << 36) | frames
    info = bytes(10) + packed.to_bytes(8, "big") + bytes(16)
    return b"fLaC" + bytes([0x80 | first_block_type]) + len(info).to_bytes(3, "big") + info


class Corpus(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.audio = Path(self._tmp.name)
        self.manifest = self.audio / "corpus.json"
        wav = self.audio / "tone.wav"
        with wave.open(str(wav), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(8000)
            w.writeframes(struct.pack("<800h", *([0] * 800)))
        (self.audio / "music.flac").write_bytes(flac_bytes())
        (self.audio / "stream.ec3").write_bytes(b"\x0b\x77" * 512)
        self.entries = [
            {"fixture": "tone.wav", "kind": "synthetic", "channels": 1, "sample_rate": 8000,
             "bits": 16, "duration_s": 0.1},
            {"fixture": "music.flac", "kind": "programme", "channels": 2, "sample_rate": 48000,
             "bits": 16, "duration_s": 2.0},
            {"fixture": "stream.ec3", "kind": "bitstream", "bytes": 1024},
        ]
        self.rehash()

    def tearDown(self):
        self._tmp.cleanup()

    def rehash(self):
        for e in self.entries:
            e["sha256"] = hashlib.sha256((self.audio / e["fixture"]).read_bytes()).hexdigest()
        self.write_manifest()

    def write_manifest(self):
        self.manifest.write_text(json.dumps({"corpus_version": 7, "fixtures": self.entries}))

    def run_main(self):
        buf = io.StringIO()
        with mock.patch.object(cc, "AUDIO", self.audio), \
                mock.patch.object(cc, "MANIFEST", self.manifest), contextlib.redirect_stdout(buf):
            rc = cc.main()
        return rc, buf.getvalue()

    def test_matching_corpus_passes(self):
        (self.audio / "subdir").mkdir()   # directories are not fixtures
        rc, out = self.run_main()
        self.assertEqual(rc, 0, out)
        self.assertIn("corpus_version 7", out)
        self.assertIn("2ch 48000 Hz 16-bit 2.00s", out)
        self.assertIn("all 3 fixtures match", out)

    def test_changed_bytes_fail(self):
        (self.audio / "stream.ec3").write_bytes(b"\x0b\x77" * 513)
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("stream.ec3: SHA-256", out)
        self.assertIn("bump CORPUS_VERSION", out)

    def test_missing_and_unregistered_fixtures_fail(self):
        (self.audio / "tone.wav").rename(self.audio / "renamed.wav")
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("tone.wav: named in the manifest but not in the tree", out)
        self.assertIn("renamed.wav: present in tests/golden/audio/ but not in the corpus", out)

    def test_structural_mismatches_are_named(self):
        self.entries[1].update(channels=6, sample_rate=44100, bits=24, duration_s=3.0)
        self.write_manifest()
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        for text in ("channels 2 != manifest 6", "sample_rate 48000 != manifest 44100",
                     "bits 16 != manifest 24", "duration 2.000s != manifest 3.0s"):
            self.assertIn(text, out)

    def test_unreadable_header_is_reported_not_fatal(self):
        (self.audio / "music.flac").write_bytes(flac_bytes(first_block_type=4))
        self.rehash()
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("could not read audio parameters", out)
        self.assertIn("not STREAMINFO", out)

    def test_missing_manifest_fails(self):
        self.manifest.unlink()
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("has no manifest", out)

    def test_flac_parser(self):
        path = self.audio / "x.flac"
        path.write_bytes(flac_bytes(channels=6, rate=44100, bits=24, frames=123456789))
        self.assertEqual(cc.flac_streaminfo(path), (6, 44100, 24, 123456789))
        path.write_bytes(b"RIFF....")
        with self.assertRaisesRegex(ValueError, "not a FLAC file"):
            cc.flac_streaminfo(path)


if __name__ == "__main__":
    unittest.main()
