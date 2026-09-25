#!/usr/bin/env python3
"""The browser installer's firmware, from a release (planning/esp32-ota.md, O9 and decision 15).

    python tools/hearth/installer_site.py --release latest --out docs/assets/sink-installer

(--run N or --dir PATH take a CI run's images, or a directory of them, instead:
a way to try the page before a release publishes any.)

docs/hearth/sink-installer.md is a page on the documentation site built on ESP
Web Tools, which flashes a board over Web Serial from a manifest of parts. A
page cannot fetch GitHub release assets, which carry no CORS headers, so
docs.yml runs this before it deploys the site: it takes the release's
hearth-sink-manifest.json, downloads each image's parts.zip, checks it against
the manifest's SHA-256 and the release's SHA512SUMS, and writes into OUT:

- <image>/<part>: the bootloader, partition table, empty otadata, app, audio
  and storage, as the parts.zip holds them;
- <image>.json: ESP Web Tools' manifest for that image, its parts at their
  offsets. It lists the pieces, not the merged factory image, and sets
  new_install_prompt_erase, so the person installing chooses whether to erase:
  a new board is erased, and a board already in use keeps its NVS and moves to
  this layout;
- index.json: the release (its tag, its page and when it was published), the
  repository and the manifest's name, and each image's name, title, chip and
  manifest. The page reads it to show what it offers, board by board, and
  asks GitHub's API whether a newer release has firmware than the one the
  site took.

A release that publishes no sink firmware yet leaves an index.json with no
images, and the page says so. Standard library only, and ota.py's own reading
of a release, so the tool and the site take a release the same way.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ota

# ESP Web Tools' chip family for each target (esp-web-tools 10.4.0's
# Build["chipFamily"]).
CHIP_FAMILIES = {"esp32s3": "ESP32-S3", "esp32c6": "ESP32-C6", "esp32p4": "ESP32-P4"}

# What the page calls each image: the board it is for, in a person's words.
TITLES = {
    "hearth-sink-esp32s3": (
        "ESP32-S3 with 16 MB of flash and octal PSRAM (such as the DevKitC-1 N16R8)"
    ),
    "hearth-sink-esp32c6": "ESP32-C6 with 4 MB of flash (any C6 module)",
    "hearth-sink-esp32c6-16mb": "ESP32-C6 with 16 MB of flash",
    "hearth-sink-esp32p4-rev1": "ESP32-P4 of silicon revision v1.x (such as the FireBeetle 2)",
}


class SiteError(Exception):
    """Why the installer's firmware cannot be written; main() prints it."""


def read_directory(directory: Path) -> ota.Published:
    """A set of images already on disk: a release's files, or CI's esp32-firmware artifact."""
    try:
        manifest = json.loads((directory / ota.MANIFEST_NAME).read_text("utf-8"))
    except (OSError, ValueError) as error:
        raise SiteError(f"{directory} has no readable {ota.MANIFEST_NAME}: {error}") from None
    sums = directory / "SHA512SUMS"
    return ota.Published(
        manifest,
        directory,
        f"directory {directory}",
        ota.read_sums(sums.read_text("utf-8")) if sums.is_file() else {},
    )


def read_file(published: ota.Published, file_name: str) -> bytes:
    """A published file: downloaded from the release, or read where the run or directory has it."""
    if file_name in published.urls:
        return ota.github_get(published.urls[file_name])
    path = published.directory / file_name
    if not file_name or not path.is_file():
        raise SiteError(f"{published.source} names {file_name}, which it does not publish")
    return path.read_bytes()


def unpack_parts(
    published: ota.Published, image: dict[str, Any], out: Path
) -> list[dict[str, Any]]:
    """The image's parts, checked and written under out/<image>/, as ESP Web Tools' parts list."""
    name = str(image["name"])
    entry = (image.get("files") or {}).get("parts") or {}
    file_name = str(entry.get("name", ""))
    data = read_file(published, file_name)
    if hashlib.sha256(data).hexdigest() != entry.get("sha256"):
        raise SiteError(f"{file_name}'s SHA-256 is not the one {ota.MANIFEST_NAME} gives")
    if published.sums and published.sums.get(file_name) != hashlib.sha512(data).hexdigest():
        raise SiteError(f"{file_name}'s SHA-512 is not the one SHA512SUMS gives")
    directory = out / name
    directory.mkdir(parents=True, exist_ok=True)
    parts = []
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for part in image.get("parts") or []:
            member = str(part["file"])
            content = archive.read(member)
            if hashlib.sha256(content).hexdigest() != part.get("sha256"):
                raise SiteError(
                    f"{file_name}'s {member} is not the part {ota.MANIFEST_NAME} describes"
                )
            (directory / member).write_bytes(content)
            parts.append({"path": f"{name}/{member}", "offset": int(part["offset"])})
    return parts


def empty_index() -> dict[str, Any]:
    """What the page reads when no release has sink firmware yet."""
    return {
        "repository": ota.REPOSITORY,
        "manifest": ota.MANIFEST_NAME,
        "version": "",
        "tag": "",
        "page": "",
        "published": "",
        "images": [],
    }


def write_site(published: ota.Published, out: Path) -> dict[str, Any]:
    out.mkdir(parents=True, exist_ok=True)
    manifest = published.manifest
    index = empty_index()
    index.update(
        version=str(manifest.get("version", "")),
        tag=published.tag,
        page=published.page,
        published=published.published,
    )
    for image in manifest.get("images") or []:
        name = str(image["name"])
        family = CHIP_FAMILIES.get(str(image.get("target", "")))
        if family is None:
            raise SiteError(
                f"{name} is for {image.get('target')}, which ESP Web Tools has no family for"
            )
        installer = {
            "name": f"Hearth sink, {TITLES.get(name, name)}",
            "version": str(image.get("version", "")),
            "new_install_prompt_erase": True,
            "builds": [{"chipFamily": family, "parts": unpack_parts(published, image, out)}],
        }
        (out / f"{name}.json").write_text(json.dumps(installer, indent=2) + "\n", "utf-8")
        index["images"].append(
            {
                "name": name,
                "title": TITLES.get(name, name),
                "chip": family,
                "manifest": f"{name}.json",
            }
        )
    (out / "index.json").write_text(json.dumps(index, indent=2) + "\n", "utf-8")
    return index


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    source = parser.add_mutually_exclusive_group()
    source.add_argument(
        "--release", default="latest", help="a release tag, or latest (the default)"
    )
    source.add_argument(
        "--run", help="a CI run's number: its esp32-firmware artifact, to try the page early"
    )
    source.add_argument(
        "--dir", type=Path, help="a directory with a release's firmware files, for the same"
    )
    parser.add_argument(
        "--out", type=Path, required=True, help="where the site's installer assets go"
    )
    args = parser.parse_args(argv)
    try:
        with tempfile.TemporaryDirectory(prefix="installer-") as temporary:
            if args.run:
                published = ota.fetch_run(args.run, Path(temporary))
            elif args.dir:
                published = read_directory(args.dir)
            else:
                published = ota.fetch_release(args.release, Path(temporary))
            index = write_site(published, args.out)
    except ota.NoPublishedFirmware as error:
        # No release carries sink firmware yet: the page says so. Anything
        # else - the API not answering, a download that does not check out -
        # fails the deploy rather than publish an installer with nothing in it.
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "index.json").write_text(json.dumps(empty_index(), indent=2) + "\n", "utf-8")
        print(f"installer_site: {error}; the installer offers nothing yet")
        return 0
    except (SiteError, ota.UsageError, KeyError, zipfile.BadZipFile) as error:
        print(f"installer_site: {error}", file=sys.stderr)
        return 1
    print(f"installer_site: {published.source}, {len(index['images'])} images into {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
