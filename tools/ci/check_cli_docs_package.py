#!/usr/bin/env python3
"""Assert a runtime package carries ac3cli's man page and its four completions.

apps/cli/CMakeLists.txt generates ac3cli.1 and the bash/zsh/fish/PowerShell
completion scripts by running the freshly built ac3cli (roadmap IO8), so a
cross build - which cannot run its own output - installs none of them. The
test for "is this a cross build" was CMAKE_CROSSCOMPILING until #527, and that
variable is TRUE on every native Linux and macOS build in this tree, because
CMakeDetermineSystem sets it from the mere presence of CMAKE_SYSTEM_NAME and
cmake/toolchains/{linux.gcc,linux.llvm,macos.llvm}.toolchain.cmake each set
that unconditionally while every Linux/macOS preset chainloads one. Both Linux
.tar.gz, both .deb, both .rpm and the universal .dmg therefore shipped no man
page and no completions, for as long as the guard stood, and nothing went red:
the only test asserting their presence is the `test do` block in
packaging/homebrew/Formula/ac3forge.rb, and a Homebrew build passes no
toolchain file, so it kept passing. This is the assertion that was missing:

    python3 tools/ci/check_cli_docs_package.py packages/ac3forge-0.7.0-Linux-x86_64.tar.gz
    python3 tools/ci/check_cli_docs_package.py packages/ac3forge-0.7.0-Darwin.zip
    python3 tools/ci/check_cli_docs_package.py merged            # an install tree

Run from .github/workflows/_build.yml on the Linux and macOS legs, and over
the lipo-merged tree the universal .dmg is built from, which is its own
opportunity to lose these files - the merge walks one architecture's tree and
copies through what it finds.

The archive it reads is the runtime component's .tar.gz (Linux) or .zip
(macOS). The .deb and .rpm are built from the same install tree in the same
cpack run and carry the same five files, so inspecting them as well would
catch nothing more - and the RPM generator gzips the man page to
ac3cli.1.gz, which would make the expected names generator-specific for no
extra failure mode found. The same reasoning check_crucible_package.py's
Linux half already uses for reading the tarball rather than the .deb.
"""

from __future__ import annotations

import os
import sys
import tarfile
import zipfile

# GNUInstallDirs' MANDIR/DATAROOTDIR under the install prefix, one entry per
# install(FILES ...) rule in apps/cli/CMakeLists.txt's generated-docs block.
# bash and zsh look their file up by a fixed name; fish and PowerShell want a
# suffixed script, and PowerShell has no convention-driven search path at all,
# which is why its script sits under the project's own share directory.
REQUIRED = (
    "share/man/man1/ac3cli.1",
    "share/bash-completion/completions/ac3cli",
    "share/zsh/site-functions/_ac3cli",
    "share/fish/vendor_completions.d/ac3cli.fish",
    "share/ac3forge/completions/ac3cli.ps1",
)

# Directories a name relative to the install prefix can start with. Used to
# decide whether an archive wraps its tree in a top-level directory.
INSTALL_DIRS = ("bin/", "share/", "lib/", "include/", "libexec/")


def strip_toplevel(names: list[str]) -> list[str]:
    """Return `names` relative to the install prefix.

    CPack's archive generators wrap the whole tree in one top-level directory
    or in none at all (CPACK_INCLUDE_TOPLEVEL_DIRECTORY, shared by ZIP and
    TGZ); the component archives here use none, but that is a variable someone
    can flip. Deciding once for the archive rather than per name matters:
    stripping every name unconditionally turned bin/ac3cli into ac3cli and
    reported everything missing on a correct archive (the bug
    check_crucible_package.py records), and stripping only the names that do
    not already start at an install directory turns macOS's root-level
    ac3gui.app/Contents/... into Contents/..., which is a different wrong
    answer. If anything in the archive already starts at an install
    directory, nothing is wrapped.
    """
    names = [name.removeprefix("./") for name in names]
    if any(name.startswith(INSTALL_DIRS) for name in names):
        return names
    return [name.split("/", 1)[1] for name in names if "/" in name]


def names_in(path: str) -> list[str]:
    """Every file in an install tree, a .tar.* or a .zip, as install-relative names."""
    if os.path.isdir(path):
        # An explicit prefix: these names are already install-relative, so the
        # top-level question above does not arise.
        return [
            os.path.relpath(os.path.join(root, name), path).replace(os.sep, "/")
            for root, _dirs, files in os.walk(path)
            for name in files
        ]
    if path.endswith((".tar.gz", ".tgz", ".tar.xz")):
        with tarfile.open(path) as archive:
            return strip_toplevel([m.name for m in archive.getmembers() if m.isfile()])
    with zipfile.ZipFile(path) as archive:
        return strip_toplevel([i.filename for i in archive.infolist() if not i.is_dir()])


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <package.tar.gz|package.zip|install-tree/>", file=sys.stderr)
        return 2
    path = argv[1]
    names = names_in(path)
    print(f"{path}: {len(names)} files")

    missing = [name for name in REQUIRED if name not in names]
    for name in missing:
        print(f"::error::{path}: missing {name}")
    if missing:
        # The one thing worth saying at the point of failure: where the files
        # come from, and the guard that decides whether they are produced.
        print(
            "::error::this package ships no ac3cli man page or shell completions. They are "
            "generated by running the built ac3cli (apps/cli/CMakeLists.txt) and skipped when "
            "the host and target differ; check the configure log's 'CLI man page' status line "
            "for which branch that guard took on this leg."
        )
        return 1
    print("ok: the package holds the ac3cli man page and the bash, zsh, fish and "
          "PowerShell completions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
