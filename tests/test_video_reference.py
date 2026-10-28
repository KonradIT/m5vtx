"""Independent checks against NTSC signal specifications and color vectors.

These exercise the palette/waveform exporter, not the receiver's inverse
color conversion. Values come from SMPTE 170M section 10 and Tektronix's
NTSC vector diagram, rather than being recomputed from exporter coefficients:
https://pub.smpte.org/pub/st170/st0170-2004_stable2010.pdf
https://download.tek.com/manual/070763504_0.pdf (figure 2-8)

The raster intentionally retains esp_8_bit's 262p/228-clock progressive
timing, so its refresh is approximately 59.923 Hz rather than broadcast
interlaced NTSC's 59.94 fields/s.
"""
from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from render_reference import SAMPLE_RATE, composite_frame, dac_palette, test_card as render_card


class VideoReferenceTests(unittest.TestCase):
    def test_ntsc_hue_vectors(self):
        """A V-sign reversal would mirror hues and fail these standard targets."""
        palette = dac_palette().astype(float)
        # Positive-frequency Fourier coefficient: no color-matrix inversion.
        oscillator = np.exp(-2j * np.pi * np.arange(4) / 4)
        phasors = palette @ oscillator
        targets = {224: 104, 28: 241, 3: 347, 31: 284, 252: 167, 227: 61}
        for index, degrees in targets.items():
            with self.subTest(rgb332=index):
                angle = np.angle(phasors[index], deg=True) % 360
                error = (angle - degrees + 180) % 360 - 180
                self.assertLess(abs(error), 1.6)  # 8-bit DAC phase quantization

    def test_luminance_and_neutral_colors(self):
        palette = dac_palette().astype(float)
        # SMPTE setup 7.5 IRE, nominal white100, and 100% primary luminances.
        # Code levels round those analog values to an 8-bit DAC.
        luma_ire = (palette.mean(axis=1) - 20) / 53 * 100
        targets = {0: 7.5, 255: 100, 224: 35.1575, 28: 61.7975, 3: 18.045}
        for index, ire in targets.items():
            with self.subTest(rgb332=index):
                self.assertAlmostEqual(luma_ire[index], ire, delta=1.1)
        self.assertEqual(np.ptp(palette[0]), 0)
        self.assertEqual(np.ptp(palette[255]), 0)

    def test_sync_burst_and_progressive_refresh(self):
        raster = composite_frame(render_card()).reshape(262, -1)
        self.assertTrue(np.all(raster[:240, :64] == 0))
        self.assertTrue(np.all(raster[245:248, :840] == 0))
        self.assertTrue(np.all(raster[245:248, 840:] == 20))
        burst = raster[0, 80:120].astype(float)
        self.assertEqual(burst.mean(), 20)
        # Burst is opposite the U axis (180 degrees), 10 carrier cycles.
        carrier = np.exp(-2j * np.pi * np.arange(len(burst)) / 4)
        phasor = np.dot(burst - burst.mean(), carrier)
        self.assertAlmostEqual(abs(np.angle(phasor, deg=True)), 180, delta=.01)
        burst_pp_ire = np.ptp(burst) / 53 * 100
        self.assertAlmostEqual(burst_pp_ire, 40, delta=2.5)
        self.assertAlmostEqual(SAMPLE_RATE / 4, 315e6 / 88, delta=2)
        self.assertAlmostEqual(SAMPLE_RATE / raster.size, 59.923, delta=.002)
        self.assertAlmostEqual(raster.shape[1] / SAMPLE_RATE * 1e6, 63.695, delta=.002)

    def test_palette_and_image_do_not_clip(self):
        palette = dac_palette()
        # Saturated colors may go below black or above white. They must retain
        # their peaks without hitting the DAC rails (which would distort hue).
        self.assertGreater(int(palette.min()), 0)
        self.assertLess(int(palette.max()), 255)
        self.assertLess(int(palette.min()), 24)
        self.assertGreater(int(palette.max()), 73)
        for pattern in range(3):
            pixels = render_card(pattern)
            self.assertEqual(pixels.shape, (240, 256))
            self.assertEqual(pixels.dtype, np.uint8)
            frame = composite_frame(pixels).reshape(262, 912)
            # Every active pixel fits before the next line's sync pulse.
            self.assertTrue(np.all(frame[:240, 904:] == 20))
        self.assertEqual(len(np.unique(render_card(1))), 256)
        self.assertEqual(set(np.unique(render_card(2))), {0, 255})


if __name__ == "__main__":
    unittest.main()
