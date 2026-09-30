"""AC-4 opt-in by build configuration (planning/ac4.md, phases D8 and I4).

AC3FORGE_BUILD_AC4 is on by default, and the AC-4 libraries (src/ac4, src/ac4core,
src/ac4dec, src/ac4enc) are part of the default target. D8 found three builds that
linked none of them and turned the option off there instead of compiling all four
for nothing: the Android app's CMake wrapper, the WebAssembly preset and the Python
wheel. Phase I4 binds AC-4 into the C API, Python, Rust and WebAssembly:

- Python and WebAssembly turn the option back on AND link ac4:: targets (the pybind11
  extension's `ac4` submodule; the apps/wasm/ ac3forge_wasm_ac4 embind module).
- Android turns the option back on too - the libraries depend on nothing outside this
  tree (packaging/vcpkg-port/ac3forge/vcpkg.json's own "ac4" feature description says
  the same) and cross-compile under the NDK with no extra package friction - but the
  app's own CMake wrapper links none of them: giving the Shield app an AC-4 feature is
  later application work, not this phase's.

D14 gives the minimum-footprint decode profile the AC-4 decoder, through an option of its own,
AC3FORGE_MINIMAL_AC4: the ESP-IDF component's Kconfig sets it, and so do the bare-metal probe's
AC-4 presets (apps/baremetal/ac4_probe.cpp, tools/checks/run_baremetal_probe.sh --ac4).
AC3FORGE_BUILD_AC4 stays off in every minimal preset, since it also builds the encoder, the
applications and the tests. The hidden minimal-decoder and minimal-encoder presets and the ordinary
minimal ones leave AC3FORGE_MINIMAL_AC4 off, and the three config-*-minimal-ac4 presets turn it
on, in float. The option builds the decoder, the inspector and the core statically and without
exceptions, and the AC-4 encoder is built for none of the parts.
"""

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

ANDROID = ROOT / "apps" / "android" / "app" / "src" / "main" / "cpp" / "CMakeLists.txt"
ROOT_CMAKE = ROOT / "CMakeLists.txt"
BAREMETAL_CMAKE = ROOT / "apps" / "baremetal" / "CMakeLists.txt"
WASM_CMAKE = ROOT / "apps" / "wasm" / "CMakeLists.txt"
PYTHON_CMAKE = ROOT / "python" / "CMakeLists.txt"
PRESETS = ROOT / "CMakePresets.json"
PYPROJECT = ROOT / "python" / "pyproject.toml"


def _ac4_referenced(path: Path) -> bool:
    return re.search(r"\biclforge::ac4", path.read_text(encoding="utf-8")) is not None


def _resolved_cache_variables(presets: dict, name: str) -> dict:
    """A configure preset's cacheVariables with what it inherits, as CMake resolves them: its own
    over those of the presets it inherits, the earlier of those over the later."""
    by_name = {p["name"]: p for p in presets["configurePresets"]}
    preset = by_name[name]
    resolved: dict = {}
    for parent in reversed(preset.get("inherits", [])):
        resolved.update(_resolved_cache_variables(presets, parent))
    resolved.update(preset.get("cacheVariables", {}))
    return resolved


class Ac4BuildConfigurations(unittest.TestCase):
    def test_android_wrapper_no_longer_forces_ac4_off(self):
        text = ANDROID.read_text(encoding="utf-8")
        self.assertNotRegex(text, r'set\(AC3FORGE_BUILD_AC4 OFF CACHE BOOL "" FORCE\)')

    def test_android_wrapper_still_links_no_ac4_target(self):
        # The libraries build (the root default, now unforced above); nothing in this
        # app's own CMake wrapper links them yet - a later phase's application work.
        self.assertFalse(_ac4_referenced(ANDROID))

    def test_wasm_preset_no_longer_turns_ac4_off(self):
        presets = json.loads(PRESETS.read_text(encoding="utf-8"))
        wasm = next(p for p in presets["configurePresets"] if p["name"] == "wasm-emscripten")
        self.assertNotEqual(wasm.get("cacheVariables", {}).get("AC3FORGE_BUILD_AC4"), "OFF")

    def test_wasm_links_ac4(self):
        self.assertTrue(_ac4_referenced(WASM_CMAKE))

    def test_wheel_turns_ac4_on(self):
        # The [tool.scikit-build.cmake.define] table, up to the next table: read as text, since
        # tomllib is newer than the Python 3.10 these scripts support.
        text = PYPROJECT.read_text(encoding="utf-8")
        header = r"^\[tool\.scikit-build\.cmake\.define\]\n"
        table = re.search(header + r"(.*?)(?=^\[)", text, re.M | re.S)
        self.assertIsNotNone(table)
        self.assertRegex(table.group(1), r'(?m)^AC3FORGE_BUILD_AC4 = "ON"$')

    def test_python_links_ac4(self):
        self.assertTrue(_ac4_referenced(PYTHON_CMAKE))

    def test_minimal_base_presets_still_turn_ac4_off(self):
        # The hidden bases the ordinary minimal presets and the encode probe's inherit,
        # not wasm-emscripten: the AC-4 presets below override them.
        presets = json.loads(PRESETS.read_text(encoding="utf-8"))
        for name in ("minimal-decoder", "minimal-encoder"):
            with self.subTest(preset=name):
                preset = next(p for p in presets["configurePresets"] if p["name"] == name)
                self.assertEqual(preset["cacheVariables"].get("AC3FORGE_BUILD_AC4"), "OFF")

    def test_ordinary_minimal_and_encoder_presets_have_no_ac4(self):
        presets = json.loads(PRESETS.read_text(encoding="utf-8"))
        for name in ("config-linux-gcc-minimal", "config-linux-llvm-minimal",
                     "config-arm-none-eabi-minimal", "config-arm-none-eabi-minimal-icount",
                     "config-linux-gcc-minimal-encoder", "config-arm-none-eabi-minimal-encoder",
                     "config-arm-none-eabi-minimal-encoder-icount"):
            with self.subTest(preset=name):
                resolved = _resolved_cache_variables(presets, name)
                self.assertEqual(resolved.get("AC3FORGE_BUILD_AC4"), "OFF")
                self.assertNotEqual(resolved.get("AC3FORGE_MINIMAL_AC4"), "ON")

    def test_minimal_ac4_presets_carry_the_decoder_in_float(self):
        presets = json.loads(PRESETS.read_text(encoding="utf-8"))
        names = ("config-linux-gcc-minimal-ac4", "config-arm-none-eabi-minimal-ac4",
                 "config-arm-none-eabi-minimal-ac4-icount")
        for name in names:
            with self.subTest(preset=name):
                resolved = _resolved_cache_variables(presets, name)
                self.assertEqual(resolved.get("AC3FORGE_MINIMAL_AC4"), "ON")
                self.assertEqual(resolved.get("AC3FORGE_BUILD_AC4"), "OFF")
                self.assertEqual(resolved.get("AC3FORGE_DECODE_SCALAR"), "float")
                self.assertEqual(resolved.get("AC3FORGE_MINIMAL_DECODER"), "ON")
        # The instruction-counting leg is the same build with the timer clock.
        icount = _resolved_cache_variables(presets, "config-arm-none-eabi-minimal-ac4-icount")
        self.assertEqual(icount.get("AC3FORGE_BAREMETAL_CLOCK"), "timer")
        build_presets = {p["configurePreset"] for p in presets["buildPresets"]}
        for name in names:
            self.assertIn(name, build_presets)

    def test_root_adds_the_ac4_decoder_to_the_decode_profile_and_never_its_encoder(self):
        text = ROOT_CMAKE.read_text(encoding="utf-8")
        self.assertIn("option(AC3FORGE_MINIMAL_AC4", text)
        self.assertIn("if(AC3FORGE_MINIMAL_AC4 AND NOT AC3FORGE_MINIMAL_DECODER)", text)
        # The profile's branch builds the inspector, the core and the decoder, and no encoder.
        branch = text.split("elseif(AC3FORGE_MINIMAL_AC4)", 1)[1].split("\nendif()", 1)[0]
        for directory in ("src/ac4", "src/ac4core", "src/ac4dec"):
            self.assertIn(f"add_subdirectory({directory})", branch)
        self.assertNotIn("src/ac4enc", branch)

    def test_baremetal_ac4_probe_links_the_decoder_and_nothing_of_the_encoder(self):
        text = BAREMETAL_CMAKE.read_text(encoding="utf-8")
        self.assertIn("ac4_probe.cpp", text)
        self.assertIn("elseif(AC3FORGE_MINIMAL_AC4)", text)
        self.assertIn("iclforge::ac4dec_static", text)
        self.assertNotIn("ac4enc", text)


if __name__ == "__main__":
    unittest.main()
