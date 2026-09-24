"""Unit tests for check_abi_symbols.py, the exported-ABI allowlist gate.

What the gate must catch: a symbol newly exported (or one silently dropped)
relative to the checked-in allowlist must exit 1 and name the symbol, while
libstdc++/ABI-runtime template noise - including the const-member `_ZNKSt`
form that once leaked through - must be filtered BEFORE demangling so it can
never register as project surface.

nm and c++filt are not run: subprocess.run is patched with a fake that
serves canned `nm -D --defined-only` output and a trivial "demangler".

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_abi_symbols as abi


class FakeTools:
    """Stands in for nm (per-library canned listing) and c++filt (prefixes
    'D:' so the test can see only survivors reached the demangler)."""

    def __init__(self, listings):
        self.listings = listings
        self.demangle_inputs = []

    def __call__(self, cmd, **kwargs):
        if cmd[0] == "nm":
            self.assertion = cmd[:3]
            return subprocess.CompletedProcess(cmd, 0, stdout=self.listings[Path(cmd[3]).name])
        if cmd[0] == "c++filt":
            self.demangle_inputs.append(kwargs["input"])
            out = "\n".join("D:" + n for n in kwargs["input"].splitlines())
            return subprocess.CompletedProcess(cmd, 0, stdout=out)
        raise AssertionError(cmd)


NM = """\
0000000000001000 T _ZN7ac3forge6encodeEv
0000000000001010 T _ZN7ac3forge6encodeEv
0000000000001020 W _ZNSt6vectorIiSaIiEE9push_backEv
0000000000001030 W _ZNKSt10_Hashtable4findEv
0000000000001040 V _ZTVSt9exception
0000000000001050 W _ZN9__gnu_cxx13new_allocatorEv
0000000000001060 T ac3forge_c_open
                 U malformed
"""


class ExportedSymbols(unittest.TestCase):
    def test_stdlib_is_filtered_on_mangled_name_and_rest_deduplicated(self):
        fake = FakeTools({"libx.so": NM})
        with mock.patch.object(abi.subprocess, "run", fake):
            symbols = abi.exported_symbols(Path("/b/libx.so"))
        self.assertEqual(symbols, ["D:_ZN7ac3forge6encodeEv", "D:ac3forge_c_open"])
        self.assertEqual(fake.assertion, ["nm", "-D", "--defined-only"])
        self.assertNotIn("_ZNKSt", fake.demangle_inputs[0])

    def test_all_filtered_skips_demangler(self):
        fake = FakeTools({"libx.so": "0 W _ZNSt3fooEv\n"})
        with mock.patch.object(abi.subprocess, "run", fake):
            self.assertEqual(abi.exported_symbols(Path("libx.so")), [])
        self.assertEqual(fake.demangle_inputs, [])


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.allow = Path(self._tmp.name) / "allow"

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, listings, *extra):
        argv = ["check_abi_symbols.py", "--allowlist-dir", str(self.allow)]
        for name in listings:
            argv += ["--lib", f"/build/{name}"]
        buf = io.StringIO()
        with mock.patch.object(abi.subprocess, "run", FakeTools(listings)), \
                mock.patch.object(sys, "argv", argv + list(extra)), \
                contextlib.redirect_stdout(buf):
            rc = abi.main()
        return rc, buf.getvalue()

    def test_update_then_check_round_trips(self):
        rc, out = self.run_main({"liba.so": NM, "libempty.so": ""}, "--update")
        self.assertEqual(rc, 0)
        self.assertIn("Wrote 2 symbols for liba.so", out)
        self.assertEqual((self.allow / "liba.so.txt").read_text(),
                         "D:_ZN7ac3forge6encodeEv\nD:ac3forge_c_open\n")
        self.assertEqual((self.allow / "libempty.so.txt").read_text(), "")
        rc, out = self.run_main({"liba.so": NM})
        self.assertEqual(rc, 0, out)
        self.assertIn("OK  liba.so: 2 exported symbols", out)
        self.assertIn("All libraries match", out)

    def test_new_and_removed_exports_fail(self):
        self.allow.mkdir(parents=True)
        (self.allow / "liba.so.txt").write_text("D:ac3forge_c_open\nD:ac3forge_gone\n")
        rc, out = self.run_main({"liba.so": NM})
        self.assertEqual(rc, 1)
        self.assertIn("MISMATCH  liba.so vs liba.so.txt", out)
        self.assertIn("::warning::+ D:_ZN7ac3forge6encodeEv (newly exported", out)
        self.assertIn("::warning::- D:ac3forge_gone (allowlisted, no longer exported)", out)
        self.assertIn("regenerate the allowlist with --update", out)

    def test_library_without_allowlist_fails(self):
        """A brand-new library is not silently accepted."""
        rc, out = self.run_main({"libnew.so": NM})
        self.assertEqual(rc, 1)
        self.assertIn("MISMATCH  libnew.so", out)


if __name__ == "__main__":
    unittest.main()
