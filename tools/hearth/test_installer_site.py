"""Unit tests for installer_site.py, which puts a release's sink firmware into the
documentation site for the browser installer (planning/esp32-ota.md, O9).

stdlib unittest, as ci.yml's script-lint job runs it. The release is the four
images CI publishes, packaged by package_firmware.py from sink_build_fixture.py's
builds, and served by a stand-in for the GitHub API, so nothing leaves this
machine.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import installer_site
import ota
import package_firmware
import sink_build_fixture


class InstallerSite(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name)
        self.published = self.tmp / "published"
        self.builds: dict[str, Path] = {}
        for name, settings in sink_build_fixture.RELEASE_IMAGES.items():
            self.builds[name] = sink_build_fixture.make_build(
                self.tmp / "builds" / name, **settings
            )
            package_firmware.package(self.builds[name], name, self.published)
        package_firmware.merge_manifest(self.published)
        self.files = {path.name: path.read_bytes() for path in self.published.iterdir()}
        self.files["SHA512SUMS"] = "".join(
            f"{hashlib.sha512(data).hexdigest()}  {name}\n"
            for name, data in self.files.items()
            if name.endswith((".bin", ".zip"))
        ).encode()
        self.releases: list[dict[str, Any]] = [
            {"tag_name": "v0.12.0-beta.1", "assets": []},
            {
                "tag_name": "v0.11.0",
                "html_url": "https://github.com/example/releases/tag/v0.11.0",
                "published_at": "2026-10-01T09:30:00Z",
                "assets": [
                    {"name": name, "browser_download_url": f"https://example.invalid/{name}"}
                    for name in self.files
                ],
            },
        ]
        self.out = self.tmp / "site"

    def github(self, url: str) -> bytes:
        if url.endswith("/releases?per_page=30"):
            return json.dumps(self.releases).encode()
        if "/releases/tags/" in url:
            tag = url.rsplit("/", 1)[1]
            return json.dumps(next(r for r in self.releases if r["tag_name"] == tag)).encode()
        return self.files[url.rsplit("/", 1)[1]]

    def run_site(self, get=None, source: list[str] | None = None) -> tuple[int, str]:
        output = io.StringIO()
        with (
            mock.patch.object(ota, "github_get", side_effect=get or self.github),
            contextlib.redirect_stdout(output),
            contextlib.redirect_stderr(output),
        ):
            code = installer_site.main(
                [*(source or ["--release", "latest"]), "--out", str(self.out)]
            )
        return code, output.getvalue()

    def test_each_image_gets_its_parts_and_an_installer_manifest(self) -> None:
        code, output = self.run_site()
        self.assertEqual(code, 0, output)
        index = json.loads((self.out / "index.json").read_text("utf-8"))
        self.assertEqual(index["version"], "v0.11.0")
        self.assertEqual(
            [image["name"] for image in index["images"]],
            sorted(sink_build_fixture.RELEASE_IMAGES),
        )
        # What the page shows about the release, and asks GitHub about.
        self.assertEqual(index["tag"], "v0.11.0")
        self.assertEqual(index["page"], "https://github.com/example/releases/tag/v0.11.0")
        self.assertEqual(index["published"], "2026-10-01T09:30:00Z")
        self.assertEqual(index["repository"], ota.REPOSITORY)
        self.assertEqual(index["manifest"], ota.MANIFEST_NAME)
        # The page lists every chip; each image says which it is for.
        self.assertEqual(
            {image["name"]: image["chip"] for image in index["images"]},
            {
                "hearth-sink-esp32c6": "ESP32-C6",
                "hearth-sink-esp32c6-16mb": "ESP32-C6",
                "hearth-sink-esp32p4-rev1": "ESP32-P4",
                "hearth-sink-esp32s3": "ESP32-S3",
            },
        )
        p4 = json.loads((self.out / "hearth-sink-esp32p4-rev1.json").read_text("utf-8"))
        self.assertEqual(p4["version"], "v0.11.0")
        self.assertTrue(p4["new_install_prompt_erase"])
        self.assertEqual(len(p4["builds"]), 1)
        self.assertEqual(p4["builds"][0]["chipFamily"], "ESP32-P4")
        self.assertEqual(
            [(part["offset"], part["path"]) for part in p4["builds"][0]["parts"]],
            [
                (0x2000, "hearth-sink-esp32p4-rev1/bootloader.bin"),
                (0x8000, "hearth-sink-esp32p4-rev1/partition-table.bin"),
                (0x10000, "hearth-sink-esp32p4-rev1/ota_data_initial.bin"),
                (0x20000, "hearth-sink-esp32p4-rev1/ac3forge_hearth_sink.bin"),
                (0x830000, "hearth-sink-esp32p4-rev1/sample.ac3"),
                (0x870000, "hearth-sink-esp32p4-rev1/storage.bin"),
            ],
        )
        build = self.builds["hearth-sink-esp32p4-rev1"]
        self.assertEqual(
            (self.out / "hearth-sink-esp32p4-rev1" / "ac3forge_hearth_sink.bin").read_bytes(),
            (build / "ac3forge_hearth_sink.bin").read_bytes(),
        )
        c6 = json.loads((self.out / "hearth-sink-esp32c6.json").read_text("utf-8"))
        self.assertEqual(c6["builds"][0]["chipFamily"], "ESP32-C6")
        self.assertIn("4 MB", c6["name"])

    def test_no_release_with_sink_firmware_yet_leaves_an_empty_installer(self) -> None:
        self.releases = [{"tag_name": "v0.10.0", "assets": []}]
        code, output = self.run_site()
        self.assertEqual(code, 0, output)
        self.assertIn("the installer offers nothing yet", output)
        index = json.loads((self.out / "index.json").read_text("utf-8"))
        self.assertEqual(index, installer_site.empty_index())
        self.assertEqual(index["images"], [])
        # Still enough for the page to ask GitHub whether a release has firmware since.
        self.assertEqual(index["repository"], ota.REPOSITORY)
        self.assertEqual(index["manifest"], ota.MANIFEST_NAME)

    def test_a_directory_of_images_fills_the_installer_as_a_release_does(self) -> None:
        (self.published / "SHA512SUMS").write_bytes(self.files["SHA512SUMS"])

        def offline(url: str) -> bytes:
            raise AssertionError(f"a directory needs no download, but {url} was asked for")

        code, output = self.run_site(offline, ["--dir", str(self.published)])
        self.assertEqual(code, 0, output)
        index = json.loads((self.out / "index.json").read_text("utf-8"))
        self.assertEqual(
            [image["name"] for image in index["images"]],
            sorted(sink_build_fixture.RELEASE_IMAGES),
        )
        self.assertEqual((index["tag"], index["page"], index["published"]), ("", "", ""))
        build = self.builds["hearth-sink-esp32s3"]
        self.assertEqual(
            (self.out / "hearth-sink-esp32s3" / "ac3forge_hearth_sink.bin").read_bytes(),
            (build / "ac3forge_hearth_sink.bin").read_bytes(),
        )

    def test_a_directory_whose_parts_do_not_check_out_fails(self) -> None:
        name = next(
            n
            for n in self.files
            if n.startswith("hearth-sink-esp32c6-16mb-") and n.endswith("-parts.zip")
        )
        path = self.published / name
        data = path.read_bytes()
        path.write_bytes(data[:-1] + bytes([data[-1] ^ 0xFF]))
        code, output = self.run_site(source=["--dir", str(self.published)])
        self.assertEqual(code, 1)
        self.assertIn(f"{name}'s SHA-256 is not the one", output)

    def test_parts_that_do_not_check_out_fail_the_deploy(self) -> None:
        name = next(
            n
            for n in self.files
            if n.startswith("hearth-sink-esp32s3-") and n.endswith("-parts.zip")
        )
        # The last byte flipped: a zip's own last byte is already 0 (its
        # comment's length), so writing 0 there would change nothing.
        self.files[name] = self.files[name][:-1] + bytes([self.files[name][-1] ^ 0xFF])
        code, output = self.run_site()
        self.assertEqual(code, 1)
        self.assertIn(f"{name}'s SHA-256 is not the one", output)

    def test_an_api_that_does_not_answer_fails_the_deploy_rather_than_empty_the_installer(
        self,
    ) -> None:
        def unanswered(url: str) -> bytes:
            raise ota.UsageError(f"{url} got no answer: timed out")

        code, output = self.run_site(unanswered)
        self.assertEqual(code, 1)
        self.assertIn("got no answer", output)
        self.assertFalse((self.out / "index.json").exists())


if __name__ == "__main__":
    unittest.main()
