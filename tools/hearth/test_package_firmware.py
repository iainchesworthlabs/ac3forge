"""Unit tests for package_firmware.py, which packages hearth_sink board builds
for publishing (planning/esp32-ota.md, O8).

stdlib unittest, as ci.yml's script-lint job runs it. The builds are made from
nothing by sink_build_fixture.py, with an app image the bootloader's checks
would pass.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import package_firmware as packager
import sink_build_fixture as fixture


class Package(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.out = self.root / "out"

    def tearDown(self) -> None:
        self.dir.cleanup()

    def test_a_build_becomes_the_five_files(self) -> None:
        build = fixture.make_build(self.root / "s3")
        fragment = packager.package(build, "hearth-sink-esp32s3", self.out)
        self.assertEqual(
            sorted(path.name for path in self.out.iterdir()),
            [
                "hearth-sink-esp32s3-v0.11.0-elf.zip",
                "hearth-sink-esp32s3-v0.11.0-factory.bin",
                "hearth-sink-esp32s3-v0.11.0-parts.zip",
                "hearth-sink-esp32s3-v0.11.0.bin",
                "hearth-sink-esp32s3.json",
            ],
        )
        app = (build / "ac3forge_hearth_sink.bin").read_bytes()
        self.assertEqual((self.out / "hearth-sink-esp32s3-v0.11.0.bin").read_bytes(), app)
        self.assertEqual(fragment["version"], "v0.11.0")
        self.assertEqual(fragment["target"], "esp32s3")
        self.assertEqual(fragment["chip"], "ESP32-S3")
        self.assertEqual(fragment["chip_id"], 9)
        self.assertEqual((fragment["min_rev_full"], fragment["max_rev_full"]), (0, 99))
        self.assertEqual(fragment["flash_size"], "16MB")
        self.assertTrue(fragment["psram"])
        self.assertEqual(fragment["slot_bytes"], 0x400000)
        self.assertFalse(fragment["network_built_in"])
        self.assertEqual(fragment["project"], "ac3forge_hearth_sink")
        self.assertEqual(fragment["elf_sha256"], bytes((1 + i) & 0xFF for i in range(32)).hex())
        self.assertEqual(
            [entry["label"] for entry in fragment["partitions"]][3:5], ["ota_0", "ota_1"]
        )
        self.assertEqual(
            [(part["offset"], part["file"]) for part in fragment["parts"]],
            [
                (0x0, "bootloader.bin"),
                (0x8000, "partition-table.bin"),
                (0x10000, "ota_data_initial.bin"),
                (0x20000, "ac3forge_hearth_sink.bin"),
                (0x830000, "sample.ac3"),
                (0x870000, "storage.bin"),
            ],
        )
        self.assertEqual(
            json.loads((self.out / "hearth-sink-esp32s3.json").read_text("utf-8")), fragment
        )
        with zipfile.ZipFile(self.out / "hearth-sink-esp32s3-v0.11.0-elf.zip") as archive:
            self.assertEqual(archive.namelist(), ["hearth-sink-esp32s3-v0.11.0.elf"])

    def test_the_parts_archive_flashes_with_paths_of_its_own(self) -> None:
        build = fixture.make_build(self.root / "p4", target="esp32p4", min_rev=100, max_rev=199)
        packager.package(build, "hearth-sink-esp32p4-rev1", self.out)
        with zipfile.ZipFile(self.out / "hearth-sink-esp32p4-rev1-v0.11.0-parts.zip") as archive:
            flash_args = archive.read("flash_args").decode().splitlines()
            self.assertEqual(flash_args[0], "--flash-mode dio --flash-freq 80m --flash-size 16MB")
            # The P4's bootloader at 0x2000, and the audio source by its own name.
            self.assertEqual(flash_args[1], "0x2000 bootloader.bin")
            self.assertEqual(flash_args[5], "0x830000 sample.ac3")
            self.assertEqual(
                archive.read("sample.ac3"),
                (self.root / "p4" / "stream" / "sample.ac3").read_bytes(),
            )
            for line in flash_args[1:]:
                self.assertIn(line.split()[1], archive.namelist())

    def test_the_factory_image_is_every_region_at_its_offset_over_erased_flash(self) -> None:
        build = fixture.make_build(
            self.root / "c6", target="esp32c6", flash_size="4MB", psram=False
        )
        packager.package(build, "hearth-sink-esp32c6", self.out)
        factory = (self.out / "hearth-sink-esp32c6-v0.11.0-factory.bin").read_bytes()
        app = (build / "ac3forge_hearth_sink.bin").read_bytes()
        storage = (build / "storage.bin").read_bytes()
        self.assertEqual(len(factory), 0x3C0000 + len(storage))
        self.assertEqual(factory[0x20000 : 0x20000 + len(app)], app)
        self.assertEqual(factory[0x3C0000:], storage)
        # Between the bootloader and the partition table, and after the app:
        # erased flash, not zeros.
        self.assertEqual(set(factory[0x2000:0x8000]), {0xFF})
        self.assertEqual(set(factory[0x20000 + len(app) : 0x30000]), {0xFF})

    def test_packaging_one_build_twice_gives_the_same_bytes(self) -> None:
        build = fixture.make_build(self.root / "s3")
        first = packager.package(build, "hearth-sink-esp32s3", self.root / "one")
        second = packager.package(build, "hearth-sink-esp32s3", self.root / "two")
        self.assertEqual(first["files"], second["files"])

    def test_a_build_with_a_network_built_in_is_refused(self) -> None:
        build = fixture.make_build(self.root / "s3", wifi_ssid="home")
        with self.assertRaisesRegex(packager.PackageError, "Wi-Fi network built into its image"):
            packager.package(build, "hearth-sink-esp32s3", self.out)
        self.assertFalse(self.out.exists())

    def test_an_image_for_another_chip_than_the_build_is_refused(self) -> None:
        app = fixture.app_image(fixture.CHIP_IDS["esp32c6"], "v0.11.0")
        build = fixture.make_build(self.root / "s3", app=app)
        with self.assertRaisesRegex(packager.PackageError, "not an image for the ESP32-S3"):
            packager.package(build, "hearth-sink-esp32s3", self.out)

    def test_a_flash_args_that_does_not_write_the_app_is_refused(self) -> None:
        build = fixture.make_build(self.root / "s3")
        args = (
            (build / "flash_args")
            .read_text("utf-8")
            .replace("0x20000 ac3forge_hearth_sink.bin\n", "")
        )
        (build / "flash_args").write_text(args, "utf-8")
        with self.assertRaisesRegex(
            packager.PackageError, "does not write the app image exactly once"
        ):
            packager.package(build, "hearth-sink-esp32s3", self.out)

    def test_the_manifest_merges_every_fragment(self) -> None:
        for name, settings in fixture.RELEASE_IMAGES.items():
            build = fixture.make_build(self.root / name, **settings)
            packager.package(build, name, self.out)
        manifest = packager.merge_manifest(self.out)
        self.assertEqual(manifest["format"], 1)
        self.assertEqual(manifest["version"], "v0.11.0")
        self.assertEqual(
            [image["name"] for image in manifest["images"]], sorted(fixture.RELEASE_IMAGES)
        )
        written = json.loads((self.out / packager.MANIFEST_NAME).read_text("utf-8"))
        self.assertEqual(written, manifest)
        # Merged again, the manifest is not read as an image of its own.
        self.assertEqual(len(packager.merge_manifest(self.out)["images"]), 4)

    def test_main_reports_a_refusal_on_stderr_and_exits_1(self) -> None:
        build = fixture.make_build(self.root / "s3", wifi_ssid="home")
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            code = packager.main(
                [
                    "package",
                    "--build-dir",
                    str(build),
                    "--name",
                    "hearth-sink-esp32s3",
                    "--out",
                    str(self.out),
                ]
            )
        self.assertEqual(code, 1)
        self.assertIn(
            "package_firmware: this build has a Wi-Fi network built into its image",
            stderr.getvalue(),
        )


if __name__ == "__main__":
    unittest.main()
