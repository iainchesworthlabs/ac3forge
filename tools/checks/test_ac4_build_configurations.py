"""The builds that link no AC-4 library compile none (planning/ac4.md, phase D8).

AC3FORGE_BUILD_AC4 is on by default, and the AC-4 libraries (src/ac4, src/ac4core,
src/ac4dec, src/ac4enc) are part of the default target, so a configuration that
builds everything and links none of them compiled all four for nothing: the Android
app's CMake wrapper, the WebAssembly preset and the Python wheel. D8 turns the
option off in each. Plan phase I4, which binds AC-4 into the C API, Python, Rust and
WebAssembly, turns it back on where it links the libraries, and changes this check
with it.
"""

import json
import re
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

ANDROID = ROOT / "apps" / "android" / "app" / "src" / "main" / "cpp" / "CMakeLists.txt"
PRESETS = ROOT / "CMakePresets.json"
PYPROJECT = ROOT / "python" / "pyproject.toml"

# What each of those builds compiles its own code from; none may name an AC-4 target
# while the option is off there.
LINKERS = [
    ANDROID,
    ROOT / "apps" / "wasm" / "CMakeLists.txt",
    ROOT / "python" / "CMakeLists.txt",
]


class Ac4BuildConfigurations(unittest.TestCase):
    def test_android_wrapper_turns_ac4_off(self):
        text = ANDROID.read_text(encoding="utf-8")
        self.assertRegex(text, r'set\(AC3FORGE_BUILD_AC4 OFF CACHE BOOL "" FORCE\)')

    def test_wasm_preset_turns_ac4_off(self):
        presets = json.loads(PRESETS.read_text(encoding="utf-8"))
        wasm = next(p for p in presets["configurePresets"] if p["name"] == "wasm-emscripten")
        self.assertEqual(wasm["cacheVariables"].get("AC3FORGE_BUILD_AC4"), "OFF")

    def test_wheel_turns_ac4_off(self):
        pyproject = tomllib.loads(PYPROJECT.read_text(encoding="utf-8"))
        define = pyproject["tool"]["scikit-build"]["cmake"]["define"]
        self.assertEqual(define.get("AC3FORGE_BUILD_AC4"), "OFF")

    def test_none_of_them_links_ac4(self):
        for path in LINKERS:
            with self.subTest(path=path.relative_to(ROOT).as_posix()):
                self.assertIsNone(re.search(r"\bac4::", path.read_text(encoding="utf-8")))


if __name__ == "__main__":
    unittest.main()
