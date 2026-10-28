"""Receiver tests use independently generated analog composite, not decoder internals."""
import sys
from pathlib import Path
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from sdr_video import decode_ntsc, fm_demodulate


COLORS = np.array([[1,1,1], [1,1,0], [0,1,1], [0,1,0],
                   [1,0,1], [1,0,0], [0,0,1], [0,0,0]], float)


def analog_bars(rate=14e6, lines=290, phase=.71, polarity=1, noise=.001, chroma_response=1):
    """Make 262p NTSC with fractional line lengths and arbitrary carrier phase."""
    line_s = 912/14_318_180
    t = np.arange(int(lines*line_s*rate))/rate
    line = np.floor(t/line_s).astype(int)
    lt = t-line*line_s
    cvbs = np.zeros(len(t))
    cvbs[lt<4.47e-6] = -.4
    wave = 2*np.pi*3_579_545*t+phase
    burst = (lt>5.59e-6)&(lt<8.38e-6)
    cvbs[burst] = -.2*np.cos(wave[burst])*chroma_response
    active = (lt>9.498e-6)&(lt<63.137e-6)&(line%262<240)
    idx = np.clip(((lt-9.498e-6)/(53.639e-6)*8).astype(int),0,7)
    r,g,b = COLORS[idx].T
    # Encode direct coefficients rather than call an inverse decoder helper.
    y = .299*r+.587*g+.114*b
    u = -.147108*r-.288804*g+.435912*b
    v = .614777*r-.514799*g-.099978*b
    # SMPTE170M: with burst=-cos(wt), U is cos and V is negative sin.
    cvbs[active] = .075+y[active]+chroma_response*(u[active]*np.cos(wave[active])-v[active]*np.sin(wave[active]))
    # Serrated vertical-sync interval, followed by regular blanking rows.
    cvbs[((line%262>=245)&(line%262<248))&(lt<30e-6)] = -.4
    return polarity*cvbs+np.random.default_rng(12).normal(0,noise,len(t))


class DecoderTests(unittest.TestCase):
    def check_bars(self, frame):
        measured=np.stack([frame[40:200,x-4:x+4].mean(axis=(0,1))/255 for x in range(16,256,32)])
        np.testing.assert_allclose(measured,COLORS,atol=.09)

    def test_color_reconstruction_and_negative_fm_polarity(self):
        for polarity in (1,-1):
            cvbs=analog_bars(polarity=polarity)
            frame,metrics,_=decode_ntsc(cvbs,14e6)
            self.check_bars(frame)
            self.assertEqual(metrics["fm_polarity"],polarity)
            self.assertGreater(metrics["burst_lock_fraction"],.95)
            self.assertAlmostEqual(metrics["line_period_us"],63.695,delta=.08)
            self.assertAlmostEqual(metrics["subcarrier_hz"],3_579_545,delta=500)
            self.assertGreater(metrics["color_pixel_fraction"],.7)

    def test_fm_round_trip_preserves_chroma_and_dc_offset(self):
        fs=28e6
        original=analog_bars(rate=fs,noise=0)
        # Moderate FM deviation and 1.2MHz receiver tuning error.
        phase=np.cumsum(original*.35+2*np.pi*1.2e6/fs)
        iq=np.exp(1j*phase).astype(np.complex64)
        rng=np.random.default_rng(35)
        iq+=.002*(rng.normal(size=len(iq))+1j*rng.normal(size=len(iq)))
        cvbs,rate=fm_demodulate(iq,fs)
        frame,metrics,_=decode_ntsc(cvbs,rate)
        self.check_bars(frame)
        self.assertGreater(metrics["burst_amplitude_ire"],18)

    def test_noise_does_not_claim_video_sync(self):
        with self.assertRaisesRegex(ValueError,"No stable horizontal sync"):
            decode_ntsc(np.random.default_rng(3).normal(size=500000),14e6)

    def test_burst_agc_compensates_chroma_passband_loss(self):
        frame,metrics,_=decode_ntsc(analog_bars(chroma_response=.5),14e6)
        self.check_bars(frame)
        self.assertAlmostEqual(metrics["chroma_agc_gain"],2,delta=.05)

    def test_luma_alone_does_not_claim_color_lock(self):
        with self.assertRaisesRegex(ValueError,"No color burst lock"):
            decode_ntsc(analog_bars(chroma_response=0),14e6)

    def test_short_capture_fails_explicitly(self):
        with self.assertRaisesRegex(ValueError,"Need at least"):
            decode_ntsc(analog_bars(lines=100),14e6)


if __name__=="__main__":
    unittest.main()
