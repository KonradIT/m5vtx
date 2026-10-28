# M5Stack Basic v2.7 VTX controller

PlatformIO firmware for the original ESP32 M5Stack Basic v2.7: an RGB332 image
is scanned continuously to a composite-video VTX, while the LCD and three
buttons control transmitter settings through a single half-duplex control wire.

## Wiring

| M5Stack | VTX |
| --- | --- |
| GPIO26 / DAC2 | Composite VIDEO input |
| GPIO16, selected during this session | RX / SmartAudio / Tramp control |
| 5V | 5V supply input |
| GND | GND |

The initial control connection was GPIO17. GPIO16 is the current firmware
default after the wire was moved; both pins are selectable on the screen.
The choice is stored in NVS. These pins are available on the **Basic**;
this configuration is not intended for a PSRAM-equipped M5Stack Fire.
GPIO25 is the Basic's speaker pin. `cfg.internal_spk = false` is set before
`M5.begin()`, and DAC1 stays disabled. Video uses only GPIO26/DAC2.

The connected DarwinFPV VTX was identified from CRC-validated replies as
**SmartAudio v2, 4800 baud, 8N2**. It needs the receive pin floating and a dummy
zero both before and after each command. Omitting the final zero made replies
arrive one command late. The working profile is tried first on startup.
The **Protocol** menu selects SmartAudio or Tramp and remembers the choice
across restarts. Each mode probes only its own transport variants. Tramp uses
9600 baud, 8N1, and queries its frequency/power limits and current settings.
Writes require a validated connection. Channel, power and pit commands are
confirmed by settings readback; selecting a protocol itself sends only queries.

## Build and flash

Run PlatformIO in PowerShell. The project pins Espressif32 6.12.0 /
Arduino-ESP32 2.0.17, M5Unified 0.2.28, and M5GFX 0.2.35. Low-level legacy
I2S registers require this classic ESP32 framework, rather than an S3 or
Arduino-ESP32 3.x build. The configuration uses the board's actual 16 MB flash.

```powershell
$env:PYTHONIOENCODING = 'utf-8'
pio run
pio run --target upload
pio device monitor
```

Upload and monitor ports default to COM24, at 460800 and 115200 baud respectively.
If `pio` is not on PATH, use
`& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"` in its place.
Close serial monitors before uploading or running the bounded console tool.

## Buttons

- A: select the next row.
- B / C: decrease / increase the selected value. An asterisk marks an edit.
- Hold C for 0.8 seconds: apply the selected setting and wait for readback.
- Hold B for 0.8 seconds: repeat discovery with the selected protocol and pin.
- Band row: B / C select A, B, E, F or Raceband, preserving the channel number.
- Channel row: B / C select 1..8 within the selected band. Hold C on either
  Band or Channel to apply the combined choice. Frequency is shown as information.
- Protocol row: B / C choose SmartAudio or Tramp; hold C to save and reconnect.
- Image row: B / C immediately select the test card, all 256 colors, or gray ramp.
- Control pin row: B / C select GPIO16 or GPIO17; hold C to store and probe it.

The LCD updates only when a displayed row changes. Each row is rendered in a
small off-screen RGB565 buffer and transferred as finished pixels, avoiding
the visible flash caused by clearing and redrawing the whole display. Serial
`UI_RENDER` reports buffer availability and the number of changed-row transfers.
The [LCD fix validation](artifacts/ui-flicker-fix.json) records the original
display fix, zero transfers while idle, and two row transfers when moving the
selection. The [protocol menu validation](artifacts/protocol-menu-update.json)
records the latest firmware hash and protocol/band/channel checks. Earlier RF
capture/build records below are retained from the original verification session.

The top of the screen shows settings actually reported by the VTX. A queued
command is not confirmation: the edit stays pending until a later settings
reply matches, or the UI reports that confirmation timed out. Discovery sends
only queries; it does not change power, channel, frequency or pit mode.

SmartAudio v2.1 advertises a dBm power table. SmartAudio v2 exposes indices;
only its known range is offered, without guessing mW. This DarwinFPV unit accepts
SET indices 0..2 but reports GET values 1..3. The firmware normalizes that
measured offset, so screen/console levels 0..2 match the command indices.
The original setting is level 2 (raw readback 3). `sa2GetPowerOffset` and
`sa2VerifiedPowerCount` in `src/main.cpp` describe this device; a standard
SmartAudio v2 VTX uses offset 0. Observed range information is retained in NVS
so lowering power and rebooting does not hide the higher levels. SmartAudio v1 has
manufacturer-specific DAC power codes, so power writes are unavailable.
Tramp uses its reported power/frequency limits. Pit support depends on protocol:
Tramp supports on/off, SmartAudio v2 supports exit, and v2.1 supports on/off.

## Serial diagnostics

Use PlatformIO's Python environment, which includes pyserial:

```powershell
$boardPython = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
& $boardPython tools/board_console.py --seconds 5 --command status
& $boardPython tools/board_console.py --seconds 75 --command probe --output artifacts/probe.log
& $boardPython tools/board_console.py --seconds 5 --command 'pin 16'
& $boardPython tools/board_console.py --seconds 8 --command 'protocol tramp'
& $boardPython tools/board_console.py --seconds 5 --command 'protocol smartaudio'
& $boardPython tools/board_console.py --seconds 3 --command 'pattern 1'
```

Other newline-terminated commands are `query`, `freq 5806`, `channel 36`,
`power 25`, `power-index 0`, `pit 0`, and `pattern 0..2`. Channel indices are
0..39 in A/B/E/F/R order; e.g. R5 is 36. `button a`, `button b`, `button c`,
`button holdc`, and `button holdb` exercise the same UI handlers over USB.
Setting commands require a validated VTX connection and still require readback.
`protocol auto` enables the original mixed-protocol discovery from the serial
console. The screen selector offers SmartAudio and Tramp explicitly. Forced
SmartAudio scans 34 variants, forced Tramp scans 3, and Auto scans all 37.

The probe tries Tramp and SmartAudio with baud/framing/idle variants, checks
received CRCs, rejects local echoes, and disconnects after missed replies.
Raw TX/RX, weak-pull line levels, post-TX edge activity and GPIO output-enable
state are logged. A floating-looking input can be a normal idle input or an
open connection; it does not identify a protocol. The probe stops after all
profiles for the selected mode rather than treating transmitted echoes as a VTX.

`STATUS` distinguishes normalized `power_index` from the received `power_raw`.
SmartAudio v2 does not advertise the output mW of each level. This project
deliberately leaves those labels unknown for this unidentified DarwinFPV revision.

## RF video verification

The LibreSDR B210 received the generated card at **5806 MHz**, with a 28 MS/s
finite capture, RX2/channel0, gain40, and a 3 MHz LO offset. The independently
decoded NTSC color image has 100% burst lock, 0.994 luma correlation against the
expected image, and mean RGB error 7.98/255. All eight bars have their expected
hues, with no hue adjustment or reference-based color fitting.

The UI handlers also switched to the 256-color palette: 254/256 received cell
centers classified to the expected RGB332 color. A channel change from R5
(5806 MHz) to R6 (5843 MHz) was confirmed by protocol readback and the SDR;
the previous channel dropped 40.33 dB and lost sync. Lowering power from the
original level to the middle level reduced received RF by 18.13 dB while
retaining the correct color image. These are relative receiver measurements,
not calibrated VTX output mW. See [verification results](artifacts/verification.json).

![Received over RF by the B210](artifacts/final_restored_testcard_5806_decoded.png)

The actual measured color carrier was 3.579491 MHz. The output is a retro-style
262-line, 256x240 progressive NTSC raster at approximately 59.923 Hz. It is an
8-bit RGB332 framebuffer with chroma encoded onto a real NTSC color subcarrier.
See [SDR commands and decoder design](tools/SDR.md), the [capture metrics](artifacts/final_restored_testcard_5806_decoded.json),
and [expected card](artifacts/reference.png). Raw IQ/CVBS NPZ files remain local
and are git-ignored; receiver metadata, metrics and PNG evidence are retained.
The final restart re-detected SmartAudio in 0.523 seconds with no CRC errors.
R5/5806 MHz, original power level 2 (raw 3), pit off, and the test card were
restored. Build hash and bench state are recorded in [artifacts/build.json](artifacts/build.json).

## Verification commands

```powershell
$sdrPython = 'C:\Users\konra\GitHub\bandera-sdr\sdr\.venv\Scripts\python.exe'
& $sdrPython -m unittest discover -s tests -v
g++ -std=c++11 -I tests/vtx_host/stubs -I include -I lib/VTXControl/src tests/vtx_host/test_vtx.cpp src/vtx_manager.cpp -o tests/vtx_host/test_vtx.exe
./tests/vtx_host/test_vtx.exe
```

The Python tests independently check color reconstruction, FM polarity/noise,
burst AGC, rejection of luma-only/noisy input, standard NTSC vectors, timing and
DAC range. C++ host tests compile the real VTX parser/state machine and include
published TBS frames, CRC/echo rejection, fragmented data, readback, disconnect,
bounded probing and pin switching. Host tests do not establish a physical VTX
connection; only actual device replies and RF measurements can do that.

## Upstream code

- [rossumur/esp_8_bit](https://github.com/rossumur/esp_8_bit), revision
  `55eb7b86eda290d96b02c38c3e787efb8ae6a8c0`: adapted I2S DMA/APLL video engine
  in [lib/esp8bit_video](lib/esp8bit_video/README.md), with its original license.
  Emulator/audio/IR code is omitted, RGB332 is added, and the actual DAC route
  is changed to GPIO26. Changing the upstream pin macro alone was insufficient.
- [RandyReover/VTXControl](https://github.com/RandyReover/VTXControl), revision
  `389627138d068a92d6e9a7e2321345448783b734`: retained protocol structures,
  constants, CRC and frame builders in [lib/VTXControl](lib/VTXControl/README.md).
  Its AVR-only software serial transport is replaced by ESP32 UART2 with GPIO
  direction switching. The combined project is supplied under GPL-3.0; see LICENSE.
- [M5Stack Basic v2.7 hardware reference](https://docs.m5stack.com/en/core/Basic_v2.7).
- [DarwinFPV older BabyApe factory files](https://darwinfpv.com/blogs/news/darwinfpv-babyape-3)
  specify SmartAudio; the exact connected VTX revision is not known.
