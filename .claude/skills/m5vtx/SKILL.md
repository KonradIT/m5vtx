---
name: m5vtx
description: Control and read an M5VTX bench board over USB serial, a known-good NTSC/PAL composite signal source (test cards, SD movies) that also commands a 5.8 GHz analog VTX over SmartAudio or Tramp. Use this whenever debugging analog FPV video hardware on a bench where an M5VTX or an M5Stack wired to a VTX may be attached, such as a receiver or grabber with no picture, rolling, tearing or wrong colors, a VTX that will not answer or change channel or power, SmartAudio or Tramp wiring, or composite timing and levels. Also use it when a task needs a known analog video stimulus, a channel or power sweep, or a vtxtable loaded onto the board, even if M5VTX is not named.
---

# M5VTX bench board

The M5VTX is an ESP32 M5Stack Basic or M5StickC Plus2 that generates composite video on its DAC and commands a VTX over a single control wire. On a bench it plays two roles: a stimulus whose signal is known, and a transmitter you can retune from a script. Use it to move the boundary of a fault along the chain until one link is left.

```
M5Stack GPIO26 (DAC2) -- composite --> VTX video in -- 5.8 GHz RF --> receiver --> display or grabber
M5Stack GPIO16 or 17 <-- SmartAudio or Tramp --> VTX control pad
```

On an M5StickC Plus2 the control wire is on G0, G32 or G33 instead, video is still on G26, and there are no SD movies: the stick has no card slot. The boot line `M5 board=... firmware="..."` names the board the firmware was built for, and `MEDIA_STATE` says when there is no slot.

## What the board can and cannot tell you

The board's evidence stops at its own pins. It sees the control wire and its own raster. It never sees the video wire, the RF or the receiver, so a green report here says nothing about what is on the air.

| The board says | It means | It does not mean |
| --- | --- | --- |
| `connected=1` | The VTX returned a CRC-valid settings reply | The video wire is connected, or RF is going out |
| `tune` outcome `confirmed`, `frequency=5800` | The VTX reports 5800 MHz | The VTX radiates on 5800 MHz |
| `video=1`, `frames` rising about 30/s (NTSC) or 25/s (PAL) | The DAC raster is running | The signal reaches the VTX |
| `power_mw=0` | The VTX does not report milliwatts | RF is off. SmartAudio v2 never reports them |
| `COMMAND freq 5800 queued` | The command was accepted for sending | The setting took effect |

Only a receiver or a spectrum analyzer shows what is transmitted. Report findings by the link they cover, for example: "the VTX reports 5800 MHz by SmartAudio readback; RF not checked".

## Getting the tool

`m5vtxctl` is a Go program in the M5VTX repository. Look for it on PATH, then at `bin/m5vtxctl` in the checkout, or `bin/m5vtxctl.exe` on Windows. To build it from the checkout, either of:

```sh
make m5vtxctl                          # into bin/
go install -C tools ./cmd/m5vtxctl     # onto PATH
```

If this machine has no checkout, ask where it is. Find the board before anything else:

```sh
m5vtxctl ports -identify     # takes 6 s; listens only, writes nothing to any port
export M5VTX_PORT=COM24      # or pass -port to each command
```

Benches often hold other serial devices, such as the receiver's console or an SDR. Do not guess the port, because commands go to whatever you name. Only one program can hold a port, so close `pio device monitor` first.

## Commands

Every command is bounded in time and prints one JSON value, except `send`, which prints the board's own lines. Flags go before any console lines.

| Command | Does |
| --- | --- |
| `m5vtxctl status` | The board's full report, without changing anything |
| `m5vtxctl status -tune-args` | Only the `tune` flags that put the VTX back as it is now, such as `-band 5 -channel 1 -power 2` |
| `m5vtxctl tune -band B -channel C -power P` | Sets the VTX by vtxtable numbers, 1-based, and waits for its readback. Any subset works |
| `m5vtxctl tune -freq MHz` or `-pit 0\|1` | Sets a frequency or pit mode directly, and watches the readback |
| `m5vtxctl send "pattern 3"` | Sends console lines and prints what comes back. `-seconds`, `-grep` and `-until REGEXP` bound it |
| `m5vtxctl send -seconds 30 -grep "VTX "` | With no lines it only listens |
| `m5vtxctl load FILE` | Sends a vtxtable file line by line and reports errors and setup state |
| `m5vtxctl reset` | Restarts the board: the image returns to 0, NTSC, and discovery restarts |

Exit status: 0 done, 1 port or board problem, 2 usage, 3 the board answered and the setting was refused or not confirmed. `tune` and `load` give `outcome`, `reason`, `events`, `after` and, for `tune`, `before`. In `before` and `after`, everything except `power_level`, `pattern` and `standard` is what the VTX last reported:

```json
{"outcome":"confirmed","before":{"connected":true,"frequency":5658,"band":5,"channel":1,"power_level":2,"power_index":1,"power_raw":2,"power_value":1,"power_mw":0,"pit":false,"pattern":0,"standard":"NTSC","state":"Settings read at 3070398 ms"},"after":{"frequency":5800,"band":4,"channel":4,"...":"..."},"events":["UI Saved; sending the drafts","UI Sent; check reported settings above","UI Setting confirmed by VTX readback"],"rebooted":false}
```

| `outcome` | Meaning |
| --- | --- |
| `confirmed` | The VTX's own readback matches every requested setting |
| `not_confirmed` | The VTX answered, but did not report the setting. `after` shows where it really is |
| `not_connected` | No VTX is answering on the control wire. Nothing was sent |
| `refused` | Setup is unfinished, an earlier unsent edit is on the board, or the firmware rejected the value |
| `loaded`, `errors` | `load` only: the file went in cleanly, or with `###ERROR` lines listed |

A report with `rebooted: true` means the board restarted during that command, so the image is back to 0. Note it, because it can explain a blip on the receiver.

## Working a problem

1. **Take the starting state.** Run `status` and keep it, along with `status -tune-args`, since you will restore both at the end. Read `ui.setup_done`, `status.connected`, `status.video`, `status.standard`, `status.pattern`, `status.frequency`, `status.pit`. Run `status` again a few seconds later if the raster is in question, and compare `frames`.
2. **Learn the other end.** Find out what frequency the receiver is tuned to, which standards it takes, and how the picture is being judged. An agent cannot see a picture unless a grabber or a person reports it, so ask, or read it from the device under test.
3. **Pick a stimulus** from the table below, and change one thing at a time.
4. **Move the boundary.** Follow [references/troubleshooting.md](references/troubleshooting.md), which has the checks for a silent VTX, an unconfirmed setting and each picture fault.
5. **Restore and report.** Put back the VTX with `m5vtxctl tune` and the saved flags, and the image with `m5vtxctl send "image N"`, where N is the saved `status.pattern`. Confirm with `status`, and say what was and was not verified at each link.

## Choosing a stimulus

Built-in images are `pattern 0` to `pattern 4`. SD movies, on a Basic only, are `play N`, listed by `send media`. All are NTSC except pattern 4.

| To check | Use | Why |
| --- | --- | --- |
| Whether anything locks at all | `pattern 0`, Test card | Bars, a scene and a ramp in one picture |
| Palette, posterizing | `pattern 1`, 256 colors | Every RGB332 entry |
| Black level, gamma, crushed or clipped levels | `pattern 2`, Gray ramp, then `pattern 3`'s PLUGE | A neutral ramp, and sub-black set in signal terms |
| Hue and saturation | `pattern 3`, SMPTE bars | SMPTE EG 1 at 75%, with -I and +Q |
| PAL lock, geometry, frequency response | `pattern 4`, Philips PM5544 | 625 lines, definition lines at 0.8 to 4.8 MHz, phase check |
| Motion, frame pacing, compression | `play N`, a `.m5v` movie | Real motion, 15 fps is the reliable rate |

**Pattern 4 changes the signal standard.** The raster restarts as PAL, so the receiver must relock and an NTSC-only one loses the picture until another image is selected. A lost picture right after `pattern 4` is expected, and it is not a fault.

**Do not blame the device under test for these** without a second source:

- The SMPTE and PM5544 cards and the PAL raster have been verified on the host only, and never received over RF. PAL chroma gain was measured at 3.58 MHz, not 4.43 MHz.
- Tramp has been verified against wire fixtures only. No real Tramp transmitter has been connected.
- The chroma boost of 1.74 and the 8 degree lead were tuned on one VTX, a DarwinFPV. On another VTX, some saturation or hue error can come from the generator.
- The DAC has no 75 ohm driver, and levels were set by FM deviation through that VTX. They are not a calibrated 1 V into 75 ohms.

Details and timing tables are in [references/signal.md](references/signal.md). To compare a capture with what the board should be sending, `referencerender -pattern N -output card.png -waveform frame.u8` draws the ideal picture and one frame of ideal DAC samples.

## Changing VTX settings

```sh
m5vtxctl tune -band 4 -channel 4 -power 1
```

Band, channel and power are numbers in the board's vtxtable; `m5vtxctl send -grep vtxtable vtxtable` prints it, with power labels. A band marked CUSTOM goes out by frequency, and some VTXs acknowledge that and stay put.

For a sweep, loop over `tune` and check each result, because a `not_confirmed` step did not retune the VTX and its measurement belongs to the old channel:

```sh
start=$(m5vtxctl status -tune-args) || exit 1    # refuses if no VTX is connected
trap 'm5vtxctl tune $start' EXIT                 # unquoted: it holds several flags

for ch in 1 2 3 4 5 6 7 8; do
  m5vtxctl tune -band 4 -channel "$ch" >> sweep.jsonl
  case $? in
    0) sleep 2                     # let the receiver relock
       measure_receiver "$ch" ;;   # whatever reads the device under test
    3) echo "F$ch not confirmed; skipping" >&2 ;;
    *) exit 1 ;;                   # port or board problem
  esac
done
```

A `tune` takes about 3 to 5 s, or 9 s when a setting fails, so a band takes a little under a minute.

Restore by band and channel, as `-tune-args` does, not with `-freq`. Some VTXs follow a channel and ignore a frequency command: the DarwinFPV does, so `-freq` would leave it on the last channel of the sweep.

A sweep moves only the transmitter. If the question is about a receiver's front end, the receiver has to be retuned at each step too. A receiver left on one frequency only shows how far the carrier has moved away from it.

**Power and frequency act on the physical world.**

- Check that an antenna or a dummy load is fitted before raising power. A VTX transmitting into nothing can be damaged, and the board cannot detect this. If you cannot find out, ask, or stay at the current level.
- Stay on frequencies the user named or that the task needs. Legal bands and power limits vary by country, and some vtxtable channels lie outside them. Do not sweep a whole table unasked.
- A VTX on a bench has no airflow, so keep long dwells at high power short.

**Some commands persist across restarts**, so change them only when the problem calls for it, and say that you did: `save` and `load` (the vtxtable), `protocol`, `pin`, `sa2-offset`. The image is not stored and returns to 0 at boot. The VTX stores its own channel and power.

**`defaults` erases** the vtxtable, protocol and `sa2-offset`, then restarts into setup. `send` refuses it without `-allow-defaults`, and `load` refuses it outright. Get the user's agreement first, and make sure a copy of the table exists: `m5vtxctl send vtxtable` prints it. Likewise do not reflash the firmware unless asked.

## A board that shows SETUP

With `setup_done=0`, the board has no complete VTX configuration. `tune` refuses, and if the protocol is unset nothing goes out on the control wire. The test cards still run. To finish:

```sh
m5vtxctl load path/to/vtxtable.txt      # ticks bands and power; the file must contain save
m5vtxctl send "protocol smartaudio"     # or tramp, or auto to try both
```

The table must be the one for this transmitter: from the vendor, from a flight controller's `diff`, or from `vtxtables/` in the checkout. Power values are what goes on the wire, so another VTX's table can select the wrong power. If you do not have the right one, ask rather than writing one from memory.

## Reference files

- [references/troubleshooting.md](references/troubleshooting.md): read when the VTX does not answer, a setting does not confirm, or the picture is wrong.
- [references/console.md](references/console.md): read when you need a raw console command, the meaning of a report field, or the hex traffic.
- [references/signal.md](references/signal.md): read when comparing timing, levels or waveforms, or preparing a movie.
