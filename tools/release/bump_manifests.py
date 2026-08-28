"""Bump the four staged packaging manifests to a new release, given its digests.

Pure file editing, no network access - `.github/workflows/manifest-bump.yml` downloads
the release assets, computes and cross-checks the digests, and passes the results in as
plain hex strings. Keeping the two apart means this half is fully testable offline
against any past release's already-known digests (see that workflow's `dry_run` input,
and docs/releasing.md), and a bug here can't be mistaken for a bad download.

Each manifest is edited in place with a targeted regex substitution, not re-serialized
from a parsed structure - every one of these files carries hand-written prose comments
(a JSON port manifest aside, which has none), and a round-tripped
json.dump/yaml.dump would either drop them or reformat the file into a diff nobody could
review. winget is the one exception: its three files are entirely mechanical key/value
pairs with no prose to preserve, so a new version directory is rendered whole from a
template instead of copy-and-edit (avoids having to decide which existing version
directory is "latest" to copy from).

Two manifests - the vcpkg port and the Conan recipe - are pending submission to their
respective upstream curated registries and are not, themselves, what an end user
installs from yet; bumping them here keeps the in-tree staging current regardless.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = "iainchesworthlabs/ac3forge"


@dataclass
class EditResult:
    path: Path
    status: str  # "changed", "unchanged", "skipped"
    detail: str = ""


@dataclass
class BumpPlan:
    version: str
    source_sha512: str
    source_sha256: str
    dmg_sha256: str | None = None
    winzip_sha256: str | None = None
    repo: str = REPO
    results: list[EditResult] = field(default_factory=list)

    @property
    def tag(self) -> str:
        return f"v{self.version}"


class ManifestPatternNotFound(RuntimeError):
    pass


def _substitute_once(text: str, pattern: str, replacement: str, *, path: Path) -> str:
    new_text, count = re.subn(pattern, replacement, text, count=1)
    if count == 0:
        raise ManifestPatternNotFound(f"{path}: pattern not found: {pattern!r}")
    return new_text


def _apply(path: Path, transform, *, dry_run: bool) -> EditResult:
    original = path.read_text(encoding="utf-8")
    updated = transform(original)
    if updated == original:
        return EditResult(path, "unchanged")
    if not dry_run:
        # newline="\n" always: these files are read by shell scripts
        # (check_packaging_versions.sh) that assume bare LF - the platform default
        # write_text() would otherwise use (CRLF on Windows) silently corrupts every
        # line-anchored grep/sed pattern in them, not just the lines this function
        # touched.
        path.write_text(updated, encoding="utf-8", newline="\n")
    detail = "".join(f"  {line}\n" for line in _unified_diff_lines(original, updated, path))
    return EditResult(path, "changed", detail)


def _unified_diff_lines(before: str, after: str, path: Path) -> list[str]:
    return list(
        difflib.unified_diff(
            before.splitlines(),
            after.splitlines(),
            fromfile=str(path),
            tofile=str(path),
            lineterm="",
        )
    )


def bump_vcpkg(root: Path, plan: BumpPlan, *, dry_run: bool) -> None:
    vcpkg_json = root / "packaging/vcpkg-port/ac3forge/vcpkg.json"
    portfile = root / "packaging/vcpkg-port/ac3forge/portfile.cmake"

    def edit_json(text: str) -> str:
        return _substitute_once(
            text,
            r'"version-semver":\s*"[^"]+"',
            f'"version-semver": "{plan.version}"',
            path=vcpkg_json,
        )

    def edit_portfile(text: str) -> str:
        return _substitute_once(
            text,
            r"SHA512 [0-9A-Fa-f]{128}",
            f"SHA512 {plan.source_sha512}",
            path=portfile,
        )

    plan.results.append(_apply(vcpkg_json, edit_json, dry_run=dry_run))
    plan.results.append(_apply(portfile, edit_portfile, dry_run=dry_run))


def bump_homebrew_formula(root: Path, plan: BumpPlan, *, dry_run: bool) -> None:
    formula = root / "packaging/homebrew/Formula/ac3forge.rb"

    def edit(text: str) -> str:
        text = _substitute_once(
            text,
            r"archive/refs/tags/v[0-9][^\"']*\.tar\.gz",
            f"archive/refs/tags/{plan.tag}.tar.gz",
            path=formula,
        )
        return _substitute_once(
            text,
            r'sha256 "[0-9A-Fa-f]{64}"',
            f'sha256 "{plan.source_sha256}"',
            path=formula,
        )

    plan.results.append(_apply(formula, edit, dry_run=dry_run))


def bump_homebrew_cask(root: Path, plan: BumpPlan, *, dry_run: bool) -> None:
    if plan.dmg_sha256 is None:
        plan.results.append(
            EditResult(
                root / "packaging/homebrew/Casks/ac3gui.rb",
                "skipped",
                "no ac3forge-*-Darwin.dmg release asset - macOS leg did not package this release",
            )
        )
        return

    cask = root / "packaging/homebrew/Casks/ac3gui.rb"

    def edit(text: str) -> str:
        # No `^`/MULTILINE anchor needed: this is the file's first `version "..."`
        # occurrence (the cask's own top-level stanza field) - the later
        # `dmg_version = version.major_minor_patch` reference has no quote after
        # `version`, so it can't collide with this pattern.
        text = _substitute_once(
            text,
            r'version "[0-9][^"]*"',
            f'version "{plan.version}"',
            path=cask,
        )
        return _substitute_once(
            text,
            r'sha256 "[0-9A-Fa-f]{64}"',
            f'sha256 "{plan.dmg_sha256}"',
            path=cask,
        )

    plan.results.append(_apply(cask, edit, dry_run=dry_run))


def bump_conan(root: Path, plan: BumpPlan, *, dry_run: bool) -> None:
    conandata = root / "packaging/conan/conandata.yml"

    # Appended at the end of the file, after the existing sources: block - matches
    # the file's own existing oldest-to-newest ordering. Idempotent: re-running for
    # a version already present is a no-op, not a duplicate entry.
    original = conandata.read_text(encoding="utf-8")
    if f'"{plan.version}":' in original:
        plan.results.append(EditResult(conandata, "unchanged"))
        return
    url = f"https://github.com/{plan.repo}/archive/refs/tags/{plan.tag}.tar.gz"
    entry = f'  "{plan.version}":\n    url: "{url}"\n    sha256: "{plan.source_sha256}"\n'
    updated = (original if original.endswith("\n") else original + "\n") + entry
    if not dry_run:
        conandata.write_text(updated, encoding="utf-8", newline="\n")
    diff = "".join(f"  {line}\n" for line in _unified_diff_lines(original, updated, conandata))
    plan.results.append(EditResult(conandata, "changed", diff))


_WINGET_INSTALLER_TEMPLATE = """\
# Points at the win64.zip release asset (.github/workflows/release.yml's
# windows-msvc leg), not an NSIS .exe: this release didn't produce one
# (makensis wasn't on the runner - see cmake/Packaging.cmake), and the zip
# already carries both end-user binaries (bin/ac3cli.exe, bin/ac3gui.exe -
# CPack's "runtime" component, see docs/releasing.md#what-gets-published).
# InstallerType zip + NestedInstallerType portable installs straight from
# that archive with no separate installer to build - see
# docs/releasing.md#winget-manifest for what changes this if a real NSIS
# installer becomes available for a future release.
# yaml-language-server: $schema=https://aka.ms/winget-manifest.installer.1.12.0.schema.json
PackageIdentifier: iainchesworthlabs.ac3forge
PackageVersion: {version}
InstallerLocale: en-US
Platform:
  - Windows.Desktop
MinimumOSVersion: 10.0.17763.0
InstallerType: zip
NestedInstallerType: portable
NestedInstallerFiles:
  - RelativeFilePath: bin/ac3cli.exe
    PortableCommandAlias: ac3cli
  - RelativeFilePath: bin/ac3gui.exe
    PortableCommandAlias: ac3gui
Installers:
  - Architecture: x64
    InstallerUrl: {installer_url}
    # Computed directly (sha256sum) from the release asset - winget wants
    # SHA256, unlike the SHA512SUMS release.yml publishes (see
    # docs/releasing.md#what-gets-published). If `winget install` ever
    # reports a mismatch, trust winget's reported hash over this one and
    # update it here, same policy as the vcpkg port's SHA512 comment.
    InstallerSha256: {winzip_sha256}
ManifestType: installer
ManifestVersion: 1.12.0
"""

_WINGET_VERSION_TEMPLATE = """\
# Created with the winget-pkgs three-file manifest convention. Staged here
# (packaging/winget/) at the exact manifests/<firstletter>/<publisher>/<package>/<version>/
# path a microsoft/winget-pkgs submission uses, so this directory can be copied
# straight into a winget-pkgs fork - see docs/releasing.md.
# yaml-language-server: $schema=https://aka.ms/winget-manifest.version.1.12.0.schema.json
PackageIdentifier: iainchesworthlabs.ac3forge
PackageVersion: {version}
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.12.0
"""

_WINGET_LOCALE_TEMPLATE = """\
# yaml-language-server: $schema=https://aka.ms/winget-manifest.defaultLocale.1.12.0.schema.json
PackageIdentifier: iainchesworthlabs.ac3forge
PackageVersion: {version}
PackageLocale: en-US
Publisher: iainchesworthlabs
PublisherUrl: https://github.com/iainchesworthlabs
PublisherSupportUrl: https://github.com/iainchesworthlabs/ac3forge/issues
PackageName: ac3forge
PackageUrl: https://github.com/iainchesworthlabs/ac3forge
License: GPL-3.0-or-later
LicenseUrl: https://github.com/iainchesworthlabs/ac3forge/blob/main/LICENSE
ShortDescription: Clean-room AC-3/E-AC-3 encoder, decoder and Atmos object-layer CLI/GUI
Description: >-
  ac3forge is a clean-room C++23 implementation of the AC-3 (ATSC A/52, "Dolby Digital") and
  E-AC-3 ("Dolby Digital Plus") codecs, including a spatial object layer for Atmos-style
  authoring and decode. This package installs ac3cli (the command-line encoder/decoder) and
  ac3gui (the Qt6 desktop application) as portable executables.
Moniker: ac3forge
Tags:
  - audio
  - codec
  - ac3
  - dolby-digital
  - atmos
ReleaseNotesUrl: {release_notes_url}
# defaultLocale, not locale: this is the only locale manifest, and the
# version manifest's DefaultLocale: en-US names it as such - winget-pkgs
# requires the two to agree.
ManifestType: defaultLocale
ManifestVersion: 1.12.0
"""


def bump_winget(root: Path, plan: BumpPlan, *, dry_run: bool) -> None:
    base = root / "packaging/winget/manifests/i/iainchesworthlabs/ac3forge"

    if plan.winzip_sha256 is None:
        plan.results.append(
            EditResult(
                base / plan.version,
                "skipped",
                "no ac3forge-*-win64.zip release asset - windows-msvc leg did not package "
                "this release",
            )
        )
        return

    version_dir = base / plan.version
    bare_version = plan.version.split("-", 1)[0]
    installer_url = (
        f"https://github.com/{plan.repo}/releases/download/{plan.tag}/"
        f"ac3forge-{bare_version}-win64.zip"
    )
    release_notes_url = f"https://github.com/{plan.repo}/releases/tag/{plan.tag}"

    installer_yaml = _WINGET_INSTALLER_TEMPLATE.format(
        version=plan.version, installer_url=installer_url, winzip_sha256=plan.winzip_sha256
    )
    version_yaml = _WINGET_VERSION_TEMPLATE.format(version=plan.version)
    locale_yaml = _WINGET_LOCALE_TEMPLATE.format(
        version=plan.version, release_notes_url=release_notes_url
    )
    files = {
        version_dir / "iainchesworthlabs.ac3forge.installer.yaml": installer_yaml,
        version_dir / "iainchesworthlabs.ac3forge.yaml": version_yaml,
        version_dir / "iainchesworthlabs.ac3forge.locale.en-US.yaml": locale_yaml,
    }

    if version_dir.is_dir():
        plan.results.append(EditResult(version_dir, "unchanged", "directory already exists"))
        return

    if not dry_run:
        version_dir.mkdir(parents=True, exist_ok=True)
        for path, content in files.items():
            path.write_text(content, encoding="utf-8", newline="\n")

    detail = "".join(f"  new file: {path}\n" for path in files)
    plan.results.append(EditResult(version_dir, "changed", detail))


def run(plan: BumpPlan, root: Path, *, dry_run: bool) -> int:
    steps = [bump_vcpkg, bump_homebrew_formula, bump_homebrew_cask, bump_conan, bump_winget]
    try:
        for step in steps:
            step(root, plan, dry_run=dry_run)
    except ManifestPatternNotFound as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    for result in plan.results:
        print(f"{result.status:>9}  {result.path}")
        if result.detail:
            print(result.detail, end="")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="bare version, e.g. 0.9.0-beta.1")
    parser.add_argument("--source-sha512", required=True, help="SHA512 of the source tarball")
    parser.add_argument("--source-sha256", required=True, help="SHA256 of the source tarball")
    parser.add_argument("--dmg-sha256", help="SHA256 of ac3forge-*-Darwin.dmg, if it was built")
    parser.add_argument("--winzip-sha256", help="SHA256 of ac3forge-*-win64.zip, if it was built")
    parser.add_argument("--repo", default=REPO)
    parser.add_argument("--root", type=Path, default=Path())
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    plan = BumpPlan(
        version=args.version,
        source_sha512=args.source_sha512,
        source_sha256=args.source_sha256,
        dmg_sha256=args.dmg_sha256,
        winzip_sha256=args.winzip_sha256,
        repo=args.repo,
    )
    return run(plan, args.root, dry_run=args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
