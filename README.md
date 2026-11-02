# M5VTX - VTX bench debugger

Turn your ESP32 M5Stack Basic (v2.7) into a VTX bench debugging tool, it generates composite video on
its DAC (test cards, or silent movies from the SD card) and controls the VTX over
SmartAudio or Tramp. Allows input from USB serial for automation as well as the screen buttons.

<img width="1448" height="1086" alt="M5VTX bench debugger with real LCD" src="https://github.com/user-attachments/assets/54d1f7d5-fff3-4bc1-b1b1-d14de17d2697" />

## Hardware

| M5Stack | VTX |
| --- | --- |
| GPIO26 / DAC2 | Composite video input |
| GPIO16 (or GPIO17, selectable on screen) | SmartAudio / Tramp control wire |
| 5V | 5V supply |
| GND | GND |

The video engine uses the classic ESP32's internal DAC and I2S0 DMA, so this is
for the ESP32 Basic only, **not the S3 or a PSRAM Fire.** GPIO25 is the Basic's
speaker pin: the speaker is left disabled and DAC1 off, and video uses DAC2
alone. The control pin choice is stored in NVS.

Any SmartAudio v1, v2 or v2.1 VTX, or a Tramp one, should work: the controller
speaks the protocols generically, probes a range of transport variants (baud,
framing, idle level, dummy bytes) and accepts only CRC-valid replies.

## Flash

Download .bin from releases, use https://espflash.app to flash.

## Build locally

PlatformIO, with Espressif32 6.12.0 / Arduino-ESP32 2.0.17, M5Unified 0.2.28 and
M5GFX 0.2.35 pinned in `platformio.ini`. The low-level I2S registers need this
classic ESP32 framework, not Arduino-ESP32 3.x.

```powershell
pio run
pio run --target upload
pio device monitor
```

PlatformIO picks the serial port by itself; pass `--upload-port COMx` or set
`upload_port` in `platformio.ini` to pin it. Upload is 460800 baud, the monitor
115200. The VTX protocol definitions, CRCs and frame builders come from
[igorka48/VTXControl_ESP32](https://github.com/igorka48/VTXControl_ESP32), fetched
by PlatformIO at a pinned commit; `tools/vtx_dependency.py` keeps its standalone
application out of the build. See [tools/VTX_DEPENDENCY.md](tools/VTX_DEPENDENCY.md).

## First-time setup

<img width="1448" height="1086" alt="M5VTX setup on a test-pattern backdrop" src="https://github.com/user-attachments/assets/710032db-0228-42a6-a7c3-6f9fce03ba5a" />

A board without a VTX configuration shows a setup list instead of the main
screen, and sends nothing on the control wire until it is done:

```
SETUP
[ ] VTX bands / channels
[ ] VTX power levels
[ ] VTX protocol          SmartAudio
```

The first two are ticked once a [vtxtable](#vtx-table) with bands and with
power levels has been entered over USB serial and saved. The protocol is the
one item set on the board: button B and C choose SmartAudio or Tramp, holding C saves
it and ticks it (`protocol smartaudio`, `protocol tramp` or `protocol auto` over
serial do the same). With all three ticked the main screen appears and
discovery starts. `defaults` over serial clears the vtxtable, protocol and
`sa2-offset` from NVS and restarts into setup; the control pin stays.

## Screen and buttons

The top line shows what the VTX reports: protocol, frequency and power. A queued
command is not confirmation; an edit stays marked until a later settings reply
matches it, or the screen says the confirmation timed out. Discovery sends only
queries and changes nothing on the VTX.

- A: select the next row.
- B / C: decrease / increase the selected value. An asterisk marks an edit.
- Hold C (0.8 s): apply the selected setting and wait for readback.
- Hold B (0.8 s): repeat discovery with the selected protocol and pin.
- Band and Channel: the transmitter's [vtxtable](#vtx-table), with the
  frequency shown. Hold C on either to apply both. Bands and channels the VTX
  has but the table marks as unavailable cannot be applied.
- Power: the vtxtable's levels by their labels. Without any, SmartAudio v2.1
  shows its advertised dBm table, v2 the specification's four indices, Tramp
  mW up to the reported maximum, and SmartAudio v1, whose power codes are
  manufacturer-specific, nothing.
- Pit mode: Tramp on/off, SmartAudio v2 exit only, v2.1 on/off.
- Protocol: SmartAudio or Tramp; hold C to save it and reconnect. Each probes
  only its own transport variants (34 for SmartAudio, 3 for Tramp).
- Image: a built-in test card, or a movie from the SD card. Movies start at once;
  hold C to restart one. The last entry, Rescan SD, re-reads the card when held.
  Selecting **Philips PM5544 PAL** switches the output to PAL; an NTSC-only
  receiver loses the picture until another image is selected.
- Control pin: GPIO16 or GPIO17; hold C to store it and probe.

The LCD redraws only rows that changed, through a small off-screen buffer, so
nothing flashes. SD and LCD share the SPI bus under a mutex; LCD updates defer
rather than block the button and VTX loop.

## Serial console

115200 baud, newline-terminated commands. `STATUS` and `MEDIA` lines are
reported every five seconds; all VTX traffic is logged as hex.

| Command | Effect |
| --- | --- |
| `status`, `media` | Report now; `media` also lists the movie catalog |
| `probe`, `query` | Repeat discovery; ask for settings now |
| `protocol smartaudio`, `protocol tramp`, `protocol auto` | Select the protocol; `auto` probes all 37 variants |
| `pin 16`, `pin 17` | Select and probe the control pin |
| `vtxtable ...`, `set vtx_band = N`, `set vtx_channel = N`, `set vtx_power = N`, `get vtx`, `save`, `diff`, `defaults` | Betaflight's VTX configuration, see [VTX table](#vtx-table) |
| `freq MHz`, `power mW`, `power-index N`, `pit 0/1` | Set a frequency, Tramp power, a SmartAudio power index, or pit mode directly |
| `sa2-offset 0/1` | SmartAudio v2 device setting, see [Device notes](#device-notes) |
| `pattern 0..4`, `image N`, `play N`, `sdscan` | Select a built-in image, any image, movie N, or rescan the card |
| `button a/b/c/holdc/holdb` | The UI handlers, over USB |

Setting commands need a validated connection and are confirmed by readback, not
by the transmitted command. `STATUS` reports the band and channel the VTX is on
by the vtxtable, its raw SmartAudio channel byte, and both the normalized
`power_index` and the `power_raw` byte the VTX sent.

`m5vtxctl`, built with the other [tools](#tools-and-tests), drives this console
for scripts and agents. It prints JSON, is bounded in time, leaves DTR and RTS low
so that opening the port does not reset the board, and exits non-zero unless the
VTX's readback confirms a setting:

```sh
m5vtxctl ports -identify
m5vtxctl -h
m5vtxctl status -port COM24
m5vtxctl tune -port COM24 -band 4 -channel 4 -power 1
m5vtxctl load -port COM24 vtxtables/darwinfpv-babyape.txt
m5vtxctl send -port COM24 "pattern 3"
```

The [m5vtx skill](.claude/skills/m5vtx/SKILL.md) teaches Claude agents to use it
when debugging analog FPV video hardware: what the board's reports do and do not
prove, which card answers which question, and how to isolate a fault along the
chain. Claude Code picks it up inside this repository; to use it from other
projects, copy `.claude/skills/m5vtx` to `~/.claude/skills/`.

## VTX table

Which bands, channels and power levels a transmitter has is its vtxtable, in
the syntax of Betaflight's CLI, so a vendor's vtxtable file or the lines from a
flight controller's `diff` can be pasted into the serial console as they are.
Comments, blank lines, `batch start`, `batch end`, the `vtx` aux ranges and
other `set` lines pass in silence.

```
vtxtable bands 6
vtxtable channels 8
vtxtable band 1 BOSCAM_A A FACTORY 5865 5845 5825 5805 5785 5765 5745 5725
vtxtable band 3 BOSCAM_E E FACTORY 5705 5685 5665    0 5885 5905    0    0
vtxtable band 6 IMD6     I CUSTOM  5732 5765 5828 5840 5866 5740    0    0
vtxtable powerlevels 3
vtxtable powervalues 0 1 2
vtxtable powerlabels 25 100 200
set vtx_band = 5
set vtx_channel = 1
set vtx_power = 3
save
```

- `vtxtable` alone prints the table; `vtxtable bands N`, `channels N` and
  `powerlevels N` size it (at most 8 each); `vtxtable band N NAME L FACTORY|CUSTOM
  F1 .. FN` fills a band, with exactly `channels` frequencies and 0 for a channel
  the VTX does not have; `powervalues` and `powerlabels` take exactly
  `powerlevels` entries. Names are cut to 8 characters and labels to 3, upper
  case, as Betaflight does; the errors are Betaflight's.
- A **FACTORY** band is one the VTX numbers itself: SmartAudio selects its
  channels as `band * 8 + channel`. A **CUSTOM** band is selected by frequency.
  Tramp always takes frequencies.
- **Power values** are what goes on the wire: a SmartAudio v2.1 level is dBm
  (`14 20 26 30`), a v2 or v1 level the raw byte the VTX takes (`0 1 2` on v2),
  a Tramp level milliwatts (`25 200 500 1000`). The labels are what the screen
  shows.
- `set vtx_band`, `set vtx_channel` and `set vtx_power` are 1-based and draft
  the selection, like the buttons do; `save` stores the table in NVS and sends
  the drafted settings to the VTX, one readback at a time, now or once it is
  connected. `get vtx` and `diff` print the table and settings back.

Until a table is saved the serial commands work on the five usual bands, A, B,
E, F and Raceband, as FACTORY, with no power levels, and the screen stays on
[setup](#first-time-setup). [vtxtables/darwinfpv-babyape.txt](vtxtables/darwinfpv-babyape.txt)
is the table of the transmitter this was developed with; vendors publish
theirs, such as [GEPRC's RAD Mini](https://geprc.com/wp-content/uploads/2022/06/VTX-RADMini.txt),
whose six bands include a low race band and whose power values are milliwatts.

## Test cards

| Image | Name on screen | Signal | What it is |
| --- | --- | --- | --- |
| 0 | Test card | NTSC | Bars, an 8-bit scene and a ramp, in RGB332 |
| 1 | 256 colors | NTSC | Every RGB332 palette entry |
| 2 | Gray ramp | NTSC | Dithered neutral ramp |
| 3 | SMPTE bars NTSC | NTSC | SMPTE EG 1: 75% bars, reversed bars, -I, white, +Q, PLUGE |
| 4 | Philips PM5544 PAL | PAL | Circle pattern on 625 lines at 50 fields/s |

The output is what a camera sends: 525 interlaced lines at 29.97 frames/s with
the vertical interval and 227.5 color cycles a line of SMPTE 170M, or for the
PM5544, 625 lines at 25 frames/s with the V switch and Bruch blanking of
ITU-R BT.470 B/G. The standard follows the image: image 4 restarts the raster
as PAL and anything else returns it to NTSC. `STATUS` reports `standard=`.

The SMPTE card and the PM5544 bring their own palettes, since RGB332 has neither
75% levels nor the PM5544's colors; -I, +Q and the PLUGE's sub-black are set in
signal terms, which RGB cannot reach. The PM5544 is redrawn from the pattern's
layout at 256x288: it has the line-alternating phase check, burst gate check,
250 kHz square wave, 75% bars, reflection checks and a six-step gray scale. The
0.8, 1.8, 2.8, 3.8 and 4.8 MHz definition lines are beyond what a framebuffer
column can hold, so they are prepared DAC samples the scanline interrupt sends
in place of those rows' middle 135 columns; each burst is 27 columns rather than
the pattern's 28. The boxes read `M5 VTX` and `PAL 625`.

On the board, NTSC takes 36.5 us of each 63.6 us line in the scanline interrupt
and PAL 25.8 us of 64; PAL support costs about 32 KB of RAM at all times. Timing,
DAC levels, measurements against a camera and the open questions are in
[lib/esp8bit_video/README.md](lib/esp8bit_video/README.md).

`referencerender` (below) draws any of the five images as a PNG and can write one
frame of its ideal DAC waveform, for comparing against what a receiver shows.

## SD video playback

The Image row lists the five built-in images, then every compatible `.m5v` in
the FAT32 card's root (up to 32, sorted by name), then Rescan SD. The card is
read at boot and on rescan, not while scrolling. Files with a bad header, the
wrong dimensions or rate, or a size that does not match their frame count are
skipped and logged. A movie loops until another image is selected.

Movies are silent 256x240 frames of one byte per pixel, 1..30 fps, streamed by
a task on the other core into two internal framebuffers that swap at vertical
blanking. 15 fps plays with no late frames on the cards tried; 20 fps was
marginal, since a frame read takes about 43 ms through the Arduino SD driver's
20 MHz SPI. The DAC's raster runs on regardless of the movie's rate: a late
frame repeats the previous one.

Convert clips on the PC with `convertvideo`, which needs `ffmpeg` and `ffprobe`
on PATH:

```sh
convertvideo -fps 15 clip.mp4 E:/clip.m5v
convertvideo -palette rgb332 -quantization ordered clip.mp4 E:/clip_rgb332.m5v
convertvideo -validate E:/clip.m5v
```

The default trains one 256-color palette on the whole clip and stores it in the
header (format version 2); `-palette rgb332` writes the fixed palette (version
1) with ordered dithering, nearest levels or plain truncation. The source is
fitted to the 4:3 display without cropping, so square footage gets side bars.
Existing outputs need `-force`. `-report FILE` writes a JSON record of the
conversion and `-previews DIR` five frames as PNG, at raster size and as the
television shows them. The converter writes a temporary file, validates it with
the same rules as the firmware and reads the published file back before it
reports success. The format is in [tools/M5V_FORMAT.md](tools/M5V_FORMAT.md).

## Tools and tests

The PC tools are Go, under `tools/`, built by the Makefile into `bin/`:

```sh
make            # bin/convertvideo, bin/referencerender and bin/m5vtxctl
make test       # Go tests, then the C++ host tests
make vtx-host-test   # the VTX parser against the pinned headers; needs one pio run first
```

GNU make with a POSIX shell (Git Bash or MSYS2 on Windows), Go 1.22 or newer and g++.
Without make, `go build -C tools ./cmd/...` builds the tools and
`go test -C tools ./...` runs their tests.

The Go tests check the written headers byte for byte, every rejection the
firmware applies, quantization and palette training, geometry, and, with
ffmpeg present, a real conversion. The reference package is checked against
SMPTE 170M and BT.470 timing and the standard color vectors, and with g++ on
PATH it compiles the firmware's real card and PAL line renderers and compares
them sample for sample. The console package runs `m5vtxctl`'s logic against a
simulated board and checks the report formats it reads against `src/main.cpp`. The
C++ host tests cover the framebuffer exchange and
palette cache, the M5V decoder, the SD player with a simulated card, the
vtxtable CLI against two real Betaflight dumps, and the VTX state machine with
published SmartAudio and Tramp frames. None of them touch hardware; only a
receiver shows what the VTX sends.

## Device notes

SmartAudio v2 advertises neither its power table, which the vtxtable supplies,
nor whether the index it reports is the one it accepts. `sa2-offset 1`, stored
in NVS, is for a VTX whose GET reports one more than its SET uses; the default,
0, is the specification's behavior.

The DarwinFPV transmitter this was developed with needs `sa2-offset 1` and a
three-level table; [vtxtables/darwinfpv-babyape.txt](vtxtables/darwinfpv-babyape.txt)
has both. It takes SET 0..2 and reports GET 1..3, reads none of its levels back as mW, and
never entered pit mode on command. It acknowledges `SET_FREQUENCY` and then stays
on its channel, so its CUSTOM band (IMD6) cannot be selected; the firmware reports
the setting as not confirmed and shows the readback. It needed the receive line
floating and a dummy zero before and after each command; that transport variant
is probed first.

## Verification status

- NTSC, through the DarwinFPV VTX and a C5VRX P4 receiver at bench distance:
  480 of 480 rows at 30 frames/s, no vertical slips, 10..20 ns sync jitter, burst
  locked; the test card's bars fit the expected hues within a few degrees. An
  SD movie played with no late rows or vertical misses.
- The SMPTE and PM5544 cards and the PAL raster are verified on the host only,
  against the firmware's real renderers and the C5VRX grabber code; neither has
  been received over RF yet, and the chroma gain was measured at 3.58 MHz, not
  at PAL's 4.43 MHz.
- Tramp is verified against wire fixtures only; no Tramp transmitter was
  connected.

## Upstream code and license

- [rossumur/esp_8_bit](https://github.com/rossumur/esp_8_bit), revision
  `55eb7b86eda290d96b02c38c3e787efb8ae6a8c0`: the I2S DMA and APLL video engine
  in [lib/esp8bit_video](lib/esp8bit_video/README.md), under its original
  license, with its raster replaced by broadcast NTSC and PAL and its output
  moved to GPIO26.
- [igorka48/VTXControl_ESP32](https://github.com/igorka48/VTXControl_ESP32),
  revision `54c21169305499667b9039022282288177ef1699`: SmartAudio and Tramp
  definitions, CRCs and frame builders, used through its headers by the
  nonblocking `VtxManager` in `src/vtx_manager.cpp`.
- [M5Stack Basic v2.7](https://docs.m5stack.com/en/core/Basic_v2.7).

The project is GPL-3.0; see LICENSE.
