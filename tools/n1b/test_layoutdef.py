"""Unit tests for layoutdef.py, where every path goes in the layout of planning/layout.md.

stdlib `unittest`, and no test reads the real tree: the cases are paths written out here, so a file
added to main cannot fail one. They pin the rules stage S2 will run: where each library's files go,
what the flattened variant trees are called, which spelling reaches a header before and after, and
that the assignment of src/forge files to libraries equals the one tools/checks/layering.json gives
the dependency check.
"""

import json
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import layoutdef

LAYERING = Path(__file__).resolve().parents[1] / "checks" / "layering.json"

SRC_MOVES = {
    "src/forge/include/ac3/core/bitreader.hpp": "src/base/include/iclforge/base/bitreader.hpp",
    "src/forge/include/ac3/core/layout.hpp": "src/base/include/iclforge/base/layout.hpp",
    "src/forge/include/ac3/core/downmix_target.hpp": (
        "src/base/include/iclforge/base/downmix_target.hpp"
    ),
    "src/forge/include/ac3/core/tables.hpp": "src/ac3/include/iclforge/ac3/core/tables.hpp",
    "src/forge/include/ac3/dsp/qmf.hpp": "src/dsp/include/iclforge/dsp/qmf.hpp",
    "src/forge/include/ac3/core/fft.hpp": "src/dsp/include/iclforge/dsp/fft.hpp",
    "src/forge/src/core/fft.cpp": "src/dsp/src/fft.cpp",
    "src/forge/src/core/fft_kernel.hpp": "src/dsp/include/iclforge/dsp/detail/fft_kernel.hpp",
    "src/forge/include/ac3/render/layout.hpp": "src/render/include/iclforge/render/layout.hpp",
    "src/forge/include/ac3/render/pcm_block.hpp": (
        "src/render/include/iclforge/render/pcm_block.hpp"
    ),
    "src/forge/include/ac3/spatial/spatial.hpp": "src/render/include/iclforge/render/spatial.hpp",
    "src/forge/src/spatial/spatial.cpp": "src/render/src/spatial.cpp",
    "src/forge/include/ac3/decoder/serving.hpp": (
        "src/ac3/include/iclforge/ac3/decoder/serving.hpp"
    ),
    "src/forge/include/ac3/oba/scene.hpp": "src/objects/include/iclforge/objects/scene.hpp",
    "src/forge/include/ac3/oba/placement.hpp": "src/objects/include/iclforge/objects/placement.hpp",
    "src/forge/include/ac3/oba/joc_domain.hpp": (
        "src/objects/include/iclforge/objects/joc_domain.hpp"
    ),
    "src/forge/include/ac3/oba/atmos.hpp": "src/ac3/include/iclforge/ac3/oba/atmos.hpp",
    "src/forge/src/oba/scene_text.hpp": "src/objects/src/scene_text.hpp",
    "src/forge/include/ac3/emdf/emdf.hpp": "src/objects/include/iclforge/objects/emdf.hpp",
    "src/forge/src/emdf/emdf.cpp": "src/objects/src/emdf.cpp",
    "src/forge/include/ac3/iec61937/iec61937.hpp": (
        "src/iec61937/include/iclforge/iec61937/iec61937.hpp"
    ),
    "src/forge/src/internal/cpu/cpu_features.cpp": "src/base/src/cpu_features.cpp",
    "src/forge/src/internal/cpu/cpu_features.hpp": (
        "src/base/include/iclforge/base/detail/cpu_features.hpp"
    ),
    "src/forge/src/internal/avx2/mdct_avx2.cpp": "src/ac3/src/internal/avx2/mdct_avx2.cpp",
    "src/forge/CMakeLists.txt": "src/ac3/CMakeLists.txt",
    "src/ac3adm/src/adm.cpp": "src/adm/src/adm.cpp",
    "src/capi/include/ac3forge_c/ac3forge.h": "src/capi/include/iclforge_c/iclforge.h",
    "src/audio/include/ac3/audio/speakers.hpp": "src/audio/include/iclforge/audio/speakers.hpp",
    "src/sendspin/include/ac3/sendspin/messages.hpp": (
        "src/sendspin/include/iclforge/sendspin/messages.hpp"
    ),
    "src/mp4/include/mp4/mp4.hpp": "src/mp4/include/iclforge/mp4/mp4.hpp",
    "src/ac4dec/include/ac4dec/decoder.hpp": "src/ac4dec/include/iclforge/ac4dec/decoder.hpp",
    "src/arithmetic/include/ac3/internal/fixed32.hpp": (
        "src/arithmetic/include/iclforge/arithmetic/fixed32.hpp"
    ),
    "src/ac4core/src/tables/huffman_tables.hpp": (
        "src/ac4core/include/iclforge/ac4core/tables/huffman_tables.hpp"
    ),
    "src/ac4core/src/dsp/qmf_vector.hpp": "src/ac4core/include/iclforge/ac4core/dsp/qmf_vector.hpp",
}

VARIANT_MOVES = {
    "src/forge/src/internal/cpu/probe/msvc/ac3/internal/cpu/hardware_avx2.hpp": (
        "src/base/variants/cpu-probe-msvc/iclforge/base/detail/hardware_avx2.hpp"
    ),
    "src/forge/src/internal/profiling/tracy_enabled/ac3/internal/profiling.hpp": (
        "src/base/variants/profiling-tracy_enabled/iclforge/base/detail/profiling.hpp"
    ),
    "src/forge/src/internal/scalar/float32/ac3/internal/decode_scalar.hpp": (
        "src/ac3/variants/decode-scalar-float32/iclforge/ac3/detail/decode_scalar.hpp"
    ),
    "src/forge/src/internal/scalar/encode/float32/ac3/internal/encode_scalar.hpp": (
        "src/ac3/variants/encode-scalar-float32/iclforge/ac3/detail/encode_scalar.hpp"
    ),
    "src/forge/src/internal/profile/full/ac3/internal/profile.hpp": (
        "src/ac3/variants/profile-full/iclforge/ac3/detail/profile.hpp"
    ),
    "src/ac4core/src/internal/scalar/float/ac4/detail/real.hpp": (
        "src/ac4core/variants/scalar-float/iclforge/ac4core/detail/real.hpp"
    ),
    "src/ac4core/src/internal/profiling/stage_timers/ac4/detail/profiling.hpp": (
        "src/ac4core/variants/profiling-stage_timers/iclforge/ac4core/detail/profiling.hpp"
    ),
    # D14a moved the SIMD seam from src/forge to src/arithmetic
    "src/arithmetic/arch/x86_64/ac3/internal/arch/simd.hpp": (
        "src/arithmetic/variants/arch-x86_64/iclforge/arithmetic/detail/simd.hpp"
    ),
}


class SrcMoves(unittest.TestCase):
    def test_every_library_of_the_split_gets_its_files(self) -> None:
        for old, new in SRC_MOVES.items():
            with self.subTest(old):
                self.assertEqual(layoutdef.l2_new(old), new)

    def test_variant_trees_are_flattened_to_axis_and_choice(self) -> None:
        for old, new in VARIANT_MOVES.items():
            with self.subTest(old):
                self.assertEqual(layoutdef.l2_new(old), new)

    def test_a_second_run_moves_nothing(self) -> None:
        moved = {**SRC_MOVES, **VARIANT_MOVES}
        for old, new in moved.items():
            with self.subTest(new):
                self.assertIn(layoutdef.l2_new(new), (None, new), old)

    def test_files_that_stay_have_no_move(self) -> None:
        for path in ("src/ac4dec/src/decoder.cpp", "src/mp4/src/reader.cpp", "docs/index.md"):
            with self.subTest(path):
                self.assertIsNone(layoutdef.l2_new(path))


class Spellings(unittest.TestCase):
    def test_a_public_header_is_reached_by_its_path_below_include(self) -> None:
        self.assertEqual(
            layoutdef.spelling_of("src/forge/include/ac3/core/tables.hpp"), "ac3/core/tables.hpp"
        )
        self.assertEqual(
            layoutdef.spelling_of("src/base/include/iclforge/base/layout.hpp"),
            "iclforge/base/layout.hpp",
        )

    def test_a_template_is_reached_as_the_header_it_generates(self) -> None:
        self.assertEqual(
            layoutdef.spelling_of("src/forge/include/ac3/version.hpp.in"), "ac3/version.hpp"
        )

    def test_a_variant_header_is_reached_by_the_path_below_its_variant_directory(self) -> None:
        old = "src/arithmetic/arch/x86_64/ac3/internal/arch/simd.hpp"
        self.assertEqual(layoutdef.spelling_of(old), "ac3/internal/arch/simd.hpp")
        new = "src/arithmetic/variants/arch-x86_64/iclforge/arithmetic/detail/simd.hpp"
        self.assertEqual(layoutdef.spelling_of(new), "iclforge/arithmetic/detail/simd.hpp")

    def test_a_private_source_header_has_no_spelling(self) -> None:
        self.assertIsNone(layoutdef.spelling_of("src/forge/src/decoder/block_norm.hpp"))


class LibraryOf(unittest.TestCase):
    def test_forge_files_are_split_and_the_rest_keep_their_directory(self) -> None:
        self.assertEqual(layoutdef.library_of("src/forge/include/ac3/render/layout.hpp"), "render")
        self.assertEqual(layoutdef.library_of("src/forge/src/decoder/decoder.cpp"), "ac3")
        self.assertEqual(layoutdef.library_of("src/ac3adm/src/adm.cpp"), "adm")
        self.assertEqual(layoutdef.library_of("src/ac4enc/src/encoder.cpp"), "ac4enc")
        self.assertIsNone(layoutdef.library_of("tests/core/test_bits.cpp"))

    def test_the_dependency_checks_assignment_equals_the_movers(self) -> None:
        """tools/checks/layering.json splits src/forge the way FORGE_RULES does, for every path."""
        raw = json.loads(LAYERING.read_text(encoding="utf-8"))["layout"]
        split = [(re.compile(p), lib) for p, lib in raw["split"]]

        def by_json(path: str) -> str:
            for pattern, lib in split:
                if pattern.search(path):
                    return lib
            raise AssertionError(path)

        paths = [p for p in {**SRC_MOVES, **VARIANT_MOVES} if p.startswith("src/forge/")]
        paths += [
            "src/forge/src/core/fft_kernel.hpp",
            "src/forge/src/oba/oamd.cpp",
            "src/forge/src/oba/joc.cpp",
            "src/forge/include/ac3/oba/joc.hpp",
            "src/forge/src/render/nothing.hpp",
            "src/forge/src/verify/mirror.cpp",
            "src/forge/include/ac3/encoder/encoder.hpp",
        ]
        for path in paths:
            with self.subTest(path):
                self.assertEqual(layoutdef.forge_lib(path), by_json(path))


class TestsAndPackages(unittest.TestCase):
    def test_tests_mirror_their_library(self) -> None:
        self.assertEqual(
            layoutdef.mirrored_tests_new("tests/decoder/test_decoder.cpp"),
            "tests/ac3/decoder/test_decoder.cpp",
        )
        self.assertEqual(
            layoutdef.mirrored_tests_new("tests/backend/test_alsa.cpp"),
            "tests/audio/backend/test_alsa.cpp",
        )
        self.assertEqual(
            layoutdef.mirrored_tests_new("tests/ac3iab/test_iab.cpp"), "tests/iab/test_iab.cpp"
        )

    def test_a_mixed_directory_follows_the_library_its_file_includes_most(self) -> None:
        moves = layoutdef.mirrored_tests_new(
            "tests/core/test_fft.cpp", {"tests/core/test_fft.cpp": "dsp"}
        )
        self.assertEqual(moves, "tests/dsp/test_fft.cpp")

    def test_tests_that_stay_have_no_move(self) -> None:
        for path in (
            "tests/cli/test_cli.cpp",
            "tests/golden/audio/corpus.json",
            "tests/CMakeLists.txt",
        ):
            with self.subTest(path):
                self.assertIsNone(layoutdef.mirrored_tests_new(path))

    def test_package_directories_are_renamed_to_the_family(self) -> None:
        self.assertEqual(
            layoutdef.package_new("esp-idf/ac3forge/include/ac3forge/player.hpp"),
            "esp-idf/iclforge/include/iclforge/player.hpp",
        )
        self.assertEqual(
            layoutdef.package_new("rust/ac3forge-sys/build.rs"), "rust/iclforge-sys/build.rs"
        )
        self.assertEqual(
            layoutdef.package_new("python/src/ac3forge_ext/bindings.cpp"),
            "python/src/iclforge_ext/bindings.cpp",
        )
        self.assertEqual(
            layoutdef.package_new("cmake/ac3forgeConfig.cmake.in"), "cmake/iclforgeConfig.cmake.in"
        )
        self.assertIsNone(layoutdef.package_new("apps/cli/main.cpp"))


if __name__ == "__main__":
    unittest.main()
