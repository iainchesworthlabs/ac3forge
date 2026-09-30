"""Unit tests for n1b_idents.py: the identifier pass of stage S4.

stdlib `unittest`. Every case is a line written out here, in the form the tree has it, run
through transform() with the path of the file it comes from: the path matters, since the bare word
means the family in a library's comment and a program's own name in that program's window. What
must stay is as much of the test as what must change: the repository slug, the SonarCloud project,
the QSettings names of the programs, the Windows driver, the Android package, the icons, the
display name `AC3Forge`, the QML module URIs, and the bytes of an example key.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import n1b_idents as I


def run(path: str, text: str) -> str:
    return I.transform(path, text)


class Case(unittest.TestCase):
    def renamed(self, path: str, before: str, after: str) -> None:
        self.assertEqual(run(path, before), after)
        self.assertEqual(run(path, after), after, "a second run changes nothing")

    def kept(self, path: str, text: str) -> None:
        self.assertEqual(run(path, text), text)


class Scope(unittest.TestCase):
    def test_pages_history_and_byte_exact_trees_are_not_read(self) -> None:
        for path in (
            "docs/library/index.md",
            "README.md",
            "CHANGELOG.md",
            "planning/ac4.md",
            "tools/n1b/README.md",
            "tests/golden/bitstream-hashes.json",
            "packaging/winget/manifests/i/x/ac3forge/0.10.0-beta.1/x.yaml",
            "esp-idf/iclforge/README.md",
            "apps/hearth/notices/x.md",
        ):
            with self.subTest(path):
                self.assertFalse(I.in_scope(path))

    def test_code_build_files_workflows_and_fixtures_are_read(self) -> None:
        for path in (
            "src/capi/include/iclforge_c/iclforge.h",
            "CMakeLists.txt",
            "cmake/Packaging.cmake",
            ".github/workflows/release.yml",
            "tools/ci/check_abi_symbols.py",
            "apps/gui/translations/ac3gui_de.ts",
            "fuzz/seeds/fuzz_sendspin_json/settings.json",
            "python/uv.lock",
            "rust/Cargo.lock",
            "sonar-project.properties",
        ):
            with self.subTest(path):
                self.assertTrue(I.in_scope(path))

    def test_the_docs_sites_wasm_fallbacks_follow_apps_wasm_except_the_binaries(self) -> None:
        self.assertTrue(I.in_scope("docs/assets/wasm-decode-demo/demo.js"))
        self.assertTrue(I.in_scope("docs/assets/wasm-encode-demo/package/index.d.ts"))
        self.assertFalse(I.in_scope("docs/assets/wasm-decode-demo/iclforge_decode.wasm"))
        self.assertFalse(I.in_scope("docs/assets/data/support-catalogue.json"))


class Identifiers(Case):
    def test_the_c_api_and_its_macros(self) -> None:
        self.renamed(
            "src/capi/include/iclforge_c/iclforge.h",
            "AC3FORGEC_EXPORT ac3forge_status_t ac3forge_encoder_create(void);\n"
            "#define AC3FORGE_OK 0\n",
            "ICLFORGE_C_EXPORT iclforge_status_t iclforge_encoder_create(void);\n"
            "#define ICLFORGE_OK 0\n",
        )

    def test_the_c_apis_version_macros_and_the_variables_that_feed_them(self) -> None:
        self.renamed(
            "src/capi/include/iclforge_c/version.h.in",
            "#define AC3FORGE_C_VERSION_MAJOR @AC3FORGEC_VERSION_MAJOR@\n",
            "#define ICLFORGE_C_VERSION_MAJOR @ICLFORGE_C_VERSION_MAJOR@\n",
        )

    def test_cmake_options_definitions_and_the_package(self) -> None:
        self.renamed(
            "CMakeLists.txt",
            "option(AC3FORGE_BUILD_ADM OFF)\n"
            "find_package(ac3forge CONFIG REQUIRED)\n"
            "cmake -DAC3FORGE_BUILD_ADM=ON\n"
            "install(FILES ac3forgeConfig.cmake)\n"
            "function(ac3forge_install_pkgconfig)\n",
            "option(ICLFORGE_BUILD_ADM OFF)\n"
            "find_package(iclforge CONFIG REQUIRED)\n"
            "cmake -DICLFORGE_BUILD_ADM=ON\n"
            "install(FILES iclforgeConfig.cmake)\n"
            "function(iclforge_install_pkgconfig)\n",
        )

    def test_kconfig_and_the_environment(self) -> None:
        self.renamed(
            "esp-idf/iclforge/Kconfig",
            'config AC3FORGE_SENDSPIN\nif(CONFIG_AC3FORGE_SENDSPIN)\nmenu "ac3forge"\n',
            'config ICLFORGE_SENDSPIN\nif(CONFIG_ICLFORGE_SENDSPIN)\nmenu "iclforge"\n',
        )
        self.renamed(
            "src/signing/src/signing_key.cpp",
            'std::getenv("AC3FORGE_SIGNING_KEY_FILE");\n',
            'std::getenv("ICLFORGE_SIGNING_KEY_FILE");\n',
        )

    def test_the_wire_and_the_format_strings_at_both_ends(self) -> None:
        self.renamed(
            "src/sendspin/src/iclforge_player.cpp",
            'constexpr auto kRole = "_ac3forge_player@v1";\n'
            'constexpr auto kSupport = "_ac3forge_player@v1_support";\n'
            'json.member("schema", "ac3forge.probe/1");\n'
            'json.member("schema", "ac3forge.hearth.media/1");\n'
            'const std::string project = "ac3forge_hearth_sink";\n'
            'out << "{\\"ac3forge_scene\\": 1}";\n',
            'constexpr auto kRole = "_iclforge_player@v1";\n'
            'constexpr auto kSupport = "_iclforge_player@v1_support";\n'
            'json.member("schema", "iclforge.probe/1");\n'
            'json.member("schema", "iclforge.hearth.media/1");\n'
            'const std::string project = "iclforge_hearth_sink";\n'
            'out << "{\\"iclforge_scene\\": 1}";\n',
        )

    def test_the_container_default_and_the_version_line(self) -> None:
        self.renamed(
            "src/mp4/include/iclforge/mp4/mp4.hpp",
            '    std::string writing_app{"ac3forge"};\n',
            '    std::string writing_app{"iclforge"};\n',
        )
        self.renamed(
            "src/ac3/src/version.cpp",
            '"ac3forge {}\\n  release: {}"\n',
            '"iclforge {}\\n  release: {}"\n',
        )

    def test_the_namespaces_of_the_esp_component_and_the_c_apis_own(self) -> None:
        self.renamed(
            "esp-idf/iclforge/include/iclforge/improv.hpp",
            "namespace ac3forge::improv {\n}  // namespace ac3forge::improv\n",
            "namespace iclforge::improv {\n}  // namespace iclforge::improv\n",
        )
        self.renamed(
            "src/capi/src/internal.hpp",
            "namespace ac3forge_c {\nac3forge::FirmwareSlot slot;\n",
            "namespace iclforge_c {\niclforge::FirmwareSlot slot;\n",
        )

    def test_a_header_or_a_directory_named_in_a_path(self) -> None:
        self.renamed(
            "apps/hearth/engine/sink_firmware.hpp",
            '#include "ac3forge/firmware_image.hpp"\n'
            '#include "iclforge/sendspin/ac3forge_player.hpp"\n'
            "// see ac3forge_c/ac3forge.h and esp-idf/ac3forge/include\n",
            '#include "iclforge/firmware_image.hpp"\n'
            '#include "iclforge/sendspin/iclforge_player.hpp"\n'
            "// see iclforge_c/iclforge.h and esp-idf/iclforge/include\n",
        )

    def test_the_members_named_for_the_role_and_a_catch2_tag(self) -> None:
        self.renamed(
            "tests/hearth/test_group.cpp",
            'client.ac3forge_support = {}; TEST_CASE("x", "[hearth][ac3forge]") {}\n',
            'client.iclforge_support = {}; TEST_CASE("x", "[hearth][iclforge]") {}\n',
        )

    def test_hyphenated_and_dotted_names_of_the_family(self) -> None:
        self.renamed(
            ".github/workflows/release.yml",
            '"release-artifacts/ac3forge-[0-9]*-win64.zip"\n'
            "gpg --export > release-artifacts/ac3forge-signing-key.asc\n"
            "npm pack ac3forge-wasm-decoder\n"
            "for the ac3forge.ac4 module and ac3forge.h\n",
            '"release-artifacts/iclforge-[0-9]*-win64.zip"\n'
            "gpg --export > release-artifacts/iclforge-signing-key.asc\n"
            "npm pack iclforge-wasm-decoder\n"
            "for the iclforge.ac4 module and iclforge.h\n",
        )

    def test_the_debian_and_rpm_names_of_the_family(self) -> None:
        self.renamed(
            "cmake/Packaging.cmake",
            'set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "ac3forge")\n'
            'set(CPACK_DEBIAN_LIBRUNTIME_PACKAGE_NAME "libac3forge0")\n'
            'set(CPACK_DEBIAN_LIBRARY_PACKAGE_NAME "libac3forge-dev")\n'
            'set(CPACK_RPM_LIBRARY_PACKAGE_NAME "ac3forge-devel")\n'
            'set(CPACK_ARCHIVE_DEV_FILE_NAME "ac3forge-dev-${V}-${S}")\n',
            'set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "iclforge")\n'
            'set(CPACK_DEBIAN_LIBRUNTIME_PACKAGE_NAME "libiclforge0")\n'
            'set(CPACK_DEBIAN_LIBRARY_PACKAGE_NAME "libiclforge-dev")\n'
            'set(CPACK_RPM_LIBRARY_PACKAGE_NAME "iclforge-devel")\n'
            'set(CPACK_ARCHIVE_DEV_FILE_NAME "iclforge-dev-${V}-${S}")\n',
        )

    def test_the_word_in_a_library_comment_and_a_message(self) -> None:
        self.renamed(
            "src/ac3/src/emdf/frame_layout.cpp",
            "// The one frame shape this walker maps - ac3forge's Atmos output.\n",
            "// The one frame shape this walker maps - iclforge's Atmos output.\n",
        )
        self.renamed(
            "python/src/iclforge_ext/bindings.cpp",
            'std::runtime_error("ac3forge encode failed: ")\n',
            'std::runtime_error("iclforge encode failed: ")\n',
        )

    def test_a_pipewire_stream_name_of_the_library(self) -> None:
        self.renamed(
            "src/audio/src/backend/pipewire/capture.cpp",
            '"ac3forge capture", props\n',
            '"iclforge capture", props\n',
        )


class JavaScript(Case):
    def test_the_factories_and_classes_of_the_wasm_package_are_identifiers(self) -> None:
        self.renamed(
            "apps/wasm/demo.js",
            "return await createAc3ForgeModule();\n"
            "const n = await Ac3ForgeDecoderNode.create(ctx);\n"
            "url: new URL('./ac3forge_decode.js', location.href)\n",
            "return await createIclForgeModule();\n"
            "const n = await IclForgeDecoderNode.create(ctx);\n"
            "url: new URL('./iclforge_decode.js', location.href)\n",
        )
        self.renamed(
            "js/src/index.ts",
            "export type Ac3ForgeModuleFactory = () => Promise<Ac3ForgeEmbindModule>;\n",
            "export type IclForgeModuleFactory = () => Promise<IclForgeEmbindModule>;\n",
        )

    def test_the_esphome_components_class(self) -> None:
        self.renamed(
            "esphome/components/iclforge/__init__.py",
            'Ac3ForgeComponent = ac3forge_ns.class_("Ac3ForgeComponent", cg.Component)\n',
            'IclForgeComponent = iclforge_ns.class_("IclForgeComponent", cg.Component)\n',
        )

    def test_a_qml_module_uri_that_looks_alike_is_a_programs(self) -> None:
        self.kept("apps/gui/main.cpp", 'engine.loadFromModule("Ac3Forge", "Main");\n')
        self.kept("apps/hearth/ui/main.cpp", 'engine.loadFromModule("Ac3ForgeHearth", "Main");\n')
        self.kept(
            "apps/crucible/ui/main.cpp",
            'qmlRegisterSingletonInstance("Ac3ForgeCrucibleLanguage");\n',
        )


class Kept(Case):
    def test_the_repository_slug_and_what_hangs_from_it(self) -> None:
        for text in (
            "url = https://github.com/iainchesworthlabs/ac3forge/releases\n",
            "docs: https://iainchesworthlabs.github.io/ac3forge/library/\n",
            "git clone https://github.com/iainchesworthlabs/ac3forge.git\n",
            "tap=iainchesworthlabs/homebrew-ac3forge\n",
            "if: github.repository == 'iainchesworthlabs/ac3forge'\n",
            "git clone https://github.com/iainchesworthlabs/ac3forge && cd ac3forge\n",
        ):
            with self.subTest(text):
                self.kept("tools/release/x.py", text)

    def test_the_sonarcloud_project(self) -> None:
        self.kept("sonar-project.properties", "sonar.projectKey=iainchesworthlabs_ac3forge\n")
        self.kept("sonar-project.properties", "sonar.projectName=ac3forge\n")
        self.kept(
            ".github/workflows/sonarcloud.yml", "--project-key iainchesworthlabs_ac3forge \\\n"
        )

    def test_what_a_runner_derives_from_the_repository_name(self) -> None:
        self.kept(".github/workflows/_build.yml", "# the workspace is /__w/ac3forge/ac3forge\n")
        self.kept(".github/workflows/_ci-windows.yml", "# occurrence (ac3forge#796, job 106)\n")

    def test_the_winget_identity_a_later_release_is_written_under_is_renamed(self) -> None:
        self.renamed(
            "tools/release/bump_manifests.py",
            "PackageIdentifier: iainchesworthlabs.ac3forge\nMoniker: ac3forge\n",
            "PackageIdentifier: iainchesworthlabs.iclforge\nMoniker: iclforge\n",
        )

    def test_the_old_name_the_hand_written_commit_says_on_purpose_is_kept(self) -> None:
        self.kept("python/pyproject.toml", 'description = "A codec (formerly ac3forge)"\n')
        self.kept(
            ".github/workflows/manifest-bump.yml",
            "          # what maps the old names (ac3forge, ac3gui) to the new ones\n",
        )
        self.kept(
            ".github/workflows/manifest-bump.yml",
            "          git rm -q --ignore-unmatch Formula/ac3forge.rb Casks/ac3gui.rb\n",
        )
        self.kept(
            "tools/checks/check_packaging_versions.sh",
            "for winget_package in ac3forge iclforge; do\n",
        )
        self.kept(
            "tools/release/bump_manifests.py",
            "    # iainchesworthlabs.ac3forge stay in the ac3forge directory as they were made.\n",
        )

    def test_the_same_word_on_another_line_of_those_files_is_renamed(self) -> None:
        self.renamed("python/pyproject.toml", 'name = "ac3forge"\n', 'name = "iclforge"\n')
        self.renamed(
            "tools/checks/check_packaging_versions.sh",
            'installer="$dir/iainchesworthlabs.ac3forge.installer.yaml"\n',
            'installer="$dir/iainchesworthlabs.iclforge.installer.yaml"\n',
        )

    def test_a_file_about_the_old_names_is_not_read(self) -> None:
        self.assertFalse(I.in_scope(".git-blame-ignore-revs"))
        self.assertFalse(I.in_scope("packaging/homebrew/tap_migrations.json"))

    def test_an_asset_of_a_release_that_exists(self) -> None:
        text = "url: https://github.com/iainchesworthlabs/ac3forge/releases/download/v0.10.0-beta.1/ac3forge-0.10.0-win64.zip\n"
        self.kept("packaging/homebrew/Casks/iclforge.rb", text)

    def test_a_template_for_the_next_release_is_renamed(self) -> None:
        self.renamed(
            "packaging/homebrew/Casks/iclforge.rb",
            'url "https://github.com/iainchesworthlabs/ac3forge/releases/download/v#{version}/ac3forge-#{v}-Darwin.dmg"\n',
            'url "https://github.com/iainchesworthlabs/ac3forge/releases/download/v#{version}/iclforge-#{v}-Darwin.dmg"\n',
        )

    def test_the_bytes_of_an_example_key(self) -> None:
        self.kept("examples/object_signing.cpp", 'as_bytes("ac3forge-example-key-DO-NOT-USE")\n')

    def test_the_windows_driver_in_every_case_form(self) -> None:
        for text in (
            "$inf = Join-Path $PackageDir 'Ac3ForgeNullSink.inf'\n",
            "$script:HardwareId = 'ROOT\\Ac3ForgeNullSink'\n",
            "foreach ($f in 'Ac3ForgeNullSink.sys', 'ac3forgenullsink.cat') {\n",
            "<Ac3ForgeWdkNuGetVersion>10.0.28000.2526</Ac3ForgeWdkNuGetVersion>\n",
            "name: ac3forge-nullsink-driver-testsigned\n",
        ):
            with self.subTest(text):
                self.kept("apps/windows/driver/x.ps1", text)

    def test_the_android_package_and_its_jni_names(self) -> None:
        for text in (
            "package com.ac3forge.shield\n",
            "Java_com_ac3forge_shield_NativeBridge_nativeSetScene(JNIEnv* env)\n",
            'constexpr char kTag[] = "ac3forge.shield.file_replay";\n',
            'System.loadLibrary("ac3forge_jni")\n',
            'keystore_path="$RUNNER_TEMP/ac3forge-shield-release.keystore"\n',
        ):
            with self.subTest(text):
                self.kept("apps/android/app/src/main/cpp/x.cpp", text)

    def test_the_icons_and_the_packages_named_for_a_program(self) -> None:
        for text in (
            'QStringLiteral(":/icons/ac3forge-32.png")\n',
            'set(MACOSX_BUNDLE_ICON_FILE "ac3forge.icns")\n',
            '"${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge.ico"\n',
            'DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/ac3forge-crucible"\n',
            'set(CPACK_DEBIAN_HEARTH_PACKAGE_NAME "ac3forge-hearth")\n',
            "packages/ac3forge-crucible-*.zip\n",
        ):
            with self.subTest(text):
                self.kept("apps/crucible/CMakeLists.txt", text)

    def test_the_display_name_and_the_registrations_that_carry_it(self) -> None:
        for text in (
            'line("AC3Forge Crucible diagnostics");\n',
            'WriteRegStr HKCR "AC3Forge.Stream\\DefaultIcon" "" "$INSTDIR\\bin\\ac3gui.exe,0"\n',
            'config.device_info = {.manufacturer = "AC3Forge"};\n',
        ):
            with self.subTest(text):
                self.kept("cmake/Packaging.cmake", text)

    def test_a_string_escape_against_the_word_is_not_part_of_a_name(self) -> None:
        # `\n` before the word is a newline, and after it a newline and not a path separator
        for path, text in (
            (
                "tests/hearth/test_diagnostics.cpp",
                'has(report, "\\n# version\\nac3forge 1.2.3\\n")\n',
            ),
            ("apps/gui/main.cpp", 'text = "ac3forge\\n";\n'),
            ("apps/gui/main.cpp", 'text = "x\\tac3forge\\t";\n'),
        ):
            with self.subTest(text):
                self.kept(path, text)
        self.renamed("tools/x.py", 'print("ac3forge\\n")\n', 'print("iclforge\\n")\n')
        self.renamed("tools/x.py", 'print("a\\nAC3FORGE_X\\n")\n', 'print("a\\nICLFORGE_X\\n")\n')

    def test_a_windows_path_separator_after_the_word_is_a_path(self) -> None:
        self.renamed(
            "tools/x.ps1",
            "$p = 'C:\\ac3forge\\bin'\n",
            "$p = 'C:\\iclforge\\bin'\n",
        )

    def test_a_programs_bare_word_is_its_own(self) -> None:
        for path, text in (
            (
                "apps/gui/main.cpp",
                'QGuiApplication::setOrganizationName(QStringLiteral("ac3forge"));\n',
            ),
            (
                "apps/crucible/ui/main.cpp",
                'QStringLiteral("ac3forge"), QStringLiteral("Crucible")\n',
            ),
            ("apps/gui/qml/Main.qml", 'title: qsTr("ac3forge - %1").arg(label)\n'),
            ("apps/wasm/index.html", "<title>ac3forge - WASM decode demo</title>\n"),
            ("apps/gui/translations/ac3gui_de.ts", "<source>ac3forge</source>\n"),
            (
                "apps/gui/tests/qml_test_main.cpp",
                'setOrganizationName(QStringLiteral("ac3forge-tests"));\n',
            ),
            ("apps/crucible/engine/platform/windows/driver_tools.cpp", 'dir /= L"ac3forge";\n'),
            (
                "apps/gui/qml/PreferencesDialog.qml",
                'PrefsKicker { text: qsTr("WHEN AC3FORGE OPENS") }\n',
            ),
        ):
            with self.subTest(path):
                self.kept(path, text)

    def test_but_what_both_ends_read_is_renamed_in_a_program_too(self) -> None:
        self.renamed(
            "apps/gui/gui_diagnostics.cpp",
            'row("AC3FORGE_SIGNING_KEY_FILE", set);\n',
            'row("ICLFORGE_SIGNING_KEY_FILE", set);\n',
        )
        self.renamed(
            "apps/gui/translations/ac3gui_de.ts",
            "<source>AC3FORGE_SIGNING_KEY: nicht gesetzt</source>\n",
            "<source>ICLFORGE_SIGNING_KEY: nicht gesetzt</source>\n",
        )
        self.renamed(
            "apps/hearth/engine/media_info.cpp",
            'json.member("schema", "ac3forge.hearth.media/1");\n',
            'json.member("schema", "iclforge.hearth.media/1");\n',
        )
        self.renamed(
            "apps/cli/CMakeLists.txt",
            "# via find_package(ac3forge). iclforge::audio stays link-only\n"
            'install(FILES x DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/ac3forge/completions")\n',
            "# via find_package(iclforge). iclforge::audio stays link-only\n"
            'install(FILES x DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/iclforge/completions")\n',
        )


class PrefixFamilies(Case):
    def test_the_export_macros_take_the_name_iclforge_add_library_makes(self) -> None:
        self.renamed(
            "src/mp4/include/iclforge/mp4/mp4.hpp",
            "class MP4_EXPORT Writer;\nclass AC4_EXPORT Decoder;\nclass AC4DEC_NO_EXPORT X;\n",
            "class ICLFORGE_MP4_EXPORT Writer;\nclass ICLFORGE_AC4_EXPORT Decoder;\n"
            "class ICLFORGE_AC4DEC_NO_EXPORT X;\n",
        )
        self.renamed(
            "src/signing/include/iclforge/signing/signing.hpp",
            "AC3SIGNING_EXPORT bool verify();\nAC3IAB_EXPORT int f();\nAC3ADM_EXPORT int g();\n"
            "AC3ADMBRIDGE_EXPORT int h();\nIAMF_EXPORT int i();\nMPEGTS_EXPORT int j();\n",
            "ICLFORGE_SIGNING_EXPORT bool verify();\nICLFORGE_IAB_EXPORT int f();\n"
            "ICLFORGE_ADM_EXPORT int g();\nICLFORGE_ADMBRIDGE_EXPORT int h();\n"
            "ICLFORGE_IAMF_EXPORT int i();\nICLFORGE_MPEGTS_EXPORT int j();\n",
        )

    def test_the_static_define_and_the_building_shared_macro_and_their_d_flags(self) -> None:
        self.renamed(
            "src/mp4/CMakeLists.txt",
            "target_compile_definitions(x PRIVATE MP4_BUILDING_SHARED)\n"
            "set(STATIC_DEFINE MP4_STATIC_DEFINE)\n"
            "-DAC4_STATIC_DEFINE -DAC4DEC_STATIC_DEFINE\n",
            "target_compile_definitions(x PRIVATE ICLFORGE_MP4_BUILDING_SHARED)\n"
            "set(STATIC_DEFINE ICLFORGE_MP4_STATIC_DEFINE)\n"
            "-DICLFORGE_AC4_STATIC_DEFINE -DICLFORGE_AC4DEC_STATIC_DEFINE\n",
        )

    def test_the_base_name_generate_export_header_is_given_moves_with_them(self) -> None:
        self.renamed(
            "src/ac4dec/CMakeLists.txt",
            "generate_export_header(x\n    BASE_NAME AC4DEC\n    EXPORT_MACRO_NAME AC4DEC_EXPORT\n"
            "    STATIC_DEFINE AC4DEC_STATIC_DEFINE)\n"
            "generate_export_header(y\n    BASE_NAME AC3FORGEC\n"
            "    EXPORT_MACRO_NAME AC3FORGEC_EXPORT)\n"
            "generate_export_header(z\n    BASE_NAME ADMBRIDGE\n"
            "    EXPORT_MACRO_NAME AC3ADMBRIDGE_EXPORT)\n",
            "generate_export_header(x\n    BASE_NAME ICLFORGE_AC4DEC\n"
            "    EXPORT_MACRO_NAME ICLFORGE_AC4DEC_EXPORT\n"
            "    STATIC_DEFINE ICLFORGE_AC4DEC_STATIC_DEFINE)\n"
            "generate_export_header(y\n    BASE_NAME ICLFORGE_C\n"
            "    EXPORT_MACRO_NAME ICLFORGE_C_EXPORT)\n"
            "generate_export_header(z\n    BASE_NAME ICLFORGE_ADMBRIDGE\n"
            "    EXPORT_MACRO_NAME ICLFORGE_ADMBRIDGE_EXPORT)\n",
        )

    def test_a_name_that_only_ends_like_an_export_macro_is_left(self) -> None:
        self.kept("src/x/x.hpp", "JNIEXPORT void JNICALL f();\nint MY_MP4_EXPORT_COUNT = 0;\n")

    def test_the_old_single_librarys_macros(self) -> None:
        self.renamed(
            "tools/checks/x.sh",
            "-DAC3FORGE_STATIC_DEFINE  AC3FORGE_EXPORT\n",
            "-DICLFORGE_AC3_STATIC_DEFINE  ICLFORGE_AC3_EXPORT\n",
        )

    def test_the_cmake_helper_targets_and_their_real_names(self) -> None:
        self.renamed(
            "src/mp4/CMakeLists.txt",
            '"$<BUILD_INTERFACE:ac3::warnings>"\n"$<BUILD_INTERFACE:ac3::fmt_private>"\n'
            "target_link_libraries(x PRIVATE ac3::coverage ac3::tracy ac3::fmt)\n"
            "ac3::minimal_profile ac3::crucible_engine\n"
            "add_library(ac3_warnings INTERFACE)\n"
            "target_compile_options(ac3_fmt_private INTERFACE)\n",
            '"$<BUILD_INTERFACE:iclforge::warnings>"\n"$<BUILD_INTERFACE:iclforge::fmt_private>"\n'
            "target_link_libraries(x PRIVATE iclforge::coverage iclforge::tracy iclforge::fmt)\n"
            "iclforge::minimal_profile iclforge::crucible_engine\n"
            "add_library(iclforge_warnings INTERFACE)\n"
            "target_compile_options(iclforge_fmt_private INTERFACE)\n",
        )

    def test_a_cpp_qualifier_that_only_looks_like_a_helper_is_left(self) -> None:
        self.kept("src/x/x.cpp", "iclforge::ac3::warnings_count();\nauto x = ac3::fmt_helper();\n")

    def test_the_profiling_macros(self) -> None:
        self.renamed(
            "src/base/variants/profiling-tracy_enabled/iclforge/base/detail/profiling.hpp",
            'AC3_ZONE_SCOPED_N("x"); AC3_ZONE_BEGIN(a); AC3_ZONE_END(a);\n'
            'AC3_FRAME_MARK(); AC3_FRAME_MARK_NAMED("UI");\n'
            '#define AC3_PROFILING_ZONE_NAME_2 "z"\n',
            'ICLFORGE_ZONE_SCOPED_N("x"); ICLFORGE_ZONE_BEGIN(a); ICLFORGE_ZONE_END(a);\n'
            'ICLFORGE_FRAME_MARK(); ICLFORGE_FRAME_MARK_NAMED("UI");\n'
            '#define ICLFORGE_PROFILING_ZONE_NAME_2 "z"\n',
        )

    def test_the_other_ac3_names_are_not_the_brand(self) -> None:
        self.kept(
            "cmake/CompilerWarnings.cmake",
            "set(AC3_MSVC_WARNINGS /W4)\nset(AC3_BURST_BYTES 6144)\nset(AC3_GUI_ICON_ICO x)\n",
        )

    def test_the_ac3_librarys_files_as_comments_still_name_them(self) -> None:
        self.renamed(
            "src/capi/CMakeLists.txt",
            "# libac3forge.so and libac3forge_static.a; on Windows ac3forge.dll,"
            " ac3forge_static.lib\n"
            "# libac3forge_c.so and libac3forge0 and libac3forge-dev\n",
            "# libiclforge_ac3.so and libiclforge_ac3_static.a; on Windows iclforge_ac3.dll, "
            "iclforge_ac3_static.lib\n"
            "# libiclforge_c.so and libiclforge0 and libiclforge-dev\n",
        )


class Reports(unittest.TestCase):
    def test_every_occurrence_is_recorded_with_the_rule_that_decided_it(self) -> None:
        hits: list = []
        text = (
            "AC3FORGE_X ac3forge_y\n"
            "https://github.com/iainchesworthlabs/ac3forge\n"
            "AC3Forge Crucible\n"
        )
        I.transform("tools/x.py", text, hits)
        self.assertEqual(
            [(rule, number) for rule, number, _ in hits],
            [("identifier", 1), ("identifier", 1), ("slug", 2), ("n1a-display", 3)],
        )

    def test_an_unknown_spelling_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            I.rename_of("aC3forge")

    def test_a_second_run_over_a_mixed_file_changes_nothing(self) -> None:
        text = (
            "MP4_EXPORT ac3::warnings AC3_ZONE_END AC3FORGEC_EXPORT libac3forge.so\n"
            "_ac3forge_player@v1 ac3forge::Firmware createAc3ForgeModule find_package(ac3forge)\n"
        )
        once = run("tools/x.py", text)
        self.assertNotIn("ac3forge", once.lower())
        self.assertEqual(run("tools/x.py", once), once)


if __name__ == "__main__":
    unittest.main()
