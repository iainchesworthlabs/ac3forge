"""The pages, stage S5 of the plan: the documentation follows the new names, and every address
follows the new repository name.

    n1b_docs.py --root <worktree> --phase text|urls|words|all [--dry-run] [--report <file>]
                [--json <file>]

Three phases, each committed alone (`all` is the three in order).

`text` renames what the earlier stages left in living text. A page (every tracked `.md`,
`mkdocs.yml`, `overrides/`, the docs site's data and scripts under `docs/`, the generated snippets;
the history is not read) takes these rules in this order:

  the include spelling of every header that moved (`HEADER_MAP`: one decision per header, from the
  layout), the CMake aliases of the old single library (`ac3::forge` is `iclforge::ac3`: the table
  of n1b_cmake.py), the C++ qualifiers (`ac3::oba::X` is `iclforge::oba::X`, `ac4::X` is
  `iclforge::ac4::X`: the rules of n1b_names.py), the programs and their variables (the table of
  n1b_programs.py: `ac3cli` is `forge`, `AC3GUI_X` is `ICLFORGE_GUI_X`), and the brand (the
  decisions of n1b_idents.py, and those of n1b_programs.py for the names a program owns).

The one decision a page adds is the bare word. In code (a fenced block, a code span, a `<script>`,
a `<code>`, a data file) it is the identifier `iclforge`; in prose it is the family's name, "ICL
Forge"; "AC3Forge Hearth" and "AC3Forge Crucible" are "Hearth" and "Crucible". The text of a link
that is the bare word is prose, and a link's fragment is left alone here: a heading that changes
changes its anchor, and the link pass that ends the phase gives every link that names it the new
one, in every page that has one, the history included (a link is not text of the history).

Every other file that is not C, C++ or Rust (CMake, QML, JavaScript, Python, Kotlin, workflows, the
properties files, and the committed WASM fallbacks, which copy their sources) takes the first
three rules only: S3 renamed the qualifiers in the C and C++ sources and left these comments and
strings to this stage. A Rust file takes them in its comment lines, since `ac4::` is a path there.

`urls` moves the addresses in every tracked text file but the history: the repository
(`iainchesworthlabs/ac3forge`), the Pages site (`iainchesworthlabs.github.io/ac3forge`), the
Homebrew tap (`homebrew-ac3forge`), a reference to an issue (`ac3forge#796`), the directory a
clone makes and the path a runner makes from the repository's name. It keeps the URL of a release
that exists (`releases/download/v0.10.0-beta.1/...`, `archive/refs/tags/v0.10.0-beta.1.tar.gz`, a
link into a tag), the winget manifests' directory (a path in the tree), the SonarCloud project key
(it carries no slash), and the lines the hand-written commit writes (`HAND_LINES`: the guards that
test `github.repository`, and the constant the bump script reads).

`words` is the last of the three: the words of build and tool text that still name a library by the
name it had (`ac3adm`, `ac3iab`, `ac3audio`, `libac3iab.so`) or a CMake target of the old single
library by its raw name (`forge_shared`, `forge_c_static`). S2 renamed the targets and S3 the
namespaces, and each left a comment that says the word, or half of a pair
(`iclforge_ac3_static/forge_shared`). It reads the files of the third kind above (CMake, workflows,
shell, Python, the presets), never a page, a C or C++ file (their comments and strings keep these
words, and the census lists them) or a file name (`ac3iab.hpp`, `test_ac3iab.cpp`). A line that
tells a past event in the name of its time is left (`HISTORICAL_LINES`).

The history is `CHANGELOG.md`, `planning/`, `tests/golden`, the released winget manifests,
`.git-blame-ignore-revs` and this migration's scripts; no phase reads it. Three pages narrate
a tree that was named differently (`RECORD_PAGES`) and keep their names. A page line that
describes the rename itself is left (`HAND_PAGE_LINES`). `--report` lists every decision, `--json`
writes the rules, the counts and the header table. A second run changes nothing.
"""

from __future__ import annotations

import importlib
import json
import re
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

import layoutdef
import n1b_cmake
import n1b_idents as idents
import n1b_programs as programs
from n1b_lib import CPP_EXT, Repo, base_parser

# --- which files are read -------------------------------------------------------------------------

# The history and the byte-exact trees: neither phase reads them, and the link pass touches only a
# link's fragment in the pages among them.
HISTORY_FILES = ("CHANGELOG.md", ".git-blame-ignore-revs")
HISTORY_PREFIXES = (
    "planning/",
    "tools/n1b/",
    "tests/golden/",
    "packaging/winget/manifests/",
)
# Pages that narrate the tree as it was named when the work was done: the names stay, a note at
# the head of each says how to read them, and the `urls` phase moves their addresses.
RECORD_PAGES = (
    "docs/history.md",
    "docs/crucible/design/promotion.md",
    "docs/platforms/windows-demo.md",
)
# The committed fallbacks of the docs site's WASM demos are copies of apps/wasm and js/ (docs.yml
# checks the page files byte for byte): they take the rules of the files they copy.
FALLBACK_PREFIXES = ("docs/assets/wasm-decode-demo/", "docs/assets/wasm-encode-demo/")
PAGE_SUFFIXES = (".md",)
# a page that is data or script: a bare `ac3forge` is an identifier there, never the family's name
BARE_IS_CODE_SUFFIXES = (".json", ".js", ".css", ".yml", ".txt", ".toml")
# not read at all
SKIP_SUFFIXES = (
    ".png", ".jpg", ".jpeg", ".gif", ".ico", ".icns", ".svg", ".pdf", ".wasm", ".woff", ".woff2",
    ".ttf", ".otf", ".zip", ".gz", ".tgz", ".bin", ".map", ".qm", ".ec3", ".ac3", ".ac4", ".wav",
    ".mp4", ".mkv", ".b64", ".keystore", ".jar", ".dll", ".exe", ".lib", ".so", ".a",
)  # fmt: skip


def is_history(path: str) -> bool:
    return path in HISTORY_FILES or path.startswith(HISTORY_PREFIXES)


def kind_of(path: str) -> str:
    """page | record | code | rust | cpp | skip"""
    if is_history(path) or path.endswith(SKIP_SUFFIXES):
        return "skip"
    if path in RECORD_PAGES:
        return "record"
    if path.startswith(FALLBACK_PREFIXES):
        return "code"
    if path.endswith(PAGE_SUFFIXES):
        return "page"
    if path in ("mkdocs.yml",) or path.startswith(("overrides/", "docs/", "docs-snippets/")):
        return "page"
    if "/translations/" in path and path.endswith(".ts"):
        return "cpp"  # a Qt catalogue: its sources are lupdate's
    if path.endswith(".rs"):
        return "rust"
    if path.endswith(tuple(CPP_EXT)) or path.endswith((".hpp.in", ".h.in")):
        return "cpp"  # S3 and S4 read the C and C++ sources; the urls phase reads them too
    return "code"


# --- the tables -----------------------------------------------------------------------------------

# The include spelling of every header that moved, old to new: one decision per header, from
# layoutdef.py applied to the tree before S2 (the parent of "S2: move src and tests") and S4's
# renames. test_docs.py holds each new spelling to a header of the tree.
# The tree it was derived from: `main` as it stood before "S2: move src and tests" (#1160).
HEADER_MAP_BASE = "7a7e4f5f291c44785eaf592c4128eabe5df3a551"
# HEADER_MAP-BEGIN
HEADER_MAP: dict[str, str] = {
    "ac3/admbridge/bridge.hpp": "iclforge/admbridge/bridge.hpp",
    "ac3/admbridge/coordinates.hpp": "iclforge/admbridge/coordinates.hpp",
    "ac3/admbridge/iab_bridge.hpp": "iclforge/admbridge/iab_bridge.hpp",
    "ac3/analysis/levels.hpp": "iclforge/ac3/analysis/levels.hpp",
    "ac3/audio/audio_backend.hpp": "iclforge/audio/audio_backend.hpp",
    "ac3/audio/capture.hpp": "iclforge/audio/capture.hpp",
    "ac3/audio/device_watcher.hpp": "iclforge/audio/device_watcher.hpp",
    "ac3/audio/live_positions.hpp": "iclforge/audio/live_positions.hpp",
    "ac3/audio/monitor.hpp": "iclforge/audio/monitor.hpp",
    "ac3/audio/passthrough.hpp": "iclforge/audio/passthrough.hpp",
    "ac3/audio/pcm_output.hpp": "iclforge/audio/pcm_output.hpp",
    "ac3/audio/playback_counter.hpp": "iclforge/audio/playback_counter.hpp",
    "ac3/audio/render_devices.hpp": "iclforge/audio/render_devices.hpp",
    "ac3/audio/resampler.hpp": "iclforge/audio/resampler.hpp",
    "ac3/audio/ring_buffer.hpp": "iclforge/audio/ring_buffer.hpp",
    "ac3/audio/sink_capabilities.hpp": "iclforge/audio/sink_capabilities.hpp",
    "ac3/audio/spatial.hpp": "iclforge/audio/spatial.hpp",
    "ac3/audio/speakers.hpp": "iclforge/audio/speakers.hpp",
    "ac3/audio/watchdog.hpp": "iclforge/audio/watchdog.hpp",
    "ac3/core/aht_tables.hpp": "iclforge/ac3/core/aht_tables.hpp",
    "ac3/core/bitalloc.hpp": "iclforge/ac3/core/bitalloc.hpp",
    "ac3/core/bitalloc_tables.hpp": "iclforge/ac3/core/bitalloc_tables.hpp",
    "ac3/core/bitreader.hpp": "iclforge/base/bitreader.hpp",
    "ac3/core/bitwriter.hpp": "iclforge/base/bitwriter.hpp",
    "ac3/core/coupling.hpp": "iclforge/ac3/core/coupling.hpp",
    "ac3/core/crc16.hpp": "iclforge/ac3/core/crc16.hpp",
    "ac3/core/downmix_target.hpp": "iclforge/base/downmix_target.hpp",
    "ac3/core/eac3_tables.hpp": "iclforge/ac3/core/eac3_tables.hpp",
    "ac3/core/eac3_tools.hpp": "iclforge/ac3/core/eac3_tools.hpp",
    "ac3/core/exponents.hpp": "iclforge/ac3/core/exponents.hpp",
    "ac3/core/fft.hpp": "iclforge/dsp/fft.hpp",
    "ac3/core/layout.hpp": "iclforge/base/layout.hpp",
    "ac3/core/mantissas.hpp": "iclforge/ac3/core/mantissas.hpp",
    "ac3/core/mdct.hpp": "iclforge/ac3/core/mdct.hpp",
    "ac3/core/tables.hpp": "iclforge/ac3/core/tables.hpp",
    "ac3/core/window.hpp": "iclforge/ac3/core/window.hpp",
    "ac3/decoder/decoder.hpp": "iclforge/ac3/decoder/decoder.hpp",
    "ac3/decoder/diagnostics.hpp": "iclforge/ac3/decoder/diagnostics.hpp",
    "ac3/decoder/output.hpp": "iclforge/ac3/decoder/output.hpp",
    "ac3/decoder/serving.hpp": "iclforge/ac3/decoder/serving.hpp",
    "ac3/decoder/syntax_trace.hpp": "iclforge/ac3/decoder/syntax_trace.hpp",
    "ac3/decoder/transient_prenoise.hpp": "iclforge/ac3/decoder/transient_prenoise.hpp",
    "ac3/dsp/biquad.hpp": "iclforge/dsp/biquad.hpp",
    "ac3/dsp/qmf.hpp": "iclforge/dsp/qmf.hpp",
    "ac3/dsp/resampler.hpp": "iclforge/dsp/resampler.hpp",
    "ac3/emdf/emdf.hpp": "iclforge/objects/emdf.hpp",
    "ac3/emdf/frame_layout.hpp": "iclforge/ac3/emdf/frame_layout.hpp",
    "ac3/encoder/assignment.hpp": "iclforge/ac3/encoder/assignment.hpp",
    "ac3/encoder/bandwidth.hpp": "iclforge/ac3/encoder/bandwidth.hpp",
    "ac3/encoder/eac3_frame.hpp": "iclforge/ac3/encoder/eac3_frame.hpp",
    "ac3/encoder/encoder.hpp": "iclforge/ac3/encoder/encoder.hpp",
    "ac3/encoder/plan.hpp": "iclforge/ac3/encoder/plan.hpp",
    "ac3/encoder/silent_frame.hpp": "iclforge/ac3/encoder/silent_frame.hpp",
    "ac3/encoder/transient.hpp": "iclforge/ac3/encoder/transient.hpp",
    "ac3/iec61937/iec61937.hpp": "iclforge/iec61937/iec61937.hpp",
    "ac3/internal/arch/simd.hpp": "iclforge/arithmetic/detail/simd.hpp",
    "ac3/internal/cpu/hardware_avx2.hpp": "iclforge/base/detail/hardware_avx2.hpp",
    "ac3/internal/decode_scalar.hpp": "iclforge/ac3/detail/decode_scalar.hpp",
    "ac3/internal/encode_scalar.hpp": "iclforge/ac3/detail/encode_scalar.hpp",
    "ac3/internal/fixed32.hpp": "iclforge/arithmetic/fixed32.hpp",
    "ac3/internal/profile.hpp": "iclforge/ac3/detail/profile.hpp",
    "ac3/internal/profiling.hpp": "iclforge/base/detail/profiling.hpp",
    "ac3/internal/scalar_math.hpp": "iclforge/arithmetic/scalar_math.hpp",
    "ac3/io/dec3.hpp": "iclforge/ac3/io/dec3.hpp",
    "ac3/io/elementary.hpp": "iclforge/ac3/io/elementary.hpp",
    "ac3/io/metadata_edit.hpp": "iclforge/ac3/io/metadata_edit.hpp",
    "ac3/io/object_strip.hpp": "iclforge/ac3/io/object_strip.hpp",
    "ac3/io/probe.hpp": "iclforge/ac3/io/probe.hpp",
    "ac3/io/stream_accumulator.hpp": "iclforge/ac3/io/stream_accumulator.hpp",
    "ac3/io/wav.hpp": "iclforge/ac3/io/wav.hpp",
    "ac3/latency.hpp": "iclforge/ac3/latency.hpp",
    "ac3/meta/bsi.hpp": "iclforge/ac3/meta/bsi.hpp",
    "ac3/meta/drc.hpp": "iclforge/ac3/meta/drc.hpp",
    "ac3/meta/loudness.hpp": "iclforge/ac3/meta/loudness.hpp",
    "ac3/meta/mixing.hpp": "iclforge/ac3/meta/mixing.hpp",
    "ac3/meta/qc.hpp": "iclforge/ac3/meta/qc.hpp",
    "ac3/oba/atmos.hpp": "iclforge/ac3/oba/atmos.hpp",
    "ac3/oba/joc.hpp": "iclforge/ac3/oba/joc.hpp",
    "ac3/oba/joc_domain.hpp": "iclforge/objects/joc_domain.hpp",
    "ac3/oba/joc_tables.hpp": "iclforge/ac3/oba/joc_tables.hpp",
    "ac3/oba/motion.hpp": "iclforge/objects/motion.hpp",
    "ac3/oba/oamd.hpp": "iclforge/objects/oamd.hpp",
    "ac3/oba/placement.hpp": "iclforge/objects/placement.hpp",
    "ac3/oba/scene.hpp": "iclforge/objects/scene.hpp",
    "ac3/oba/scene_osc.hpp": "iclforge/objects/scene_osc.hpp",
    "ac3/quality/distortion.hpp": "iclforge/ac3/quality/distortion.hpp",
    "ac3/quality/perceptual.hpp": "iclforge/ac3/quality/perceptual.hpp",
    "ac3/render/float_biquad.hpp": "iclforge/render/float_biquad.hpp",
    "ac3/render/identify.hpp": "iclforge/render/identify.hpp",
    "ac3/render/layout.hpp": "iclforge/render/layout.hpp",
    "ac3/render/pcm_block.hpp": "iclforge/render/pcm_block.hpp",
    "ac3/render/render.hpp": "iclforge/render/render.hpp",
    "ac3/render/routing.hpp": "iclforge/render/routing.hpp",
    "ac3/render/trim_delay.hpp": "iclforge/render/trim_delay.hpp",
    "ac3/sendspin/ac3forge_player.hpp": "iclforge/sendspin/iclforge_player.hpp",
    "ac3/sendspin/arbiter.hpp": "iclforge/sendspin/arbiter.hpp",
    "ac3/sendspin/base64.hpp": "iclforge/sendspin/base64.hpp",
    "ac3/sendspin/base64url.hpp": "iclforge/sendspin/base64url.hpp",
    "ac3/sendspin/channel.hpp": "iclforge/sendspin/channel.hpp",
    "ac3/sendspin/chunks.hpp": "iclforge/sendspin/chunks.hpp",
    "ac3/sendspin/clock_sync.hpp": "iclforge/sendspin/clock_sync.hpp",
    "ac3/sendspin/codec.hpp": "iclforge/sendspin/codec.hpp",
    "ac3/sendspin/cpace.hpp": "iclforge/sendspin/cpace.hpp",
    "ac3/sendspin/crypto.hpp": "iclforge/sendspin/crypto.hpp",
    "ac3/sendspin/dialect.hpp": "iclforge/sendspin/dialect.hpp",
    "ac3/sendspin/discovery.hpp": "iclforge/sendspin/discovery.hpp",
    "ac3/sendspin/firewall.hpp": "iclforge/sendspin/firewall.hpp",
    "ac3/sendspin/frames.hpp": "iclforge/sendspin/frames.hpp",
    "ac3/sendspin/handshake.hpp": "iclforge/sendspin/handshake.hpp",
    "ac3/sendspin/handshake_session.hpp": "iclforge/sendspin/handshake_session.hpp",
    "ac3/sendspin/json.hpp": "iclforge/sendspin/json.hpp",
    "ac3/sendspin/mdns.hpp": "iclforge/sendspin/mdns.hpp",
    "ac3/sendspin/messages.hpp": "iclforge/sendspin/messages.hpp",
    "ac3/sendspin/noise.hpp": "iclforge/sendspin/noise.hpp",
    "ac3/sendspin/pairing.hpp": "iclforge/sendspin/pairing.hpp",
    "ac3/sendspin/pairing_flow.hpp": "iclforge/sendspin/pairing_flow.hpp",
    "ac3/sendspin/pairing_messages.hpp": "iclforge/sendspin/pairing_messages.hpp",
    "ac3/sendspin/player_session.hpp": "iclforge/sendspin/player_session.hpp",
    "ac3/sendspin/server_host.hpp": "iclforge/sendspin/server_host.hpp",
    "ac3/sendspin/server_session.hpp": "iclforge/sendspin/server_session.hpp",
    "ac3/sendspin/server_store.hpp": "iclforge/sendspin/server_store.hpp",
    "ac3/sendspin/session.hpp": "iclforge/sendspin/session.hpp",
    "ac3/sendspin/session_driver.hpp": "iclforge/sendspin/session_driver.hpp",
    "ac3/sendspin/state_roles.hpp": "iclforge/sendspin/state_roles.hpp",
    "ac3/sendspin/stream_roles.hpp": "iclforge/sendspin/stream_roles.hpp",
    "ac3/sendspin/transport.hpp": "iclforge/sendspin/transport.hpp",
    "ac3/sendspin/websocket.hpp": "iclforge/sendspin/websocket.hpp",
    "ac3/signing/emdf_atmos_signer.hpp": "iclforge/signing/emdf_atmos_signer.hpp",
    "ac3/signing/signing_key.hpp": "iclforge/signing/signing_key.hpp",
    "ac3/spatial/spatial.hpp": "iclforge/render/spatial.hpp",
    "ac3/verify/bap_census.hpp": "iclforge/ac3/verify/bap_census.hpp",
    "ac3/verify/eac3_mirror.hpp": "iclforge/ac3/verify/eac3_mirror.hpp",
    "ac3/verify/eac3_selfcheck.hpp": "iclforge/ac3/verify/eac3_selfcheck.hpp",
    "ac3/verify/mirror.hpp": "iclforge/ac3/verify/mirror.hpp",
    "ac3/verify/selfcheck.hpp": "iclforge/ac3/verify/selfcheck.hpp",
    "ac3/verify/trace_export.hpp": "iclforge/ac3/verify/trace_export.hpp",
    "ac3/version.hpp": "iclforge/ac3/version.hpp",
    "ac3adm/ac3adm.hpp": "iclforge/adm/ac3adm.hpp",
    "ac3adm/model.hpp": "iclforge/adm/model.hpp",
    "ac3forge_c/ac3forge.h": "iclforge_c/iclforge.h",
    "ac3forge_c/version.h": "iclforge_c/version.h",
    "ac3iab/ac3iab.hpp": "iclforge/iab/ac3iab.hpp",
    "ac3iab/model.hpp": "iclforge/iab/model.hpp",
    "ac3iab/mxf.hpp": "iclforge/iab/mxf.hpp",
    "ac4/ac4.hpp": "iclforge/ac4/ac4.hpp",
    "ac4/detail/profiling.hpp": "iclforge/ac4core/detail/profiling.hpp",
    "ac4/detail/real.hpp": "iclforge/ac4core/detail/real.hpp",
    "ac4/syntax.hpp": "iclforge/ac4/syntax.hpp",
    "ac4dec/decoder.hpp": "iclforge/ac4dec/decoder.hpp",
    "ac4enc/encoder.hpp": "iclforge/ac4enc/encoder.hpp",
    "iamf/iamf.hpp": "iclforge/iamf/iamf.hpp",
    "matroska/matroska.hpp": "iclforge/matroska/matroska.hpp",
    "matroska/reader.hpp": "iclforge/matroska/reader.hpp",
    "mp4/dash.hpp": "iclforge/mp4/dash.hpp",
    "mp4/hls.hpp": "iclforge/mp4/hls.hpp",
    "mp4/mp4.hpp": "iclforge/mp4/mp4.hpp",
    "mp4/reader.hpp": "iclforge/mp4/reader.hpp",
    "mpegts/mpegts.hpp": "iclforge/mpegts/mpegts.hpp",
    "mpegts/reader.hpp": "iclforge/mpegts/reader.hpp",
}
# HEADER_MAP-END


def derive_header_map(root: Path, before: str) -> dict[str, str]:
    """HEADER_MAP, derived again: the include spelling of every header of the tree at `before` (a
    commit, the parent of "S2: move src and tests") and where layout L2 and the package renames of
    S4 put it, by layoutdef.py. `--header-map <commit>` prints it."""
    out = subprocess.run(
        ["git", "-C", str(root), "ls-tree", "-r", "--name-only", before],
        capture_output=True,
        check=True,
    ).stdout.decode("utf-8", "surrogateescape")
    table: dict[str, str] = {}
    for old in (f for f in out.splitlines() if f.startswith("src/")):
        spelling = layoutdef.spelling_of(old)
        if spelling:
            new = layoutdef.l2_new(old, True) or old
            table[spelling] = layoutdef.spelling_of(layoutdef.package_new(new) or new)
    return dict(sorted(table.items()))


# CMake aliases: the table of n1b_cmake.py, with the lookahead a sentence needs (`ac3::forge:`,
# `ac3::forge's`): the alias is not followed by a name character or by `::`.
_ALIAS_RX = [
    (re.compile(r"(?<![\w])(?<!::)" + re.escape(old) + r"(?![\w]|::)"), new)
    for old, new in n1b_cmake.ALIASES
]

# C++ qualifiers, the rules of n1b_names.py without the C++-only ones (a global-qualified name, and
# libadm's own `adm::`, which a comment still means as libadm's). The qualifier must be followed by
# a name, which keeps a slice (`x[::2]`) and a bare `ac3::` out of it.
_NAME = r"(?=[A-Za-z_])"
_NAMESPACE_RX = [
    (re.compile(r"(?<![\w])(?<!::)ac3iab::" + _NAME), "iclforge::iab::"),
    (re.compile(r"(?<![\w])(?<!::)ac3adm::" + _NAME), "iclforge::adm::"),
    (re.compile(r"(?<![\w])(?<!::)ac3::" + _NAME), "iclforge::"),
    (
        re.compile(r"(?<![\w])(?<!::)(ac4|mp4|mpegts|matroska|iamf)::" + _NAME),
        r"iclforge::\1::",
    ),
]
_HEADER_RX = re.compile(
    r"(?<![A-Za-z0-9_./-])("
    + "|".join(re.escape(k) for k in sorted(HEADER_MAP, key=len, reverse=True))
    + r")(?![A-Za-z0-9_])"
)


def new_header(path: str) -> str:
    return HEADER_MAP.get(path, path)


# --- the decisions --------------------------------------------------------------------------------

KEEP, RENAME = "keep", "rename"


@dataclass(frozen=True)
class Rule:
    name: str
    action: str
    group: str
    reason: str


RULES: dict[str, Rule] = {
    r.name: r
    for r in (
        Rule("header", RENAME, "code", "the include spelling of a header that moved"),
        Rule("cmake-target", RENAME, "code", "a CMake alias of the old single library"),
        Rule("namespace", RENAME, "code", "a C++ qualifier in text or in a comment"),
        Rule("program", RENAME, "name", "a program (n1b_programs.PROGRAMS)"),
        Rule("variable", RENAME, "name", "a variable of a program (AC3CLI_*)"),
        Rule("identifier", RENAME, "name", "the brand as an identifier, a file or a package name"),
        Rule("owned", RENAME, "name", "a name a program owns: package, icon, QML module, JNI name"),
        Rule("display-family", RENAME, "display", "the family's name in prose: ICL Forge"),
        Rule("display-member", RENAME, "display", "AC3Forge Hearth and AC3Forge Crucible"),
        Rule("bare-code", RENAME, "display", "the bare word in code: the identifier iclforge"),
        Rule("clone-dir", RENAME, "display", "the directory a clone of the repository makes"),
        Rule("library-word", RENAME, "code", "a library by its old name in build text: ac3adm"),
        Rule("raw-target", RENAME, "code", "a CMake target of the old library by its raw name"),
        Rule("literal", RENAME, "name", "a quoted Kconfig menu title, or the Homebrew cask's name"),
        Rule("external", KEEP, "kept", "an address: the urls phase's, or the owner's"),
        Rule(
            "old-release-asset", KEEP, "kept", "the file name of an asset of a release that exists"
        ),
        Rule("driver", KEEP, "kept", "the Windows driver's installed identity"),
        Rule("signature", KEEP, "kept", "the bytes of an example signing key"),
        Rule("past-output", KEEP, "kept", "the output of a past release, quoted"),
    )
}

# the file name of an asset of a release that exists: `ac3forge-0.10.0-win64.exe`,
# `ac3forge-dev-0.10.0-beta.1-Darwin.zip`; a placeholder (`<version>`) is the next release's
_ASSET_RIGHT = re.compile(r"^-(?:dev-|conformance-vectors-|shield-v)?\d+\.\d+\.\d+")
# `ac3forge 0.10.0-beta.1+2637`: the version line a release printed
_PAST_OUTPUT_RIGHT = re.compile(r"^ \d+\.\d+\.\d+-beta\.\d+")
NEUTRAL = "docs/_page"  # a path with no tree of its own: the page rules, wherever the file is

# A line that is a fragment of a link: a markdown link target, a reference definition, an href.
_FRAG_RX = [
    re.compile(r"(\]\([^)\s]*)#([^)\s]*)(?=[\s)])"),
    re.compile(r"""(href=["'][^"'#]*)#([^"']*)(?=["'])"""),
    re.compile(r"(^\s*\[[^\]]+\]:\s*\S*)#(\S+)"),
]
_SENTINEL = chr(0xE000)


def protect_fragments(line: str) -> tuple[str, list[str]]:
    """The line with each link fragment replaced by a sentinel, and the fragments."""
    frags: list[str] = []

    def stash(m: re.Match) -> str:
        frags.append(m.group(2))
        return f"{m.group(1)}#{_SENTINEL}{len(frags) - 1}{_SENTINEL}"

    for rx in _FRAG_RX:
        line = rx.sub(stash, line)
    return line, frags


def restore_fragments(line: str, frags: list[str]) -> str:
    if not frags:
        return line
    return re.sub(_SENTINEL + r"(\d+)" + _SENTINEL, lambda m: frags[int(m.group(1))], line)


# --- text regions ---------------------------------------------------------------------------------

_FENCE = re.compile(r"^\s*(`{3,}|~{3,})")
_RAW_OPEN = re.compile(r"<(script|style|pre)\b", re.IGNORECASE)
_RAW_CLOSE = re.compile(r"</(script|style|pre)\s*>", re.IGNORECASE)
_HTML_CODE = re.compile(r"(<code\b[^>]*>.*?</code>)", re.IGNORECASE)


def split_code(line: str) -> list[tuple[str, bool]]:
    """The line cut into (text, is_code) pieces: code spans (a run of backticks to the run that
    closes it) and `<code>` elements are code."""
    pieces: list[tuple[str, bool]] = []
    i, cursor, n = 0, 0, len(line)
    while i < n:
        if line[i] != "`":
            i += 1
            continue
        j = i
        while j < n and line[j] == "`":
            j += 1
        ticks = j - i
        k, closed = j, -1
        while k < n:
            if line[k] == "`":
                end = k
                while end < n and line[end] == "`":
                    end += 1
                if end - k == ticks:
                    closed = end
                    break
                k = end
            else:
                k += 1
        if closed < 0:
            i = j
            continue
        if cursor < i:
            pieces.append((line[cursor:i], False))
        pieces.append((line[i:closed], True))
        cursor = i = closed
    if cursor < n:
        pieces.append((line[cursor:], False))
    out: list[tuple[str, bool]] = []
    for text, is_code in pieces:
        if is_code:
            out.append((text, True))
            continue
        for part in _HTML_CODE.split(text):
            if part:
                out.append((part, bool(_HTML_CODE.fullmatch(part))))
    return out


# --- one segment ----------------------------------------------------------------------------------


def rename_brand(
    seg: str, is_code: bool, bare_code: bool, hits: list | None, counts: Counter | None
) -> str:
    """The brand `ac3forge` (any case) in one piece of text, one decision per occurrence."""
    out: list[str] = []
    cursor = 0
    for m in idents.BRAND.finditer(seg):
        s, e = m.span()
        if s < cursor:
            continue
        word = seg[s:e]
        right = seg[e:]
        if seg[:s].endswith("[") and right.startswith(("](", "][")):
            rule = "prose"  # the text of a link, not a Catch2 tag
        else:
            rule = idents.decide(NEUTRAL, seg, s, e)
        name, repl, stop = None, None, e
        if rule in ("slug", "sonar", "runner-path", "old-release"):
            if (
                rule == "runner-path"
                and (is_code or bare_code)
                and idents._CLONE_DIR_LEFT.search(seg[:s])
            ):
                name, repl = "clone-dir", idents.rename_of(word)
            else:
                name = "external"
        elif rule == "signature-key":
            name = "signature"
        elif rule in ("n1a-token", "n1a-mixed", "n1a-display"):
            if _ASSET_RIGHT.match(right):
                name = "old-release-asset"
            elif word == "AC3Forge" and _PAST_OUTPUT_RIGHT.match(right):
                name = "past-output"
            else:
                done = programs.brand_replacement(NEUTRAL, seg, s, e)
                if done is None:
                    name = "driver"
                else:
                    repl, stop = done
                    name = (
                        "display-member"
                        if word == "AC3Forge" and repl in ("Hearth", "Crucible")
                        else ("display-family" if word == "AC3Forge" else "owned")
                    )
        elif rule in ("wire", "identifier", "namespace", "path-name", "js-name"):
            if rule == "path-name" and _ASSET_RIGHT.match(right):
                name = "old-release-asset"
            else:
                repl = idents.rename_of(word)
                if word == "AC3FORGE" and idents._is_c_library_prefix(seg, e):
                    repl, stop = "ICLFORGE_C", e + 1
                name = "identifier"
        elif rule == "prose":
            if word == "ac3forge" and _PAST_OUTPUT_RIGHT.match(right) and is_code:
                name = "past-output"
            elif word == "ac3forge":
                if is_code or bare_code:
                    name, repl = "bare-code", "iclforge"
                else:
                    name, repl = "display-family", "ICL Forge"
            else:  # a bare AC3FORGE: the identifier in code, the family's name in capitals in prose
                name, repl = (
                    ("bare-code", "ICLFORGE")
                    if (is_code or bare_code)
                    else (
                        "display-family",
                        "ICL FORGE",
                    )
                )
        else:
            name = "external"
        if hits is not None:
            hits.append((name, seg[s:stop], repl if repl is not None else seg[s:stop]))
        if counts is not None:
            counts[name] += 1
        if repl is None:
            continue
        out.append(seg[cursor:s])
        out.append(repl)
        cursor = stop
    out.append(seg[cursor:])
    return "".join(out)


def _sub_logged(
    rx: re.Pattern, to, text: str, name: str, hits: list | None, counts: Counter | None
) -> str:
    def sub(m: re.Match) -> str:
        new = m.expand(to) if isinstance(to, str) else to(m)
        if hits is not None:
            hits.append((name, m.group(0), new))
        if counts is not None:
            counts[name] += 1
        return new

    return rx.sub(sub, text)


# Names a rule of the brand cannot tell from a program or from prose, by what stands beside them:
# the titles of the Kconfig menus the pages quote (menuconfig shows `iclforge hearth sink`), and
# the Homebrew cask, whose name is the family's and not the GUI's (`brew install --cask tap/name`).
_PAGE_LITERALS: tuple[tuple[re.Pattern, str], ...] = (
    (re.compile(r"\bac3forge (hearth sink|I2S player)\b"), r"iclforge \1"),
    (re.compile(r"(?<![\w/])(iainchesworthlabs/ac3forge/)ac3gui(?![\w-])"), r"\1iclforge"),
    (re.compile(r"(--cask\s+)ac3gui(?![\w-])"), r"\1iclforge"),
)


def _sub_each(
    table: tuple[tuple[re.Pattern, str], ...],
    text: str,
    name: str,
    hits: list | None,
    counts: Counter | None,
) -> str:
    for rx, to in table:
        text = _sub_logged(rx, to, text, name, hits, counts)
    return text


def rewrite_segment(
    seg: str, is_code: bool, bare_code: bool, hits: list | None, counts: Counter | None
) -> str:
    """Every rule of the text phase on one piece of text, in the order the module says."""
    t = _sub_logged(_HEADER_RX, lambda m: new_header(m.group(1)), seg, "header", hits, counts)
    for rx, new in _ALIAS_RX:
        t = _sub_logged(rx, new, t, "cmake-target", hits, counts)
    for rx, new in _NAMESPACE_RX:
        t = _sub_logged(rx, new, t, "namespace", hits, counts)
    families: Counter = Counter()
    t2 = idents.prefix_families(t, families)
    if counts is not None:
        counts["identifier"] += sum(families.values())
    t = rename_brand(t2, is_code, bare_code, hits, counts)
    used: Counter = Counter()
    sub: list = []
    t2 = programs.rename_programs(t, used, sub)
    if counts is not None:
        counts["variable"] += used.pop("variable", 0)
        counts["program"] += sum(used.values())
    if hits is not None:
        hits.extend(
            ("variable" if fam == "variable" else "program", old, new)
            for _kind, fam, old, new in sub
        )
    return t2


# the article before a name that begins with a consonant now: "an `ac3cli` run" is "a `forge` run"
_AN_RX = re.compile(
    r"\b([Aa]n)(\s+[`*_\"'(\[]*)"
    r"(forge|forge-gui|hearth|hearth-\w+|crucible|crucible-run|shield|Hearth|Crucible)(?![A-Za-z0-9_])"
)


def fix_articles(before: str, after: str) -> str:
    if before == after:
        return after
    return _AN_RX.sub(
        lambda m: ("A" if m.group(1) == "An" else "a") + m.group(2) + m.group(3), after
    )


# A page line that describes the rename itself: the rules would turn "`ac3cli` becomes `forge`" into
# "`forge` becomes `forge`", so the line is left for the hand-written commit, which says what was
# done and not what was going to be.
HAND_PAGE_LINES: dict[str, tuple[str, ...]] = {
    "ROADMAP.md": ("N1 has not run. N1A renames the programs",),
}


def transform_page(
    path: str, text: str, hits: list | None = None, counts: Counter | None = None
) -> str:
    """A page's text with the rules of the text phase carried out."""
    bare_code = path.endswith(BARE_IS_CODE_SUFFIXES)
    hand = HAND_PAGE_LINES.get(path, ())
    out: list[str] = []
    fence: str | None = None
    raw: str | None = None
    for number, line in enumerate(text.split("\n"), 1):
        if any(fragment in line for fragment in hand):
            if counts is not None:
                counts["hand"] += 1
            if hits is not None:
                hits.append(("hand", path, number, line.strip()[:120], ""))
            out.append(line)
            continue
        m = _FENCE.match(line)
        if fence is not None:
            if m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence):
                fence = None
                out.append(line)
            else:
                out.append(page_line(line, True, bare_code, hits, counts, path, number))
            continue
        if m:
            # the fence line: its info string names a language, not a name to rename
            fence = m.group(1)
            out.append(line)
            continue
        in_block = raw is not None
        if raw is None:
            opened = _RAW_OPEN.search(line)
            if opened and not _RAW_CLOSE.search(line, opened.end()):
                raw = opened.group(1).lower()
        elif _RAW_CLOSE.search(line):
            raw = None
        out.append(page_line(line, in_block, bare_code, hits, counts, path, number))
    return "\n".join(out)


def page_line(
    line: str,
    in_block: bool,
    bare_code: bool,
    hits: list | None,
    counts: Counter | None,
    path: str,
    number: int,
) -> str:
    safe, frags = protect_fragments(line)
    line_hits: list = []
    safe = _sub_each(
        _PAGE_LITERALS, safe, "literal", line_hits if hits is not None else None, counts
    )
    pieces = [(safe, True)] if in_block else split_code(safe)
    done = "".join(
        rewrite_segment(seg, is_code, bare_code, line_hits if hits is not None else None, counts)
        for seg, is_code in pieces
    )
    done = fix_articles(safe, done)
    if hits is not None:
        hits.extend((name, path, number, old, new) for name, old, new in line_hits)
    return restore_fragments(done, frags)


def transform_code(
    path: str, text: str, hits: list | None = None, counts: Counter | None = None
) -> str:
    """A file that is not a page, not C or C++: the header spellings, the CMake aliases and the
    C++ qualifiers its comments and strings name. A Rust file takes them in its comment lines."""
    rust = path.endswith(".rs")
    out: list[str] = []
    for number, line in enumerate(text.split("\n"), 1):
        if rust and not line.lstrip().startswith("//"):
            out.append(line)
            continue
        new, n = _HEADER_RX.subn(lambda m: new_header(m.group(1)), line)
        if n and counts is not None:
            counts["header"] += n
        for rx, to in _ALIAS_RX:
            new, n = rx.subn(to, new)
            if n and counts is not None:
                counts["cmake-target"] += n
        for rx, to in _NAMESPACE_RX:
            new, n = rx.subn(to, new)
            if n and counts is not None:
                counts["namespace"] += n
        if new != line and hits is not None:
            hits.append(("code", path, number, line.strip()[:100], new.strip()[:100]))
        out.append(new)
    return "\n".join(out)


# --- literals the rules cannot say ----------------------------------------------------------------

# A fragment that is one decision by itself, by path, applied to a page before the rules.
LITERALS: tuple[tuple[str, str, str], ...] = (
    ("mkdocs.yml", "site_name: ac3forge", "site_name: ICL Forge"),
)


def apply_literals(path: str, text: str) -> str:
    for p, old, new in LITERALS:
        if path == p and old in text:
            text = text.replace(old, new)
    return text


# --- anchors: a heading that changes changes its slug, and the links that name it follow ----------


def _check_doc_paths():
    """tools/checks/check_doc_paths.py, which owns the slug rules the site and GitHub apply."""
    here = str(Path(__file__).resolve().parents[1] / "checks")
    if here not in sys.path:
        sys.path.insert(0, here)
    return importlib.import_module("check_doc_paths")


def heading_slugs(text: str) -> list[tuple[str, str]]:
    """The (mkdocs, github) slug of each heading of a page, in order: the two slugifiers of
    check_doc_paths.py, which is the reader of these links."""
    cdp = _check_doc_paths()
    lines = text.replace("\r\n", "\n").split("\n")
    used: set[str] = set()
    counts: dict[str, int] = {}
    out: list[tuple[str, str]] = []
    for raw in cdp.raw_headings(lines):
        heading = raw
        attrs = cdp.ATTR_LIST_TAIL.search(raw)
        if attrs and attrs.group(1).strip():
            explicit = [t[1:] for t in attrs.group(1).split() if t.startswith("#")]
            heading = raw[: attrs.start()]
            if explicit:
                used.add(explicit[-1])
                out.append((explicit[-1], explicit[-1]))
                continue
        plain = cdp.heading_text(heading)
        out.append(
            (
                cdp.unique_mkdocs(cdp.slug_mkdocs(plain), used),
                cdp.unique_github(cdp.slug_github(plain), counts),
            )
        )
    return out


def anchor_map(before: str, after: str) -> dict[str, str]:
    """old slug to new slug for the headings that changed; nothing when the headings did not."""
    b, a = heading_slugs(before), heading_slugs(after)
    if len(b) != len(a):
        return {}
    changed: dict[str, str] = {}
    for (bm, bg), (am, ag) in zip(b, a, strict=True):
        if bm != am:
            changed[bm] = am
        if bg != ag:
            changed[bg] = ag
    return changed


_REPO_URL = r"https://github\.com/iainchesworthlabs/ac3forge/(?:blob|tree)/main/"
_SITE_URL = r"https://iainchesworthlabs\.github\.io/ac3forge/"
# the pages that include another file as a snippet answer to its anchors too
SNIPPET_PAGES = {
    "docs/roadmap.md": "ROADMAP.md",
    "docs/contributing.md": "CONTRIBUTING.md",
    "docs/security.md": "SECURITY.md",
}


def link_pages(src: str, target: str) -> list[str]:
    """The tracked files a link target can name: one, or two for a route of the site (a page is
    `docs/<route>.md` or `docs/<route>/index.md`)."""
    if not target:
        return [src]
    m = re.match(_REPO_URL + r"(.+)$", target)
    if m:
        return [m.group(1)]
    m = re.match(_SITE_URL + r"(.*)$", target)
    if m:
        route = m.group(1).strip("/")
        return ["docs/index.md"] if not route else [f"docs/{route}.md", f"docs/{route}/index.md"]
    if re.match(r"^[a-z][a-z0-9+.-]*:", target) or target.startswith("//"):
        return []
    base = src.rsplit("/", 1)[0] if "/" in src else ""
    parts: list[str] = [p for p in base.split("/") if p]
    for p in target.split("/"):
        if p == "..":
            if parts:
                parts.pop()
        elif p and p != ".":
            parts.append(p)
    return ["/".join(parts)]


def remap_links(src: str, text: str, maps: dict[str, dict[str, str]]) -> str:
    """The fragments of the links of `text` that name a heading that changed."""

    def target_of(prefix: str) -> str:
        m = re.search(r"""(?:\]\(|href=["']|\]:\s*)([^)\s"']*)$""", prefix)
        return m.group(1) if m else ""

    def fix(m: re.Match) -> str:
        prefix, frag = m.group(1), m.group(2)
        for page in link_pages(src, target_of(prefix)):
            amap = maps.get(page)
            if amap is None and page in SNIPPET_PAGES:
                amap = maps.get(SNIPPET_PAGES[page])
            if amap and frag in amap:
                return f"{prefix}#{amap[frag]}"
        return m.group(0)

    for rx in _FRAG_RX:
        text = rx.sub(fix, text)
    return text


# --- the addresses --------------------------------------------------------------------------------

# A line the hand-written commit writes: the guards that test the repository's name (they become a
# test of the owner, so that the job is not skipped in the window between the rename and an edit),
# and the constant the bump script builds its URLs from (it is read from the environment).
HAND_LINES: dict[str, tuple[str, ...]] = {
    ".github/workflows/dependabot-auto-merge.yml": (
        "github.repository == 'iainchesworthlabs/ac3forge'",
    ),
    ".github/workflows/sonarcloud.yml": ("github.repository == 'iainchesworthlabs/ac3forge'",),
    "tools/release/bump_manifests.py": ('REPO = "iainchesworthlabs/ac3forge"',),
}

# What follows the repository's name in the URL of a release that exists: it stays as published.
_OLD_RELEASE_AFTER = re.compile(
    r"^/(?:releases/(?:download|tag)/v\d|archive/(?:refs/tags/)?v\d|blob/v\d|tree/v\d|compare/v\d)"
)
_WINGET_LEFT = re.compile(r"manifests/i/$")
_SLUG_RX = re.compile(r"(?<![\w.-])iainchesworthlabs/ac3forge(?![\w-])")
_SITE_RX = re.compile(r"iainchesworthlabs\.github\.io/ac3forge(?![\w-])")
_TAP_RX = re.compile(r"homebrew-ac3forge(?![\w-])")
_ISSUE_RX = re.compile(r"(?<![\w/.#-])ac3forge#(\d+)")
_RUNNER_RX = re.compile(r"(/__w|/home/runner/work)/ac3forge/ac3forge(?![\w-])")
_CLONE_RX = re.compile(r"(git clone \S*iainchesworthlabs/\S+\s*&&\s*cd )ac3forge(?![\w-])")


def transform_urls(
    path: str, text: str, hits: list | None = None, counts: Counter | None = None
) -> str:
    out: list[str] = []
    hand = HAND_LINES.get(path, ())
    for number, line in enumerate(text.split("\n"), 1):
        if any(fragment in line for fragment in hand):
            if counts is not None:
                counts["hand"] += 1
            if hits is not None:
                hits.append(("hand", path, number, line.strip()[:120], ""))
            out.append(line)
            continue
        new = line

        def slug(m: re.Match, line: str = line) -> str:
            after = line[m.end() :]
            if _OLD_RELEASE_AFTER.match(after) or _WINGET_LEFT.search(line[: m.start()]):
                if counts is not None:
                    counts["kept-old-release"] += 1
                return m.group(0)
            if counts is not None:
                counts["repository"] += 1
            return "iainchesworthlabs/iclforge"

        new = _SLUG_RX.sub(slug, new)
        for name, rx, to in (
            ("site", _SITE_RX, "iainchesworthlabs.github.io/iclforge"),
            ("tap", _TAP_RX, "homebrew-iclforge"),
            ("issue", _ISSUE_RX, r"iclforge#\1"),
            ("runner", _RUNNER_RX, r"\1/<repo>/<repo>"),
            ("clone", _CLONE_RX, r"\1iclforge"),
        ):
            new, n = rx.subn(to, new)
            if n and counts is not None:
                counts[name] += n
        if new != line and hits is not None:
            hits.append(("url", path, number, line.strip()[:120], new.strip()[:120]))
        out.append(new)
    return "\n".join(out)


# --- the words of build text ----------------------------------------------------------------------

# A line that tells a past event in the name of its time, by file: a fragment of the line. The
# library was called `ac3iab` when the allowlist file `libac3iab.so.txt` went missing, and the
# measurements of the coverage report are listed by the names the components had then.
HISTORICAL_LINES: dict[str, tuple[str, ...]] = {
    ".github/workflows/_ci-core.yml": (
        "let libac3iab.so go uncovered",
        "from the day ac3iab landed until this fix",
    ),
    "tools/ci/check_abi_symbols.py": ("libac3iab.so.txt was",),
    "tools/checks/coverage_report.sh": (
        "ac3adm 87.9/82.4",
        "ac3adm 87.2/81.5",
        "ac3iab 95.4/92.9",
    ),
}

_LIBRARIES = "admbridge|adm|iab|audio|signing|sendspin|arithmetic"
# The libraries that had a name of their own. A word is not part of a longer one, of a path
# (`iclforge/adm/ac3adm.hpp`: that is the header's file name) or of a qualified name, and it is
# not followed by an extension (`test_ac3iab.cpp`).
_WORDS: tuple[tuple[str, re.Pattern, str], ...] = (
    (
        "library-word",
        re.compile(r"(?<![\w.:-])libac3(" + _LIBRARIES + r")(?![A-Za-z0-9])"),
        r"libiclforge_\1",
    ),
    (
        "library-word",
        re.compile(r"(?<![\w/.:-])ac3(" + _LIBRARIES + r")(?=\.so\b)"),
        r"libiclforge_\1",
    ),
    (
        "library-word",
        re.compile(r"(?<![\w/.:-])ac3(" + _LIBRARIES + r")(?![\w]|\.(?:hpp|cpp|h)\b)"),
        r"iclforge::\1",
    ),
    ("raw-target", re.compile(r"(?<![\w:])forge_c_(static|shared)(?!\w)"), r"iclforge_c_\1"),
    ("raw-target", re.compile(r"(?<![\w:])forge_c(?!\w)"), "iclforge_c"),
    (
        "raw-target",
        re.compile(r"(?<![\w:])forge_(static|shared|objects|minimal)(?!\w)"),
        r"iclforge_ac3_\1",
    ),
    ("raw-target", re.compile(r"(NAMESPACE\s+)ac3::"), r"\1iclforge::"),
)


def transform_words(
    path: str, text: str, hits: list | None = None, counts: Counter | None = None
) -> str:
    out: list[str] = []
    past = HISTORICAL_LINES.get(path, ())
    for number, line in enumerate(text.split("\n"), 1):
        if any(fragment in line for fragment in past):
            out.append(line)
            continue
        new = line
        for name, rx, to in _WORDS:
            new, n = rx.subn(to, new)
            if n and counts is not None:
                counts[name] += n
        if new != line and hits is not None:
            hits.append(("words", path, number, line.strip()[:120], new.strip()[:120]))
        out.append(new)
    return "\n".join(out)


# --- the run --------------------------------------------------------------------------------------


def read_text(root: Path, rel: str) -> str | None:
    try:
        data = (root / rel).read_bytes()
    except OSError:
        return None
    if b"\0" in data[:8192]:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def text_phase(repo: Repo, root: Path) -> tuple[dict[str, str], dict[str, str], list, Counter]:
    """(old texts, new texts, hits, counts) of every file the text phase and the link pass change
    or read; the new text of a file the rules leave alone is the old."""
    old: dict[str, str] = {}
    new: dict[str, str] = {}
    hits: list = []
    counts: Counter = Counter()
    for f in repo.files:
        kind = kind_of(f)
        if kind in ("skip", "cpp") and not (f.endswith(".md") and is_history(f)):
            continue
        text = read_text(root, f)
        if text is None:
            continue
        old[f] = text
        if kind == "page":
            new[f] = transform_page(f, apply_literals(f, text), hits, counts)
        elif kind in ("code", "rust"):
            new[f] = transform_code(f, text, hits, counts)
        else:
            new[f] = text  # a record, or the history: read for its headings and its links only
    # a heading that changed changes its anchor: every link that names it follows
    maps = {f: m for f in old if f.endswith(".md") and (m := anchor_map(old[f], new[f]))}
    n_links = 0
    for f in old:
        if not (f.endswith(".md") or f == "mkdocs.yml" or f.startswith("overrides/")):
            continue
        fixed = remap_links(f, new[f], maps)
        if fixed != new[f]:
            n_links += 1
            new[f] = fixed
    counts["anchor-pages"] = len(maps)
    counts["link-pages"] = n_links
    return old, new, hits, counts


def url_phase(repo: Repo, root: Path) -> tuple[dict[str, str], dict[str, str], list, Counter]:
    old: dict[str, str] = {}
    new: dict[str, str] = {}
    hits: list = []
    counts: Counter = Counter()
    for f in repo.files:
        if kind_of(f) == "skip":
            continue
        text = read_text(root, f)
        if text is None:
            continue
        old[f] = text
        new[f] = transform_urls(f, text, hits, counts)
    return old, new, hits, counts


def words_phase(repo: Repo, root: Path) -> tuple[dict[str, str], dict[str, str], list, Counter]:
    old: dict[str, str] = {}
    new: dict[str, str] = {}
    hits: list = []
    counts: Counter = Counter()
    for f in repo.files:
        if kind_of(f) != "code" or f.startswith(FALLBACK_PREFIXES):
            continue
        text = read_text(root, f)
        if text is None:
            continue
        old[f] = text
        new[f] = transform_words(f, text, hits, counts)
    return old, new, hits, counts


# --- what is left ---------------------------------------------------------------------------------

# Why a file may still carry an old name, by prefix: the first that matches. A file that matches
# none is "other", which is what a stage's review reads.
WHY: tuple[tuple[str, str], ...] = (
    ("CHANGELOG.md", "history"),
    ("planning/", "history"),
    (".git-blame-ignore-revs", "history"),
    ("tests/golden/", "byte-exact"),
    ("packaging/winget/manifests/", "byte-exact"),
    ("tools/n1b/", "this migration's scripts"),
    ("docs/history.md", "a page that narrates the old names"),
    ("docs/crucible/design/promotion.md", "a page that narrates the old names"),
    ("docs/platforms/windows-demo.md", "a page that narrates the old names"),
    ("apps/windows/driver", "the Windows driver's installed identity"),
    ("apps/gui/settings_migration.", "the old settings store, read on purpose"),
    ("tests/gui/test_settings_migration.cpp", "the old settings store, read on purpose"),
)

# the old names, by family; each pattern finds a place
FAMILIES: tuple[tuple[str, re.Pattern], ...] = (
    ("address", re.compile(r"iainchesworthlabs[/.]\S*?ac3forge|homebrew-ac3forge|ac3forge#\d+")),
    ("sonar key", re.compile(r"iainchesworthlabs_ac3forge")),
    ("program", programs._PROGRAM_RX),
    ("variable", programs._VAR_RX),
    ("brand", idents.BRAND),
    (
        "qualifier",
        re.compile(r"(?<![\w:])(?:ac3|ac4|mp4|mpegts|matroska|iamf|ac3iab|ac3adm)::" + _NAME),
    ),
    ("cmake target", re.compile(r"ac3::(?:forge|audio|sendspin|signing|admbridge|arithmetic)\b")),
    (
        "library name",
        re.compile(
            r"(?<![\w/.-])(?:ac3adm|ac3iab|ac3signing|ac3audio|ac3sendspin|forge_objects|"
            r"forge_static|forge_shared|forge_minimal)(?![\w])"
        ),
    ),
)


def why_of(path: str) -> str:
    for prefix, why in WHY:
        if path == prefix or path.startswith(prefix):
            return why
    return "other"


def residual(root: Path, listing: str | None = None) -> list[str]:
    """The old names still in the tracked text files, by family and by why the file has them; with
    `listing` (a family), every place of it that is in a file of no listed reason."""
    places: dict[tuple[str, str], Counter] = {}
    lines: list[str] = []
    for f in Repo(str(root)).files:
        if f.endswith(SKIP_SUFFIXES):
            continue
        text = read_text(root, f)
        if text is None:
            continue
        why = why_of(f)
        for number, line in enumerate(text.split("\n"), 1):
            for family, rx in FAMILIES:
                for m in rx.finditer(line):
                    places.setdefault((family, why), Counter())[f] += 1
                    if listing == family and why == "other":
                        lines.append(f"{f}:{number}: [{m.group(0)}] {line.strip()[:140]}")
    if listing:
        return lines
    rows = [f"{'family':14s} {'why':42s} {'places':>7s} {'files':>6s}"]
    for (family, why), per_file in sorted(places.items()):
        rows.append(f"{family:14s} {why:42s} {sum(per_file.values()):7d} {len(per_file):6d}")
    return rows


def run(
    root: Path, phase: str = "all", dry_run: bool = False
) -> tuple[dict[str, list[str]], dict[str, dict[str, int]], list[str]]:
    """The phases, in order. Returns the files each changed, the counts of each and the report."""
    changed: dict[str, list[str]] = {}
    totals: dict[str, dict[str, int]] = {}
    report: list[str] = []
    for name, step in (("text", text_phase), ("urls", url_phase), ("words", words_phase)):
        if phase not in (name, "all"):
            continue
        # the files are read from the disk each time: the text phase has written its own
        old, new, hits, counts = step(Repo(str(root)), root)
        changed[name] = [f for f in new if new[f] != old.get(f)]
        if not dry_run:
            for f in changed[name]:
                (root / f).write_bytes(new[f].encode("utf-8"))
        totals[name] = dict(counts)
        report.extend(name + "\t" + "\t".join(str(x) for x in h) for h in hits)
    return changed, totals, report


def main() -> int:
    ap = base_parser(__doc__)
    ap.add_argument("--phase", choices=["text", "urls", "words", "all"], default="all")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--report", default=None, help="write every decision to this file")
    ap.add_argument("--json", default=None, help="write the header table and the counts here")
    ap.add_argument(
        "--header-map",
        default=None,
        metavar="COMMIT",
        help="print the header table as derived from the tree at COMMIT, and stop",
    )
    ap.add_argument(
        "--residual",
        nargs="?",
        const="*",
        default=None,
        metavar="FAMILY",
        help="print the old names still in the tree by family and by why, or the places of FAMILY "
        "in files that have no reason to keep them, and stop",
    )
    a = ap.parse_args()
    if a.residual:
        sys.stdout.reconfigure(encoding="utf-8")
        listing = None if a.residual == "*" else a.residual
        print(chr(10).join(residual(Path(a.root), listing)))
        return 0
    if a.header_map:
        print(json.dumps(derive_header_map(Path(a.root), a.header_map), indent=1))
        return 0
    changed, totals, report = run(Path(a.root), a.phase, a.dry_run)
    for name, files in changed.items():
        print(f"{name}: {'would change' if a.dry_run else 'changed'} {len(files)} files")
        for what, n in sorted(totals[name].items()):
            print(f"  {what:22s} {n:6d}")
    if a.report:
        Path(a.report).write_text("\n".join(report) + "\n", encoding="utf-8", newline="\n")
    if a.json:
        Path(a.json).write_text(
            json.dumps(
                {
                    "rules": {k: [v.action, v.group, v.reason] for k, v in RULES.items()},
                    "counts": totals,
                    "header_map": HEADER_MAP,
                },
                indent=1,
            ),
            encoding="utf-8",
            newline="\n",
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
