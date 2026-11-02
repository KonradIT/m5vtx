# The signal the board sends

Sources in the M5VTX checkout: `lib/esp8bit_video/README.md` holds the measurements, `README.md` the card list.

## Timing

NTSC follows SMPTE 170M, with four samples per color cycle. Fields are interlaced, and both show the same 240 framebuffer rows.

| NTSC | Samples | us |
| --- | --- | --- |
| Line, at 14.318186 MS/s | 910 | 63.556 |
| Horizontal sync | 67 | 4.68 |
| Equalizing pulse, 6 before and 6 after each vertical sync | 34 | 2.37 |
| Broad pulse, 6 per field | 387 | 27.03 |
| Burst start, 9 cycles | 76 | 5.31 |
| Picture start | 130 | 9.08 |
| Front porch | 21 | 1.47 |

525 lines, 29.97 frames/s, 227.5 color cycles per line. Field 1 has picture on lines 23..262, and field 2 on 286..525. Burst runs on lines 10..263 and 273..525. Columns 1..253 of 256 are sent.

PAL follows ITU-R BT.470 B/G, also at four samples per color cycle.

| PAL | Samples | us |
| --- | --- | --- |
| Line, at 17.734477 MS/s | 1135 | 63.9996 |
| Horizontal sync | 83 | 4.68 |
| Equalizing pulse, 5 before and 5 after each vertical sync | 42 | 2.37 |
| Broad pulse, 5 per field | 484 | 27.29 |
| Burst start, 10 cycles | 99 | 5.58 |
| Picture start | 186 | 10.49 |
| Picture | 922 | 51.99 |
| Front porch | 27 | 1.52 |

625 lines, 25 frames/s, 283.75 color cycles per line, with 288 rows per field on lines 23..310 and 336..623. The V switch and Bruch blanking are present. The standard's extra 25 Hz on the subcarrier is left out, so the phase repeats every four frames and the line is 5.7 ppm short.

## Levels

These are 8-bit DAC codes on GPIO26.

| | Sync | Blank | Black | White | Burst |
| --- | --- | --- | --- | --- | --- |
| NTSC | 14 | 33 | 36 | 79 | amplitude 16 |
| PAL | 14 | 33 | 33 | 77 | `a` of 11 codes on two axes |

- Codes up to about 12 all give the same output level, so that is the floor. Sync stays above it on purpose.
- There is no output amplifier or 75 ohm driver. Levels were set by FM deviation at the receiver, through one DarwinFPV VTX, with a DAC staircase. A scope reading at GPIO26 depends on the load, and will not match a 1 V into 75 ohms specification.
- The DAC and VTX input pass the subcarrier at about 0.61 of what the samples say, so burst and chroma are raised by 1.74. The most saturated colors reach the floor at their peaks.
- NTSC chroma arrived about 8 degrees behind burst at the receiver, so it is sent 8 degrees ahead. It is not known whether the lag is in the DAC or the VTX. PAL does not use this lead.

## Comparing against the ideal

`referencerender` is built by `make referencerender` in the checkout.

```sh
referencerender -pattern 3 -output smpte.png -waveform smpte.u8
```

- The PNG is the card as the television shows it, for comparing against a grabbed frame.
- The waveform is one frame of raw unsigned bytes, one per sample. They are the DAC codes above, at 14.318186 MS/s for NTSC and 17.734477 MS/s for PAL. It is what the DAC is told to send, and not a measurement of what comes out.

For comparison, a real camera measured through a grabber: on one NTSC camera through the same VTX and a C5VRX P4 receiver, sync measured 4.78 us, equalizing 2.45, broad 27.08, picture start about 9.0, and front porch about 1.2. The same path reads pulses about 0.07 us long. Burst on that camera started 0.6 to 0.9 us late, at 48 IRE peak to peak against the standard's 40.

## Movies

Movies are silent 256x240 NTSC, one byte per pixel, at 1 to 30 fps, in the root of a FAT32 card, up to 32 of them. 15 fps plays with no late frames on the cards tried. 20 fps was marginal, since a frame read takes about 43 ms at the card's 20 MHz SPI. The raster runs on regardless, and a late frame repeats the previous one.

`convertvideo` needs `ffmpeg` and `ffprobe` on PATH. It is built by `make convertvideo` in the checkout.

```sh
convertvideo -fps 15 clip.mp4 E:/clip.m5v
convertvideo -validate E:/clip.m5v
```

After copying files to the card, run `m5vtxctl send sdscan` and then `m5vtxctl send media` to list them. The scan logs any file it skips with `SD_VIDEO skip`.
