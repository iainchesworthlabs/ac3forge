"""How much of the per-library CMake is the same text with the library's name swapped?

For each library CMakeLists.txt (and cmake/InstallLibrary.cmake, cmake/ac3forgeConfig.cmake.in) the
comment-free lines are normalised (the library's own names replaced by @) and counted across files.
A line appearing in 4 or more files is boilerplate. Reports lines, files, and the repeated share.

Usage: cmake_repeat.py [--root R]
"""

from __future__ import annotations

import re
from collections import defaultdict

from n1b_lib import Repo, base_parser, emit, md_table

NAMES = {
    "src/forge/CMakeLists.txt": ["forge", "ac3forge", "AC3FORGE"],
    "src/signing/CMakeLists.txt": ["signing", "ac3signing"],
    "src/admbridge/CMakeLists.txt": ["admbridge"],
    "src/iamf/CMakeLists.txt": ["iamf"],
    "src/matroska/CMakeLists.txt": ["matroska"],
    "src/mp4/CMakeLists.txt": ["mp4"],
    "src/mpegts/CMakeLists.txt": ["mpegts"],
    "src/ac3iab/CMakeLists.txt": ["ac3iab"],
    "src/ac3adm/CMakeLists.txt": ["ac3adm"],
    "src/ac4/CMakeLists.txt": ["ac4"],
    "src/ac4dec/CMakeLists.txt": ["ac4dec", "decoder"],
    "src/ac4enc/CMakeLists.txt": ["ac4enc", "encoder"],
    "src/capi/CMakeLists.txt": ["forge_c", "capi", "ac3forge_c"],
}


def norm(line: str, names: list[str]) -> str | None:
    s = line.strip()
    if not s or s.startswith("#"):
        return None
    s = re.sub(r"\s+#.*$", "", s)
    for n in sorted(names, key=len, reverse=True):
        s = re.sub(re.escape(n), "@", s, flags=re.I)
    return re.sub(r"\s+", " ", s)


def main() -> None:
    ap = base_parser(__doc__)
    a = ap.parse_args()
    repo = Repo(a.root)
    per_file = {}
    seen = defaultdict(set)
    for path, names in NAMES.items():
        if not repo.exists(path):
            continue
        lines = [norm(x, names) for x in repo.read(path).splitlines()]
        lines = [x for x in lines if x]
        per_file[path] = lines
        for x in set(lines):
            seen[x].add(path)
    rows = []
    tot = rep = 0
    for path, lines in per_file.items():
        r = sum(1 for x in lines if len(seen[x]) >= 4)
        rows.append([path, len(lines), r, f"{100 * r // max(1, len(lines))}%"])
        tot += len(lines)
        rep += r
    rows.append(["**total**", tot, rep, f"{100 * rep // max(1, tot)}%"])
    # the install block and config template
    inst = repo.read("cmake/InstallLibrary.cmake").splitlines()
    cfg = repo.read("cmake/ac3forgeConfig.cmake.in").splitlines()
    inst_code = [x for x in inst if x.strip() and not x.strip().startswith("#")]
    cfg_code = [x for x in cfg if x.strip() and not x.strip().startswith("#")]
    text = md_table(
        ["library CMakeLists.txt", "code lines", "lines in >=4 files (name swapped)", "share"],
        rows,
        ["l", "r", "r", "r"],
    )
    text += (
        f"\ncmake/InstallLibrary.cmake: {len(inst)} lines, {len(inst_code)} code lines; "
        f"cmake/ac3forgeConfig.cmake.in: {len(cfg)} lines, {len(cfg_code)} code lines\n"
    )
    emit(text, a.out)


if __name__ == "__main__":
    main()
