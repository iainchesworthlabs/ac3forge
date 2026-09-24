"""Unit tests for sonar_to_sarif.py, the SonarCloud -> GitHub Code Scanning
bridge.

What must hold: every result GitHub receives has a location (one without is
dropped, because a single locationless result makes GitHub reject the whole
upload), severities map to SARIF levels, ruleIndex points at the right rule,
only VULNERABILITY rules carry a security-severity, pagination stops at the
last page, and a failed API call aborts with the HTTP status rather than
writing a partial file.

No network: urllib.request.urlopen is patched with a fake Sonar server.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import base64
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
import unittest.mock as mock
import urllib.error
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sonar_to_sarif as s2s


def issue(rule, severity="MAJOR", itype="BUG", line=None, text_range=None, hash_=None,
          component="proj:src/a.cpp"):
    out = {"rule": rule, "severity": severity, "type": itype, "message": f"msg {rule}",
           "component": component}
    if line is not None:
        out["line"] = line
    if text_range is not None:
        out["textRange"] = text_range
    if hash_:
        out["hash"] = hash_
    return out


class FakeSonar:
    def __init__(self, pages, rules, fail_path=None, total=None):
        self.pages = pages
        self.rules = rules
        self.fail_path = fail_path
        self.total = total
        self.requests = []

    def __call__(self, request, timeout=None):
        url = urllib.parse.urlsplit(request.full_url)
        params = dict(urllib.parse.parse_qsl(url.query))
        self.requests.append((url.path, params, request.get_header("Authorization")))
        if url.path == self.fail_path:
            raise urllib.error.HTTPError(request.full_url, 403, "Forbidden", {},
                                         io.BytesIO(b"insufficient privileges"))
        if url.path == "/api/issues/search":
            page = int(params["p"])
            total = self.total if self.total is not None else sum(len(p) for p in self.pages)
            body = {"paging": {"total": total}, "issues": self.pages[page - 1],
                    "components": [{"key": "proj:src/a.cpp", "path": "src/a.cpp"},
                                   {"key": "proj:nopath"}]}
        else:
            keys = params["rule_keys"].split(",")
            body = {"rules": [{"key": k, "name": self.rules[k]} for k in keys if k in self.rules]}
        return io.BytesIO(json.dumps(body).encode())


class Conversion(unittest.TestCase):
    def test_locationless_issues_are_dropped_with_a_warning(self):
        issues = [issue("r:1", line=3), issue("r:2")]
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            sarif = s2s.build_sarif(issues, {}, {}, "proj", "https://s", "org")
        results = sarif["runs"][0]["results"]
        self.assertEqual(len(results), 1)
        self.assertTrue(all(r.get("locations") for r in results))
        self.assertIn("Dropped 1 issue(s)", err.getvalue())
        self.assertEqual([r["id"] for r in sarif["runs"][0]["tool"]["driver"]["rules"]], ["r:1"])

    def test_results_regions_levels_and_rule_index(self):
        issues = [
            issue("z:vuln", "BLOCKER", "VULNERABILITY",
                  text_range={"startLine": 2, "startOffset": 0, "endLine": 2, "endOffset": 4},
                  hash_="abc"),
            issue("a:bug", "MINOR", line=7, component="my proj:src/b.cpp"),
            issue("a:bug", "WEIRD", line=8, component="other:x"),
        ]
        with contextlib.redirect_stderr(io.StringIO()):
            sarif = s2s.build_sarif(issues, {"proj:src/a.cpp": "src/a.cpp"},
                                    {"z:vuln": "Vuln name"}, "my proj", "https://s", "o/rg")
        run = sarif["runs"][0]
        rules = run["tool"]["driver"]["rules"]
        self.assertEqual([r["id"] for r in rules], ["a:bug", "z:vuln"])
        self.assertEqual(rules[0]["name"], "a:bug")     # falls back to the key
        self.assertNotIn("security-severity", rules[0]["properties"])
        self.assertEqual(rules[1]["properties"]["security-severity"], "6.0")
        self.assertIn("open=z%3Avuln", rules[1]["helpUri"])
        self.assertIn("organization=o%2Frg", rules[1]["helpUri"])
        self.assertIn("id=my%20proj", run["tool"]["driver"]["informationUri"])
        vuln, minor, weird = run["results"]
        self.assertEqual((vuln["level"], vuln["ruleIndex"]), ("error", 1))
        self.assertEqual(vuln["locations"][0]["physicalLocation"]["region"],
                         {"startLine": 2, "startColumn": 1, "endLine": 2, "endColumn": 5})
        self.assertEqual(vuln["partialFingerprints"], {"issueHash": "abc"})
        self.assertEqual((minor["level"], minor["ruleIndex"]), ("note", 0))
        self.assertEqual(minor["locations"][0]["physicalLocation"]["artifactLocation"]["uri"],
                         "src/b.cpp")
        self.assertEqual(weird["level"], "warning")
        self.assertEqual(weird["locations"][0]["physicalLocation"]["artifactLocation"]["uri"],
                         "other:x")


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.out = Path(self._tmp.name) / "o.sarif"

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, fake, *extra, token="tok"):
        argv = ["x", "--organization", "org", "--project-key", "proj",
                "--output", str(self.out), *extra]
        env = {"SONAR_TOKEN": token} if token else {}
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.dict(os.environ, env, clear=True), \
                mock.patch.object(s2s.urllib.request, "urlopen", fake), \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            s2s.main()
        return stdout.getvalue(), stderr.getvalue()

    def test_paginates_and_writes_sarif(self):
        with mock.patch.object(s2s, "PAGE_SIZE", 2), mock.patch.object(s2s, "RULES_CHUNK", 1):
            fake = FakeSonar([[issue("r:1", line=1), issue("r:2", line=2)],
                              [issue("r:1", line=3)]], {"r:1": "One", "r:2": "Two"})
            out, _ = self.run_main(fake, "--branch", "main", "--types", "BUG, ,VULNERABILITY")
        paths = [p for p, _, _ in fake.requests]
        self.assertEqual(paths.count("/api/issues/search"), 2)
        self.assertEqual(paths.count("/api/rules/search"), 2)   # chunked
        first = fake.requests[0][1]
        self.assertEqual((first["branch"], first["types"], first["resolved"]),
                         ("main", "BUG,VULNERABILITY", "false"))
        self.assertEqual(fake.requests[0][2], "Basic " + base64.b64encode(b"tok:").decode())
        sarif = json.loads(self.out.read_text())
        self.assertEqual(len(sarif["runs"][0]["results"]), 3)
        self.assertEqual({r["name"] for r in sarif["runs"][0]["tool"]["driver"]["rules"]},
                         {"One", "Two"})
        self.assertIn("Wrote 3 issues across 2 distinct rules", out)

    def test_summary_counts_only_what_was_written(self):
        """Regression: the "Wrote N issues across M rules" line counted every
        fetched issue and rule, including the locationless ones build_sarif
        drops, so it overstated what reached the SARIF file."""
        fake = FakeSonar([[issue("r:1", line=1), issue("r:2"), issue("r:3")]],
                         {"r:1": "One", "r:2": "Two", "r:3": "Three"})
        out, _ = self.run_main(fake)
        sarif = json.loads(self.out.read_text())
        self.assertEqual(len(sarif["runs"][0]["results"]), 1)
        self.assertIn("Wrote 1 issues across 1 distinct rules", out)
        self.assertIn("2 locationless issue(s) dropped", out)

    def test_warns_above_the_elasticsearch_cap(self):
        fake = FakeSonar([[]], {}, total=s2s.ES_RESULT_CAP + 1)
        _, err = self.run_main(fake)
        self.assertIn("above the 10000 Elasticsearch pagination cap", err)
        self.assertNotIn("branch", fake.requests[0][1])

    def test_http_error_aborts_without_output(self):
        fake = FakeSonar([[]], {}, fail_path="/api/issues/search")
        with self.assertRaises(SystemExit) as ctx:
            self.run_main(fake)
        self.assertIn("HTTP 403", str(ctx.exception))
        self.assertIn("insufficient privileges", str(ctx.exception))
        self.assertFalse(self.out.exists())

    def test_missing_token_and_empty_types_are_refused(self):
        with self.assertRaisesRegex(SystemExit, "SONAR_TOKEN"):
            self.run_main(FakeSonar([[]], {}), token="")
        with self.assertRaisesRegex(SystemExit, "empty list"):
            self.run_main(FakeSonar([[]], {}), "--types", " , ")


if __name__ == "__main__":
    unittest.main()
