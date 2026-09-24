"""Unit tests for generate_support_matrices.py, which renders the committed
support-matrix Markdown from docs/assets/data/support-catalogue.json and, with
--check, fails CI when the committed files are stale.

What it must catch: a stale or hand-edited generated file, an orphan file in
the generated directory, a catalogue that references an unknown state /
evidence id, a row with the wrong cell count, a page or download link that
does not resolve, a duplicate or incomplete download entry, and a schema
version it does not understand. A synthetic catalogue + docs tree is built in
a temp root.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import copy
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_support_matrices as gsm

CATALOGUE = {
    "schema_version": 1,
    "states": {"yes": {"label": "Supported"}, "no": {"label": "Not yet"}},
    "evidence": {"ci": "CI-tested", "none": ""},
    "platform_groups": [{"id": "linux", "label": "Linux", "page": "platforms/linux.md"},
                        {"id": "esp", "label": "ESP|32", "page": "platforms/esp.md"}],
    "overview": [{"label": "CLI", "page": "cli.md",
                  "cells": {"linux": ["yes", "ci", "x86 and arm"], "esp": ["no", "none", ""]}}],
    "feature_overview": [{"label": "Decode", "page": "decode.md",
                          "cells": {"linux": ["yes", "ci", ""],
                                    "esp": ["yes", "none", "line\nbreak"]}}],
    "application_capabilities": {"columns": ["CLI", "App"], "rows": [["Encode", "yes", "no"]]},
    "platform_details": {"linux": {"title": "Linux", "variants": ["x86", "arm"],
                                   "rows": [["Encode", ["yes", "ci", ""], ["no", "none", ""]]]}},
    "downloads": [{"product": "cli", "platform": "linux", "variant": "x86", "state": "yes",
                   "artifact": "tar", "action": "Download", "url": "/install/linux/",
                   "note": "n"},
                  {"product": "cli", "platform": "linux", "variant": "arm", "state": "yes",
                   "artifact": "tar", "action": "Download", "url": "https://example.com/x",
                   "note": "n"}],
}


class Generate(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        docs = self.root / "docs"
        for page in ("platforms/linux.md", "platforms/esp.md", "cli.md", "decode.md",
                     "install/linux/index.md"):
            (docs / page).parent.mkdir(parents=True, exist_ok=True)
            (docs / page).write_text("#\n")
        self.write_catalogue(CATALOGUE)

    def tearDown(self):
        self._tmp.cleanup()

    def write_catalogue(self, data):
        path = self.root / gsm.CATALOGUE
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data))

    def run_main(self, *extra):
        buf = io.StringIO()
        with mock.patch.object(sys, "argv", ["x", "--root", str(self.root), *extra]), \
                contextlib.redirect_stdout(buf):
            rc = gsm.main()
        return rc, buf.getvalue()

    def test_generate_then_check_is_current(self):
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertEqual(out.count("wrote "), 4)
        overview = (self.root / gsm.GENERATED / "platform-product-overview.md").read_text()
        self.assertIn("Do not edit by hand", overview)
        self.assertIn("[ESP&#124;32](esp.md)", overview)
        self.assertIn("[CLI](../cli.md)", overview)
        self.assertIn("**Supported**<br>CI-tested<br>x86 and arm", overview)
        self.assertIn("| **Not yet** |", overview)
        feature = (self.root / gsm.GENERATED / "platform-feature-overview.md").read_text()
        self.assertIn("line break", feature)
        rc, out = self.run_main("--check")
        self.assertEqual(rc, 0, out)
        self.assertIn("4 generated file(s) current", out)

    def test_check_fails_on_stale_missing_and_orphan_files(self):
        self.run_main()
        gen = self.root / gsm.GENERATED
        (gen / "platform-linux.md").write_text("hand edited\n")
        (gen / "application-capabilities.md").unlink()
        (gen / "platform-retired.md").write_text("orphan\n")
        rc, out = self.run_main("--check")
        self.assertEqual(rc, 1)
        for name in ("platform-linux.md", "application-capabilities.md", "platform-retired.md"):
            self.assertIn(f"{name}::generated support matrix is stale", out)

    def assert_catalogue_error(self, mutate, message):
        data = copy.deepcopy(CATALOGUE)
        mutate(data)
        self.write_catalogue(data)
        rc, out = self.run_main("--check")
        self.assertEqual(rc, 1)
        self.assertIn("::error file=docs/assets/data/support-catalogue.json::", out)
        self.assertIn(message, out)

    def test_schema_version(self):
        self.assert_catalogue_error(lambda d: d.update(schema_version=2),
                                    "schema_version must be 1")

    def test_bad_cells(self):
        self.assert_catalogue_error(
            lambda d: d["overview"][0]["cells"].update(linux=["maybe", "ci", ""]),
            "unknown support state: maybe")
        self.assert_catalogue_error(
            lambda d: d["overview"][0]["cells"].update(linux=["yes", "rumour", ""]),
            "unknown evidence state: rumour")
        self.assert_catalogue_error(
            lambda d: d["overview"][0]["cells"].update(linux=["yes"]),
            "matrix cell must have [state, evidence, note]")

    def test_row_shape_errors(self):
        self.assert_catalogue_error(lambda d: d["overview"][0]["cells"].pop("esp"),
                                    "overview row 'CLI' has missing={'esp'}")
        self.assert_catalogue_error(
            lambda d: d["platform_details"]["linux"]["rows"][0].pop(),
            "linux row 'Encode' has 1 cells for 2 variants")
        self.assert_catalogue_error(
            lambda d: d["application_capabilities"]["rows"][0].append("extra"),
            "application row 'Encode' has 3 cells for 2 columns")

    def test_link_errors(self):
        self.assert_catalogue_error(lambda d: d["overview"][0].update(page="gone.md"),
                                    "catalogue page does not exist: docs/gone.md")
        self.assert_catalogue_error(lambda d: d["downloads"][0].update(url="/nowhere/"),
                                    "download action does not resolve to a docs page: /nowhere/")

    def test_download_errors(self):
        self.assert_catalogue_error(lambda d: d["downloads"][0].pop("note"),
                                    "download entry 0 has missing={'note'}")
        self.assert_catalogue_error(lambda d: d["downloads"][0].update(state="beta"),
                                    "download entry 0 has unknown state 'beta'")
        self.assert_catalogue_error(lambda d: d["downloads"][1].update(variant="x86"),
                                    "duplicate download selector path")
        self.assert_catalogue_error(lambda d: d["downloads"][0].update(note=""),
                                    "download entry 0 contains an empty value")

    def test_unreadable_catalogue(self):
        (self.root / gsm.CATALOGUE).write_text("{")
        rc, _out = self.run_main("--check")
        self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
