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

import ast
import contextlib
import io
import sys
import unittest
import unittest.mock as mock
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

    def test_esp_component_packaging_script_lights_only_esp(self):
        # esp-component.yml's own path filter names this file directly - see
        # docs/ci-lanes.md.
        hits = gate.classify(["tools/packaging/pack_esp_component.py"])
        self.assertEqual(lit(hits, *ALL_LANES), {"esp"})

    def test_python_examples_light_only_python(self):
        # wheels.yml's own path filter names examples/python/ directly - the
        # rest of examples/ is plain C++, core's concern via its own build.
        hits = gate.classify(["examples/python/basic_encode.py"])
        self.assertEqual(lit(hits, *ALL_LANES), {"python"})

    def test_rust_only_change_lights_only_rust(self):
        hits = gate.classify(["rust/ac3forge/src/lib.rs"])
        self.assertEqual(lit(hits, *ALL_LANES), {"rust"})

    def test_windows_driver_change_does_not_light_other_platforms(self):
        hits = gate.classify(["apps/windows/driver/ac3sink.inf"])
        self.assertEqual(lit(hits, *ALL_LANES), {"windows"})
        self.assertFalse(hits["core"])


class SharedDesktopAppTest(unittest.TestCase):
    """apps/cli, apps/gui, apps/common, apps/crucible, apps/hearth: one program on three OSes."""

    def test_shared_cli_change_lights_all_three_desktop_platforms_only(self):
        hits = gate.classify(["apps/cli/commands/audio_io.cpp"])
        self.assertEqual(lit(hits, *ALL_LANES), {"windows", "linux", "macos"})

    def test_crucible_change_lights_all_three_desktop_platforms_only(self):
        hits = gate.classify(["apps/crucible/engine/engine.cpp"])
        self.assertEqual(lit(hits, *ALL_LANES), {"windows", "linux", "macos"})

    def test_hearth_change_lights_all_three_desktop_platforms_only(self):
        hits = gate.classify(["apps/hearth/engine/player.cpp"])
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


class SatellitesDirectTest(unittest.TestCase):
    """The run after a merge: a satellite lane runs only for a path in its own tree."""

    def classify(self, *paths):
        return gate.classify(list(paths), satellites_direct=True)

    def test_a_core_change_reaches_the_platforms_and_no_satellite(self):
        hits = self.classify("src/coder/eac3_encoder.cpp")
        self.assertEqual(lit(hits, *ALL_LANES), {"core", "windows", "linux", "macos"})

    def test_the_nightly_still_fans_a_core_change_out_to_every_satellite(self):
        hits = gate.classify(["src/coder/eac3_encoder.cpp"])
        self.assertTrue(all(hits[lane] for lane in gate.SATELLITES if lane != "npm"))

    def test_a_satellites_own_tree_still_lights_it(self):
        for path, lane in (
            ("apps/android/app/build.gradle.kts", "android"),
            ("esp-idf/ac3forge/CMakeLists.txt", "esp"),
            ("rust/ac3forge/src/lib.rs", "rust"),
            ("python/ac3forge/__init__.py", "python"),
            ("apps/wasm/src/main.cpp", "wasm"),
        ):
            with self.subTest(path=path):
                self.assertEqual(lit(self.classify(path), *ALL_LANES), {lane})

    def test_a_core_change_and_a_satellite_change_together_light_both(self):
        hits = self.classify("src/coder/x.cpp", "rust/ac3forge/src/lib.rs")
        self.assertEqual(lit(hits, *ALL_LANES), {"core", "windows", "linux", "macos", "rust"})

    def test_a_path_only_a_platform_owns_is_unchanged(self):
        hits = self.classify("apps/cli/commands/audio_io.cpp")
        self.assertEqual(lit(hits, *ALL_LANES), {"windows", "linux", "macos"})

    def test_the_conservative_cases_still_light_everything(self):
        for paths in ([], ["planning/notes.txt"], [".github/workflows/ci.yml"]):
            with self.subTest(paths=paths):
                self.assertEqual(self.classify(*paths), dict.fromkeys(gate.LANES, True))

    def test_force_all_still_wins(self):
        hits = gate.classify(["docs/a.md"], force_all=True, satellites_direct=True)
        self.assertEqual(hits, dict.fromkeys(gate.LANES, True))

    def test_the_satellites_are_lanes_and_only_they_are_held_back(self):
        self.assertTrue(set(gate.SATELLITES) <= set(gate.LANES))
        held = set(gate.CORE_FANOUT) & set(gate.SATELLITES)
        self.assertEqual(held, {"android", "wasm", "esp", "rust", "python"})

    def test_the_trees_the_esp_component_ships_light_the_esp_lane_too(self):
        for path in (
            "src/forge/coder/eac3_encoder.cpp",
            "src/arithmetic/fixed32.hpp",
            "cmake/Compiler.cmake",
            "CMakeLists.txt",
        ):
            with self.subTest(path=path):
                hits = self.classify(path)
                # The other satellites stay with the nightly run.
                self.assertEqual(lit(hits, *gate.SATELLITES), {"esp"})
                self.assertTrue(hits["core"])

    def test_the_ac4_trees_leave_the_esp_lane_to_the_nightly_run(self):
        hits = self.classify("src/ac4dec/src/decoder.cpp")
        self.assertEqual(lit(hits, *ALL_LANES), {"core", "windows", "linux", "macos"})

    def test_a_root_file_the_component_does_not_ship_does_not_light_it(self):
        hits = self.classify("CMakePresets.json")
        self.assertFalse(hits["esp"])
        self.assertTrue(hits["core"])


class EspComponentStagingTest(unittest.TestCase):
    """The lane has to light for everything the ESP-IDF component's packer stages.

    tools/packaging/pack_esp_component.py stages an explicit list, and a tree or file
    added to it without the lane learning about it is a change the run after a merge
    would not build (the 2026-09-29 break was a new src/ tree missing from that list).
    """

    @staticmethod
    def staged(name):
        packaging = Path(__file__).resolve().parents[2] / "tools" / "packaging"
        packer = packaging / "pack_esp_component.py"
        for node in ast.parse(packer.read_text(encoding="utf-8")).body:
            if isinstance(node, ast.Assign) and any(
                getattr(target, "id", "") == name for target in node.targets
            ):
                return ast.literal_eval(node.value)
        raise AssertionError(f"{name} is not assigned in {packer}")

    def test_every_staged_tree_lights_the_esp_lane_in_the_run_after_a_merge(self):
        trees = self.staged("STAGED_TREES")
        self.assertTrue(trees)
        for tree in trees:
            with self.subTest(tree=tree):
                hits = gate.classify([f"{tree}/anything.cpp"], satellites_direct=True)
                self.assertTrue(hits["esp"])

    def test_every_staged_file_a_build_reads_lights_it_too(self):
        files = self.staged("STAGED_FILES")
        self.assertIn("CMakeLists.txt", files)
        for name in files:
            if name.endswith(gate.DOCS_SUFFIX) or name in gate.DOCS_ROOT_FILES:
                continue  # documentation: no build reads it
            with self.subTest(name=name):
                self.assertTrue(gate.classify([name], satellites_direct=True)["esp"])


class MainTest(unittest.TestCase):
    """main() reads paths from stdin and writes lane=true|false lines that the
    workflow feeds to $GITHUB_OUTPUT."""

    def run_main(self, argv, stdin=""):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "stdin", io.StringIO(stdin)), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = gate.main(argv)
        lanes = dict(line.split("=", 1) for line in out.getvalue().splitlines())
        return rc, lanes

    def test_force_all_ignores_stdin(self):
        rc, lanes = self.run_main(["x", "--force-all"], stdin="docs/readme.md\n")
        self.assertEqual(rc, 0)
        self.assertEqual(set(lanes), set(gate.LANES))
        self.assertTrue(all(v == "true" for v in lanes.values()))

    def test_stdin_paths_are_classified(self):
        rc, lanes = self.run_main(["x"], stdin="apps/android/app/build.gradle.kts\n\n")
        self.assertEqual(rc, 0)
        self.assertEqual(lanes["android"], "true")
        self.assertIn("false", lanes.values())

    def test_satellites_direct_flag_holds_the_fanout_back(self):
        _, lanes = self.run_main(["x", "--satellites-direct"], stdin="src/coder/x.cpp\n")
        self.assertEqual((lanes["core"], lanes["linux"]), ("true", "true"))
        self.assertEqual(
            {lanes[s] for s in ("android", "wasm", "esp", "rust", "python", "npm")}, {"false"}
        )

    def test_empty_stdin_runs_everything(self):
        """No paths is not 'nothing changed' - fail open, run every lane."""
        _, lanes = self.run_main(["x"], stdin="")
        self.assertTrue(all(v == "true" for v in lanes.values()))


if __name__ == "__main__":
    unittest.main()
