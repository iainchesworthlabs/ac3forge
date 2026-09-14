"""Unit tests for classify_changes.py, the CI lane classifier.

stdlib `unittest`, not pytest, for the reason test_write_measurement_badges.py
gives: this runs in ci.yml's script-lint job, which installs nothing beyond
its linters, and the script under test is stdlib-only itself.

Three things are worth holding down here, each one a way this classifier
could quietly break the CI lane partitions it exists to drive (see
docs/ci-lanes.md): a platform-only change must NOT light every lane (that
would defeat the whole point of splitting them), a core change MUST fan out
to every platform (a Windows-only leg has no way to know it also depends on
src/), and anything this script does not recognise - an unmapped top-level
directory, an empty file list, a workflow/action edit - must default to
building rather than silently skipping.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import classify_changes as gate


def lit(hits, *lanes):
    """The subset of LANES that came back true, as a set - order-independent."""
    return {lane for lane in lanes if hits[lane]}


ALL_LANES = set(gate.LANES)


class PlatformOnlyChangeTest(unittest.TestCase):
    """The reason this classifier exists: a platform-only PR skips the rest."""

    def test_android_only_change_lights_only_android(self):
        hits = gate.classify(["apps/android/app/build.gradle.kts"])
        self.assertEqual(lit(hits, *ALL_LANES), {"android"})

    def test_esp_only_change_lights_only_esp(self):
        hits = gate.classify(["esp-idf/ac3forge/CMakeLists.txt"])
        self.assertEqual(lit(hits, *ALL_LANES), {"esp"})

    def test_rust_only_change_lights_only_rust(self):
        hits = gate.classify(["rust/ac3forge/src/lib.rs"])
        self.assertEqual(lit(hits, *ALL_LANES), {"rust"})

    def test_windows_driver_change_does_not_light_other_platforms(self):
        hits = gate.classify(["apps/windows/driver/ac3sink.inf"])
        self.assertEqual(lit(hits, *ALL_LANES), {"windows"})
        self.assertFalse(hits["core"])


class SharedDesktopAppTest(unittest.TestCase):
    """apps/cli, apps/gui, apps/common, apps/crucible are one program on three OSes."""

    def test_shared_cli_change_lights_all_three_desktop_platforms_only(self):
        hits = gate.classify(["apps/cli/commands/audio_io.cpp"])
        self.assertEqual(lit(hits, *ALL_LANES), {"windows", "linux", "macos"})

    def test_crucible_change_lights_all_three_desktop_platforms_only(self):
        hits = gate.classify(["apps/crucible/engine/engine.cpp"])
        self.assertEqual(lit(hits, *ALL_LANES), {"windows", "linux", "macos"})


class CoreFanoutTest(unittest.TestCase):
    """A library change has to be validated everywhere it is built."""

    def test_src_change_fans_out_to_every_platform_and_language_lane(self):
        hits = gate.classify(["src/coder/eac3_encoder.cpp"])
        self.assertEqual(
            lit(hits, *ALL_LANES),
            {"core", "windows", "linux", "macos", "android", "wasm", "esp", "rust", "python"},
        )
        # npm is deliberately not in the fan-out: see LANE_PREFIXES's comment.
        self.assertFalse(hits["npm"])

    def test_root_cmakelists_counts_as_core_but_a_nested_one_does_not(self):
        self.assertTrue(gate.classify(["CMakeLists.txt"])["core"])
        hits = gate.classify(["apps/wasm/CMakeLists.txt"])
        self.assertFalse(hits["core"])
        self.assertEqual(lit(hits, *ALL_LANES), {"wasm"})

    def test_tools_ci_itself_is_core(self):
        # The classifier's own directory - if this ever stops being core, a
        # change to classify_changes.py would stop re-validating itself.
        self.assertTrue(gate.classify(["tools/ci/classify_changes.py"])["core"])


class ConservativeDefaultTest(unittest.TestCase):
    """Unrecognised or absent input must build, never silently skip."""

    def test_empty_file_list_lights_every_lane(self):
        self.assertEqual(gate.classify([]), dict.fromkeys(gate.LANES, True))

    def test_blank_lines_only_counts_as_empty(self):
        self.assertEqual(gate.classify(["", "  ", "\n"]), dict.fromkeys(gate.LANES, True))

    def test_unmapped_top_level_directory_lights_every_lane(self):
        hits = gate.classify(["planning/some-notes.txt"])
        self.assertEqual(hits, dict.fromkeys(gate.LANES, True))

    def test_one_unmapped_path_among_many_still_lights_everything(self):
        # A mostly-recognisable PR with one path this script has no rule for
        # must not fall back to "only what matched" - the unmapped path is
        # exactly the case the fallback exists for.
        hits = gate.classify(["apps/android/app/build.gradle.kts", "planning/notes.txt"])
        self.assertEqual(hits, dict.fromkeys(gate.LANES, True))

    def test_force_all_ignores_the_path_list_entirely(self):
        hits = gate.classify(["apps/android/app/build.gradle.kts"], force_all=True)
        self.assertEqual(hits, dict.fromkeys(gate.LANES, True))

    def test_ci_self_change_lights_every_lane(self):
        hits = gate.classify([".github/workflows/ci.yml"])
        self.assertEqual(hits, dict.fromkeys(gate.LANES, True))

    def test_shared_action_change_lights_every_lane(self):
        hits = gate.classify([".github/actions/setup-vcpkg/action.yml"])
        self.assertEqual(hits, dict.fromkeys(gate.LANES, True))


class DocsOnlyChangeTest(unittest.TestCase):
    """Not a build lane, but should not accidentally light one either."""

    def test_docs_only_change_lights_only_docs(self):
        hits = gate.classify(["docs/ci-lanes.md"])
        self.assertEqual(lit(hits, *ALL_LANES), {"docs"})

    def test_root_markdown_file_counts_as_docs(self):
        hits = gate.classify(["README.md"])
        self.assertEqual(lit(hits, *ALL_LANES), {"docs"})

    def test_license_counts_as_docs(self):
        hits = gate.classify(["LICENSE"])
        self.assertEqual(lit(hits, *ALL_LANES), {"docs"})


class NpmAndWasmSplitTest(unittest.TestCase):
    """js/ backs both the wasm E2E demo and the npm package's own tests."""

    def test_js_change_lights_both_npm_and_wasm_but_nothing_else(self):
        hits = gate.classify(["js/src/index.ts"])
        self.assertEqual(lit(hits, *ALL_LANES), {"npm", "wasm"})


if __name__ == "__main__":
    unittest.main()
