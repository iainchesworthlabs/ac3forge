"""Unit tests for check_firmware_package.py, the gate on published hearth_sink images.

stdlib unittest, as the script-lint job runs it. The packages are made the way
CI makes them: tools/hearth/sink_build_fixture.py builds a hearth_sink build
directory from nothing and tools/hearth/package_firmware.py packages it. Each
test then breaks one promise an image's name makes and holds the gate to
finding it: the release set whole, the chip and the revision range, the flash
size and PSRAM, the image's own checks, the smallest slot, the parts against
the factory image, the version, a network built in, and the files the
manifest describes.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "hearth"))

import check_firmware_package as gate  # noqa: E402
import package_firmware as packager  # noqa: E402
import sink_build_fixture as fixture  # noqa: E402


class Gate(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.out = self.root / "out"

    def tearDown(self) -> None:
        self.dir.cleanup()

    def release(self, **overrides: dict) -> None:
        """The four images a release publishes, each built with its overrides, and the manifest."""
        for name, settings in fixture.RELEASE_IMAGES.items():
            build = fixture.make_build(self.root / name, **{**settings, **overrides.get(name, {})})
            packager.package(build, name, self.out)
        packager.merge_manifest(self.out)

    def run_gate(self, *args: str) -> tuple[int, str]:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = gate.main([str(self.out), *args])
        return code, output.getvalue()

    def manifest(self) -> dict:
        return json.loads((self.out / gate.MANIFEST_NAME).read_text("utf-8"))

    def rewrite_manifest(self, change) -> None:
        manifest = self.manifest()
        change(manifest)
        (self.out / gate.MANIFEST_NAME).write_text(json.dumps(manifest), "utf-8")

    def image(self, name: str) -> dict:
        return next(image for image in self.manifest()["images"] if image["name"] == name)

    def test_a_whole_release_passes(self) -> None:
        self.release()
        code, output = self.run_gate()
        self.assertEqual(code, 0, output)
        for name in fixture.RELEASE_IMAGES:
            self.assertIn(f"OK  {name} v0.11.0", output)

    def test_every_published_image_has_to_be_there(self) -> None:
        self.release()
        self.rewrite_manifest(
            lambda m: m.update(
                images=[i for i in m["images"] if i["name"] != "hearth-sink-esp32c6"]
            )
        )
        code, output = self.run_gate()
        self.assertEqual(code, 1)
        self.assertIn(
            "hearth-sink-esp32c6 is not in the manifest: every release publishes it", output
        )
        self.assertEqual(self.run_gate("--allow-missing")[0], 0)

    def test_an_image_without_a_rule_is_refused(self) -> None:
        build = fixture.make_build(self.root / "h2", target="esp32s3")
        packager.package(build, "hearth-sink-esp32h2", self.out)
        packager.merge_manifest(self.out)
        code, output = self.run_gate("--allow-missing")
        self.assertEqual(code, 1)
        self.assertIn("no rule for an image named 'hearth-sink-esp32h2'", output)

    def test_an_image_under_another_chips_name_is_refused(self) -> None:
        self.release(**{"hearth-sink-esp32c6": {"target": "esp32s3"}})
        code, output = self.run_gate()
        self.assertEqual(code, 1)
        self.assertIn("hearth-sink-esp32c6: it is for esp32s3, and its name says esp32c6", output)

    def test_a_p4_image_that_would_take_v3_silicon_is_refused(self) -> None:
        self.release(**{"hearth-sink-esp32p4-rev1": {"max_rev": 399}})
        code, output = self.run_gate()
        self.assertEqual(code, 1)
        self.assertIn("hearth-sink-esp32p4-rev1: it accepts chip revisions above 199", output)

    def test_an_image_that_leaves_out_revisions_its_name_promises_is_refused(self) -> None:
        self.release(**{"hearth-sink-esp32s3": {"min_rev": 2}})
        code, output = self.run_gate()
        self.assertEqual(code, 1)
        self.assertIn("hearth-sink-esp32s3: it accepts chip revisions 2 to 99", output)

    def test_the_wrong_flash_size_or_psram_is_refused(self) -> None:
        self.release(
            **{
                "hearth-sink-esp32c6-16mb": {"flash_size": "4MB"},
                "hearth-sink-esp32s3": {"psram": False},
            }
        )
        code, output = self.run_gate()
        self.assertEqual(code, 1)
        self.assertIn(
            "hearth-sink-esp32c6-16mb: it was built for another flash size than 16MB", output
        )
        self.assertIn("hearth-sink-esp32s3: its PSRAM setting is not its board's", output)

    def test_a_damaged_app_is_refused(self) -> None:
        self.release()
        entry = self.image("hearth-sink-esp32s3")["files"]["app"]
        path = self.out / entry["name"]
        data = bytearray(path.read_bytes())
        data[600] ^= 0x01
        path.write_bytes(bytes(data))
        code, output = self.run_gate()
        self.assertEqual(code, 1)
        self.assertIn(f"{entry['name']} is not the file the manifest describes", output)
        self.assertIn("its app image does not check out: its SHA-256 does not match", output)

    def test_parts_that_do_not_flash_the_factory_image_are_refused(self) -> None:
        self.release()
        entry = self.image("hearth-sink-esp32s3")["files"]["parts"]
        path = self.out / entry["name"]
        with zipfile.ZipFile(path) as archive:
            members = {name: archive.read(name) for name in archive.namelist()}
        members["flash_args"] = members["flash_args"].replace(
            b"0x870000 storage.bin", b"0x860000 storage.bin"
        )
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in members.items():
                archive.writestr(name, data)
        self.rewrite_manifest(lambda m: None)
        code, output = self.run_gate()
        self.assertEqual(code, 1)
        self.assertIn(
            "its parts.zip, laid out as its flash_args says, is not its factory image", output
        )

    def test_an_app_too_big_for_its_smallest_slot_is_refused(self) -> None:
        app = fixture.app_image(
            fixture.CHIP_IDS["esp32c6"], "v0.11.0", flash_code=2, segment_bytes=0x1C0000
        )
        self.release(**{"hearth-sink-esp32c6": {"app": app}})
        code, output = self.run_gate()
        self.assertEqual(code, 1)
        self.assertIn(
            "hearth-sink-esp32c6: its app is 1,835,088 bytes, and the smallest slot", output
        )

    def test_a_network_built_in_is_refused(self) -> None:
        self.release()
        self.rewrite_manifest(
            lambda m: [
                i.update(network_built_in=True)
                for i in m["images"]
                if i["name"] == "hearth-sink-esp32s3"
            ]
        )
        code, output = self.run_gate()
        self.assertEqual(code, 1)
        self.assertIn("hearth-sink-esp32s3: it has a Wi-Fi network built in", output)

    def test_a_version_the_image_does_not_carry_is_refused(self) -> None:
        self.release()
        self.rewrite_manifest(
            lambda m: [
                i.update(version="v0.11.1")
                for i in m["images"]
                if i["name"] == "hearth-sink-esp32s3"
            ]
        )
        code, output = self.run_gate()
        self.assertEqual(code, 1)
        self.assertIn(
            "the image carries version 'v0.11.0', and the manifest says 'v0.11.1'", output
        )
        self.assertIn("does not carry its version, v0.11.1", output)

    def test_a_missing_file_is_named(self) -> None:
        self.release()
        entry = self.image("hearth-sink-esp32p4-rev1")["files"]["elf"]
        (self.out / entry["name"]).unlink()
        code, output = self.run_gate()
        self.assertEqual(code, 1)
        self.assertIn(
            f"hearth-sink-esp32p4-rev1: its elf file '{entry['name']}' is not there", output
        )


if __name__ == "__main__":
    unittest.main()
