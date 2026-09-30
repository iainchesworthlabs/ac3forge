"""The candidate layouts as data: where every tracked path goes.

    L1  regroup    same library boundaries, consistent names, siblings under src/
    L2  peers      forge split into base, dsp, render, objects, iec61937 and ac3; flat src/; tests
    mirror src/ L3  libs       L2's libraries under libs/ with their tests, fuzz targets and
    benchmarks beside them

Each layout is a function old_path -> new_path (None when the file stays). For the header spellings
the rewrites need, spelling_of(path) gives the string a `#include` uses to reach a file, so
old_spelling -> new_spelling is derived from the move map, never written twice.

Used by layout_dryrun.py (the size estimates), by n1b_apply.py (the executor) and by n1b_cmake.py.

The libraries of the split are the ones planning/layout.md names: arithmetic, base, dsp, objects,
render, iec61937 and ac3 (what was src/forge), beside the AC-4 four and the rest.
tools/checks/layering.json holds the same assignment of src/forge files for the dependency check;
test_layoutdef.py keeps the two equal.
"""

from __future__ import annotations

import re

from n1b_lib import Repo

FAMILY = "iclforge"

# --- the split of src/forge (L2, L3) ------------------------------------------------------------
# The in-tree cuts (planning/layout.md, stage S1; cuts.py) leave these files behind, so the rules
# name them: core/layout.hpp        Location, Layout (from core/eac3_tables.hpp)         -> base
# core/downmix_target.hpp DownmixTarget (from decoder/output.hpp)             -> base
# render/pcm_block.hpp   PcmBlock, BlockSink (from decoder/decoder.hpp)       -> render
# oba/joc_domain.hpp     joc::Domain, reconstruction_delay (from oba/joc.hpp) -> objects
# oba/placement.hpp      ObjectPlacement (from oba/atmos.hpp)                 -> objects
# decoder/serving.hpp    was render/serving.hpp, the adapter to DecoderConfig  -> ac3 (by its
# directory) The SIMD arch headers (src/arithmetic/arch since D14a) and the CPU probe and profiling
# variants are not forge files any more or stay in base: only cpu and profiling are still under
# src/forge/src/internal.
FORGE_RULES = [
    (r"^src/forge/include/ac3/core/(bitreader|bitwriter|layout|downmix_target)\.hpp$", "base"),
    (r"^src/forge/src/internal/(cpu|profiling)/", "base"),
    (r"^src/forge/(include/ac3|src)/dsp/", "dsp"),
    (r"^src/forge/include/ac3/core/fft\.hpp$", "dsp"),
    (r"^src/forge/src/core/fft(_kernel)?\.(cpp|hpp)$", "dsp"),
    (r"^src/forge/(include/ac3|src)/render/", "render"),
    (r"^src/forge/(include/ac3|src)/spatial/", "render"),
    (
        r"^src/forge/include/ac3/oba/(scene|scene_osc|motion|oamd|joc_domain|placement)\.hpp$",
        "objects",
    ),
    (
        r"^src/forge/src/oba/(scene|scene_json|scene_osc|scene_text|motion|oamd)\.(cpp|hpp)$",
        "objects",
    ),
    (r"^src/forge/(include/ac3|src)/emdf/emdf\.(hpp|cpp)$", "objects"),
    (r"^src/forge/(include/ac3|src)/iec61937/", "iec61937"),
    (r"^src/forge/", "ac3"),
]
_FORGE = [(re.compile(p), lib) for p, lib in FORGE_RULES]

LIB_RENAMES = {"ac3adm": "adm", "ac3iab": "iab"}

# headers of the split that other libraries include and that used to be private
CROSS_DETAIL = {
    "src/forge/src/core/fft_kernel.hpp": "dsp",
    "src/forge/src/internal/cpu/cpu_features.hpp": "base",
}

_VARIANT_TAIL = re.compile(r"/(ac3/internal/.+|ac4/detail/.+)$")
# The ESP-IDF component's two copies of one header, chosen by an include directory of its own
# (`conversion/bits`, `conversion/float`): the spelling starts below that directory.
_CONVERSION_TAIL = re.compile(r"^esp-idf/[^/]+/conversion/[^/]+/(.+)$")


def forge_lib(path: str) -> str:
    for rx, lib in _FORGE:
        if rx.search(path):
            return lib
    raise ValueError(path)


def library_of(path: str) -> str | None:
    """The library a tracked path under src/ belongs to after the L2 split; None outside src/."""
    p = path.split("/")
    if p[0] != "src" or len(p) < 3:
        return None
    if p[1] == "forge":
        return forge_lib(path)
    return LIB_RENAMES.get(p[1], p[1])


def spelling_of(path: str) -> str | None:
    """The `#include` string that reaches this tracked file from outside its own directory, or None.

    Public headers: the path below `include/`. Variant-tree headers: the path below the variant
    directory (`.../variants/<axis>/`) in the new layout, or from `ac3/`/`ac4/` in the old one.
    Templates (`x.hpp.in`) are reached as the generated `x.hpp`.
    """
    g = path[:-3] if path.endswith((".hpp.in", ".h.in")) else path
    if "/include/" in g:
        return g.split("/include/", 1)[1]
    m = _CONVERSION_TAIL.match(g)
    if m:
        return m.group(1)
    if "/variants/" in g:
        tail = g.split("/variants/", 1)[1]
        return tail.split("/", 1)[1] if "/" in tail else None
    m = _VARIANT_TAIL.search(g)
    if m and "/internal/" in g:
        return m.group(1)
    return None


def _public_new(oldlib: str, below_include: str) -> str:
    """Path below include/ of a public header of a library that is not split."""
    p = below_include.split("/")
    root, rest = p[0], "/".join(p[1:])
    if root in (
        FAMILY,
        "iclforge_c",
    ):  # already under the new header root: a second run changes nothing
        return below_include
    if oldlib in ("audio", "sendspin", "signing", "admbridge") and root == "ac3":
        return f"{FAMILY}/{rest}"
    if oldlib == "arithmetic" and root == "ac3":
        return f"{FAMILY}/arithmetic/" + "/".join(p[2:])
    if oldlib == "ac3adm":
        return f"{FAMILY}/adm/{rest}"
    if oldlib == "ac3iab":
        return f"{FAMILY}/iab/{rest}"
    if oldlib == "capi":
        name = "iclforge.h" if rest == "ac3forge.h" else rest.replace("ac3forge", "iclforge")
        return f"iclforge_c/{name}"
    return f"{FAMILY}/{root}/{rest}"  # mp4, mpegts, matroska, iamf, ac4, ac4dec, ac4enc


# CMake-selected variant trees, flattened: one directory per axis and choice, holding the header at
# the spelling every user includes it by. Old paths are `<tree>/<choice...>/ac3/internal/<rest>` (or
# ac4/detail/<file>); the new path is
# `src/<lib>/variants/<axis>-<choice>/iclforge/<lib>/detail/<file>`. The arch axis lives in
# src/arithmetic since D14a moved the SIMD seam there.
_VARIANT_AXES = [
    # (regex on the old path, axis name template, owning lib for forge files)
    (
        re.compile(r"^src/arithmetic/arch/(?P<c>[^/]+)/ac3/internal/arch/(?P<f>[^/]+)$"),
        "arch-{c}",
        "arithmetic",
    ),
    (
        re.compile(
            r"^src/forge/src/internal/cpu/probe/(?P<c>[^/]+)/ac3/internal/cpu/(?P<f>[^/]+)$"
        ),
        "cpu-probe-{c}",
        "base",
    ),
    (
        re.compile(r"^src/forge/src/internal/profiling/(?P<c>[^/]+)/ac3/internal/(?P<f>[^/]+)$"),
        "profiling-{c}",
        "base",
    ),
    (
        re.compile(r"^src/forge/src/internal/profile/(?P<c>[^/]+)/ac3/internal/(?P<f>[^/]+)$"),
        "profile-{c}",
        "ac3",
    ),
    (
        re.compile(
            r"^src/forge/src/internal/scalar/encode/(?P<c>[^/]+)/ac3/internal/(?P<f>[^/]+)$"
        ),
        "encode-scalar-{c}",
        "ac3",
    ),
    (
        re.compile(r"^src/forge/src/internal/scalar/(?P<c>[^/]+)/ac3/internal/(?P<f>[^/]+)$"),
        "decode-scalar-{c}",
        "ac3",
    ),
    (
        re.compile(r"^src/ac4core/src/internal/scalar/(?P<c>[^/]+)/ac4/detail/(?P<f>[^/]+)$"),
        "scalar-{c}",
        "ac4core",
    ),
    (
        re.compile(r"^src/ac4core/src/internal/profiling/(?P<c>[^/]+)/ac4/detail/(?P<f>[^/]+)$"),
        "profiling-{c}",
        "ac4core",
    ),
]


def variant_new(path: str, force_lib: str | None = None) -> str | None:
    for rx, axis, lib in _VARIANT_AXES:
        m = rx.match(path)
        if m:
            owner = force_lib or lib
            choice = axis.format(c=m.group("c"))
            return f"src/{owner}/variants/{choice}/{FAMILY}/{owner}/detail/{m.group('f')}"
    return None


def _forge_new(path: str, split: bool) -> str:
    lib = forge_lib(path) if split else "ac3"
    if path in CROSS_DETAIL and split:
        lib = CROSS_DETAIL[path]
        return f"src/{lib}/include/{FAMILY}/{lib}/detail/{path.rsplit('/', 1)[1]}"
    p = path.split("/")
    if p[2] == "include":
        below = "/".join(p[4:])
        if lib == "ac3":
            return f"src/ac3/include/{FAMILY}/ac3/{below}"
        return f"src/{lib}/include/{FAMILY}/{lib}/{p[-1]}"
    if p[2] == "src":
        below = "/".join(p[3:])
        v = variant_new(path, None if split else "ac3")
        if v:
            return v
        if lib == "ac3":
            return f"src/ac3/src/{below}"
        if lib == "base":
            return "src/base/src/" + below.replace("internal/cpu/", "", 1).replace(
                "internal/", "", 1
            )
        return f"src/{lib}/src/{p[-1]}"
    return "src/ac3/" + "/".join(p[2:])  # CMakeLists.txt, minimal.cmake


_AC4CORE_PUBLIC = re.compile(
    r"^src/ac4core/src/(acpl|ajcc|ajoc|aspx|dsp|tables)/[^/]+\.hpp$|^src/ac4core/src/huffman_codebook\.hpp$"
)


def l2_new(path: str, split: bool = True) -> str | None:
    p = path.split("/")
    if p[0] == "src" and len(p) > 2:
        lib = p[1]
        if lib == "forge":
            return _forge_new(path, split)
        newlib = LIB_RENAMES.get(lib, lib)
        if "/include/" in path:
            below = path.split("/include/", 1)[1]
            return f"src/{newlib}/include/{_public_new(lib, below)}"
        if lib == "ac4core":
            if _AC4CORE_PUBLIC.match(path):
                return f"src/ac4core/include/{FAMILY}/ac4core/" + "/".join(p[3:])
            return variant_new(path)
        if newlib != lib:
            return f"src/{newlib}/" + "/".join(p[2:])
        return variant_new(path)
    return None


# The renames of stage S4: in these trees a path component that carries the old family name takes
# the new one. They are the packages and the bindings (the ESP-IDF component, the Rust crates, the
# Python package and its extension, the ESPHome component, the packaging files), and a few files
# elsewhere whose names are a package's or the wire extension's. What is not here does not move: the
# historic winget manifests (versions already released, whose identifiers stay as they were
# published) and every path N1A renames with a program or a registration (the Android package
# directory, the Windows driver, a program's icons, a desktop entry).
OLD_BRAND = "ac3forge"
PACKAGE_TREES = (
    "esp-idf/ac3forge/",
    "rust/ac3forge/",
    "rust/ac3forge-sys/",
    "python/src/ac3forge/",
    "python/src/ac3forge_ext/",
    "esphome/components/ac3forge/",
    "packaging/",
)
KEEP_PACKAGE_PATHS = ("packaging/winget/",)
# Files outside the trees above, named one by one. The cask is renamed with the formula
# (planning/ac4.md, decision 41): its file is named for the program it installs, not for a brand.
PACKAGE_FILES = {
    "cmake/ac3forgeConfig.cmake.in": "cmake/iclforgeConfig.cmake.in",
    "esphome/tests/ac3forge-test.yaml": "esphome/tests/iclforge-test.yaml",
    "packaging/homebrew/Casks/ac3gui.rb": "packaging/homebrew/Casks/iclforge.rb",
}
# The wire extension `_ac3forge_player@v1` names its header, its source and its test, and the seeds
# and the regression of the message fuzzer that carry it. The committed WASM fallbacks of the docs
# site (docs/assets/wasm-*-demo, which docs.yml replaces with a fresh build at every deploy) are
# named for the modules apps/wasm builds, so they follow the build's output names.
_WIRE_FILES = re.compile(
    r"^(?:src/sendspin/(?:include/iclforge/sendspin|src)/ac3forge_player\.(?:hpp|cpp)"
    r"|tests/sendspin/test_ac3forge_player\.cpp"
    r"|fuzz/(?:seeds|regressions)/fuzz_sendspin_messages/[^/]*ac3forge[^/]*"
    r"|docs/assets/wasm-(?:decode|encode)-demo/ac3forge_(?:decode|encode)\.(?:js|wasm))$"
)


def package_new(path: str) -> str | None:
    """The renames of stage S4 (packages and bindings, and the files named for the wire extension):
    common to every layout. A second run finds nothing, since a renamed path no longer matches."""
    if path in PACKAGE_FILES:
        return PACKAGE_FILES[path]
    if path.startswith(KEEP_PACKAGE_PATHS):
        return None
    if path.startswith(PACKAGE_TREES) or _WIRE_FILES.match(path):
        new = "/".join(part.replace(OLD_BRAND, FAMILY) for part in path.split("/"))
        return new if new != path else None
    return None


def l1_new(path: str) -> str | None:
    return l2_new(path, split=False) or package_new(path)


# --- tests: mirror src/ (L2) or sit beside their library (L3)
# --------------------------------------
TEST_DIR_TO_LIB = {
    "core": "ac3",
    "decoder": "ac3",
    "encoder": "ac3",
    "io": "ac3",
    "meta": "ac3",
    "oba": "ac3",
    "quality": "ac3",
    "verify": "ac3",
    "emdf": "ac3",
    "analysis": "ac3",
    "dsp": "dsp",
    "render": "render",
    "spatial": "render",
    "iec61937": "iec61937",
    "ac3iab": "iab",
    "adm": "adm",
    "admbridge": "admbridge",
    "audio": "audio",
    "backend": "audio",
    "capi": "capi",
    "iamf": "iamf",
    "sendspin": "sendspin",
    "signing": "signing",
    "ac4": "ac4",
    "ac4core": "ac4core",
    "ac4dec": "ac4dec",
    "ac4enc": "ac4enc",
}
FORGE_TEST_DIRS = (
    "core",
    "decoder",
    "encoder",
    "io",
    "meta",
    "oba",
    "quality",
    "verify",
    "emdf",
    "analysis",
)
STAYING_TEST_DIRS = {"cli", "crucible", "gui", "hearth", "golden", "performance", "platform", "crt"}


def mirrored_tests_new(path: str, per_file_lib: dict | None = None) -> str | None:
    p = path.split("/")
    if p[0] != "tests" or len(p) < 3:
        return None
    old = p[1]
    if old in STAYING_TEST_DIRS:
        return None
    if old not in TEST_DIR_TO_LIB and old != "containers":
        return None
    lib = (per_file_lib or {}).get(path) or TEST_DIR_TO_LIB.get(old)
    if lib is None:
        return None
    rest = "/".join(p[2:])
    if lib == "ac3" and old in FORGE_TEST_DIRS:
        return f"tests/ac3/{old}/{rest}"
    if old == "backend":
        return f"tests/audio/backend/{rest}"
    return f"tests/{lib}/{rest}"


def l3_new(path: str, per_file_lib: dict | None = None) -> str | None:
    n = l2_new(path, True) or package_new(path)
    cur = n or path
    p = cur.split("/")
    if p[0] == "src" and len(p) > 1:
        return "libs/" + "/".join(p[1:])
    m = mirrored_tests_new(path, per_file_lib)
    if m:
        q = m.split("/")
        return f"libs/{q[1]}/tests/" + "/".join(q[2:])
    return n


def apply(layout: str, repo: Repo, per_file_lib: dict | None = None) -> dict[str, str]:
    out = {}
    for f in repo.files:
        if layout == "L1":
            n = l1_new(f)
        elif layout == "L2":
            n = l2_new(f, True) or package_new(f) or mirrored_tests_new(f, per_file_lib)
        elif layout == "L3":
            n = l3_new(f, per_file_lib)
        else:
            raise ValueError(layout)
        if n and n != f:
            out[f] = n
    return out
