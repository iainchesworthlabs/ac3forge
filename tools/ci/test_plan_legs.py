"""Unit tests for plan_legs.py: which legs of the build matrix a run gets."""

from __future__ import annotations

import contextlib
import io
import json
import re
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import classify_changes
import plan_legs as pl

REPO = Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github" / "workflows"


def leg(name: str, preset: str, **more):
    """A valid leg; it runs on hosted labels unless `more` names a runner or a slot."""
    base = {"name": name, "preset": preset, "tier": "t2"}
    if "runner" not in more and "runner_slot" not in more:
        base["runner"] = ["ubuntu-latest"]
    base.update(more)
    return base


def catalogue(**by_platform):
    """A small valid catalogue; a platform not named gets one plain leg."""
    out = {
        "linux": [leg("Linux A", "linux-a", runner_slot="linux_runner_1")],
        "windows": [leg("Win A", "win-a", runner_slot="windows_runner_1")],
        "macos": [leg("Mac A", "mac-a", runner=["macos-latest"])],
    }
    for key in out:
        if key in by_platform:
            out[key] = by_platform[key]
    return out


ENV = {
    "LINUX_RUNNER_1": '["self-hosted","Linux","X64"]',
    "LINUX_RUNNER_2": '["ubuntu-latest"]',
    "WINDOWS_RUNNER_1": '["windows-latest"]',
}


class Parse(unittest.TestCase):
    def test_whole_line_comments_are_dropped(self):
        text = '// header\n{\n  // about a\n  "a": 1, \n    // indented\n  "b": 2\n}\n'
        self.assertEqual(pl.parse(text), {"a": 1, "b": 2})

    def test_slashes_inside_a_string_are_kept(self):
        text = '{"url": "https://example.com/a//b", "c": "// not a comment"}'
        self.assertEqual(pl.parse(text)["url"], "https://example.com/a//b")
        self.assertEqual(pl.parse(text)["c"], "// not a comment")

    def test_bad_json_names_the_problem(self):
        with self.assertRaises(pl.CatalogueError) as ctx:
            pl.parse('{"a": 1,}')
        self.assertIn("not valid JSON", str(ctx.exception))


class Problems(unittest.TestCase):
    def test_a_valid_catalogue_has_none(self):
        self.assertEqual(pl.problems(catalogue()), [])

    def test_platforms_must_be_exactly_the_three(self):
        cat = catalogue()
        del cat["macos"]
        self.assertTrue(any("platforms must be" in p for p in pl.problems(cat)))

    def test_a_leg_needs_a_known_tier(self):
        cat = catalogue(linux=[leg("L", "l", tier="nightly")])
        self.assertTrue(any("`tier`" in p for p in pl.problems(cat)))

    def test_exactly_one_of_runner_and_slot(self):
        both = leg("L", "l", runner=["ubuntu-latest"], runner_slot="linux_runner_1")
        neither = leg("N", "n")
        del neither["runner"]
        for bad in (both, neither):
            with self.subTest(leg=bad["name"]):
                cat = catalogue(linux=[bad])
                self.assertTrue(any("exactly one of" in p for p in pl.problems(cat)))

    def test_a_slot_belongs_to_its_own_platform_and_is_used_once(self):
        wrong = catalogue(linux=[leg("L", "l", runner_slot="windows_runner_1")])
        self.assertTrue(any("another platform" in p for p in pl.problems(wrong)))
        twice = catalogue(
            linux=[
                leg("A", "a", runner_slot="linux_runner_1"),
                leg("B", "b", runner_slot="linux_runner_1"),
            ]
        )
        self.assertTrue(any("used by another leg" in p for p in pl.problems(twice)))

    def test_names_and_presets_are_unique_across_platforms(self):
        cat = catalogue(windows=[leg("Linux A", "linux-a")])
        found = pl.problems(cat)
        self.assertTrue(any("name already used" in p for p in found), found)
        self.assertTrue(any("preset 'linux-a' already used" in p for p in found), found)

    def test_runner_labels_must_be_a_nonempty_list_of_strings(self):
        for bad in ([], "ubuntu-latest", [""], [1]):
            with self.subTest(runner=bad):
                cat = catalogue(macos=[leg("M", "m", runner=bad)])
                self.assertTrue(any("`runner` must be" in p for p in pl.problems(cat)))


class Select(unittest.TestCase):
    def setUp(self):
        self.cat = catalogue(
            linux=[
                leg("Fast", "fast", runner_slot="linux_runner_1"),
                leg("Slow", "slow", tier="deep", runner_slot="linux_runner_2"),
            ]
        )

    def names(self, chosen):
        return {p: [x["name"] for x in legs] for p, legs in chosen.items()}

    def test_all_is_every_leg_in_catalogue_order(self):
        self.assertEqual(self.names(pl.select(self.cat, "all"))["linux"], ["Fast", "Slow"])

    def test_t2_and_deep_split_the_catalogue(self):
        self.assertEqual(self.names(pl.select(self.cat, "t2"))["linux"], ["Fast"])
        self.assertEqual(self.names(pl.select(self.cat, "deep"))["linux"], ["Slow"])

    def test_a_platform_with_nothing_left_is_an_empty_list(self):
        self.assertEqual(self.names(pl.select(self.cat, "deep"))["windows"], [])

    def test_only_picks_by_preset_whatever_the_tier(self):
        chosen = pl.select(self.cat, "t2", ["slow", "mac-a"])
        self.assertEqual(self.names(chosen), {"linux": ["Slow"], "windows": [], "macos": ["Mac A"]})

    def test_an_unknown_preset_lists_the_valid_ones(self):
        with self.assertRaises(pl.CatalogueError) as ctx:
            pl.select(self.cat, "all", ["fast", "typo"])
        msg = str(ctx.exception)
        self.assertIn("typo", msg)
        self.assertIn("fast", msg)
        self.assertIn("slow", msg)

    def test_an_unknown_tier_is_an_error(self):
        with self.assertRaises(pl.CatalogueError):
            pl.select(self.cat, "nightly")


class Resolve(unittest.TestCase):
    def test_a_slot_takes_the_labels_check_runners_chose(self):
        cat = catalogue()
        got = pl.resolve(cat["linux"][0], ENV)
        self.assertEqual(got["runner"], ENV["LINUX_RUNNER_1"])
        self.assertNotIn("runner_slot", got)
        self.assertNotIn("tier", got)

    def test_literal_labels_become_json_text(self):
        got = pl.resolve(leg("M", "m", runner=["macos-15-intel"]), {})
        self.assertEqual(got["runner"], '["macos-15-intel"]')

    def test_other_fields_pass_through_untouched(self):
        entry = leg("M", "m", gui=True, timeout_minutes=150, mac_arch="x64")
        got = pl.resolve(entry, {})
        self.assertEqual((got["gui"], got["timeout_minutes"], got["mac_arch"]), (True, 150, "x64"))

    def test_a_missing_slot_variable_is_an_error_not_a_guess(self):
        entry = leg("L", "l", runner_slot="linux_runner_3")
        with self.assertRaises(pl.CatalogueError) as ctx:
            pl.resolve(entry, ENV)
        self.assertIn("LINUX_RUNNER_3", str(ctx.exception))

    def test_labels_that_are_not_a_json_list_are_an_error(self):
        entry = leg("L", "l", runner_slot="linux_runner_1")
        for bad in ("ubuntu-latest", "[]", '{"a": 1}'):
            with self.subTest(labels=bad), self.assertRaises(pl.CatalogueError):
                pl.resolve(entry, {"LINUX_RUNNER_1": bad})


class Plan(unittest.TestCase):
    def test_outputs_are_a_matrix_and_an_any_flag_per_platform(self):
        out = pl.plan(catalogue(), {**ENV, "TIER": "t2"})
        self.assertEqual(
            sorted(out),
            sorted(f"{p}_{k}" for p in pl.PLATFORMS for k in ("matrix", "any")),
        )
        include = json.loads(out["linux_matrix"])["include"]
        self.assertEqual([x["name"] for x in include], ["Linux A"])
        self.assertEqual(include[0]["runner"], ENV["LINUX_RUNNER_1"])
        self.assertEqual(out["linux_any"], "true")

    def test_an_empty_platform_is_flagged_and_its_matrix_is_empty(self):
        cat = catalogue(macos=[leg("Mac A", "mac-a", tier="deep", runner=["macos-latest"])])
        out = pl.plan(cat, {**ENV, "TIER": "t2"})
        self.assertEqual(out["macos_any"], "false")
        self.assertEqual(json.loads(out["macos_matrix"]), {"include": []})

    def test_outputs_are_single_lines_for_github_output(self):
        out = pl.plan(catalogue(), ENV)
        for key, value in out.items():
            with self.subTest(key=key):
                self.assertNotIn("\n", value)

    def test_tier_defaults_to_all_and_legs_overrides_it(self):
        cat = catalogue(macos=[leg("Mac A", "mac-a", tier="deep", runner=["macos-latest"])])
        self.assertEqual(pl.plan(cat, ENV)["macos_any"], "true")
        only = pl.plan(cat, {**ENV, "TIER": "t2", "LEGS": " linux-a , "})
        self.assertEqual(
            (only["linux_any"], only["windows_any"], only["macos_any"]),
            ("true", "false", "false"),
        )

    def test_describe_lists_what_runs_and_what_does_not(self):
        cat = catalogue(macos=[leg("Mac A", "mac-a", tier="deep", runner=["macos-latest"])])
        text = pl.describe(cat, {"TIER": "t2"})
        self.assertIn("TIER=t2", text)
        self.assertIn("linux: Linux A", text)
        self.assertIn("macos: none", text)
        self.assertIn("not in this run: Mac A", text)


class Lanes(unittest.TestCase):
    def test_every_lane_is_reported_and_only_the_requested_platforms_are_true(self):
        got = pl.lanes(catalogue(), {"LEGS": "linux-a,mac-a"})
        self.assertEqual(sorted(got), sorted(classify_changes.LANES))
        self.assertEqual([lane for lane, on in got.items() if on], ["linux", "macos"])

    def test_without_legs_the_tier_decides(self):
        cat = catalogue(macos=[leg("Mac A", "mac-a", tier="deep", runner=["macos-latest"])])
        got = pl.lanes(cat, {"TIER": "t2"})
        self.assertEqual([lane for lane, on in got.items() if on], ["windows", "linux"])

    def test_the_command_line_prints_one_line_per_lane(self):
        out = io.StringIO()
        with (
            mock.patch.dict("os.environ", {"LEGS": "linux-gcc"}, clear=False),
            contextlib.redirect_stdout(out),
        ):
            code = pl.main(["plan_legs.py", "--lanes"])
        self.assertEqual(code, 0)
        lines = out.getvalue().splitlines()
        self.assertEqual(sorted(x.split("=")[0] for x in lines), sorted(classify_changes.LANES))
        self.assertIn("linux=true", lines)
        self.assertIn("core=false", lines)
        self.assertIn("windows=false", lines)


class RealCatalogue(unittest.TestCase):
    """The checked-in .github/ci/legs.jsonc, against the workflows that consume it."""

    @classmethod
    def setUpClass(cls):
        cls.cat = pl.load()

    def test_it_loads_and_validates(self):
        self.assertEqual(pl.problems(self.cat), [])

    def test_the_tier_split_never_loses_a_leg(self):
        t2 = pl.select(self.cat, "t2")
        deep = pl.select(self.cat, "deep")
        everything = pl.select(self.cat, "all")
        for platform in pl.PLATFORMS:
            with self.subTest(platform=platform):
                merged = sorted(x["preset"] for x in t2[platform] + deep[platform])
                self.assertEqual(merged, sorted(x["preset"] for x in everything[platform]))

    def test_every_platform_keeps_a_leg_in_t2(self):
        # The run after a merge is the only place Windows and macOS are built at all.
        for platform, legs in pl.select(self.cat, "t2").items():
            with self.subTest(platform=platform):
                self.assertTrue(legs)

    def test_the_slots_are_the_ones_check_runners_outputs(self):
        build = (WORKFLOWS / "_build.yml").read_text(encoding="utf-8")
        for legs in self.cat.values():
            for entry in legs:
                slot = entry.get("runner_slot")
                if slot:
                    with self.subTest(slot=slot):
                        self.assertIn(f"{slot}: ${{{{ steps.decide.outputs.{slot} }}}}", build)
                        self.assertIn(
                            f"{slot.upper()}: ${{{{ needs.check-runners.outputs.{slot} }}}}", build
                        )

    def _code(self, workflow: str) -> str:
        lines = (WORKFLOWS / workflow).read_text(encoding="utf-8").splitlines()
        return "\n".join(x for x in lines if not x.lstrip().startswith("#"))

    def test_steps_read_only_fields_some_leg_defines(self):
        # The workflows' matrix is dynamic now, so actionlint can no longer check this.
        for platform, workflow in (
            ("linux", "_ci-linux.yml"),
            ("windows", "_ci-windows.yml"),
            ("macos", "_ci-macos.yml"),
        ):
            with self.subTest(platform=platform):
                read = set(re.findall(r"\bmatrix\.([A-Za-z_]\w*)", self._code(workflow)))
                defined = {"runner"}
                for entry in self.cat[platform]:
                    defined |= set(entry) - set(pl.PLANNER_FIELDS)
                self.assertEqual(sorted(read - defined), [])

    def test_no_leg_sets_a_field_its_workflow_never_reads(self):
        for platform, workflow in (
            ("linux", "_ci-linux.yml"),
            ("windows", "_ci-windows.yml"),
            ("macos", "_ci-macos.yml"),
        ):
            with self.subTest(platform=platform):
                text = (WORKFLOWS / workflow).read_text(encoding="utf-8")
                read = set(re.findall(r"\bmatrix\.([A-Za-z_]\w*)", text))
                defined = set()
                for entry in self.cat[platform]:
                    defined |= set(entry) - set(pl.PLANNER_FIELDS) - {"runner"}
                self.assertEqual(sorted(defined - read), [])

    def test_the_platform_calls_take_their_matrix_from_the_planner(self):
        build = (WORKFLOWS / "_build.yml").read_text(encoding="utf-8")
        for platform in pl.PLATFORMS:
            with self.subTest(platform=platform):
                self.assertIn(
                    f"matrix: ${{{{ needs.plan-legs.outputs.{platform}_matrix }}}}", build
                )
                self.assertIn(f"needs.plan-legs.outputs.{platform}_any == 'true'", build)

    def test_the_platform_workflows_take_the_matrix_as_an_input(self):
        for workflow in ("_ci-linux.yml", "_ci-windows.yml", "_ci-macos.yml"):
            with self.subTest(workflow=workflow):
                text = (WORKFLOWS / workflow).read_text(encoding="utf-8")
                self.assertIn("matrix: ${{ fromJSON(inputs.matrix) }}", text)
                self.assertNotIn("        include:", text)


if __name__ == "__main__":
    unittest.main()
