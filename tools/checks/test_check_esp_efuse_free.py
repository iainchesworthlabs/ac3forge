"""Unit tests for check_esp_efuse_free.py, the USB-recovery settings check.

stdlib `unittest`, not pytest, for the same reason the script itself is
stdlib-only: this runs in ci.yml's script-lint job, which installs ruff,
shellcheck and actionlint and nothing else.

Each test builds a small temporary tree and runs the check over it, so the
cases are the rules the script's header states: a fragment that turns a
refused option on fails and names the file, the line and the option; the forms
for off pass; every refused option is caught; a build's own sdkconfig and the
directories a build or the component manager writes are not read; both
project roots are read; and a tree with no fragments fails rather than passing
vacuously.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_esp_efuse_free

HEARTH = "esp-idf/ac3forge/examples/hearth_sink"


def _write(root: Path, relative: str, text: str) -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


class EfuseFreeCheck(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def _run(self) -> tuple[int, str]:
        argv = sys.argv
        sys.argv = ["check_esp_efuse_free.py", "--root", str(self.root)]
        buffer = io.StringIO()
        try:
            with redirect_stdout(buffer):
                code = check_esp_efuse_free.main()
        finally:
            sys.argv = argv
        return code, buffer.getvalue()

    def test_clean_fragments_pass(self) -> None:
        _write(self.root, f"{HEARTH}/sdkconfig.defaults", 'CONFIG_IDF_TARGET="esp32s3"\n')
        _write(self.root, f"{HEARTH}/sdkconfig.hw", "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y\n")
        code, out = self._run()
        self.assertEqual(code, 0, out)
        self.assertIn("2 fragment(s) read, 0 refused setting(s)", out)

    def test_refused_option_fails_and_names_file_line_and_option(self) -> None:
        _write(
            self.root,
            f"{HEARTH}/sdkconfig.release",
            "# a release overlay\nCONFIG_SECURE_FLASH_ENC_ENABLED=y\n",
        )
        code, out = self._run()
        self.assertEqual(code, 1)
        self.assertIn(
            f"::error file={HEARTH}/sdkconfig.release,line=2::CONFIG_SECURE_FLASH_ENC_ENABLED=y",
            out,
        )
        self.assertIn("1 refused setting(s)", out)

    def test_every_refused_option_is_caught(self) -> None:
        text = "".join(f"{option}=y\n" for option in check_esp_efuse_free.REFUSED)
        _write(self.root, f"{HEARTH}/sdkconfig.everything", text)
        code, out = self._run()
        self.assertEqual(code, 1)
        for option in check_esp_efuse_free.REFUSED:
            self.assertIn(f"::{option}=y", out)
        self.assertIn(f"{len(check_esp_efuse_free.REFUSED)} refused setting(s)", out)

    def test_the_forms_for_off_pass(self) -> None:
        _write(
            self.root,
            f"{HEARTH}/sdkconfig.defaults",
            "# CONFIG_SECURE_BOOT is not set\nCONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n\n",
        )
        code, out = self._run()
        self.assertEqual(code, 0, out)

    def test_whitespace_around_the_value_still_counts(self) -> None:
        _write(self.root, f"{HEARTH}/sdkconfig.defaults", "CONFIG_SECURE_BOOT = y \n")
        code, out = self._run()
        self.assertEqual(code, 1)
        self.assertIn("CONFIG_SECURE_BOOT=y", out)

    def test_an_option_whose_name_only_starts_the_same_passes(self) -> None:
        # CONFIG_SECURE_BOOT_V2_RSA_SUPPORTED-style names share a prefix with a
        # refused option but are not it.
        _write(
            self.root,
            f"{HEARTH}/sdkconfig.defaults",
            "CONFIG_SECURE_BOOT_V2_RSA_SUPPORTED=y\nCONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y\n",
        )
        code, out = self._run()
        self.assertEqual(code, 0, out)

    def test_a_builds_own_sdkconfig_and_build_directories_are_not_read(self) -> None:
        _write(self.root, f"{HEARTH}/sdkconfig.defaults", "CONFIG_FREERTOS_HZ=1000\n")
        _write(self.root, f"{HEARTH}/sdkconfig", "CONFIG_SECURE_BOOT=y\n")
        _write(self.root, f"{HEARTH}/sdkconfig.old", "CONFIG_SECURE_BOOT=y\n")
        _write(self.root, f"{HEARTH}/build/config/sdkconfig.json", "CONFIG_SECURE_BOOT=y\n")
        _write(self.root, f"{HEARTH}/build-p4/sdkconfig.cmake", "CONFIG_SECURE_BOOT=y\n")
        _write(
            self.root,
            f"{HEARTH}/managed_components/espressif__x/examples/a/sdkconfig.defaults",
            "CONFIG_SECURE_BOOT=y\n",
        )
        code, out = self._run()
        self.assertEqual(code, 0, out)
        self.assertIn("1 fragment(s) read", out)

    def test_the_baremetal_projects_are_read_too(self) -> None:
        _write(self.root, f"{HEARTH}/sdkconfig.defaults", "CONFIG_FREERTOS_HZ=1000\n")
        _write(
            self.root,
            "apps/baremetal/platform/esp32c6/sdkconfig.defaults",
            "CONFIG_SECURE_DISABLE_ROM_DL_MODE=y\n",
        )
        code, out = self._run()
        self.assertEqual(code, 1)
        self.assertIn("file=apps/baremetal/platform/esp32c6/sdkconfig.defaults,line=1", out)

    def test_no_fragments_fails_rather_than_passing_vacuously(self) -> None:
        (self.root / "esp-idf").mkdir()
        code, out = self._run()
        self.assertEqual(code, 1)
        self.assertIn("no sdkconfig fragments found", out)


if __name__ == "__main__":
    unittest.main()
