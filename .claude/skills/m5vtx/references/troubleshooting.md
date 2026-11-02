# Troubleshooting

## Contents

- [The VTX does not answer](#the-vtx-does-not-answer)
- [A setting does not confirm](#a-setting-does-not-confirm)
- [No picture](#no-picture)
- [A wrong picture](#a-wrong-picture)

All field names are from `m5vtxctl status`: `status.*`, `line.*`, `ui.*`, `media.*`.

## The VTX does not answer

`status.connected=0`. Read these first: `status.probing`, `state`, `status.queries`, `status.rx`, `status.valid`, `status.crc_errors`, `line.rx_edges`, `line.pullup`, `line.pulldown`.

| Reading | Meaning | Next |
| --- | --- | --- |
| `ui.setup_protocol=0` | No protocol was chosen, so the control wire was never started | Finish setup. See SKILL.md |
| `probing=1` | Discovery is running: 34 SmartAudio variants or 3 Tramp, two tries each at about 0.9 s, so up to about a minute | `m5vtxctl send -seconds 70 -grep DETECTED -until "VTX DETECTED"` |
| `probing=0`, `state` says no validated response | Discovery tried every variant and gave up | Work down this table, then `send probe` |
| `line.rx_edges=0` with `queries` rising | Nothing came back on the wire at all, not even garbage | Wrong pin, wire not landed, wrong pad, or VTX unpowered |
| `line.rx_edges>0`, `valid=0` | Something replied, and it did not decode | Wrong inversion or baud for the variants tried, or noise. Read the `VTX RX` hex |
| `crc_errors` rising, `valid=0` | Frames arrive damaged | Noise or a poor ground. Shorten the wire and check common ground |
| `line.pullup=1`, `line.pulldown=0` | The line follows the weak pulls, so it is high impedance: nothing drives it when idle | The working DarwinFPV reads this too, so alone it proves nothing. With `rx_edges=0`, it also fits a wire that goes nowhere |
| `line.pullup` and `line.pulldown` equal | Something holds the line at that level | At 0, look for a short to ground. At 1, an idle-high VTX, or a short to a supply |

`rx` and `echoes` rising do not mean the VTX is talking. The board hears its own transmission on the shared wire, so `rx` grows with `queries` even with no VTX, and `echoes` counts those frames. Judge by `valid` and `rx_edges`, which exclude them.

Then, one at a time:

1. Confirm VTX power, and measure 5 V at the VTX under load. The M5Stack's 5 V rail is limited, and a VTX at high power can pull it down.
2. Confirm a common ground between the M5Stack and the VTX.
3. `m5vtxctl send -seconds 70 -grep DETECTED -until "VTX DETECTED" probe` repeats discovery. It sends only queries and changes nothing on the VTX.
4. `m5vtxctl send -seconds 70 -grep DETECTED -until "VTX DETECTED" "pin 17"` tries the other pin. This is stored, so put it back if it was not the answer.
5. `m5vtxctl send -seconds 70 -grep DETECTED -until "VTX DETECTED" "protocol auto"` tries all 37 variants of both protocols. This is stored too, so every later boot would search them all. Once `status.protocol` names what replied, select that one, or put back the earlier one if nothing did.

If `rx_edges` stays 0 through all of that, a scope or logic analyzer on the control wire separates the rest. Query pulses at the VTX pad, with nothing after them, clear the wire and leave the VTX or the choice of pad. A VTX known to work, on the same wire, clears the M5Stack side. The board cannot tell these apart by itself.

`VTX reply lost` means replies stopped after a good connection: four misses, or more than 8 s of silence. The board keeps polling on the same variant and reconnects by itself if the VTX comes back. Suspect a VTX brown-out or restart, or a loose wire.

## A setting does not confirm

| `tune` says | Meaning | Next |
| --- | --- | --- |
| `UI Setting not confirmed; showing readback` | 6.5 s passed with no matching readback. `after` is where the VTX really is | On a CUSTOM band, the VTX may ignore `SET_FREQUENCY`. Use a FACTORY band's entry at the same frequency. Otherwise suspect a table that does not match the VTX |
| `UI Channel not available in the vtxtable` | The table has 0 there. Nothing was sent. The `UI Unavailable` after it is the same event | Choose another channel |
| `the VTX took freq N and its readback did not change` | Same as the first row, for `-freq` | The DarwinFPV does this. Use `-band` and `-channel` |
| `COMMAND ... rejected` | The firmware did not send it: a setting in flight, a value out of range, or a command this protocol version lacks | SmartAudio v2 can leave pit mode and not enter it, v1 has no power indices, and a Tramp VTX with race lock refuses all sets |
| `refused`, `unsent edit` | Someone edited a row on the board and did not apply it, and `save` would send that too | `m5vtxctl send probe` clears it. So does applying it on the board |
| `refused`, `MUST BE 1..N` | The number is outside the vtxtable | Print the table with `m5vtxctl send vtxtable` |

**Power that never confirms on SmartAudio v2.** Some clones report one more than they accept. Run `tune -power 1` on a table whose first value is 0, and read `after.power_raw`:

- If it is 1, and `sa2_offset` is 0, the VTX has the offset. Set it with `m5vtxctl send "sa2-offset 1"`, which is stored.
- If it is 0, the offset is right at 0.

**Power values are wire values:** dBm on SmartAudio v2.1 (`14 20 26 30`), the raw byte on v2 and v1 (`0 1 2`), milliwatts on Tramp. A table from another VTX can be accepted and still set the wrong power. `power_mw` is only reported on v2.1 and Tramp.

**The tune confirms and the receiver does not follow.** The control path is fine, and the fault is after it. Check `pit`, the receiver's tuned frequency, and antennas, then go to [No picture](#no-picture). R7 and F8 are both 5880 MHz, so `band` and `channel` can differ from what was expected while the frequency is right.

## No picture

Walk the chain from the source, one link at a time.

1. **Raster.** `status.video=1`, and `frames` rises between two `status` calls. If not, `m5vtxctl reset`. A `Video allocation/setup FAILED` notice at boot is a firmware problem.
2. **Standard.** `status.standard` against what the receiver takes. `pattern 0` returns to NTSC. A PAL failure proves little, since PAL has never been verified over RF.
3. **Frequency.** `status.frequency` against the receiver's tuning, and `status.pit=0`. Remember that this is what the VTX reports. Confirm the carrier with a spectrum analyzer or a second receiver if you can.
4. **Video wire.** GPIO26 to the VTX's video input, and a common ground. Nothing in the report covers this wire, so check it with a meter. A scope on GPIO26 shows sync pulses every 63.556 us on NTSC or 64 us on PAL.
5. **Bypass and substitute.** Change one thing, then read the table.

| Test | Good picture means | Bad picture means |
| --- | --- | --- |
| GPIO26 straight into a composite display or grabber, no RF | The generator is fine; the fault is in the VTX, RF or receiver | The generator, its wiring, or that display |
| A known-good camera into the VTX, in place of the M5VTX | The VTX, RF and receiver are fine; suspect the M5VTX wiring or level into that VTX | The fault is in the VTX, RF or receiver |
| A second receiver on the same frequency | The first receiver is the fault | The VTX or its RF output |

## A wrong picture

As a baseline, NTSC through the DarwinFPV VTX and a C5VRX P4 receiver at bench distance gave 480 of 480 rows at 30 frames/s, no vertical slips, 10 to 20 ns of sync jitter, burst locked, and test card bars within a few degrees of the expected hues. If the device under test is much worse with the same VTX, suspect the device.

| Symptom | Check |
| --- | --- |
| Rolling, or a picture that slips vertically | Standard mismatch first. A grabber that expects fixed line counts should see 525 lines at 29.97 frames/s, or 625 at 25. Interlace: field 1 and field 2 show the same 240 rows |
| Black and white, no color | The receiver's standard, then signal strength, since weak RF can kill color. Try `pattern 3` |
| Consistent hue rotation | The generator sends chroma 8 degrees ahead, to cancel a lag measured on the DarwinFPV. On another VTX, up to that much can be the generator |
| Over-saturated or clipped colors | The most saturated colors reach the DAC's floor by design. Chroma is boosted 1.74 for that VTX's loss. Judge by the 75% bars of `pattern 3`, not by the Test card |
| Crushed blacks, wrong contrast | `pattern 2`, then `pattern 3`'s PLUGE. Absolute levels are not calibrated, so compare with a known camera through the same VTX |
| Soft picture, ringing | `pattern 4`'s definition lines, on a PAL-capable chain. A framebuffer column cannot hold finer detail on the other cards |
| Sparkles, noise, tearing | RF first. Try another channel, check the antennas, and try lower power too, since a strong signal at bench distance can also distort |
| A movie stutters | `media.underruns` and `media.max_read_us`. A late frame repeats the previous one. 15 fps is reliable, and 20 fps is marginal on the SD card. Stutter here is not RF |
