"""Unit tests for n1d_driver_names.py: change N1D, the Windows driver's and its endpoint's names.

stdlib `unittest`. Every rewrite case is a line written out here in the form the tree has it, run
through rewrite() with the path of the file it comes from, twice (a second run changes nothing).
What is kept is as much of the test as what changes: the stored-data name `DesktopAtmos`, the
sentences about the demo, the word `Ac3Forge` anywhere but the two scripts that compile a namespace.
The INF source is the one file that is not UTF-8, and it is run through the same byte path the
pass reads and writes with.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import n1d_driver_names as D


class Case(unittest.TestCase):
    def renamed(self, path: str, before: str, after: str) -> None:
        text, _ = D.rewrite(path, before)
        self.assertEqual(text, after)
        again, _ = D.rewrite(path, after)
        self.assertEqual(again, after, "a second run changes nothing")

    def kept(self, path: str, text: str) -> None:
        self.renamed(path, text, text)


class Identity(Case):
    def test_every_case_form_of_the_name(self) -> None:
        for before, after in (
            (
                "$script:HardwareId = 'ROOT\\Ac3ForgeNullSink'\n",
                "$script:HardwareId = 'ROOT\\IclForgeNullSink'\n",
            ),
            (
                "<TargetName>Ac3ForgeNullSink</TargetName>\n",
                "<TargetName>IclForgeNullSink</TargetName>\n",
            ),
            (
                "foreach ($f in 'Ac3ForgeNullSink.sys', 'Ac3ForgeNullSink.inf', "
                "'ac3forgenullsink.cat') {\n",
                "foreach ($f in 'IclForgeNullSink.sys', 'IclForgeNullSink.inf', "
                "'iclforgenullsink.cat') {\n",
            ),
            (
                "%MfgName% = AC3FORGENULLSINK, NT$ARCH$.10.0...22000\n",
                "%MfgName% = ICLFORGENULLSINK, NT$ARCH$.10.0...22000\n",
            ),
            (
                "`ac3forge-nullsink-driver-testsigned` artifact\n",
                "`iclforge-nullsink-driver-testsigned` artifact\n",
            ),
            (
                "<Ac3ForgeWdkNuGetVersion>10.0.28000.2526</Ac3ForgeWdkNuGetVersion>\n",
                "<IclForgeWdkNuGetVersion>10.0.28000.2526</IclForgeWdkNuGetVersion>\n",
            ),
        ):
            with self.subTest(before):
                self.renamed("apps/windows/driver/x", before, after)

    def test_the_files_whose_names_carry_it_move_and_no_others(self) -> None:
        for old, new in (
            (
                "apps/windows/driver/Ac3ForgeNullSink.sln",
                "apps/windows/driver/IclForgeNullSink.sln",
            ),
            (
                "apps/windows/driver/Source/Main/Ac3ForgeNullSink.inx",
                "apps/windows/driver/Source/Main/IclForgeNullSink.inx",
            ),
            (
                "apps/windows/driver/Source/Main/Ac3ForgeNullSink.rc",
                "apps/windows/driver/Source/Main/IclForgeNullSink.rc",
            ),
        ):
            with self.subTest(old):
                self.assertEqual(D.moved_path(old), new)
                self.assertIsNone(D.moved_path(new), "a second run moves nothing")
        for path in (
            "apps/windows/driver/NullSinkDevice.ps1",
            "apps/windows/driver/Source/Main/Main.vcxproj",
            "apps/windows/driver/Package/package.VcxProj",
            "tests/crucible/test_nullsink_position.cpp",
        ):
            with self.subTest(path):
                self.assertIsNone(D.moved_path(path))

    def test_the_namespace_changes_in_the_two_scripts_that_compile_one(self) -> None:
        for path in D.NAMESPACE_FILES:
            self.renamed(path, "namespace Ac3Forge {\n", "namespace IclForge {\n")
            self.renamed(
                path, "[Ac3Forge.RootDevice]::Create()\n", "[IclForge.RootDevice]::Create()\n"
            )

    def test_the_word_alone_is_the_project_everywhere_else(self) -> None:
        self.kept("apps/windows/driver/README.md", 'the QML module "Ac3Forge" of the GUI\n')
        self.kept("apps/windows/driver-vm/Test-Driver.ps1", "Ac3Forge.Something\n")


class InfSource(Case):
    INF = (
        "[Version]\r\nProvider    = %ProviderName%\r\nCatalogFile = Ac3ForgeNullSink.cat\r\n"
        '[Strings]\r\nProviderName    = "ac3forge"\r\nMfgName         = "ac3forge"\r\n'
        'DiskName        = "Ac3ForgeNullSink Driver Disk"\r\nDeviceDesc      = "Desktop Atmos"\r\n'
    )

    def test_the_provider_strings_and_the_names(self) -> None:
        text, _ = D.rewrite("apps/windows/driver/Source/Main/x.inx", self.INF)
        self.assertIn('ProviderName    = "ICL Forge"\r\n', text)
        self.assertIn('MfgName         = "ICL Forge"\r\n', text)
        self.assertIn("CatalogFile = IclForgeNullSink.cat\r\n", text)
        self.assertIn('DiskName        = "IclForgeNullSink Driver Disk"\r\n', text)
        self.assertIn('DeviceDesc      = "Crucible Silent Output"\r\n', text)
        again, _ = D.rewrite("apps/windows/driver/Source/Main/x.inx", text)
        self.assertEqual(again, text)

    def test_the_provider_rule_is_for_an_inf_source_only(self) -> None:
        self.kept("apps/windows/driver/README.md", 'ProviderName = "ac3forge"\n')

    def test_utf16_with_a_mark_goes_back_as_it_came(self) -> None:
        data = b"\xff\xfe" + self.INF.encode("utf-16-le")
        read = D.decode(data)
        self.assertIsNotNone(read)
        assert read is not None
        self.assertEqual(read[1], "utf-16")
        self.assertEqual(D.encode(read[0], read[1]), data)
        text, _ = D.rewrite("x.inx", read[0])
        out = D.encode(text, read[1])
        self.assertTrue(out.startswith(b"\xff\xfe"), "the byte-order mark stays")
        self.assertEqual(D.decode(out)[0], text)  # type: ignore[index]

    def test_utf8_with_and_without_a_mark_goes_back_as_it_came(self) -> None:
        for data in (b"plain \xc3\xa9\r\n", b"\xef\xbb\xbfwith mark\r\nsecond\n", b""):
            with self.subTest(data):
                read = D.decode(data)
                assert read is not None
                self.assertEqual(D.encode(read[0], read[1]), data)

    def test_a_binary_file_is_not_read(self) -> None:
        self.assertIsNone(D.decode(b"\x89PNG\r\n\x1a\n\x00\x00"))
        self.assertIsNone(D.decode(b"\xff\xfe\x00"))  # a mark and half a character
        self.assertIsNone(D.decode(b"caf\xe9"))  # latin-1, not UTF-8


class Endpoint(Case):
    def test_where_it_names_the_device(self) -> None:
        for before, after in (
            (
                'CrucibleController.nullSinkName = "Desktop Atmos";\n',
                'CrucibleController.nullSinkName = "Crucible Silent Output";\n',
            ),
            (
                'compare(x, "Speakers (Desktop Atmos)");\n',
                'compare(x, "Speakers (Crucible Silent Output)");\n',
            ),
            (
                'button(s, "Send applications to Desktop Atmos");\n',
                'button(s, "Send applications to Crucible Silent Output");\n',
            ),
            (
                "write-host 'look for \"Speakers (Desktop Atmos)\" in Sound settings'\n",
                "write-host 'look for \"Speakers (Crucible Silent Output)\" in Sound settings'\n",
            ),
            (
                '{.log_tail = {"created Desktop Atmos"}};\n',
                '{.log_tail = {"created Crucible Silent Output"}};\n',
            ),
        ):
            with self.subTest(before):
                self.renamed("apps/crucible/ui/tests/x.qml", before, after)

    def test_a_sentence_about_something_else_is_left_and_reported(self) -> None:
        for line in (
            "the Desktop Atmos Demo's silent output device\n",
            "# Creates and starts the Desktop Atmos driver test guest in VMware Workstation:\n",
            'SvcDesc         = "Desktop Atmos null-sink audio driver"\n',
            "// Spike S1: does it do what the Desktop Atmos\n",
        ):
            with self.subTest(line):
                text, left = D.rewrite("apps/windows/driver/x", line)
                self.assertEqual(text, line)
                self.assertEqual(left, [f"apps/windows/driver/x:1: {line.strip()}"])

    def test_the_stored_data_name_is_not_the_endpoint(self) -> None:
        self.kept("apps/gui/settings_migration.cpp", 'QStringLiteral("DesktopAtmos")\n')
        self.kept(
            "tests/gui/test_settings_migration.cpp",
            'QStringLiteral("migration/fromDesktopAtmos")\n',
        )

    def test_line_endings_stay(self) -> None:
        self.renamed(
            "a.txt", 'one "Desktop Atmos"\r\ntwo\r\n', 'one "Crucible Silent Output"\r\ntwo\r\n'
        )


class Scope(unittest.TestCase):
    def test_what_the_pass_does_not_read(self) -> None:
        for path in (
            "docs/platforms/windows-driver-acx.md",
            "planning/recasting.md",
            "CHANGELOG.md",
            "README.md",
            "ROADMAP.md",
            "tools/n1b/test_programs.py",
            "tests/golden/external-baseline/x.txt",
            "packaging/winget/manifests/i/x/0.9.0-beta.1/x.yaml",
            ".git-blame-ignore-revs",
        ):
            with self.subTest(path):
                self.assertTrue(D.excluded(path))
        for path in (
            "apps/windows/README.md",
            "apps/windows/driver/README.md",
            "apps/crucible/engine/engine.hpp",
            ".github/workflows/_build.yml",
            "tools/ci/check_crucible_package.py",
            "tests/crucible/fake_devices.hpp",
        ):
            with self.subTest(path):
                self.assertFalse(D.excluded(path))

    def test_the_table_has_one_row_per_form_and_no_form_contains_another(self) -> None:
        olds = [n.old for n in D.IDENTITY]
        self.assertEqual(len(olds), len(set(olds)))
        for a in olds:
            for b in olds:
                if a != b:
                    self.assertNotIn(a, b)

    def test_a_second_run_of_the_whole_table_on_its_own_output_changes_nothing(self) -> None:
        sample = "".join(f"{n.old} {n.new}\n" for n in D.IDENTITY) + f"{D.ENDPOINT.old}\n"
        once, _ = D.rewrite("apps/windows/driver/x", sample)
        twice, _ = D.rewrite("apps/windows/driver/x", once)
        self.assertEqual(once, twice)
        for n in D.IDENTITY:
            self.assertNotIn(n.old, once)


class OnDisk(unittest.TestCase):
    """The phases over a throwaway repository, by bytes."""

    INSTALL = "apps/windows/driver/install.ps1"
    INX = "apps/windows/driver/Source/Main/Ac3ForgeNullSink.inx"
    PAGE = "docs/platforms/page.md"

    @staticmethod
    def utf16(text: str) -> bytes:
        return b"\xff\xfe" + text.encode("utf-16-le")

    def test_the_text_phase_writes_bytes_and_leaves_the_pages(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "-C", tmp, "init", "-q"], check=True)
            files = {
                self.INSTALL: b'$inf = "Ac3ForgeNullSink.inf"\r\nwrite "Desktop Atmos"\r\n',
                self.INX: self.utf16(
                    'ProviderName = "ac3forge"\r\nDeviceDesc = "Desktop Atmos"\r\n'
                ),
                self.PAGE: b"Ac3ForgeNullSink and Desktop Atmos\n",
                "apps/x/picture.bin": b"\x00Ac3ForgeNullSink\x00",
            }
            for name, data in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            subprocess.run(["git", "-C", tmp, "add", "-A"], check=True)
            self.assertEqual(D.phase_text(root, False), 1, "the unreadable file is reported")
            self.assertEqual(
                (root / self.INSTALL).read_bytes(),
                b'$inf = "IclForgeNullSink.inf"\r\nwrite "Crucible Silent Output"\r\n',
            )
            self.assertEqual(
                (root / self.INX).read_bytes(),
                self.utf16(
                    'ProviderName = "ICL Forge"\r\nDeviceDesc = "Crucible Silent Output"\r\n'
                ),
            )
            self.assertEqual((root / self.PAGE).read_bytes(), files[self.PAGE])
            self.assertEqual(D.phase_mv(root, False), 0)
            moved = root / "apps/windows/driver/Source/Main/IclForgeNullSink.inx"
            self.assertTrue(moved.is_file())


if __name__ == "__main__":
    unittest.main()
