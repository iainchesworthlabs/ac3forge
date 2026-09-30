"""Unit tests for n1b_programs.py: stage N1A, the program names and what they register.

stdlib `unittest`. Every case is a line written out here in the form the tree has it, run through
transform() with the path of the file it comes from, since the place decides: `ac3cli::` is a
namespace and `ac3cli encode` is a command, `AC3Forge Hearth` is a program and `AC3Forge` is the
family. What must stay is as much of the test as what must change: the Windows driver's installed
identity, the repository's address, the lines that say what the old names were on purpose. Every
renamed case is run twice, since a second run of the pass changes nothing.
"""

import argparse
import json
import sys
import tempfile
import unittest
import unittest.mock
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import n1b_programs as P


def run(path: str, text: str) -> str:
    return P.transform(path, text)


class Case(unittest.TestCase):
    def renamed(self, path: str, before: str, after: str) -> None:
        self.assertEqual(run(path, before), after)
        self.assertEqual(run(path, after), after, "a second run changes nothing")

    def kept(self, path: str, text: str) -> None:
        self.assertEqual(run(path, text), text)


class Table(unittest.TestCase):
    def test_every_program_has_one_old_and_one_new_name(self) -> None:
        olds = [p.old for p in P.PROGRAMS]
        news = [p.name for p in P.PROGRAMS]
        self.assertEqual(len(set(olds)), len(olds))
        self.assertEqual(len(set(news)), len(news))
        for p in P.PROGRAMS:
            self.assertTrue(p.old.startswith("ac3"), p.old)
            self.assertFalse(p.name.startswith("ac3"), p.name)

    def test_the_programs_the_user_named(self) -> None:
        names = {p.old: p.name for p in P.PROGRAMS}
        self.assertEqual(names["ac3cli"], "forge")
        self.assertEqual(names["ac3gui"], "forge-gui")
        self.assertEqual(names["ac3hearth"], "hearth")
        self.assertEqual(names["ac3crucible"], "crucible")
        self.assertEqual(names["ac3hearth-testsink"], "hearth-testsink")
        self.assertEqual(names["ac3hearth-testserver"], "hearth-testserver")

    def test_the_internal_programs_take_the_family_name_but_the_shield_keeps_its_word(self) -> None:
        names = {p.old: p.name for p in P.PROGRAMS}
        for old in ("tests", "probe", "perf", "bench", "membench", "kernelbench", "fuzz", "test"):
            self.assertEqual(names[f"ac3{old}"], f"iclforge-{old}")
        self.assertEqual(names["ac3crucible-run"], "crucible-run")
        self.assertEqual(names["ac3hearth-render"], "hearth-render")
        self.assertEqual(names["ac3shield"], "shield")

    def test_a_namespace_is_never_a_bare_forge_or_hearth(self) -> None:
        spaces = {p.old: p.ns for p in P.PROGRAMS if p.ns}
        self.assertEqual(spaces["ac3cli"], "forge_cli")
        self.assertEqual(spaces["ac3gui"], "forge_gui")
        self.assertEqual(spaces["ac3probe"], "iclforge_probe")
        self.assertEqual(spaces["ac3shield"], "shield")
        self.assertEqual(spaces["ac3test"], "iclforge_test")
        self.assertEqual(spaces["ac3nullsink"], "iclforge_nullsink")
        self.assertEqual(spaces["ac3fuzz"], "iclforge_fuzz")
        for ns in spaces.values():
            self.assertNotIn(ns, ("forge", "hearth", "crucible", "iclforge"))

    def test_the_variable_stems_are_the_family_prefix_and_the_program_never_forge(self) -> None:
        self.assertEqual(
            P.VAR_STEMS,
            {
                "CLI": "CLI",
                "GUI": "GUI",
                "HEARTH": "HEARTH",
                "CRUCIBLE": "CRUCIBLE",
                "DESK": "CRUCIBLE",
            },
        )
        data = P.table_as_data()
        for old, new in data["variables"].items():
            self.assertTrue(new.startswith("ICLFORGE_"), (old, new))

    def test_the_table_is_data(self) -> None:
        data = json.loads(json.dumps(P.table_as_data()))
        self.assertEqual({p["old"] for p in data["programs"]}, {p.old for p in P.PROGRAMS})
        self.assertEqual(data["qml_modules"]["Ac3ForgeHearth"], "Hearth")
        self.assertIn("ac3gui_lupdate", data["explicit"])


class Scope(unittest.TestCase):
    def test_pages_history_and_byte_exact_trees_are_not_read(self) -> None:
        for path in (
            "docs/forge/gui/localisation.md",
            "README.md",
            "CHANGELOG.md",
            "planning/ac4.md",
            "tools/n1b/README.md",
            "tests/golden/bitstream-hashes.json",
            "packaging/winget/manifests/i/x/ac3forge/0.10.0-beta.1/x.yaml",
            "packaging/homebrew/tap_migrations.json",
        ):
            with self.subTest(path):
                self.assertFalse(P.in_scope(path))

    def test_the_settings_migration_names_the_old_store_on_purpose(self) -> None:
        for path in P.FORMER_NAME_FILES:
            self.assertFalse(P.in_scope(path), path)
        self.assertTrue(P.in_scope("apps/gui/language_manager.cpp"))

    def test_the_docs_sites_demo_pages_follow_apps_wasm(self) -> None:
        self.assertTrue(P.in_scope("docs/assets/wasm-decode-demo/index.html"))


class ProgramNames(Case):
    def test_the_executable_in_a_command_a_path_and_a_target(self) -> None:
        self.renamed(
            "tools/ci/run_codec_matrix.sh",
            '"$BUILD/bin/ac3cli" encode in.wav out.ac3 448\n'
            "cmake --build . --target ac3tests ac3cli\n"
            "build/config-linux-llvm/bin/ac3cli.exe probe x\n",
            '"$BUILD/bin/forge" encode in.wav out.ac3 448\n'
            "cmake --build . --target iclforge-tests forge\n"
            "build/config-linux-llvm/bin/forge.exe probe x\n",
        )

    def test_a_namespace_is_not_a_command(self) -> None:
        self.renamed(
            "apps/cli/commands/decode.cpp",
            "namespace ac3cli::commands {\n"
            "// gating only on ac3cli::adm_capability() (declared in\n"
            "fmt::println(stderr, \"error: see 'ac3cli devices'\");\n"
            "}  // namespace ac3cli::commands\n",
            "namespace forge_cli::commands {\n"
            "// gating only on forge_cli::adm_capability() (declared in\n"
            "fmt::println(stderr, \"error: see 'forge devices'\");\n"
            "}  // namespace forge_cli::commands\n",
        )
        self.renamed(
            "apps/gui/encoder_controller.cpp",
            "using ac3gui::location_azimuth_deg;\nnamespace ac3gui {\n}  // namespace ac3gui\n",
            "using forge_gui::location_azimuth_deg;\n"
            "namespace forge_gui {\n}  // namespace forge_gui\n",
        )
        self.renamed(
            "apps/windows/driver/Source/Main/stream.h",
            "ac3nullsink::PositionClock  m_Clock = {};\nnamespace ac3nullsink {\n",
            "iclforge_nullsink::PositionClock  m_Clock = {};\nnamespace iclforge_nullsink {\n",
        )
        self.renamed(
            "tests/audio/test_alsa_null_backend.cpp",
            "namespace alsa_null = ac3test::alsa_null;\n",
            "namespace alsa_null = iclforge_test::alsa_null;\n",
        )
        self.renamed(
            "apps/baremetal/probe.hpp",
            "namespace ac3probe {\n} // namespace ac3probe\n",
            "namespace iclforge_probe {\n} // namespace iclforge_probe\n",
        )

    def test_a_namespace_alias_names_the_namespace(self) -> None:
        self.renamed("x.cpp", "namespace cli = ac3cli;\n", "namespace cli = forge_cli;\n")

    def test_the_gui_joins_its_name_with_an_underscore_to_a_word_and_keeps_the_hyphen_elsewhere(
        self,
    ) -> None:
        self.renamed(
            "apps/gui/tests/CMakeLists.txt",
            "qt_add_executable(ac3gui_qmltests main.cpp)\n"
            'set(case "ac3gui_qml_tests_${suite}")\n'
            "add_dependencies(ac3gui_qmltests ac3gui)\n"
            "bin/ac3gui.app/Contents/MacOS/ac3gui\n"
            "ac3gui-smoke.ac3\n",
            "qt_add_executable(forge_gui_qmltests main.cpp)\n"
            'set(case "forge_gui_qml_tests_${suite}")\n'
            "add_dependencies(forge_gui_qmltests forge-gui)\n"
            "bin/forge-gui.app/Contents/MacOS/forge-gui\n"
            "forge-gui-smoke.ac3\n",
        )

    def test_the_names_qt_derives_from_a_target_are_the_targets_own_spelling(self) -> None:
        self.renamed(
            "tools/ci/check_translations.sh",
            "cmake --build . --target ac3gui_lupdate ac3hearth_lupdate ac3crucible_lupdate\n",
            "cmake --build . --target forge-gui_lupdate hearth_lupdate crucible_lupdate\n",
        )

    def test_the_translation_catalogues_are_joined_with_an_underscore(self) -> None:
        self.renamed(
            "apps/gui/CMakeLists.txt",
            '"${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3gui_fr.ts"\n'
            "apps/hearth/ui/translations/ac3hearth_de.ts\n"
            "apps/crucible/translations/ac3crucible_yi.ts\n",
            '"${CMAKE_CURRENT_SOURCE_DIR}/translations/forge_gui_fr.ts"\n'
            "apps/hearth/ui/translations/hearth_de.ts\n"
            "apps/crucible/translations/crucible_yi.ts\n",
        )

    def test_the_loaders_base_name_is_the_catalogues_spelling(self) -> None:
        self.renamed(
            "apps/gui/language_manager.hpp",
            'QString translation_basename = QStringLiteral("ac3gui"),\n',
            'QString translation_basename = QStringLiteral("forge_gui"),\n',
        )
        self.renamed(
            "apps/hearth/ui/main.cpp",
            'LanguageManager language_manager(app, engine, QStringLiteral("ac3hearth"));\n',
            'LanguageManager language_manager(app, engine, QStringLiteral("hearth"));\n',
        )

    def test_the_hearth_programs_and_their_test_tools(self) -> None:
        self.renamed(
            "tools/checks/run_hearth.sh",
            "ac3hearth-testsink --wav out.wav\nac3hearth-testserver\nac3hearth-render in\n"
            "add_library(ac3hearth_testsink STATIC x.cpp)\nlink ac3hearth_engine\n",
            "hearth-testsink --wav out.wav\nhearth-testserver\nhearth-render in\n"
            "add_library(hearth_testsink STATIC x.cpp)\nlink hearth_engine\n",
        )

    def test_the_crucible_runner_and_its_static_library_file(self) -> None:
        self.renamed(
            "apps/crucible/CMakeLists.txt",
            "add_executable(ac3crucible-run runner/main.cpp)\nlibac3crucible_engine.a\n",
            "add_executable(crucible-run runner/main.cpp)\nlibcrucible_engine.a\n",
        )

    def test_the_test_programs_named_by_the_layout_study(self) -> None:
        self.renamed(
            "tests/performance/CMakeLists.txt",
            "add_executable(ac3perf x.cpp)\nadd_executable(ac3bench y.cpp)\n"
            "add_executable(ac3membench z.cpp)\nadd_executable(ac3kernelbench w.cpp)\n",
            "add_executable(iclforge-perf x.cpp)\nadd_executable(iclforge-bench y.cpp)\n"
            "add_executable(iclforge-membench z.cpp)\nadd_executable(iclforge-kernelbench w.cpp)\n",
        )
        self.renamed(
            "tests/CMakeLists.txt",
            "add_library(ac3tests_websocket OBJECT w.cpp)\n",
            "add_library(iclforge_tests_websocket OBJECT w.cpp)\n",
        )

    def test_the_probes_board_projects_and_shell_scripts(self) -> None:
        self.renamed(
            "apps/baremetal/platform/esp32s3/CMakeLists.txt",
            "project(ac3probe_esp32s3 LANGUAGES C CXX)\n",
            "project(iclforge_probe_esp32s3 LANGUAGES C CXX)\n",
        )

    def test_a_letter_against_the_name_makes_it_another_word(self) -> None:
        self.kept("tests/ac3/meta/test_drc.cpp", "struct Eac3Probe {\nEac3Probe probe_eac3(int);\n")
        self.kept("x.py", "def ac3clients():\n    pass\n")

    def test_a_troff_or_c_escape_is_not_a_letter_of_the_name(self) -> None:
        self.renamed(
            "apps/cli/usage.cpp",
            'std::string{"\\\\fBac3cli help "} + "\\nac3cli"\n',
            'std::string{"\\\\fBforge help "} + "\\nforge"\n',
        )

    def test_the_zsh_completions_functions_follow_the_command(self) -> None:
        self.renamed(
            "apps/cli/usage.cpp",
            'fmt::println("_ac3cli() {{");\n'
            'fmt::println("    local -a _ac3cli_commands _ac3cli_options");\n'
            'fmt::println("complete -F _ac3cli ac3cli");\n',
            'fmt::println("_forge() {{");\n'
            'fmt::println("    local -a _forge_commands _forge_options");\n'
            'fmt::println("complete -F _forge forge");\n',
        )

    def test_the_man_pages_title_is_the_command_in_capitals(self) -> None:
        self.renamed(
            "apps/cli/usage.cpp",
            'fmt::println(".TH AC3CLI 1 \\"ac3forge {}\\" \\"iclforge\\" \\"User Commands\\"",\n',
            'fmt::println(".TH FORGE 1 \\"iclforge {}\\" \\"iclforge\\" \\"User Commands\\"",\n',
        )

    def test_the_article_follows_a_name_that_no_longer_begins_with_a_vowel_sound(self) -> None:
        self.renamed(
            "tools/ci/fuzz.py",
            "# invoke an ac3cli binary the caller named, and An ac3gui window\n",
            "# invoke a forge binary the caller named, and A forge-gui window\n",
        )
        self.kept("tools/ci/x.py", "# an iclforge-tests binary and an iclforge library\n")


class Variables(Case):
    def test_cmake_variables_and_environment_take_the_family_prefix_and_the_program(self) -> None:
        self.renamed(
            "apps/cli/CMakeLists.txt",
            "set(AC3CLI_COMPLETION_FILES ac3cli _ac3cli ac3cli.fish ac3cli.ps1)\n"
            "add_custom_target(ac3cli_docs ALL DEPENDS ${AC3CLI_GENERATED_DOCS})\n",
            "set(ICLFORGE_CLI_COMPLETION_FILES forge _forge forge.fish forge.ps1)\n"
            "add_custom_target(forge_docs ALL DEPENDS ${ICLFORGE_CLI_GENERATED_DOCS})\n",
        )
        self.renamed(
            ".github/workflows/_ci-core.yml",
            "AC3CLI: build/config-linux-llvm/bin/ac3cli\n",
            "ICLFORGE_CLI: build/config-linux-llvm/bin/forge\n",
        )
        self.renamed(
            "apps/gui/language_manager.cpp",
            'constexpr auto kLocaleEnvOverride = "AC3GUI_LOCALE";\n',
            'constexpr auto kLocaleEnvOverride = "ICLFORGE_GUI_LOCALE";\n',
        )

    def test_crucibles_desk_variables_become_crucibles(self) -> None:
        self.renamed(
            "apps/crucible/CMakeLists.txt",
            "set(AC3DESK_SHARED_DIR x)\nset(AC3CRUCIBLE_NOTICES_FILE y)\n",
            "set(ICLFORGE_CRUCIBLE_SHARED_DIR x)\nset(ICLFORGE_CRUCIBLE_NOTICES_FILE y)\n",
        )

    def test_the_guis_underscored_variables_follow(self) -> None:
        self.renamed(
            "apps/gui/CMakeLists.txt",
            "set(AC3_GUI_ICON_ICO x)\nget_target_property(AC3_GUI_SOURCES ac3gui SOURCES)\n",
            "set(ICLFORGE_GUI_ICON_ICO x)\n"
            "get_target_property(ICLFORGE_GUI_SOURCES forge-gui SOURCES)\n",
        )

    def test_a_variable_that_names_another_program_inside_its_own(self) -> None:
        self.renamed(
            "apps/gui/tests/CMakeLists.txt",
            'target_compile_definitions(ac3gui_qmltests PRIVATE AC3GUI_TEST_AC3CLI="")\n',
            'target_compile_definitions(forge_gui_qmltests PRIVATE ICLFORGE_GUI_TEST_CLI="")\n',
        )

    def test_a_system_variable_of_another_family_is_left_alone(self) -> None:
        self.kept("CMakeLists.txt", "set(AC3_MSVC_TARGET_ARCH x64)\nset(_ac3_probe_ld y)\n")


class QmlModules(Case):
    def test_the_modules_and_their_test_and_language_modules(self) -> None:
        self.renamed(
            "apps/hearth/ui/main.cpp",
            'qmlRegisterSingletonInstance("Ac3ForgeHearthLanguage", 1, 0, "LanguageManager", &m);\n'
            'engine.loadFromModule("Ac3ForgeHearth", "Main");\n',
            'qmlRegisterSingletonInstance("HearthLanguage", 1, 0, "LanguageManager", &m);\n'
            'engine.loadFromModule("Hearth", "Main");\n',
        )
        self.renamed(
            "apps/crucible/ui/tests/qml/tst_shell.qml",
            "import Ac3ForgeCrucible\n"
            "import Ac3ForgeCrucibleTest\n"
            "import Ac3ForgeCrucibleLanguage\n",
            "import Crucible\nimport CrucibleTest\nimport CrucibleLanguage\n",
        )
        self.renamed(
            "apps/gui/qml/Main.qml",
            "import Ac3Forge\nimport QtQuick\n",
            "import ForgeGui\nimport QtQuick\n",
        )

    def test_the_build_names_the_module_and_the_shared_components_import_it(self) -> None:
        self.renamed(
            "cmake/SharedFamilyQml.cmake",
            'string(REPLACE "import Ac3Forge\\n" "import ${module_uri}\\n" c "${c}")\n',
            'string(REPLACE "import ForgeGui\\n" "import ${module_uri}\\n" c "${c}")\n',
        )
        self.renamed(
            "apps/crucible/CMakeLists.txt",
            "iclforge_stage_shared_qml(Ac3ForgeCrucible x y)\nURI Ac3ForgeCrucible\n"
            'OUTPUT_DIRECTORY "${d}/tests/Ac3ForgeCrucible"\n'
            "qrc:/qt/qml/Ac3ForgeCrucible/tray.svg\n",
            "iclforge_stage_shared_qml(Crucible x y)\nURI Crucible\n"
            'OUTPUT_DIRECTORY "${d}/tests/Crucible"\n'
            "qrc:/qt/qml/Crucible/tray.svg\n",
        )

    def test_the_drivers_dotnet_namespace_is_not_a_qml_module(self) -> None:
        self.kept(
            "apps/windows/driver/NullSinkDevice.ps1",
            "if (-not ('Ac3Forge.RootDevice' -as [type])) {\nnamespace Ac3Forge {\n"
            "$x = [Ac3Forge.RootDevice]::Create($p)\n",
        )


class Brand(Case):
    def test_the_display_name_by_context(self) -> None:
        self.renamed(
            "apps/hearth/engine/diagnostics_report.cpp",
            'line("AC3Forge Hearth diagnostics");\n',
            'line("Hearth diagnostics");\n',
        )
        self.renamed(
            "apps/crucible/engine/diagnostics.cpp",
            'line("AC3Forge Crucible diagnostics");\n',
            'line("Crucible diagnostics");\n',
        )
        self.renamed(
            "apps/gui/gui_diagnostics.cpp",
            'line("AC3Forge ac3gui diagnostics");\n',
            'line("ICL Forge forge-gui diagnostics");\n',
        )
        self.renamed(
            "apps/cli/usage.cpp",
            'fmt::println("Forge \u2014 the AC3Forge encoder tools: clean-room AC-3");\n',
            'fmt::println("Forge \u2014 the ICL Forge encoder tools: clean-room AC-3");\n',
        )

    def test_the_family_takes_the_sentence_it_was_in(self) -> None:
        self.renamed(
            "apps/notices/fragments/header.txt",
            "AC3Forge {{VERSION}} - third-party notices\n"
            "AC3Forge, and the ac3forge library its applications are built from, are free\n",
            "ICL Forge {{VERSION}} - third-party notices\n"
            "ICL Forge, and the iclforge library its applications are built from, are free\n",
        )

    def test_the_guis_window_says_forge_and_the_rest_of_the_family_says_icl_forge(self) -> None:
        self.renamed(
            "apps/gui/qml/Main.qml",
            '    title: qsTr("ac3forge \u2014 %1").arg(sourceLabel)\n    text: qsTr("ac3forge")\n',
            '    title: qsTr("Forge \u2014 %1").arg(sourceLabel)\n    text: qsTr("Forge")\n',
        )
        self.renamed(
            "apps/gui/qml/PreferencesDialog.qml",
            '    PrefsKicker { text: qsTr("WHEN AC3FORGE OPENS") }\n'
            "    // ---- When ac3forge opens / files and runs ----\n",
            '    PrefsKicker { text: qsTr("WHEN FORGE OPENS") }\n'
            "    // ---- When Forge opens / files and runs ----\n",
        )
        self.renamed(
            "apps/hearth/ui/qml/AboutDialog.qml",
            '            text: qsTr("Part of the "\n'
            '                        + "ac3forge project: the decoder")\n',
            '            text: qsTr("Part of the "\n'
            '                        + "ICL Forge project: the decoder")\n',
        )
        self.renamed(
            "apps/gui/packaging/linux/ac3gui.metainfo.xml",
            "      ac3gui is the Qt6 front end for ac3forge, a clean-room implementation\n",
            "      forge-gui is the Qt6 front end for ICL Forge, a clean-room implementation\n",
        )

    def test_the_library_is_the_library(self) -> None:
        self.renamed(
            "apps/crucible/ui/qml/AboutDialog.qml",
            'text: qsTr("AC3Forge Crucible and the ac3forge library are free software")\n',
            'text: qsTr("Crucible and the iclforge library are free software")\n',
        )

    def test_the_catalogues_carry_the_same_words_as_the_sources(self) -> None:
        self.renamed(
            "apps/gui/translations/ac3gui_fr.ts",
            "<source>the AC3Forge encoder tools, %1</source>\n"
            "<translation>les outils d&apos;encodage AC3Forge, %1</translation>\n"
            "<source>ac3forge \u2014 %1</source>\n"
            "<translation>ac3forge \u2014 %1</translation>\n"
            "<translation>la ligne ac3cli exacte</translation>\n",
            "<source>the ICL Forge encoder tools, %1</source>\n"
            "<translation>les outils d&apos;encodage ICL Forge, %1</translation>\n"
            "<source>Forge \u2014 %1</source>\n"
            "<translation>Forge \u2014 %1</translation>\n"
            "<translation>la ligne forge exacte</translation>\n",
        )
        self.renamed(
            "apps/gui/translations/ac3gui_de.ts",
            "<translation>die AC3Forge-Encoder-Werkzeuge, %1</translation>\n"
            "<translation>ac3cli-Befehlszeile</translation>\n",
            "<translation>die ICL-Forge-Encoder-Werkzeuge, %1</translation>\n"
            "<translation>forge-Befehlszeile</translation>\n",
        )

    def test_a_translation_names_what_its_source_names_in_whatever_word_order(self) -> None:
        self.renamed(
            "apps/crucible/translations/crucible_de.ts",
            "<message>\n"
            "<source>AC3Forge Crucible and the ac3forge library are free software</source>\n"
            "<translation>AC3Forge Crucible und die Bibliothek ac3forge sind freie "
            "Software</translation>\n"
            "</message>\n"
            "<message>\n"
            "<source>An ac3forge demonstration: the encoder</source>\n"
            "<translation>Eine Demonstration von AC3Forge: der Encoder</translation>\n"
            "</message>\n",
            "<message>\n"
            "<source>Crucible and the iclforge library are free software</source>\n"
            "<translation>Crucible und die Bibliothek iclforge sind freie Software</translation>\n"
            "</message>\n"
            "<message>\n"
            "<source>An ICL Forge demonstration: the encoder</source>\n"
            "<translation>Eine Demonstration von ICL Forge: der Encoder</translation>\n"
            "</message>\n",
        )

    def test_the_german_translation_s4_renamed_as_a_file_name_is_made_the_familys(self) -> None:
        self.renamed(
            "apps/crucible/translations/crucible_de.ts",
            "<translation>Eine iclforge-Demonstration: der Encoder</translation>\n",
            "<translation>Eine ICL-Forge-Demonstration: der Encoder</translation>\n",
        )

    def test_the_windows_file_type_is_registered_under_the_new_progid(self) -> None:
        self.renamed(
            "cmake/Packaging.cmake",
            'WriteRegStr HKCR ".ac3" "" "AC3Forge.Stream"\n'
            'WriteRegStr HKCR "AC3Forge.Stream\\DefaultIcon" "" "$INSTDIR\\bin\\ac3hearth.exe,0"\n'
            'DeleteRegKey HKCR "AC3Forge.Stream"\n',
            'WriteRegStr HKCR ".ac3" "" "IclForge.Stream"\n'
            'WriteRegStr HKCR "IclForge.Stream\\DefaultIcon" "" "$INSTDIR\\bin\\hearth.exe,0"\n'
            'DeleteRegKey HKCR "IclForge.Stream"\n',
        )

    def test_the_organisation_and_application_the_settings_are_stored_under(self) -> None:
        self.renamed(
            "apps/hearth/ui/shared_pairing_store.hpp",
            "QSettings settings{QSettings::defaultFormat(), QSettings::UserScope,\n"
            '                   QStringLiteral("ac3forge"), QStringLiteral("Hearth")};\n',
            "QSettings settings{QSettings::defaultFormat(), QSettings::UserScope,\n"
            '                   QStringLiteral("iclforge"), QStringLiteral("Hearth")};\n',
        )
        self.renamed(
            "apps/gui/main.cpp",
            '    QGuiApplication::setApplicationName(QStringLiteral("ac3forge"));\n'
            '    QGuiApplication::setOrganizationName(QStringLiteral("ac3forge"));\n',
            '    QGuiApplication::setApplicationName(QStringLiteral("forge-gui"));\n'
            '    QGuiApplication::setOrganizationName(QStringLiteral("iclforge"));\n',
        )
        self.renamed(
            "apps/gui/tests/qml_test_main.cpp",
            '        QCoreApplication::setOrganizationName(QStringLiteral("ac3forge-tests"));\n'
            '        QCoreApplication::setApplicationName(QStringLiteral("ac3gui_qmltests"));\n',
            '        QCoreApplication::setOrganizationName(QStringLiteral("iclforge-tests"));\n'
            '        QCoreApplication::setApplicationName(QStringLiteral("forge_gui_qmltests"));\n',
        )

    def test_the_packages_and_assets_named_for_a_program_keep_the_family_in_front(self) -> None:
        self.renamed(
            "cmake/Packaging.cmake",
            'set(CPACK_DEBIAN_CRUCIBLE_PACKAGE_NAME "ac3forge-crucible")\n'
            'set(CPACK_DEBIAN_HEARTH_PACKAGE_NAME "ac3forge-hearth")\n'
            '"ac3forge-crucible-${PROJECT_VERSION_FULL}-${CPACK_SYSTEM_NAME}"\n',
            'set(CPACK_DEBIAN_CRUCIBLE_PACKAGE_NAME "iclforge-crucible")\n'
            'set(CPACK_DEBIAN_HEARTH_PACKAGE_NAME "iclforge-hearth")\n'
            '"iclforge-crucible-${PROJECT_VERSION_FULL}-${CPACK_SYSTEM_NAME}"\n',
        )
        self.renamed(
            ".github/workflows/_build.yml",
            '( cd packages && sha512sum "ac3forge-shield-${version}.apk" )\n'
            "name: ac3forge-nullsink-driver-testsigned\n",
            '( cd packages && sha512sum "iclforge-shield-${version}.apk" )\n'
            "name: iclforge-nullsink-driver-testsigned\n",
        )

    def test_the_icons(self) -> None:
        self.renamed(
            "apps/crucible/ui/main.cpp",
            'app_icon.addFile(QStringLiteral(":/icons/ac3forge-32.png"));\n'
            'MACOSX_BUNDLE_ICON_FILE "ac3forge.icns"\n'
            "apps/gui/icons/ac3forge.ico\nassets/icon/ac3forge-icon.svg\n",
            'app_icon.addFile(QStringLiteral(":/icons/iclforge-32.png"));\n'
            'MACOSX_BUNDLE_ICON_FILE "iclforge.icns"\n'
            "apps/gui/icons/iclforge.ico\nassets/icon/iclforge-icon.svg\n",
        )

    def test_the_android_package_its_jni_names_and_its_library(self) -> None:
        self.renamed(
            "apps/android/app/src/main/cpp/jni_entry.cpp",
            "Java_com_ac3forge_shield_NativeBridge_nativeVersionString(JNIEnv* env, jclass)\n"
            'constexpr char kLogTag[] = "ac3forge.shield";\n',
            "Java_com_iclforge_shield_NativeBridge_nativeVersionString(JNIEnv* env, jclass)\n"
            'constexpr char kLogTag[] = "iclforge.shield";\n',
        )
        self.renamed(
            "apps/android/app/src/main/java/com/ac3forge/shield/NativeBridge.kt",
            "package com.ac3forge.shield\n"
            'System.loadLibrary("ac3forge_jni")\n'
            'text = "libac3forge_jni.so could not be loaded"\n',
            "package com.iclforge.shield\n"
            'System.loadLibrary("iclforge_jni")\n'
            'text = "libiclforge_jni.so could not be loaded"\n',
        )
        self.renamed(
            "apps/android/app/src/main/java/com/ac3forge/shield/AboutActivity.kt",
            'text = "ac3forge \u2014 Shield Atmos Demo"\n',
            'text = "ICL Forge \u2014 Shield Atmos Demo"\n',
        )
        self.renamed(
            "apps/android/app/src/main/cpp/live_cursor.cpp",
            "ac3shield::init_signing(x);\n}  // namespace ac3shield\n",
            "shield::init_signing(x);\n}  // namespace shield\n",
        )

    def test_the_pipewire_node_and_the_test_fixtures_application_id(self) -> None:
        self.renamed(
            "apps/crucible/engine/platform/linux/virtual_device.cpp",
            'constexpr const char* kNodeName = "ac3forge_crucible_sink";\n',
            'constexpr const char* kNodeName = "iclforge_crucible_sink";\n',
        )
        self.renamed(
            "apps/crucible/ui/tests/qml/tst_icons.qml",
            'makeIcon({ name: "Sandboxed", appId: "org.ac3forge.CrucibleFixture" })\n',
            'makeIcon({ name: "Sandboxed", appId: "org.iclforge.CrucibleFixture" })\n',
        )

    def test_the_improv_device_info_and_the_manufacturer(self) -> None:
        self.renamed(
            "esp-idf/iclforge/examples/hearth_sink/main/provision.cpp",
            '"AC3Forge Hearth sink",\n.manufacturer = "AC3Forge",\n',
            '"Hearth sink",\n.manufacturer = "ICL Forge",\n',
        )

    def test_the_device_pages_title_is_the_firmwares_heading_word(self) -> None:
        self.renamed(
            "esp-idf/iclforge/ui/iclforge_ui.html",
            "<title>ac3forge player</title>\n"
            '<div><p class="kicker">AC3Forge Hearth sink</p>'
            '<h1 id="title">Hearth sink</h1></div>\n',
            "<title>iclforge player</title>\n"
            '<div><p class="kicker">Hearth sink</p><h1 id="title">Hearth sink</h1></div>\n',
        )
        self.kept("apps/wasm/tests/device-ui/stub.js", "  'iclforge player',\n")

    def test_the_wasm_demo_pages_name_the_family_and_keep_their_link(self) -> None:
        self.renamed(
            "apps/wasm/index.html",
            "<title>ac3forge - WASM decode demo</title>\n"
            'Part of <a href="https://github.com/iainchesworthlabs/ac3forge">ac3forge</a>, UX5.\n',
            "<title>ICL Forge - WASM decode demo</title>\n"
            'Part of <a href="https://github.com/iainchesworthlabs/ac3forge">ICL Forge</a>, UX5.\n',
        )


class Kept(Case):
    def test_the_drivers_installed_identity(self) -> None:
        for path, line in (
            (
                "apps/windows/driver/NullSinkDevice.ps1",
                "$script:HardwareId = 'ROOT\\Ac3ForgeNullSink'\n",
            ),
            (
                ".github/workflows/_build.yml",
                "foreach ($f in 'Ac3ForgeNullSink.sys', 'Ac3ForgeNullSink.inf', "
                "'ac3forgenullsink.cat') {\n",
            ),
            (
                "apps/windows/driver/Source/Main/Main.vcxproj",
                "<TargetName>Ac3ForgeNullSink</TargetName>\n",
            ),
            (
                "apps/windows/driver/Directory.Build.props",
                "<Ac3ForgeWdkNuGetVersion>10.0</Ac3ForgeWdkNuGetVersion>\n",
            ),
            (
                "apps/crucible/engine/platform/windows/driver_tools.cpp",
                'dir / "Package" / "x64" / "Release" / "package" / "Ac3ForgeNullSink.inf", ec);\n',
            ),
            (
                "apps/windows/driver/Source/Main/Ac3ForgeNullSink.inx",
                "%MfgName% = AC3FORGENULLSINK, NT$ARCH$.10.0...22000\n",
            ),
        ):
            with self.subTest(line):
                self.kept(path, line)

    def test_the_drivers_display_strings_are_renamed(self) -> None:
        self.renamed(
            "apps/windows/driver/Source/Main/Ac3ForgeNullSink.inx",
            'ProviderName    = "ac3forge"\nMfgName         = "ac3forge"\n'
            'DiskName        = "Ac3ForgeNullSink Driver Disk"\nDeviceDesc      = "Desktop Atmos"\n',
            'ProviderName    = "ICL Forge"\nMfgName         = "ICL Forge"\n'
            'DiskName        = "Ac3ForgeNullSink Driver Disk"\nDeviceDesc      = "Desktop Atmos"\n',
        )
        self.renamed(
            "apps/windows/driver/Source/Main/Ac3ForgeNullSink.rc",
            '#define VER_FILEDESCRIPTION_STR "ac3forge Desktop Atmos null-sink audio driver"\n'
            '#define VER_ORIGINALFILENAME_STR "Ac3ForgeNullSink.sys"\n',
            '#define VER_FILEDESCRIPTION_STR "ICL Forge Desktop Atmos null-sink audio driver"\n'
            '#define VER_ORIGINALFILENAME_STR "Ac3ForgeNullSink.sys"\n',
        )
        self.renamed(
            "apps/windows/driver-vm/Deploy-Desk.ps1",
            "Copy-Item (Join-Path $bin 'ac3crucible.exe') $stage\n",
            "Copy-Item (Join-Path $bin 'crucible.exe') $stage\n",
        )

    def test_external_identities_are_the_owners(self) -> None:
        for path, line in (
            (
                "apps/crucible/ui/qml/AboutDialog.qml",
                'text: "<a href=\\"https://github.com/iainchesworthlabs/ac3forge\\">x</a>"\n',
            ),
            (
                "apps/crucible/packaging/linux/ac3crucible.metainfo.xml",
                '<url type="homepage">https://github.com/iainchesworthlabs/ac3forge</url>\n',
            ),
            (".github/workflows/sonarcloud.yml", "--project-key iainchesworthlabs_ac3forge \\\n"),
            (".github/workflows/_build.yml", "the workspace is /__w/ac3forge/ac3forge\n"),
            ("examples/object_signing.cpp", 'as_bytes("ac3forge-example-key-DO-NOT-USE")\n'),
        ):
            with self.subTest(line):
                self.kept(path, line)

    def test_the_lines_that_say_what_the_old_names_were_on_purpose(self) -> None:
        self.kept(
            ".github/workflows/manifest-bump.yml",
            "git rm -q --ignore-unmatch Formula/ac3forge.rb Casks/ac3gui.rb\n",
        )
        self.kept(
            ".github/workflows/manifest-bump.yml",
            "# what maps the old names (ac3forge, ac3gui) to the new ones goes to the tap's root\n",
        )

    def test_a_line_the_identifier_pass_renamed_is_not_the_programs(self) -> None:
        self.kept(
            "src/capi/include/iclforge_c/iclforge.h",
            "ICLFORGE_C_EXPORT iclforge_status_t f(void);\n",
        )


PYTHON_BEFORE = (
    '"""Runs ac3tests with `--ac3tests`."""\n'
    "import argparse\n"
    "\n"
    "\n"
    "def run(ac3tests, out):  # ac3tests is the binary\n"
    '    return [str(ac3tests), "ac3tests", out]\n'
    "\n"
    "\n"
    "parser = argparse.ArgumentParser()\n"
    'parser.add_argument("--ac3tests", required=True, help="the ac3tests binary")\n'
    "arguments = parser.parse_args()\n"
    "run(arguments.ac3tests, arguments.out)\n"
)
PYTHON_AFTER = (
    '"""Runs iclforge-tests with `--iclforge-tests`."""\n'
    "import argparse\n"
    "\n"
    "\n"
    "def run(iclforge_tests, out):  # iclforge-tests is the binary\n"
    '    return [str(iclforge_tests), "iclforge-tests", out]\n'
    "\n"
    "\n"
    "parser = argparse.ArgumentParser()\n"
    'parser.add_argument("--iclforge-tests", required=True, help="the iclforge-tests binary")\n'
    "arguments = parser.parse_args()\n"
    "run(arguments.iclforge_tests, arguments.out)\n"
)


class PythonNames(Case):
    """A hyphen is not part of a Python name: a program's name that the code uses as one is the
    stem form, and the same word in a string, a comment or a docstring is the program's name."""

    PATH = "tools/sendspin/aiosendspin_exit.py"

    def test_a_name_of_the_code_is_the_stem_and_a_word_of_the_text_is_the_program(self) -> None:
        self.renamed(self.PATH, PYTHON_BEFORE, PYTHON_AFTER)
        self.assertTrue(P.parses(PYTHON_BEFORE))
        self.assertTrue(P.parses(PYTHON_AFTER))

    def test_the_flag_sets_the_attribute_the_code_reads(self) -> None:
        action = argparse.ArgumentParser().add_argument("--iclforge-tests")
        self.assertEqual(action.dest, "iclforge_tests")

    @unittest.skipUnless(sys.version_info >= (3, 12), "a name in an f-string is a token from 3.12")
    def test_a_name_in_the_field_of_an_fstring_is_a_name_and_the_text_around_it_is_not(
        self,
    ) -> None:
        self.renamed(
            self.PATH,
            'print(f"{ac3tests}: ac3tests did not finish in {limit} s")\n',
            'print(f"{iclforge_tests}: iclforge-tests did not finish in {limit} s")\n',
        )

    def test_the_columns_are_the_characters_of_the_line(self) -> None:
        e, arrow = chr(0xE9), chr(0x2192)  # two characters that are more than one byte
        self.renamed(
            self.PATH,
            f'x = "{e}{arrow}"; run(ac3tests, "{e}")  # {e} ac3tests\n',
            f'x = "{e}{arrow}"; run(iclforge_tests, "{e}")  # {e} iclforge-tests\n',
        )

    def test_a_name_without_a_hyphen_is_the_same_name_in_both_places(self) -> None:
        self.renamed(self.PATH, 'cli = ac3cli\nname = "ac3cli"\n', 'cli = forge\nname = "forge"\n')
        self.renamed(self.PATH, "gui = ac3gui\n", "gui = forge_gui\n")
        self.renamed(self.PATH, "path = ac3hearth_render\n", "path = hearth_render\n")

    def test_a_file_of_another_language_is_text_throughout(self) -> None:
        self.renamed(
            "tools/x.sh", 'run "$BUILD/ac3tests" --list\n', 'run "$BUILD/iclforge-tests" --list\n'
        )

    def test_a_source_the_tokenizer_cannot_read_is_text(self) -> None:
        unread = "def run(ac3tests:\n"
        self.assertEqual(P.python_identifiers(self.PATH, unread), unread)
        self.assertFalse(P.breaks_python(self.PATH, unread, "def run(iclforge-tests:\n"))

    def test_a_source_that_parsed_and_does_not_is_found(self) -> None:
        before = "def run(ac3tests):\n    return ac3tests\n"
        hyphenated = before.replace("ac3tests", "iclforge-tests")
        self.assertTrue(P.breaks_python(self.PATH, before, hyphenated))
        self.assertFalse(P.breaks_python(self.PATH, before, P.transform(self.PATH, before)))
        self.assertFalse(P.breaks_python(self.PATH, before, before))
        self.assertFalse(P.breaks_python("tools/x.sh", before, hyphenated))

    def test_the_run_does_not_write_a_source_the_pass_would_break(self) -> None:
        class FakeRepo:
            files = ("tools/x.py",)

        def naive(path: str, text: str, hits: list | None = None, counts: Counter | None = None):
            return text

        old = b"def run(ac3tests):\n    return ac3tests\n"
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "tools" / "x.py"
            target.parent.mkdir()
            target.write_bytes(old)
            with unittest.mock.patch.object(P, "python_identifiers", naive):
                changed, _, broken = P.run_text(Path(tmp), FakeRepo(), {}, False, [])
            self.assertEqual((changed, broken), (0, ["tools/x.py"]))
            self.assertEqual(target.read_bytes(), old)
            changed, _, broken = P.run_text(Path(tmp), FakeRepo(), {}, False, [])
            self.assertEqual((changed, broken), (1, []))
            self.assertEqual(target.read_bytes(), old.replace(b"ac3tests", b"iclforge_tests"))


class Moves(unittest.TestCase):
    def test_the_files_named_for_a_program_or_the_brand_in_a_programs_tree(self) -> None:
        gui, hearth, crucible = "apps/gui", "apps/hearth/ui", "apps/crucible"
        java, android_test = "apps/android/app/src/main/java", "apps/android/app/src/androidTest"
        fixtures = "apps/crucible/ui/tests/fixtures/xdg/applications"
        for old, new in (
            (f"{gui}/translations/ac3gui_fr.ts", f"{gui}/translations/forge_gui_fr.ts"),
            (f"{gui}/translations/ac3gui_xx.ts", f"{gui}/translations/forge_gui_xx.ts"),
            (f"{hearth}/translations/ac3hearth_de.ts", f"{hearth}/translations/hearth_de.ts"),
            (
                f"{crucible}/translations/ac3crucible_yi.ts",
                f"{crucible}/translations/crucible_yi.ts",
            ),
            (f"{gui}/packaging/linux/ac3gui.desktop", f"{gui}/packaging/linux/forge-gui.desktop"),
            (f"{gui}/packaging/linux/ac3gui-mime.xml", f"{gui}/packaging/linux/forge-gui-mime.xml"),
            (
                f"{gui}/packaging/linux/ac3gui.metainfo.xml",
                f"{gui}/packaging/linux/forge-gui.metainfo.xml",
            ),
            (f"{gui}/icons/ac3gui.rc.in", f"{gui}/icons/forge-gui.rc.in"),
            (f"{gui}/icons/ac3forge-256.png", f"{gui}/icons/iclforge-256.png"),
            (f"{gui}/icons/ac3forge.ico", f"{gui}/icons/iclforge.ico"),
            ("assets/icon/ac3forge-icon.svg", "assets/icon/iclforge-icon.svg"),
            (
                f"{hearth}/packaging/linux/ac3hearth.desktop",
                f"{hearth}/packaging/linux/hearth.desktop",
            ),
            (
                f"{crucible}/packaging/linux/ac3crucible.metainfo.xml",
                f"{crucible}/packaging/linux/crucible.metainfo.xml",
            ),
            (
                f"{java}/com/ac3forge/shield/MainActivity.kt",
                f"{java}/com/iclforge/shield/MainActivity.kt",
            ),
            (
                f"{android_test}/java/com/ac3forge/shield/NativeBridgeInstrumentedTest.kt",
                f"{android_test}/java/com/iclforge/shield/NativeBridgeInstrumentedTest.kt",
            ),
            (
                f"{fixtures}/org.ac3forge.CrucibleFixture.desktop",
                f"{fixtures}/org.iclforge.CrucibleFixture.desktop",
            ),
        ):
            with self.subTest(old):
                self.assertEqual(P.moved_path(old), new)
                self.assertIsNone(P.moved_path(new), "a second run moves nothing")

    def test_the_drivers_files_the_released_manifests_and_the_pages_stay_where_they_are(
        self,
    ) -> None:
        for path in (
            "apps/windows/driver/Ac3ForgeNullSink.sln",
            "apps/windows/driver/Source/Main/Ac3ForgeNullSink.inx",
            "apps/windows/driver/Source/Main/Ac3ForgeNullSink.rc",
            "packaging/winget/manifests/i/iainchesworthlabs/ac3forge/0.10.0-beta.1/"
            "iainchesworthlabs.ac3forge.yaml",
            "docs/forge/gui/localisation.md",
            "apps/cli/main.cpp",
            "src/ac3/CMakeLists.txt",
        ):
            with self.subTest(path):
                self.assertIsNone(P.moved_path(path))

    def test_the_moves_of_a_tree_are_made_of_paths_that_do_not_collide(self) -> None:
        class FakeRepo:
            files = (
                "apps/gui/translations/ac3gui_fr.ts",
                "apps/gui/translations/ac3gui_de.ts",
                "apps/cli/main.cpp",
            )

        moves = P.compute_moves(FakeRepo())
        self.assertEqual(len(moves), 2)
        self.assertEqual(len(set(moves.values())), 2)


PAGE_MOVES = {
    "apps/gui/translations/ac3gui_fr.ts": "apps/gui/translations/forge_gui_fr.ts",
    "apps/gui/icons/ac3forge.ico": "apps/gui/icons/iclforge.ico",
}


class Pages(unittest.TestCase):
    MOVES = PAGE_MOVES

    def test_a_whole_old_path_becomes_its_new_one_and_nothing_else_in_the_page_changes(
        self,
    ) -> None:
        text = (
            "The catalogue is `apps/gui/translations/ac3gui_fr.ts` and the icon is\n"
            "[the icon](../apps/gui/icons/ac3forge.ico). ac3gui runs `ac3cli`.\n"
        )
        out = P.rewrite_page_paths(text, self.MOVES)
        self.assertIn("`apps/gui/translations/forge_gui_fr.ts`", out)
        self.assertIn("(../apps/gui/icons/iclforge.ico)", out)
        self.assertIn("ac3gui runs `ac3cli`", out)

    def test_a_path_that_only_ends_the_same_way_is_not_the_path(self) -> None:
        text = "apps/gui/translations/ac3gui_fr.ts.bak and old/apps/gui/icons/ac3forge.ico\n"
        self.assertEqual(P.rewrite_page_paths(text, self.MOVES), text)

    def test_which_pages(self) -> None:
        self.assertTrue(P.is_page("docs/forge/gui/localisation.md"))
        self.assertTrue(P.is_page("README.md"))
        self.assertTrue(P.is_page("apps/windows/README.md"))
        self.assertFalse(P.is_page("CHANGELOG.md"))
        self.assertFalse(P.is_page("planning/layout.md"))
        self.assertFalse(P.is_page("tools/n1b/README.md"))
        self.assertFalse(P.is_page("planning/ac4.md", {"planning/ac4.md"}))
        self.assertFalse(P.is_page("apps/cli/main.cpp"))


class Counting(unittest.TestCase):
    def test_the_report_carries_what_was_decided(self) -> None:
        hits: list = []
        counts: Counter = Counter()
        P.transform("apps/gui/qml/Main.qml", 'text: qsTr("ac3forge")\nac3cli\n', hits, counts)
        kinds = {(k, rule) for k, rule, _, _ in hits}
        self.assertIn(("renamed", "n1a-display"), kinds)
        self.assertIn(("renamed", "cli"), kinds)
        self.assertEqual(counts["cli"], 1)

    def test_a_file_that_is_all_old_names_changes_and_its_second_run_does_not(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "x.sh"
            text = "ac3cli encode\nAC3CLI_EXE=x\nAC3Forge Hearth\n"
            once = P.transform("tools/x.sh", text)
            self.assertNotEqual(once, text)
            self.assertEqual(P.transform("tools/x.sh", once), once)
            p.write_text(once, encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
