"""Unit tests for ac3ns_shadows.py: the names the move could rebind without a compiler saying so.

stdlib `unittest`. The table is written out here with the shape the tree's has: the root and `emdf`
shared with the objects library (the sync word of an E-AC-3 container, 0x5838, is
`emdf::kSyncWord`; the library's own AC-3 sync word, 0x0B77, is the root's `kSyncWord`), and `oba`
with its nested `joc`.
"""

import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ac3ns_core as core
import ac3ns_shadows as shadows

TABLE = core.Table.from_json(
    {
        "namespaces": {
            "<root>": {
                "owner": "shared",
                "names": ["kSyncWord", "FrameEncoder", "describe", "PcmBlock"],
                "others": {"src/base": ["BitReader"], "src/render": []},
            },
            "meta": {"owner": "exclusive", "names": ["QcPreset"]},
            "emdf": {
                "owner": "shared",
                "names": ["walk_frame", "FrameLayout"],
                "others": {"src/objects": ["kSyncWord", "build_container", "Payload"]},
            },
            "oba": {
                "owner": "shared",
                "names": ["AtmosEncoder"],
                "others": {"src/objects": ["Position", "describe", "joc"]},
            },
            "oba::joc": {
                "owner": "shared",
                "names": ["reconstruct"],
                "others": {"src/objects": ["Domain"]},
            },
            "render": {
                "owner": "shared",
                "names": ["serve"],
                "others": {"src/render": ["PcmBlock", "OutputLayout"]},
            },
            "internal": {
                "owner": "shared",
                "names": ["pow43"],
                "others": {"src/base": ["arch", "pow43"]},
            },
        }
    }
)


def kinds(found: list[shadows.Collision]) -> set[tuple[str, tuple[str, ...], str]]:
    return {(c.kind, c.scope, c.name) for c in found}


class Collisions(unittest.TestCase):
    def test_a_name_of_the_other_half_that_the_library_declares_wider_is_shadowed(self) -> None:
        found = shadows.collisions(TABLE)
        self.assertIn(("shadowed", ("emdf",), "kSyncWord"), kinds(found))
        sync = next(c for c in found if c.name == "kSyncWord")
        self.assertEqual(sync.now, ((),))  # the library's own, in iclforge::ac3
        self.assertEqual(sync.code_in, (("emdf",),))

    def test_code_in_a_nested_namespace_sees_the_wider_scopes_too(self) -> None:
        found = shadows.collisions(TABLE)
        describe = next(c for c in found if c.name == "describe")
        self.assertEqual(describe.scope, ("oba",))
        self.assertEqual(describe.code_in, (("oba",), ("oba", "joc")))

    def test_a_name_both_halves_of_one_namespace_declare_is_listed(self) -> None:
        self.assertIn(("both", ("internal",), "pow43"), kinds(shadows.collisions(TABLE)))

    def test_a_namespace_both_open_is_not_a_collision(self) -> None:
        # `joc` is objects' name in `oba` and the library's nested namespace: a shared one
        self.assertNotIn("joc", {c.name for c in shadows.collisions(TABLE)})

    def test_a_name_only_the_other_library_has_is_no_collision(self) -> None:
        names = {c.name for c in shadows.collisions(TABLE)}
        self.assertNotIn("Position", names)
        self.assertNotIn("OutputLayout", names)
        self.assertNotIn("Domain", names)

    def test_the_description_says_where_the_name_comes_from_now(self) -> None:
        sync = next(c for c in shadows.collisions(TABLE) if c.name == "kSyncWord")
        text = shadows.describe(sync)
        self.assertIn("iclforge::emdf::kSyncWord", text)
        self.assertIn("iclforge::ac3", text)


class Spellings(unittest.TestCase):
    def test_a_name_of_the_other_half_spelled_through_the_shared_namespace_is_found(self) -> None:
        text = "namespace iclforge::ac3::oba {\nauto c = emdf::build_container(p);\n}\n"
        found = shadows.spellings(text, TABLE)
        self.assertEqual(
            [(s.line, s.column, s.scope, s.name) for s in found],
            [(2, 10, ("emdf",), "build_container")],
        )

    def test_the_librarys_own_name_and_an_anchored_spelling_are_not(self) -> None:
        text = "auto a = emdf::walk_frame(f);\nauto b = iclforge::emdf::build_container(p);\n"
        self.assertEqual(shadows.spellings(text, TABLE), [])

    def test_a_comment_and_a_string_are_not_code(self) -> None:
        text = (
            '// emdf::build_container\nconst char* s = "emdf::Payload";\n/* render::PcmBlock */\n'
        )
        self.assertEqual(shadows.spellings(text, TABLE), [])

    def test_a_nested_namespace_decides_by_the_name_after_it(self) -> None:
        text = "auto a = oba::joc::Domain::kQmf;\nauto b = oba::joc::reconstruct(s);\n"
        found = shadows.spellings(text, TABLE)
        self.assertEqual(
            [(s.line, s.scope, s.name) for s in found], [(1, ("oba", "joc"), "Domain")]
        )

    def test_a_using_declaration_is_a_spelling(self) -> None:
        found = shadows.spellings("using render::PcmBlock;\n", TABLE)
        self.assertEqual([(s.scope, s.name) for s in found], [(("render",), "PcmBlock")])

    def test_a_spelling_inside_another_name_is_not_one(self) -> None:
        text = "auto a = my_emdf::build_container(p);\nauto b = other::emdf::build_container(p);\n"
        self.assertEqual(shadows.spellings(text, TABLE), [])


class Anchor(unittest.TestCase):
    def test_the_root_goes_in_front_of_each_spelling(self) -> None:
        text = "auto a = emdf::build_container(p);\nusing render::PcmBlock;\n"
        new, changes = shadows.anchor(text, TABLE)
        self.assertEqual(
            new,
            "auto a = iclforge::emdf::build_container(p);\nusing iclforge::render::PcmBlock;\n",
        )
        self.assertEqual(
            [(c.line, c.before, c.after) for c in changes],
            [
                (1, "emdf::build_container", "iclforge::emdf::build_container"),
                (2, "render::PcmBlock", "iclforge::render::PcmBlock"),
            ],
        )

    def test_a_second_run_changes_nothing(self) -> None:
        once, _ = shadows.anchor("auto a = emdf::Payload{};\n", TABLE)
        twice, changes = shadows.anchor(once, TABLE)
        self.assertEqual(twice, once)
        self.assertEqual(changes, [])

    def test_the_line_endings_of_a_file_are_kept(self) -> None:
        text = "int x;\r\nauto a = emdf::Payload{};\r\n"
        new, _ = shadows.anchor(text, TABLE)
        self.assertEqual(new, "int x;\r\nauto a = iclforge::emdf::Payload{};\r\n")

    def test_two_on_one_line_are_both_anchored(self) -> None:
        new, changes = shadows.anchor("f(emdf::Payload{}, emdf::build_container(p));\n", TABLE)
        self.assertEqual(new, "f(iclforge::emdf::Payload{}, iclforge::emdf::build_container(p));\n")
        self.assertEqual(len(changes), 2)


class Which(unittest.TestCase):
    def test_the_librarys_files_and_its_tests_are_read(self) -> None:
        self.assertTrue(shadows.reads("src/ac3/src/oba/atmos.cpp"))
        self.assertTrue(shadows.reads("src/ac3/include/iclforge/ac3/decoder/decoder.hpp"))
        self.assertTrue(shadows.reads("tests/ac3/oba/test_atmos.cpp"))
        self.assertFalse(shadows.reads("src/objects/src/emdf.cpp"))
        self.assertFalse(shadows.reads("apps/cli/main.cpp"))
        self.assertFalse(shadows.reads("src/ac3/README.md"))


class CommittedTree(unittest.TestCase):
    """The data and the tree the stage left, held to what was looked at."""

    ROOT = Path(__file__).resolve().parents[2]

    def test_the_collisions_of_the_committed_table_are_the_four_that_were_looked_at(self) -> None:
        path = Path(__file__).resolve().parent / "ac3ns_symbols.json"
        found = shadows.collisions(core.Table.load(path))
        # kSyncWord: the library's walker found the AC-3 sync word, not the container's (fixed by
        # writing iclforge::emdf::kSyncWord). describe: no code of the library's `oba` calls it.
        # PcmBlock and BlockSink: the library's are `using render::...` of the same types.
        # A new name here has to be looked at by a person before this list is changed.
        self.assertEqual(
            sorted((c.scope, c.name) for c in found),
            [
                (("emdf",), "kSyncWord"),
                (("oba",), "describe"),
                (("render",), "BlockSink"),
                (("render",), "PcmBlock"),
            ],
        )

    def test_the_library_spells_the_other_half_of_a_shared_namespace_with_the_root(self) -> None:
        path = Path(__file__).resolve().parent / "ac3ns_symbols.json"
        table = core.Table.load(path)
        try:
            out = subprocess.run(
                ["git", "-C", str(self.ROOT), "ls-files", "-z", "--", "src/ac3", "tests/ac3"],
                capture_output=True,
                check=True,
            ).stdout.decode("utf-8", "surrogateescape")
        except (subprocess.CalledProcessError, FileNotFoundError):
            self.skipTest("no git repository")
        left: list[str] = []
        for rel in out.split("\0"):
            if not rel or not shadows.reads(rel):
                continue
            try:
                text = (self.ROOT / rel).read_bytes().decode("utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            left += [f"{rel}:{s.line}: {s.scope}::{s.name}" for s in shadows.spellings(text, table)]
        self.assertEqual(left, [], "spell it iclforge::<namespace>::<name>")


if __name__ == "__main__":
    unittest.main()
