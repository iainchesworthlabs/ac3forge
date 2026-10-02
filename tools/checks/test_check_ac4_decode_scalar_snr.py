"""Tests for tools/checks/check_ac4_decode_scalar_snr.py: the SNR of a region, and the pins.

The oracle tests run on the plain standard library (tools/checks and tools/ci are discovered with
the system python3), so the pins are read without numpy and the measurements are skipped where
numpy is not installed."""

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_ac4_decode_scalar_snr as checker

try:
    import numpy as np
except ImportError:  # the measurements need it and the pins do not
    np = None


def tone(seconds: float, hz: float, level: float):
    n = np.arange(int(seconds * 48000))
    return level * np.sin(2.0 * np.pi * hz * n / 48000)


@unittest.skipIf(np is None, "numpy is not installed")
class RegionSnr(unittest.TestCase):
    def test_a_decode_that_is_the_reference_scores_the_cap(self):
        ref = tone(1.0, 3000.0, 0.5)
        self.assertEqual(checker.region_snr(ref, ref.copy(), 0.0, 24000.0), checker.SNR_CAP_DB)

    def test_error_at_one_thousandth_scores_sixty_dB(self):
        ref = tone(1.0, 3000.0, 0.5)
        out = ref * 1.001
        snr = checker.region_snr(ref, out, 0.0, 24000.0)
        self.assertAlmostEqual(snr, 60.0, delta=0.5)

    def test_the_error_outside_the_region_does_not_count(self):
        ref = tone(1.0, 3000.0, 0.5)
        out = ref + tone(1.0, 15000.0, 0.01)
        below = checker.region_snr(ref, out, 0.0, 10000.0)
        above = checker.region_snr(ref, out, 12000.0, 24000.0)
        # The 15 kHz error is not in the low region (bar the window's leakage, a hundred and
        # fifty dB down), and the reference has nothing above 12 kHz to score.
        self.assertGreater(below, 150.0)
        self.assertIsNone(above)

    def test_a_region_the_reference_is_silent_in_is_not_scored(self):
        ref = tone(1.0, 300.0, 1e-7)
        self.assertIsNone(checker.region_snr(ref, ref * 1.1, 0.0, 24000.0))

    def test_the_error_level_is_in_dB_of_a_full_scale_sine(self):
        ref = tone(1.0, 3000.0, 0.5)
        # An error that is a full-scale sine's own energy in a frame is 0 dB; a thousandth of it
        # in amplitude is -60.
        error = checker.error_dbfs(ref, ref + tone(1.0, 5000.0, 0.001))
        self.assertAlmostEqual(error, -60.0, delta=0.5)
        self.assertEqual(checker.error_dbfs(ref, ref.copy()), -checker.SNR_CAP_DB)


class Pins(unittest.TestCase):
    PIN_FILES = (checker.PINS, checker.FIXED_PINS)

    def test_every_committed_stream_has_a_pin_and_every_pin_a_stream(self):
        committed = {checker.relative(path) for path in checker.committed_streams()}
        for path in self.PIN_FILES:
            with self.subTest(pins=path.name):
                pins = json.loads(path.read_text(encoding="utf-8"))["streams"]
                self.assertEqual(
                    sorted(committed - set(pins)), [], "a committed stream without a pin"
                )
                self.assertEqual(
                    sorted(set(pins) - committed), [], "a pin for a stream that is not there"
                )

    def test_the_pins_are_floors_in_a_plausible_range(self):
        for path in self.PIN_FILES:
            self._plausible(json.loads(path.read_text(encoding="utf-8"))["streams"], path.name)

    def _plausible(self, pins, name):
        for key, entry in pins.items():
            with self.subTest(pins=name, stream=key):
                self.assertEqual(set(entry), {"below", "above"})
                below = entry["below"]
                self.assertIsNotNone(below)
                self.assertGreater(below, 80.0)
                self.assertLessEqual(below, checker.SNR_CAP_DB)
                above = entry["above"]
                if above is not None:
                    self.assertGreater(above, 20.0)
                    self.assertLessEqual(above, checker.SNR_CAP_DB)


if __name__ == "__main__":
    unittest.main()
