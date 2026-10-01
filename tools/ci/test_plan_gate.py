"""Unit tests for plan_gate.py: what the PR gate builds for a given change."""

from __future__ import annotations

import contextlib
import io
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import plan_gate as gate


def plan(*paths: str) -> dict[str, str]:
    return gate.plan(list(paths))


class DocsOnly(unittest.TestCase):
    def test_markdown_and_docs_tree_skip_the_build(self):
        got = plan("docs/library/index.md", "CHANGELOG.md", "planning/ac4.md")
        self.assertEqual((got["docs_only"], got["build"], got["gui"]), ("true", "false", "false"))

    def test_generated_snippets_and_assets_are_docs(self):
        got = plan("docs-snippets/generated/platform-linux.md", "assets/icon/iclforge-icon.svg")
        self.assertEqual((got["docs_only"], got["build"]), ("true", "false"))

    def test_root_docs_files(self):
        got = plan("LICENSE", "mkdocs.yml")
        self.assertEqual(got["docs_only"], "true")


class BuildRelevant(unittest.TestCase):
    def test_library_change_builds_without_the_gui(self):
        got = plan("src/ac4dec/decoder.cpp", "tests/ac4dec/test_decoder.cpp")
        self.assertEqual((got["build"], got["gui"], got["docs_only"]), ("true", "false", "false"))

    def test_cli_change_builds_without_the_gui(self):
        got = plan("apps/cli/commands/decode.cpp")
        self.assertEqual((got["build"], got["gui"]), ("true", "false"))

    def test_gui_trees_pull_qt_in(self):
        for path in (
            "apps/gui/qml/Main.qml",
            "apps/hearth/ui/qml/Main.qml",
            "apps/crucible/src/engine.cpp",
            "apps/common/settings.cpp",
            "tests/hearth/test_engine.cpp",
            "cmake/FindQt6.cmake",
        ):
            with self.subTest(path=path):
                got = plan(path)
                self.assertEqual((got["build"], got["gui"]), ("true", "true"))

    def test_top_level_build_files_pull_qt_in(self):
        for path in ("CMakeLists.txt", "CMakePresets.json", "vcpkg.json"):
            with self.subTest(path=path):
                self.assertEqual(plan(path)["gui"], "true")

    def test_tests_cmake_list_alone_does_not_pull_qt_in(self):
        got = plan("tests/CMakeLists.txt")
        self.assertEqual((got["build"], got["gui"]), ("true", "false"))

    def test_fixtures_and_gate_scripts_build(self):
        for path in (
            "tests/golden/audio/reference_51.wav",
            "tools/checks/compare_wav.py",
            "fuzz/seeds/x.bin",
        ):
            with self.subTest(path=path):
                self.assertEqual(plan(path)["build"], "true")

    def test_examples_build_but_python_examples_do_not(self):
        self.assertEqual(plan("examples/encode_wav.cpp")["build"], "true")
        self.assertEqual(plan("examples/python/encode.py")["build"], "false")

    def test_mixed_docs_and_code_builds(self):
        got = plan("docs/library/index.md", "src/forge/x.cpp")
        self.assertEqual((got["build"], got["docs_only"]), ("true", "false"))


class NotBuiltByTheLinuxGate(unittest.TestCase):
    """Lanes the Linux gate cannot exercise wait for the post-merge run."""

    def test_platform_and_language_trees(self):
        for path in (
            "esp-idf/iclforge/component.c",
            "esphome/x.yaml",
            "apps/android/app/build.gradle.kts",
            "apps/wasm/main.cpp",
            "apps/baremetal/probe.cpp",
            "python/iclforge/__init__.py",
            "rust/src/lib.rs",
            "js/package.json",
            "packaging/conan/conanfile.py",
            "requirements/requirements-lint.txt",
            "apps/linux/tray-vm/guest/provision.sh",
        ):
            with self.subTest(path=path):
                got = plan(path)
                self.assertEqual((got["build"], got["docs_only"]), ("false", "false"))

    def test_root_config_files(self):
        for path in (
            ".gitignore",
            ".clang-tidy",
            "ruff.toml",
            "sonar-project.properties",
            "tsan.supp",
        ):
            with self.subTest(path=path):
                got = plan(path)
                self.assertEqual(
                    (got["build"], got["gui"], got["docs_only"]), ("false", "false", "false")
                )

    def test_tooling_that_static_checks_already_test(self):
        for path in ("tools/ci/plan_gate.py", "tools/hearth/ota.py", "tools/release/x.py"):
            with self.subTest(path=path):
                self.assertEqual(plan(path)["build"], "false")

    def test_other_workflows_are_linted_not_built(self):
        got = plan(
            ".github/workflows/fuzz.yml", ".github/dependabot.yml", ".github/branch-protection.md"
        )
        self.assertEqual(got["build"], "false")


class GateMachinery(unittest.TestCase):
    """A change to the gate itself is proven by running all of it."""

    def test_gate_files_build_everything(self):
        for path in (
            ".github/workflows/pr-gate.yml",
            ".github/workflows/_compare.yml",
            ".github/workflows/_static.yml",
            ".github/workflows/_toolchain-versions.yml",
            ".github/actions/build-leg/action.yml",
            ".github/actions/setup-vcpkg/action.yml",
            ".github/toolchain/02-gcc-toolchain.sh",
            ".github/toolchain-versions.json",
        ):
            with self.subTest(path=path):
                got = plan(path)
                self.assertEqual((got["build"], got["gui"]), ("true", "true"))


class Machinery(unittest.TestCase):
    """A change to the gate is also proven on Windows, which a pull request skips."""

    def test_gate_files_set_it(self):
        for path in (
            ".github/workflows/pr-gate.yml",
            ".github/workflows/_compare.yml",
            ".github/actions/build-leg/action.yml",
        ):
            with self.subTest(path=path):
                self.assertEqual(plan(path)["machinery"], "true")

    def test_ordinary_changes_do_not(self):
        for path in ("src/forge/x.cpp", "docs/a.md", ".github/workflows/fuzz.yml", "python/x.py"):
            with self.subTest(path=path):
                self.assertEqual(plan(path)["machinery"], "false")

    def test_one_gate_file_among_others_is_enough(self):
        got = plan("src/forge/x.cpp", ".github/workflows/_static.yml")
        self.assertEqual(got["machinery"], "true")

    def test_full_runs_do_not_need_it(self):
        self.assertEqual(gate.plan([], force_all=True)["machinery"], "false")


class Conservative(unittest.TestCase):
    def test_unknown_path_builds_everything(self):
        got = plan("brand-new-dir/thing.bin")
        self.assertEqual((got["build"], got["gui"]), ("true", "true"))

    def test_one_unknown_path_among_docs_is_enough(self):
        got = plan("docs/a.md", "brand-new-dir/thing.bin")
        self.assertEqual((got["build"], got["gui"], got["docs_only"]), ("true", "true", "false"))

    def test_empty_list_builds_everything(self):
        got = gate.plan([])
        self.assertEqual((got["build"], got["gui"], got["docs_only"]), ("true", "true", "false"))

    def test_blank_lines_are_ignored(self):
        got = gate.plan(["", "  ", "docs/a.md"])
        self.assertEqual(got["docs_only"], "true")

    def test_force_all(self):
        got = gate.plan(["docs/a.md"], force_all=True)
        self.assertEqual((got["build"], got["gui"], got["docs_only"]), ("true", "true", "false"))


class QueueMode(unittest.TestCase):
    def test_a_library_change_builds_qt_in_the_queue(self):
        got = gate.plan(["src/forge/x.cpp"], gui_on_build=True)
        self.assertEqual((got["build"], got["gui"]), ("true", "true"))
        self.assertIn("merge queue", got["gui_reason"])

    def test_a_change_that_does_not_build_stays_off(self):
        got = gate.plan(["docs/a.md"], gui_on_build=True)
        self.assertEqual((got["build"], got["gui"]), ("false", "false"))
        got = gate.plan(["python/x.py"], gui_on_build=True)
        self.assertEqual((got["build"], got["gui"]), ("false", "false"))


class Compare(unittest.TestCase):
    """Whether a queue entry also runs the performance and memory comparisons."""

    def test_a_change_under_src_asks_for_them(self):
        for path in ("src/forge/x.cpp", "src/ac4dec/decoder.cpp", "src/audio/y.hpp"):
            with self.subTest(path=path):
                self.assertEqual(plan(path)["compare"], "true")

    def test_one_src_path_among_others_is_enough(self):
        self.assertEqual(plan("docs/a.md", "apps/cli/x.cpp", "src/forge/x.cpp")["compare"], "true")

    def test_changes_that_cannot_alter_the_library_do_not(self):
        for path in (
            "tests/forge/test_x.cpp",
            "apps/cli/commands/decode.cpp",
            "apps/gui/qml/Main.qml",
            "cmake/Compiler.cmake",
            "CMakeLists.txt",
            "tools/checks/x.py",
            "python/x.py",
        ):
            with self.subTest(path=path):
                self.assertEqual(plan(path)["compare"], "false")

    def test_documentation_under_src_is_still_documentation(self):
        got = plan("src/ac4dec/ERRATA.md")
        self.assertEqual((got["docs_only"], got["compare"]), ("true", "false"))

    def test_the_gates_own_machinery_does_not(self):
        self.assertEqual(plan(".github/workflows/pr-gate.yml")["compare"], "false")

    def test_a_full_run_and_an_empty_list_do_not(self):
        self.assertEqual(gate.plan(["src/forge/x.cpp"], force_all=True)["compare"], "false")
        self.assertEqual(gate.plan([])["compare"], "false")

    def test_the_queue_mode_asks_the_same_question(self):
        self.assertEqual(gate.plan(["src/forge/x.cpp"], gui_on_build=True)["compare"], "true")
        self.assertEqual(gate.plan(["apps/cli/x.cpp"], gui_on_build=True)["compare"], "false")


class Reason(unittest.TestCase):
    def test_names_the_first_path_that_forced_the_build(self):
        got = plan("docs/a.md", "src/forge/x.cpp", "src/forge/y.cpp")
        self.assertIn("src/forge/x.cpp", got["reason"])

    def test_names_the_path_that_pulled_qt_in(self):
        got = plan("src/forge/x.cpp", "apps/gui/qml/Main.qml")
        self.assertIn("apps/gui/qml/Main.qml", got["gui_reason"])

    def test_docs_only_reason(self):
        self.assertIn("documentation", plan("docs/a.md")["reason"])


class Cli(unittest.TestCase):
    def run_main(self, argv, stdin_text):
        out, err = io.StringIO(), io.StringIO()
        old_stdin = sys.stdin
        sys.stdin = io.StringIO(stdin_text)
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = gate.main(["plan_gate.py", *argv])
        finally:
            sys.stdin = old_stdin
        return rc, out.getvalue(), err.getvalue()

    def test_prints_github_output_lines(self):
        rc, out, _ = self.run_main([], "src/forge/x.cpp\n")
        self.assertEqual(rc, 0)
        lines = dict(line.split("=", 1) for line in out.splitlines())
        self.assertEqual(lines["build"], "true")
        self.assertEqual(lines["gui"], "false")
        self.assertEqual(lines["docs_only"], "false")

    def test_every_output_line_is_a_key_value_pair(self):
        # $GITHUB_OUTPUT is line-oriented: a stray line would be a parse error.
        _, out, _ = self.run_main([], "src/forge/x.cpp\nbrand-new-dir/thing.bin\n")
        keys = [line.split("=", 1)[0] for line in out.splitlines()]
        self.assertEqual(
            keys, ["build", "gui", "docs_only", "machinery", "compare", "reason", "gui_reason"]
        )

    def test_force_all_ignores_stdin(self):
        rc, out, _ = self.run_main(["--force-all"], "docs/a.md\n")
        self.assertEqual(rc, 0)
        self.assertIn("build=true", out.splitlines())
        self.assertIn("gui=true", out.splitlines())


if __name__ == "__main__":
    unittest.main()
