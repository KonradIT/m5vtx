This is an adapted extraction of Peter Barrett's (rossumur) `esp_8_bit`
`src/video_out.h`, revision `55eb7b86eda290d96b02c38c3e787efb8ae6a8c0`:
https://github.com/rossumur/esp_8_bit/blob/55eb7b86eda290d96b02c38c3e787efb8ae6a8c0/src/video_out.h

Its direct I2S0 DMA setup, audio PLL coefficients and DAC sample-pair ordering
are retained. Its 262-line progressive raster is not: this one is broadcast NTSC,
or PAL on request, see below. Upstream's PAL line is not used either. Emulator, IR
and all audio code are removed. The framebuffer is indexed 8-bit with either RGB332 or
a clip-specific RGB palette converted to YUV waveforms. Each framebuffer owns
its waveform table; pixels and their palette become active at the same vblank.
Palette calculations happen outside the ISR and are cached per buffer. The
GPIO26/DAC2 route is selected in hardware; changing an upstream
`VIDEO_PIN` macro alone did not change its hardcoded GPIO25 DAC selection.
GPIO25 DAC output remains disabled. No speaker or PWM peripheral is started.

This targets classic ESP32 with Arduino-ESP32 2.x / ESP-IDF 4.4. It uses low
level registers rather than the installed I2S driver and owns I2S0/APLL.
The I2S ISR and every object it reads reside in internal RAM.

The raster follows SMPTE 170M and, where it was measured, a real FPV camera:
910 samples/line at 14.318186442 MS/s (four per color cycle, 227.5 cycles/line,
63.556 us), 525 lines/frame, two interlaced fields, 29.97 frames/s.

| | Samples | us | Camera (measured, us) |
| --- | --- | --- | --- |
| Horizontal sync | 67 | 4.68 | 4.78 |
| Equalizing pulse, 6 before and 6 after each vertical sync | 34 | 2.37 | 2.45 |
| Broad pulse, 6 per field | 387 | 27.03 | 27.08 |
| Burst start, 9 cycles, on lines 10..263 and 273..525 | 76 | 5.31 | later, see below |
| Picture start | 130 | 9.08 | about 9.0 |
| Front porch | 21 | 1.47 | about 1.2 |

Figures are from one NTSC camera, through a VTX and the C5VRX P4 receiver, measured the
same way as this generator's output; a pulse measures about 0.07 us long there. Its burst
started and ended 0.6 to 0.9 us later than the standard's, and at 48 IRE peak to peak
against the standard's 40. Here burst keeps to the standard. Field 1 has its picture on lines
23..262 and field 2 on 286..525, as on that camera; both show the same 240 framebuffer rows.
Columns 1..253 of 256 are sent: 768 samples do not fit between the porches, and the others
are in the overscan on any display. A queued image starts with the next field.

The DMA ring holds four lines. With upstream's two, the FreeRTOS tick alone made the ISR miss
a line every millisecond, and the DMA sent the stale one again: fields came out 280 lines long
instead of 262.5. The ISR takes 36.5 us per line on average and 42.7 at most (8,770 and 10,250
cycles at 240 MHz), 57.5 % of its core.

DAC codes: sync 14, blank 33, black 36, white 79, burst amplitude 16. These were measured
through the VTX, with a DAC staircase and the FM deviation at the receiver. Codes up to about
12 all give the same output level. With sync at code 0, on that floor, the sync tip wandered
by a fifth of the pulse over seconds while everything else held; at code 14 it stays within
0.02 of 8.9 phase units per sample for a minute. Above the floor a code is about 0.46 phase
units per 25 ns sample (0.07 MHz). The
subcarrier arrives at about 0.61 of what the samples say, in the DAC and the VTX's input,
so burst and chroma are raised by 1.74 to arrive at their nominal size; the most saturated
colors reach the floor at their peaks. Chroma also arrived about 8 degrees behind the burst
in raw samples at the receiver, with no shear between U and V (each bar 3 to 10.5 degrees,
more on large and bright colors), so it is sent 8 degrees ahead. Whether that lag is in the
DAC or the VTX is not known. Normalized RGB is converted
to `Y=.299R+.587G+.114B`, `U=.492(B-Y)`, `V=.877(R-Y)`, rotated by those 8 degrees. The four
subcarrier samples are `Y+U`, `Y-V`, `Y-U`, `Y+V` with that gain on U and V, scaled over the
black-to-white range. Burst phases are `-A,0,+A,0` around blank, half that in the first and
last cycle. The phase turns over from line to line. DMA word pairs
are reversed to compensate the ESP32 mono I2S FIFO ordering.

The quadrature direction follows SMPTE 170M section 10's
`U*sin(t)+V*cos(t)` with burst `-sin(t)`, rotated 90 degrees to our cosine
burst reference: `U*cos(t)-V*sin(t)`.
https://pub.smpte.org/pub/st170/st0170-2004_stable2010.pdf

## PAL

`setStandard()` stops the raster, retunes the audio PLL to 17.734477 MS/s, rebuilds both
palette banks and starts over; a receiver has to lock again. This follows ITU-R BT.470
system B/G at four samples per color cycle: 625 lines, two interlaced fields, 25 frames/s,
288 rows per field on lines 23..310 and 336..623, with half lines at the first and the last.

| | Samples | us | Standard, us |
| --- | --- | --- | --- |
| Line | 1135 | 63.9996 | 64 |
| Horizontal sync | 83 | 4.68 | 4.7 |
| Equalizing pulse, 5 before and 5 after each vertical sync | 42 | 2.37 | 2.35 |
| Broad pulse, 5 per field | 484 | 27.29 | 27.3 |
| Burst start, 10 cycles | 99 | 5.58 | 5.6 |
| Picture start | 186 | 10.49 | 10.5 |
| Picture | 922 | 51.99 | 52 |
| Front porch | 27 | 1.52 | 1.5 |

Upstream's line is 1136 samples, 284 cycles and 64.056 us, because I2S DMA takes whole 32-bit
words. That is 870 ppm long, and the P4 grabber tracks a line only within 500 ppm of nominal, so
this one is 283.75 cycles. Buffers alternate between 1136 samples, ending in the next line's
first, and 1134 samples, which start on a line's second. Sync edges stay exactly 1135 apart,
and only the half-line pulses move, by half a sample. The standard's extra 25 Hz on the
subcarrier is left out, so the phase repeats every four frames and the line is 5.7 ppm short.

Levels are the same volts per code as NTSC's: sync 14, and blanking 33 is also black, with no
setup. 300 mV of sync is 19 codes, so 700 mV of picture is 44 and white is 77. The burst swings
45 degrees either way from -U and is the size of NTSC's. At four samples per cycle on those
axes it reads `-a,-a,+a,+a` or `-a,+a,+a,-a`, with `a` of 11 codes. A line with V inverted reads
the same waveform table backwards, so there is no second table. Burst is off on nine lines around
each interval (Bruch blanking), moved by a line every other field, so that every field's first and
last bursts swing the same way.

The 8 degree lead is not used on PAL: its V switch turns a lag into two opposite hue errors that a
receiver's mean of two lines removes, leaving saturation times cos 8 degrees. Reversing the table
would also turn the lead into a lag on every other line.

The chroma boost of 1.74 and the burst size are the measurements at 3.58 MHz, not repeated at
4.43 MHz, where the DAC's sample-and-hold has the same loss at this higher rate but the VTX's input
may not. The most saturated colors reach the output floor, as they do on NTSC.

256 columns fill the picture at 18 samples per 5 columns (4, 4, 3, 4, 3). Written a sample at a time
with the phase worked out for each, as on NTSC, that averaged 65 us per line and peaked at 79, over
the 64 a line has. Whole trips around the DMA ring were lost, and frames came at 22.8/s. So a group
of five columns is unrolled into plain loads and stores, and each color's table holds its four
samples twice, for a column to read four neighbors from where the line's phase says. The ISR now
takes 25.8 us per line on average and 37.2 at most, 40.3 % of its core, with 15,626 lines/s measured.

PAL support costs 30,480 bytes of RAM at all times, whichever standard is on: framebuffers of 256x288
(24,576), the longer tables (4,096) and longer DMA buffers (1,808), and 1,944 for the gratings on top.

`SampleBand` lets rows take their middle columns from prepared DAC samples. It is what carries
definition lines finer than a column.
