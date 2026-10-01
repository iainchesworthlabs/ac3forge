"""Unit tests for n1b_docs.py: the documentation and the addresses, stage S5.

stdlib `unittest`. Every case is a line written out here in the form the pages have it, run through
the transforms with the path of the file it comes from. What must stay is as much of the test as
what must change: the address (the text phase leaves it to the urls phase), the file name of an
asset of a release that exists, the output of a past release, the Windows driver's identity, the
URL of a release, the winget manifests' directory, and the lines the hand-written commit writes.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import layoutdef
import n1b_docs as D
import n1b_programs as P
from n1b_lib import DEFAULT_ROOT, Repo


def page(text: str, path: str = "docs/library/index.md") -> str:
    return D.transform_page(path, text)


def urls(text: str, path: str = "docs/library/index.md") -> str:
    return D.transform_urls(path, text)


class Case(unittest.TestCase):
    def renamed(self, before: str, after: str, path: str = "docs/library/index.md") -> None:
        self.assertEqual(page(before, path), after)
        self.assertEqual(page(after, path), after, "a second run changes nothing")

    def kept(self, text: str, path: str = "docs/library/index.md") -> None:
        self.assertEqual(page(text, path), text)


class Scope(unittest.TestCase):
    def test_what_each_kind_of_file_is(self) -> None:
        kinds = {
            "README.md": "page",
            "docs/index.md": "page",
            "docs/assets/data/support-catalogue.json": "page",
            "docs/javascripts/hero-stats.js": "page",
            "overrides/main.html": "page",
            "mkdocs.yml": "page",
            "apps/gui/tests/FEATURE_COVERAGE.md": "page",
            "esp-idf/iclforge/README.md": "page",
            "docs-snippets/generated/platform-linux.md": "page",
            "docs/history.md": "record",
            "docs/crucible/design/promotion.md": "record",
            "docs/platforms/windows-demo.md": "record",
            "docs/assets/wasm-decode-demo/index.html": "code",
            "apps/wasm/index.html": "code",
            "src/audio/CMakeLists.txt": "code",
            "python/pyproject.toml": "code",
            "rust/iclforge/src/ac4.rs": "rust",
            "src/ac3/src/decoder/decoder.cpp": "cpp",
            "apps/gui/translations/forge_gui_de.ts": "cpp",
            "CHANGELOG.md": "skip",
            "planning/ac4.md": "skip",
            "tools/n1b/README.md": "skip",
            "tests/golden/bitstream-hashes.json": "skip",
            "packaging/winget/manifests/i/iainchesworthlabs/ac3forge/0.10.0-beta.1/x.yaml": "skip",
            ".git-blame-ignore-revs": "skip",
            "docs/library/screenshot.png": "skip",
        }
        for path, kind in kinds.items():
            with self.subTest(path):
                self.assertEqual(D.kind_of(path), kind)


class Programs(Case):
    def test_a_program_is_its_new_name(self) -> None:
        self.renamed(
            "Run `ac3cli encode in.wav out.ac3 448`.", "Run `forge encode in.wav out.ac3 448`."
        )
        self.renamed(
            "`ac3gui` and `ac3hearth` open a window.", "`forge-gui` and `hearth` open a window."
        )
        self.renamed("`ac3crucible-run` and `ac3tests`", "`crucible-run` and `iclforge-tests`")

    def test_a_variable_follows_its_program(self) -> None:
        self.renamed(
            "`AC3CLI_FOO=1` and `AC3GUI_LOCALE`", "`ICLFORGE_CLI_FOO=1` and `ICLFORGE_GUI_LOCALE`"
        )

    def test_the_article_follows_the_new_sound(self) -> None:
        self.renamed(
            "Start an `ac3cli` run, an ac3hearth sink.", "Start a `forge` run, a hearth sink."
        )
        self.renamed("It is an AC3Forge Hearth sink.", "It is a Hearth sink.")

    def test_a_namespace_is_not_a_bare_program_name(self) -> None:
        self.renamed("`ac3cli::run`", "`forge_cli::run`")


class Brand(Case):
    def test_the_bare_word_is_the_family_in_prose_and_an_identifier_in_code(self) -> None:
        self.renamed("ac3forge is a clean-room codec.", "ICL Forge is a clean-room codec.")
        self.renamed(
            "AC3Forge's encoder, and ac3forge's decoder.",
            "ICL Forge's encoder, and ICL Forge's decoder.",
        )
        self.renamed("Run `pip install ac3forge` first.", "Run `pip install iclforge` first.")
        self.renamed("```\nimport ac3forge\n```", "```\nimport iclforge\n```")
        self.renamed(
            "<code>ac3forge</code> is the package.", "<code>iclforge</code> is the package."
        )

    def test_a_heading_is_prose(self) -> None:
        self.renamed("# ac3forge\n", "# ICL Forge\n")
        self.renamed("# Building ac3forge\n", "# Building ICL Forge\n")

    def test_the_members_are_their_own_names(self) -> None:
        self.renamed("AC3Forge Hearth and AC3Forge Crucible", "Hearth and Crucible")
        self.renamed(
            "[AC3Forge Crucible](../crucible/index.md)", "[Crucible](../crucible/index.md)"
        )

    def test_identifiers_are_renamed_everywhere(self) -> None:
        self.renamed("`-DAC3FORGE_BUILD_ADM=ON`", "`-DICLFORGE_BUILD_ADM=ON`")
        self.renamed(
            "`ac3forge_status_t` and `AC3FORGE_OK`", "`iclforge_status_t` and `ICLFORGE_OK`"
        )
        self.renamed("`find_package(ac3forge)`", "`find_package(iclforge)`")
        self.renamed("`CONFIG_AC3FORGE_AC4`", "`CONFIG_ICLFORGE_AC4`")
        self.renamed("`ac3forge_c/ac3forge.h`", "`iclforge_c/iclforge.h`")
        self.renamed(
            "`ac3forge-wasm-decoder`, `createAc3ForgeModule`",
            "`iclforge-wasm-decoder`, `createIclForgeModule`",
        )
        self.renamed(
            "`libac3forge.so` and `ac3forge-dev-<version>.zip`",
            "`libiclforge_ac3.so` and `iclforge-dev-<version>.zip`",
        )

    def test_what_a_program_owns_follows_the_program(self) -> None:
        self.renamed(
            "`ac3forge-crucible-*.zip`, `com.ac3forge.shield`",
            "`iclforge-crucible-*.zip`, `com.iclforge.shield`",
        )
        self.renamed("QML module `Ac3ForgeHearth`", "QML module `Hearth`")
        self.renamed("registers `AC3Forge.Stream`", "registers `IclForge.Stream`")

    def test_a_clone_directory_is_the_new_name(self) -> None:
        self.renamed("```\ncd ac3forge\n```", "```\ncd iclforge\n```")

    def test_a_menu_title_and_the_cask_are_what_the_tools_show(self) -> None:
        self.renamed(
            "under *ac3forge hearth sink* in menuconfig",
            "under *iclforge hearth sink* in menuconfig",
        )
        self.renamed(
            "`brew install --cask iainchesworthlabs/ac3forge/ac3gui`",
            "`brew install --cask iainchesworthlabs/ac3forge/iclforge`",
        )
        self.renamed("`brew uninstall --cask ac3gui`", "`brew uninstall --cask iclforge`")

    def test_what_is_kept(self) -> None:
        self.kept("The driver `Ac3ForgeNullSink` and `ROOT\\Ac3ForgeNullSink` stay installed.")
        self.kept("Download `ac3forge-0.10.0-win64.exe` from the release.")
        self.kept("`ac3forge-dev-0.10.0-beta.1-Darwin.zip`")
        self.kept("```\nac3forge 0.10.0-beta.1+2637\n```")
        self.kept("[x](https://github.com/iainchesworthlabs/ac3forge)")
        self.kept("gh run view --repo iainchesworthlabs/ac3forge")
        self.kept("project key iainchesworthlabs_ac3forge")
        self.kept("`ac3forge-example-key-DO-NOT-USE`")

    def test_the_name_of_a_link_is_renamed_and_its_address_is_not(self) -> None:
        self.renamed(
            "[ac3forge](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/x.md)",
            "[ICL Forge](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/x.md)",
        )

    def test_a_script_block_is_code(self) -> None:
        text = "<script>\nconst label = 'ac3forge';\n</script>\nac3forge says"
        self.assertEqual(
            page(text), "<script>\nconst label = 'iclforge';\n</script>\nICL Forge says"
        )

    def test_a_data_file_has_no_prose(self) -> None:
        self.assertEqual(
            page('"note": "Install with pip install ac3forge."', "docs/assets/data/x.json"),
            '"note": "Install with pip install iclforge."',
        )


class Code(Case):
    def test_headers_cmake_targets_and_namespaces(self) -> None:
        self.renamed(
            "`ac3/core/tables.hpp` and `ac3/core/layout.hpp`",
            "`iclforge/ac3/core/tables.hpp` and `iclforge/base/layout.hpp`",
        )
        self.renamed(
            "`mp4/hls.hpp`, `ac4dec/decoder.hpp`",
            "`iclforge/mp4/hls.hpp`, `iclforge/ac4dec/decoder.hpp`",
        )
        self.renamed(
            "`ac3::forge` and `ac3::forge_static`, `ac4::decoder`",
            "`iclforge::ac3` and `iclforge::ac3_static`, `iclforge::ac4dec`",
        )
        self.renamed(
            "`matroska::matroska`, `ac3iab::ac3iab`", "`iclforge::matroska`, `iclforge::iab`"
        )
        self.renamed(
            "`ac3::oba::AtmosEncoder` and `ac4::Decoder`",
            "`iclforge::oba::AtmosEncoder` and `iclforge::ac4::Decoder`",
        )
        self.renamed(
            "`mp4::mux`, `ac3adm::parse_bw64`", "`iclforge::mp4::mux`, `iclforge::adm::parse_bw64`"
        )
        self.renamed("`ac3::forge:` is the library", "`iclforge::ac3:` is the library")

    def test_a_qualifier_needs_a_name_after_it(self) -> None:
        self.kept("`x[::2]` and `ac3::`", "docs/x.md")

    def test_a_include_inside_a_new_path_is_not_renamed_again(self) -> None:
        self.kept("`src/mp4/include/iclforge/mp4/mp4.hpp` and `iclforge/ac4/ac4.hpp`")

    def test_a_non_page_takes_the_three_code_rules_only(self) -> None:
        text = "# ac3::forge and ac3::oba::X, ac4::Decoder; ac3cli\nname = ac3forge\n"
        self.assertEqual(
            D.transform_code("src/audio/CMakeLists.txt", text),
            "# iclforge::ac3 and iclforge::oba::X, iclforge::ac4::Decoder; ac3cli\n"
            "name = ac3forge\n",
        )

    def test_rust_takes_them_in_comment_lines_only(self) -> None:
        text = "/// Mirrors `ac4::Decoder`.\nuse ac4::Decoder;\n"
        self.assertEqual(
            D.transform_code("rust/iclforge/src/ac4.rs", text),
            "/// Mirrors `iclforge::ac4::Decoder`.\nuse ac4::Decoder;\n",
        )


def words(text: str, path: str = "src/adm/CMakeLists.txt") -> str:
    return D.transform_words(path, text)


class Words(unittest.TestCase):
    def changed(self, before: str, after: str, path: str = "src/adm/CMakeLists.txt") -> None:
        self.assertEqual(words(before, path), after)
        self.assertEqual(words(after, path), after, "a second run changes nothing")

    def test_a_library_is_named_by_its_target(self) -> None:
        self.changed(
            "# unlike ac3adm, see its own option()", "# unlike iclforge::adm, see its own option()"
        )
        self.changed("# ac3iab's own test file", "# iclforge::iab's own test file")
        self.changed("# not ac3admbridge:: - matches", "# not iclforge::admbridge:: - matches")
        self.changed(
            "# ac3adm/admbridge are shared-only", "# iclforge::adm/admbridge are shared-only"
        )
        self.changed(
            "# a SHARED ac3adm.so is self-contained",
            "# a SHARED libiclforge_adm.so is self-contained",
        )

    def test_a_library_file_is_named_as_it_is_now(self) -> None:
        self.changed("build-pw/src/audio/libac3audio.a", "build-pw/src/audio/libiclforge_audio.a")
        self.changed(
            "libac3signing_static.a and libac3iab.so",
            "libiclforge_signing_static.a and libiclforge_iab.so",
        )

    def test_a_raw_target_is_the_one_it_became(self) -> None:
        self.changed(
            "iclforge_ac3_static/forge_shared each wrap iclforge_ac3_objects",
            "iclforge_ac3_static/iclforge_ac3_shared each wrap iclforge_ac3_objects",
        )
        self.changed(
            "iclforge_capi_objects/forge_c_static/forge_c_shared",
            "iclforge_capi_objects/iclforge_c_static/iclforge_c_shared",
        )
        self.changed("the forge_c precedent", "the iclforge_c precedent")
        self.changed(
            "(install(EXPORT ... NAMESPACE ac3::)", "(install(EXPORT ... NAMESPACE iclforge::)"
        )

    def test_a_name_that_is_not_the_old_one_stays(self) -> None:
        for text in (
            "iclforge/adm/ac3adm.hpp and test_ac3iab.cpp",
            "see iclforge::ac3_static, iclforge_c and libiclforge_c_static.a",
            "ac3::forge",
            "check_shared_forge_binding.sh",
        ):
            with self.subTest(text):
                self.assertEqual(words(text), text)

    def test_a_line_about_the_past_is_left(self) -> None:
        past = "# let libac3iab.so go uncovered (from the day ac3iab landed until this fix)"
        self.assertEqual(words(past, ".github/workflows/_ci-core.yml"), past)
        self.assertNotEqual(words(past, ".github/workflows/other.yml"), past)

    def test_only_build_and_tool_text_is_read(self) -> None:
        for path in (
            "docs/library/adm.md",
            "src/adm/src/adm.cpp",
            "tools/n1b/notes.py",
            "planning/ac4.md",
        ):
            with self.subTest(path):
                self.assertNotEqual(D.kind_of(path), "code")


class Anchors(unittest.TestCase):
    def test_a_heading_that_changes_changes_its_slug(self) -> None:
        before = "# Using ac3::forge\n\n## `ac3cli` options\n\n## Other\n"
        after = page(before)
        self.assertEqual(
            D.anchor_map(before, after),
            {"using-ac3forge": "using-iclforgeac3", "ac3cli-options": "forge-options"},
        )

    def test_a_link_to_a_changed_heading_follows_it(self) -> None:
        maps = {"docs/library/index.md": {"using-ac3forge": "using-iclforgeac3"}}
        for text, want in (
            ("[a](index.md#using-ac3forge)", "[a](index.md#using-iclforgeac3)"),
            (
                "[a](../library/index.md#using-ac3forge)",
                "[a](../library/index.md#using-iclforgeac3)",
            ),
            (
                "[a](https://iainchesworthlabs.github.io/ac3forge/library/#using-ac3forge)",
                "[a](https://iainchesworthlabs.github.io/ac3forge/library/#using-iclforgeac3)",
            ),
            (
                "[a](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/library/index.md#using-ac3forge)",
                "[a](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/library/index.md#using-iclforgeac3)",
            ),
            ('<a href="index.md#using-ac3forge">', '<a href="index.md#using-iclforgeac3">'),
            ("[a](other.md#using-ac3forge)", "[a](other.md#using-ac3forge)"),
            ("[a](index.md#left-alone)", "[a](index.md#left-alone)"),
        ):
            with self.subTest(text):
                self.assertEqual(D.remap_links("docs/library/ac4.md", text, maps), want)

    def test_a_link_fragment_is_not_renamed_with_the_text(self) -> None:
        self.assertEqual(page("[x](a.md#ac3cli-options)"), "[x](a.md#ac3cli-options)")


class Urls(unittest.TestCase):
    def test_the_repository_the_site_the_tap_and_the_issues(self) -> None:
        for before, after in (
            (
                "https://github.com/iainchesworthlabs/ac3forge/blob/main/LICENSE",
                "https://github.com/iainchesworthlabs/iclforge/blob/main/LICENSE",
            ),
            (
                "git clone https://github.com/iainchesworthlabs/ac3forge.git",
                "git clone https://github.com/iainchesworthlabs/iclforge.git",
            ),
            (
                "https://iainchesworthlabs.github.io/ac3forge/library/",
                "https://iainchesworthlabs.github.io/iclforge/library/",
            ),
            (
                "https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/quality-history/b.json",
                "https://raw.githubusercontent.com/iainchesworthlabs/iclforge/quality-history/b.json",
            ),
            ("tap=iainchesworthlabs/homebrew-ac3forge", "tap=iainchesworthlabs/homebrew-iclforge"),
            ("brew tap iainchesworthlabs/ac3forge", "brew tap iainchesworthlabs/iclforge"),
            ("(ac3forge#796, job 1)", "(iclforge#796, job 1)"),
            ("the workspace is /__w/ac3forge/ac3forge", "the workspace is /__w/<repo>/<repo>"),
            (
                "/home/runner/work/ac3forge/ac3forge, while",
                "/home/runner/work/<repo>/<repo>, while",
            ),
            (
                "git clone https://github.com/iainchesworthlabs/ac3forge && cd ac3forge",
                "git clone https://github.com/iainchesworthlabs/iclforge && cd iclforge",
            ),
        ):
            with self.subTest(before):
                self.assertEqual(urls(before), after)
                self.assertEqual(urls(after), after, "a second run changes nothing")

    def test_what_is_kept(self) -> None:
        for text in (
            "https://github.com/iainchesworthlabs/ac3forge/releases/download/v0.10.0-beta.1/ac3forge-0.10.0-win64.exe",
            "https://github.com/iainchesworthlabs/ac3forge/archive/refs/tags/v0.10.0-beta.1.tar.gz",
            "https://github.com/iainchesworthlabs/ac3forge/releases/tag/v0.9.0-beta.1",
            "https://github.com/iainchesworthlabs/ac3forge/compare/v0.9.0-beta.1...v0.10.0-beta.1",
            "root/packaging/winget/manifests/i/iainchesworthlabs/ac3forge/$latest",
            "sonar.projectKey=iainchesworthlabs_ac3forge",
            "--project-key iainchesworthlabs_ac3forge",
            "iainchesworthlabs/ci-runners",
            "PackageIdentifier: iainchesworthlabs.ac3forge",
            "https://pypi.org/project/ac3forge/",
        ):
            with self.subTest(text):
                self.assertEqual(urls(text), text)

    def test_the_latest_release_and_the_releases_page_move(self) -> None:
        for before, after in (
            (
                "https://github.com/iainchesworthlabs/ac3forge/releases/latest",
                "https://github.com/iainchesworthlabs/iclforge/releases/latest",
            ),
            (
                "https://github.com/iainchesworthlabs/ac3forge/releases",
                "https://github.com/iainchesworthlabs/iclforge/releases",
            ),
            ("releases/download/v#{version}/", "releases/download/v#{version}/"),
        ):
            self.assertEqual(urls(before), after)

    def test_the_lines_the_hand_written_commit_writes_are_left(self) -> None:
        for path, line in (
            (
                ".github/workflows/dependabot-auto-merge.yml",
                "if: github.repository == 'iainchesworthlabs/ac3forge'",
            ),
            (
                ".github/workflows/sonarcloud.yml",
                "if: x && github.repository == 'iainchesworthlabs/ac3forge'",
            ),
            ("tools/release/bump_manifests.py", 'REPO = "iainchesworthlabs/ac3forge"'),
        ):
            with self.subTest(path):
                self.assertEqual(urls(line, path), line)
        # the same text in another file is an address like any other
        self.assertEqual(
            urls('REPO = "iainchesworthlabs/ac3forge"', "tools/hearth/ota.py"),
            'REPO = "iainchesworthlabs/iclforge"',
        )


class Tables(unittest.TestCase):
    def test_the_header_map_names_a_header_of_the_tree(self) -> None:
        spellings = {layoutdef.spelling_of(f) for f in Repo(DEFAULT_ROOT).files}
        missing = sorted(new for new in D.HEADER_MAP.values() if new not in spellings)
        self.assertEqual(missing, [])

    def test_the_header_map_is_what_the_layout_gives(self) -> None:
        have = subprocess.run(
            ["git", "-C", DEFAULT_ROOT, "cat-file", "-e", D.HEADER_MAP_BASE + "^{commit}"],
            capture_output=True,
            check=False,
        )
        if have.returncode != 0:
            self.skipTest("the tree before S2 is not in this clone")
        self.assertEqual(D.derive_header_map(Path(DEFAULT_ROOT), D.HEADER_MAP_BASE), D.HEADER_MAP)

    def test_no_header_keeps_its_name(self) -> None:
        self.assertEqual([k for k, v in D.HEADER_MAP.items() if k == v], [])

    def test_a_new_spelling_is_not_an_old_one(self) -> None:
        # a second run must not find an old spelling inside a new one
        self.assertEqual(sorted(set(D.HEADER_MAP) & set(D.HEADER_MAP.values())), [])

    def test_every_rule_a_decision_names_exists(self) -> None:
        text = "\n".join(
            (
                "ac3forge and `ac3forge` and AC3Forge Hearth and `ac3cli` and "
                "`ac3/core/tables.hpp`",
                "`ac3::forge`, `ac3::oba::X`, `AC3CLI_X`, `Ac3ForgeHearth`, `ac3forge-crucible`",
            )
        )
        hits: list = []
        D.transform_page("docs/x.md", text, hits)
        self.assertTrue(hits)
        self.assertEqual([h[0] for h in hits if h[0] not in D.RULES], [])

    def test_the_table_of_programs_is_the_one_n1a_wrote(self) -> None:
        self.assertEqual(P.BY_OLD["ac3cli"].name, "forge")


FILES = {
    "README.md": (
        "# ac3forge\n\nSee [the options](docs/a.md#ac3cli-options) and `ac3cli` in\n"
        "[the repository](https://github.com/iainchesworthlabs/ac3forge).\n"
    ),
    "ROADMAP.md": "| N1 has not run. N1A renames the programs (`ac3cli` becomes `forge`) |\n",
    "docs/a.md": "# A page\n\n## `ac3cli` options\n\nRun `ac3cli encode`.\n",
    "docs/history.md": (
        "# History\n\n`ac3cli silence out.ac3` made a stream.\n"
        "[a](https://iainchesworthlabs.github.io/ac3forge/a/)\n"
    ),
    "planning/p.md": "A plan that says `ac3cli` and links [x](../docs/a.md#ac3cli-options).\n",
    "CHANGELOG.md": "- ac3cli, see [x](docs/a.md#ac3cli-options)\n",
    "src/audio/CMakeLists.txt": "# links ac3::forge and names ac3::oba::X\nadd_library(a)\n",
    "src/adm/CMakeLists.txt": "# unlike ac3adm, see forge_shared; libac3iab.so\nadd_library(adm)\n",
    "src/ac3/x.cpp": '// ac3::oba::X, ac3cli\nconst char* kUrl = "https://github.com/iainchesworthlabs/ac3forge";\n',
    ".github/workflows/dependabot-auto-merge.yml": (
        "    if: github.repository == 'iainchesworthlabs/ac3forge'\n"
        "    uses: https://github.com/iainchesworthlabs/ac3forge/releases/latest\n"
    ),
    "tests/golden/x.txt": "ac3cli https://github.com/iainchesworthlabs/ac3forge\n",
    "tools/n1b/notes.py": '"ac3cli iainchesworthlabs/ac3forge"\n',
}


def git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True, check=True
    ).stdout


class Run(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name) / "repo"
        for rel, text in FILES.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text.encode("utf-8"))
        git(self.root, "init", "-q")
        git(self.root, "config", "user.name", "t")
        git(self.root, "config", "user.email", "t@example.invalid")
        git(self.root, "config", "core.autocrlf", "false")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "fixture")

    def text(self, rel: str) -> str:
        return (self.root / rel).read_text(encoding="utf-8")

    def go(self, phase: str, dry_run: bool = False) -> dict[str, list[str]]:
        return D.run(self.root, phase, dry_run)[0]

    def test_the_text_phase_renames_pages_and_comments_and_leaves_the_history(self) -> None:
        self.go("text")
        self.assertIn("# ICL Forge", self.text("README.md"))
        self.assertIn("`forge` in", self.text("README.md"))
        self.assertIn("Run `forge encode`.", self.text("docs/a.md"))
        self.assertIn(
            "# links iclforge::ac3 and names iclforge::oba::X",
            self.text("src/audio/CMakeLists.txt"),
        )
        for kept in ("docs/history.md", "tests/golden/x.txt", "tools/n1b/notes.py"):
            with self.subTest(kept):
                self.assertEqual(self.text(kept), FILES[kept])
        self.assertEqual(self.text("src/ac3/x.cpp"), FILES["src/ac3/x.cpp"], "a C++ file is S3's")
        self.assertEqual(self.text("ROADMAP.md"), FILES["ROADMAP.md"], "a line for the hand")

    def test_a_link_follows_a_heading_that_changed_in_a_page_and_in_the_history(self) -> None:
        self.go("text")
        self.assertIn("## `forge` options", self.text("docs/a.md"))
        for rel in ("README.md", "planning/p.md", "CHANGELOG.md"):
            with self.subTest(rel):
                self.assertIn("docs/a.md#forge-options", self.text(rel))
        self.assertIn("A plan that says `ac3cli`", self.text("planning/p.md"))

    def test_the_urls_phase_moves_the_addresses_and_keeps_what_it_says(self) -> None:
        self.go("urls")
        self.assertIn("(https://github.com/iainchesworthlabs/iclforge)", self.text("README.md"))
        self.assertIn("iainchesworthlabs.github.io/iclforge/a/", self.text("docs/history.md"))
        self.assertIn('"https://github.com/iainchesworthlabs/iclforge"', self.text("src/ac3/x.cpp"))
        guard = self.text(".github/workflows/dependabot-auto-merge.yml")
        self.assertIn("github.repository == 'iainchesworthlabs/ac3forge'", guard)
        self.assertIn("iainchesworthlabs/iclforge/releases/latest", guard)
        self.assertEqual(self.text("tests/golden/x.txt"), FILES["tests/golden/x.txt"])
        self.assertEqual(self.text("tools/n1b/notes.py"), FILES["tools/n1b/notes.py"])

    def test_the_words_phase_names_the_libraries_in_build_text_only(self) -> None:
        self.go("words")
        self.assertEqual(
            self.text("src/adm/CMakeLists.txt"),
            "# unlike iclforge::adm, see iclforge_ac3_shared; libiclforge_iab.so\n"
            "add_library(adm)\n",
        )
        for kept in ("src/ac3/x.cpp", "README.md", "docs/a.md", "tools/n1b/notes.py"):
            with self.subTest(kept):
                self.assertEqual(self.text(kept), FILES[kept])

    def test_a_second_run_of_each_phase_changes_nothing(self) -> None:
        self.go("all")
        self.assertEqual(self.go("text"), {"text": []})
        self.assertEqual(self.go("urls"), {"urls": []})
        self.assertEqual(self.go("words"), {"words": []})

    def test_the_census_counts_by_family_and_by_why(self) -> None:
        rows = D.residual(self.root)
        text = "\n".join(rows)
        self.assertIn("history", text)
        self.assertIn("byte-exact", text)
        self.assertIn("this migration's scripts", text)
        D.run(self.root, "all")
        listed = D.residual(self.root, "brand")
        self.assertTrue(
            all(not line.startswith(("CHANGELOG.md", "planning/", "tools/n1b/")) for line in listed)
        )

    def test_a_dry_run_writes_nothing(self) -> None:
        changed = self.go("all", dry_run=True)
        self.assertTrue(changed["text"])
        for rel, text in FILES.items():
            self.assertEqual(self.text(rel), text)


if __name__ == "__main__":
    unittest.main()
