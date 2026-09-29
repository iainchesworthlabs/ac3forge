"""Unit tests for cuts.py, the seven declaration moves of stage S1, on a miniature of the tree.

stdlib `unittest`. Each test builds a small git repository holding just the lines the cuts read
and write (a real one is needed: C7 uses `git mv` and `git grep`), runs the cuts over it and looks
at the text: the block is in its new header in the right namespace, the old header keeps a
using-declaration where the old spelling was used, each include is where the sort order puts it,
no rewrite leaves two blank lines, docs and planning text are not touched, and a second run
changes nothing. The miniature is written here, not copied from the tree, so that the tests keep
passing once the cuts have been made in the real one.
"""

import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import cuts

INC = "src/forge/include/ac3"
SRC = "src/forge/src"


def lines(*parts: str) -> str:
    return "\n".join(parts) + "\n"


FILES = {
    f"{INC}/core/eac3_tables.hpp": """#pragma once

#include <array>
#include <cstdint>

#include "ac3/core/exponents.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

namespace ac3::eac3::chanmap {

inline constexpr std::uint16_t kPairs = 0x1;

// One speaker feed. A pair location expands to two adjacent enumerators.
enum class Location : std::uint8_t {
    kLeft,
    kRight,  // a comment with a brace }
};

// Sixteen locations.
inline constexpr int kMaxChannels = 22;

[[nodiscard]] constexpr std::string_view name(Location location) {
    return "L";
}

// A map's locations in coded order.
struct Layout {
    std::array<Location, kMaxChannels> items{};
    int count = 0;
};

[[nodiscard]] constexpr Layout expand(std::uint16_t map) {
    Layout out;
    return out;
}

}  // namespace ac3::eac3::chanmap
""",
    f"{INC}/decoder/output.hpp": """#pragma once

#include <cstdint>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

namespace ac3 {

// Which fold to produce.
enum class DownmixTarget : std::uint8_t {
    kAsCoded,
    kLoRo,  // a fold
};

struct OutputConfig {};

}  // namespace ac3
""",
    f"{INC}/decoder/decoder.hpp": """#pragma once

#include <span>

#include "ac3/meta/mixing.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/verify/mirror.hpp"

namespace ac3 {

struct DecoderConfig {
    void* diagnostics_context = nullptr;
};

// One block of a decoded programme's PCM.
struct PcmBlock {
    int index = 0;
    std::span<const int> object_indices;
    const oba::DecodedProgram* object_metadata = nullptr;
};

// A caller's receiver for PcmBlocks.
class BlockSink {
   public:
    template <typename F>
    BlockSink(F&& f) noexcept : object_(nullptr), call_([](void* o, const PcmBlock& b) {}) {}

   private:
    void* object_;
    void (*call_)(void*, const PcmBlock&);
};

struct DecodedFrame {};

}  // namespace ac3
""",
    f"{INC}/oba/joc.hpp": """#pragma once

#include <cstdint>

#include "ac3/dsp/qmf.hpp"
#include "ac3/export.hpp"
#include "ac3/oba/joc_tables.hpp"

namespace ac3::oba::joc {

// --- Audio reconstruction ---

// Which domain reconstruct() applies the matrix in.
enum class Domain : std::uint8_t {
    kMdctBand,
    kQmf,
};

// How far the reconstruction lags the downmix it was given, in samples.
[[nodiscard]] constexpr int reconstruction_delay(Domain domain) {
    return domain == Domain::kQmf ? dsp::kQmfDelay : 256;
}

struct Other {};

}  // namespace ac3::oba::joc
""",
    f"{INC}/oba/atmos.hpp": """#pragma once

#include "ac3/export.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/spatial/spatial.hpp"

namespace ac3::oba {

struct AtmosConfig {
    int numblkscod = 3;
};

// One object's placement for one frame.
struct ObjectPlacement {
    Position position{};
    double gain = 1.0;
};

// Selects AtmosEncoder's other constructor.
struct BedTag {};

}  // namespace ac3::oba
""",
    f"{INC}/render/layout.hpp": """#pragma once

#include "ac3/core/eac3_tables.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/spatial/spatial.hpp"

namespace ac3::render {

using Loc = ac3::eac3::chanmap::Location;
inline ac3::DownmixTarget fold();

}  // namespace ac3::render
""",
    f"{INC}/render/render.hpp": """#pragma once

#include "ac3/core/eac3_tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"

namespace ac3::render {

void render(const ac3::PcmBlock& block, const ac3::eac3::chanmap::Layout& layout);

}  // namespace ac3::render
""",
    f"{INC}/render/serving.hpp": """#pragma once

#include "ac3/decoder/decoder.hpp"
#include "ac3/render/layout.hpp"

// A stereo or mono room is the decoder's fold. This is the
// ESP32 player's policy, moved beside the renderer so that every player that
// renders makes the same decision.

namespace ac3::render {}
""",
    f"{INC}/spatial/spatial.hpp": lines(
        "#pragma once",
        "",
        '#include "ac3/core/eac3_tables.hpp"',
        '#include "ac3/export.hpp"',
        "",
        "namespace ac3::spatial {",
        "Direction direction_of(eac3::chanmap::Location location);",
        "}",
    ),
    f"{SRC}/spatial/spatial.cpp": lines(
        '#include "ac3/spatial/spatial.hpp"',
        "",
        "#include <span>",
        "",
        '#include "ac3/core/eac3_tables.hpp"',
        "",
        "constexpr auto kMaxRing = static_cast<std::size_t>(eac3::chanmap::kMaxChannels);",
    ),
    "src/audio/include/ac3/audio/speakers.hpp": lines(
        "#pragma once",
        "",
        "#include <string>",
        "",
        '#include "ac3/core/eac3_tables.hpp"',
        "",
        "// vocabulary from chanmap::name(), which names the locations",
        "using Location = ac3::eac3::chanmap::Location;",
    ),
    "src/audio/src/speakers.cpp": lines(
        "#include <string>",
        "",
        '#include "ac3/core/eac3_tables.hpp"',
        "",
        "namespace ac3::audio {",
        "",
        "namespace {",
        "",
        "namespace chanmap = ac3::eac3::chanmap;",
        "",
        "struct Position {};",
        "",
        "void f(Location l) { auto s = chanmap::name(l); }",
        "",
        "}  // namespace",
        "}  // namespace ac3::audio",
    ),
    f"{INC}/oba/scene.hpp": lines(
        "#pragma once",
        "",
        '#include "ac3/export.hpp"',
        '#include "ac3/oba/atmos.hpp"',
        '#include "ac3/oba/oamd.hpp"',
    ),
    f"{INC}/oba/scene_osc.hpp": lines(
        "#pragma once", "", '#include "ac3/export.hpp"', '#include "ac3/oba/atmos.hpp"'
    ),
    f"{INC}/oba/motion.hpp": lines(
        "#pragma once",
        "",
        '#include "ac3/export.hpp"',
        '#include "ac3/oba/atmos.hpp"',
        '#include "ac3/oba/oamd.hpp"',
    ),
    f"{SRC}/oba/scene.cpp": lines(
        '#include "ac3/oba/scene.hpp"',
        "",
        "#include <vector>",
        "",
        '#include "ac3/oba/atmos.hpp"',
        '#include "ac3/oba/oamd.hpp"',
        '#include "scene_text.hpp"',
    ),
    f"{SRC}/oba/scene_osc.cpp": lines(
        '#include "ac3/oba/scene_osc.hpp"',
        "",
        "#include <vector>",
        "",
        '#include "ac3/oba/atmos.hpp"',
        '#include "ac3/oba/oamd.hpp"',
    ),
    f"{SRC}/oba/motion.cpp": lines(
        '#include "ac3/oba/motion.hpp"',
        "",
        "#include <cmath>",
        '#include "ac3/oba/atmos.hpp"',
        "#include <span>",
    ),
    f"{SRC}/iec61937/iec61937.cpp": """#include "ac3/iec61937/iec61937.hpp"

#include <array>
#include <cstddef>
#include <vector>

#include "ac3/core/eac3_tables.hpp"

namespace ac3::iec61937 {

namespace {

void put_word_le(std::vector<std::byte>& out, std::uint16_t word) {}

}  // namespace

int packer(int numblkscod) {
    return eac3::blocks_per_syncframe(numblkscod);
}

}  // namespace ac3::iec61937
""",
    "esp-idf/ac3forge/src/player.cpp": lines(
        '#include "ac3/decoder/decoder.hpp"',
        '#include "ac3/io/stream_accumulator.hpp"',
        '#include "ac3/render/render.hpp"',
        '#include "ac3/render/serving.hpp"',
        "",
        "// (ac3/render/serving.hpp) policy",
    ),
    "docs/library/header-map.md": "| `ac3/render/serving.hpp` | `serve` |\n",
    "planning/layout.md": "record: ac3/render/serving.hpp -> ac3/decoder/decoder.hpp\n",
    "CHANGELOG.md": "- moved ac3/render/serving.hpp\n",
}


def git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True, check=True
    ).stdout


class Cuts(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
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
        return (self.root / rel).read_bytes().decode("utf-8")

    def run_all(self) -> None:
        for name, cut in cuts.CUTS.items():
            cut(self.root)
            self.assertIn(name, cuts.CUTS)

    def test_c1_moves_the_speaker_vocabulary_and_leaves_using_declarations(self) -> None:
        cuts.cut_c1(self.root)
        header = self.text(f"{INC}/core/layout.hpp")
        self.assertIn("namespace ac3::base {", header)
        self.assertIn("enum class Location : std::uint8_t {", header)
        self.assertIn("kRight,  // a comment with a brace }", header)
        self.assertIn("struct Layout {", header)
        self.assertIn("inline constexpr int kMaxChannels = 22;", header)
        self.assertIn("// One speaker feed. A pair location expands", header)
        tables = self.text(f"{INC}/core/eac3_tables.hpp")
        self.assertNotIn("enum class Location", tables)
        for name in ("kMaxChannels", "Layout", "Location", "name"):
            self.assertIn(f"using ac3::base::{name};", tables)
        self.assertLess(tables.index("exponents.hpp"), tables.index("core/layout.hpp"))
        self.assertLess(tables.index("core/layout.hpp"), tables.index("core/tables.hpp"))
        self.assertIn("constexpr Layout expand(", tables)

    def test_c1_rewrites_the_codec_blind_users_and_drops_the_audio_alias(self) -> None:
        cuts.cut_c1(self.root)
        layout = self.text(f"{INC}/render/layout.hpp")
        self.assertIn("using Loc = ac3::base::Location;", layout)
        self.assertNotIn("eac3_tables.hpp", layout)
        spatial = self.text(f"{SRC}/spatial/spatial.cpp")
        self.assertIn("static_cast<std::size_t>(base::kMaxChannels)", spatial)
        speakers = self.text("src/audio/src/speakers.cpp")
        self.assertNotIn("namespace chanmap", speakers)
        self.assertIn("ac3::base::name(l)", speakers)
        self.assertIn("namespace {\n\nstruct Position", speakers)
        self.assertIn("base::name()", self.text("src/audio/include/ac3/audio/speakers.hpp"))

    def test_c2_c3_c4_c5_move_their_declarations(self) -> None:
        for cut in (cuts.cut_c2, cuts.cut_c3, cuts.cut_c4, cuts.cut_c5):
            cut(self.root)
        self.assertIn("namespace ac3::base {", self.text(f"{INC}/core/downmix_target.hpp"))
        self.assertIn("using base::DownmixTarget;", self.text(f"{INC}/decoder/output.hpp"))
        pcm = self.text(f"{INC}/render/pcm_block.hpp")
        self.assertIn("namespace ac3::render {", pcm)
        self.assertIn("class BlockSink {", pcm)
        self.assertIn("#include <concepts>", pcm)
        decoder = self.text(f"{INC}/decoder/decoder.hpp")
        self.assertIn("using render::BlockSink;", decoder)
        self.assertNotIn("struct PcmBlock", decoder)
        self.assertIn("struct DecodedFrame {}", decoder)
        domain = self.text(f"{INC}/oba/joc_domain.hpp")
        self.assertIn("namespace ac3::oba::joc {", domain)
        self.assertIn("constexpr int reconstruction_delay(Domain domain)", domain)
        self.assertNotIn("enum class Domain", self.text(f"{INC}/oba/joc.hpp"))
        placement = self.text(f"{INC}/oba/placement.hpp")
        self.assertIn("namespace ac3::oba {", placement)
        self.assertIn("struct ObjectPlacement {", placement)
        self.assertNotIn("ObjectPlacement", self.text(f"{INC}/oba/atmos.hpp"))

    def test_the_includes_a_cut_rewrites_stay_in_sort_order(self) -> None:
        self.run_all()
        for rel in (
            f"{INC}/oba/motion.hpp",
            f"{INC}/oba/scene.hpp",
            f"{SRC}/oba/scene.cpp",
            f"{INC}/render/render.hpp",
            f"{INC}/render/layout.hpp",
        ):
            includes = re.findall(r'^#include "([^"]+)"$', self.text(rel), re.MULTILINE)
            if rel.endswith(".cpp"):
                includes = includes[1:]  # a source's own header comes first
            self.assertEqual(includes, sorted(includes), rel)
        motion = self.text(f"{INC}/oba/motion.hpp")
        self.assertLess(motion.index("oba/oamd.hpp"), motion.index("oba/placement.hpp"))
        # a lone quoted include among the standard headers keeps its place
        self.assertIn(
            '#include <cmath>\n#include "ac3/oba/placement.hpp"\n#include <span>',
            self.text(f"{SRC}/oba/motion.cpp"),
        )

    def test_no_rewrite_leaves_two_blank_lines(self) -> None:
        self.run_all()
        changed = git(self.root, "status", "--porcelain").split("\n")
        for line in changed:
            if line.strip().endswith((".hpp", ".cpp")):
                rel = line.split()[-1]
                self.assertNotIn("\n\n\n", self.text(rel), rel)

    def test_c6_keeps_the_table_local_and_drops_the_include(self) -> None:
        cuts.cut_c6(self.root)
        text = self.text(f"{SRC}/iec61937/iec61937.cpp")
        self.assertNotIn("eac3_tables.hpp", text)
        self.assertIn("constexpr int blocks_per_syncframe(int numblkscod) {", text)
        self.assertIn("constexpr std::array<int, 4> counts = {1, 2, 3, 6};", text)
        self.assertIn("return blocks_per_syncframe(numblkscod);", text)
        self.assertNotIn("eac3::blocks_per_syncframe(", text)
        self.assertEqual(text.count("#include <array>"), 1)

    def test_c7_moves_serving_and_follows_its_includes_but_not_records(self) -> None:
        cuts.cut_c7(self.root)
        self.assertFalse((self.root / f"{INC}/render/serving.hpp").exists())
        moved = self.text(f"{INC}/decoder/serving.hpp")
        self.assertIn("moved out of the player so that every player that\n// renders", moved)
        player = self.text("esp-idf/ac3forge/src/player.cpp")
        self.assertIn("(ac3/decoder/serving.hpp) policy", player)
        includes = re.findall(r'^#include "([^"]+)"$', player, re.MULTILINE)
        self.assertEqual(includes, sorted(includes))
        self.assertIn("ac3/decoder/serving.hpp", includes)
        for rel in ("docs/library/header-map.md", "planning/layout.md", "CHANGELOG.md"):
            self.assertEqual(self.text(rel), FILES[rel], rel)

    def test_a_second_run_changes_nothing(self) -> None:
        self.run_all()
        git(self.root, "add", "-A")
        before = git(self.root, "diff", "--cached", "--stat")
        self.run_all()
        git(self.root, "add", "-A")
        self.assertEqual(git(self.root, "diff", "--cached", "--stat"), before)
        self.assertEqual(git(self.root, "status", "--porcelain").count("??"), 0)

    def test_a_missing_marker_stops_the_run(self) -> None:
        path = self.root / f"{INC}/decoder/output.hpp"
        path.write_bytes(
            path.read_bytes().replace(b"enum class DownmixTarget", b"enum class Renamed")
        )
        with self.assertRaises(SystemExit):
            cuts.cut_c2(self.root)

    def test_the_block_is_found_by_its_code_not_by_its_comment(self) -> None:
        path = self.root / f"{INC}/decoder/output.hpp"
        path.write_bytes(
            path.read_bytes().replace(
                b"// Which fold to produce.", b"// Reworded by the docs sweep,\n// over two lines."
            )
        )
        cuts.cut_c2(self.root)
        header = self.text(f"{INC}/core/downmix_target.hpp")
        self.assertIn(
            "// Reworded by the docs sweep,\n// over two lines.\nenum class DownmixTarget", header
        )


class Helpers(unittest.TestCase):
    def test_splice_closes_a_doubled_blank_line_only_for_a_removal(self) -> None:
        self.assertEqual(
            cuts.splice("a\n\nX\n\nb\n", 3, 5, ""),
            "a\n\nb\n".replace("b", "\nb").replace("\n\n\nb", "\n\nb"),
        )
        self.assertEqual(cuts.splice("a\n\nX\n\nb\n", 3, 5, "Y\n"), "a\n\nY\n\nb\n")

    def test_close_of_skips_braces_in_comments_and_literals(self) -> None:
        text = "struct S { // }\n  char c = '}'; const char* s = \"}\";\n};\nrest"
        self.assertEqual(text[: cuts.close_of(text, text.index("{"))].endswith("};"[:1]), True)
        self.assertEqual(text[cuts.close_of(text, text.index("{")) :].startswith(";"), True)

    def test_a_crlf_file_is_written_back_with_crlf(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "f.hpp").write_bytes(b"a\r\nb\r\n")
            src = cuts.Source(root, "f.hpp")
            self.assertEqual(src.text, "a\nb\n")
            src.text += "c\n"
            src.save()
            self.assertEqual((root / "f.hpp").read_bytes(), b"a\r\nb\r\nc\r\n")


if __name__ == "__main__":
    unittest.main()
