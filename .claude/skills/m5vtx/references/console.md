# Serial console reference

The console runs at 115200 baud with newline-terminated commands, and takes lines of up to 191 characters. Prefer `m5vtxctl tune` and `status` to the raw forms, since they wait for and check the readback. Use `m5vtxctl send "LINE"` for the rest.

## Contents

- [Commands](#commands)
- [Report fields](#report-fields)
- [Other lines the board prints](#other-lines-the-board-prints)
- [Reading the VTX traffic](#reading-the-vtx-traffic)

## Commands

An unknown or out-of-range command prints a help text and does nothing, including `pattern 9`.

| Command | Effect | Stored |
| --- | --- | --- |
| `status` | Prints the report. It also prints every 5 s by itself | |
| `media` | Prints the media report and one `MEDIA_FILE` line per movie | |
| `pattern 0..4` | Selects a built-in image; 4 switches to PAL, others to NTSC | |
| `play N` | Plays movie N, by its `index`, looping until another image is chosen. NTSC only. Basic only; an M5StickC Plus2 answers `###ERROR` | |
| `image N` | Any entry of the Image row: 0..4 built-in, then movies, then Rescan SD (the `image` in `MEDIA_FILE`) | |
| `sdscan` | Re-reads the card. It stops playback and returns to image 0. Basic only, as `play` | |
| `probe` | Repeats discovery with queries only, and clears unsent edits | |
| `query` | Asks the VTX for its settings now, instead of at the next 1.5 s poll | |
| `protocol smartaudio`, `tramp`, `auto` | Selects what discovery tries, and starts it | Yes |
| `pin N` | Selects the control pin and starts discovery: 16 or 17 on a Basic, 0, 32 or 33 on an M5StickC Plus2 | Yes |
| `sa2-offset 0`, `1` | 1 for a SmartAudio v2 VTX whose GET power reads one above its SET | Yes |
| `vtxtable` | Prints the table | |
| `vtxtable bands N`, `channels N`, `powerlevels N` | Sizes the table, at most 8 each | By `save` |
| `vtxtable band N NAME L FACTORY\|CUSTOM F1..Fn` | Fills a band: exactly `channels` frequencies, 0 for one the VTX lacks | By `save` |
| `vtxtable powervalues V1..Vn`, `powerlabels L1..Ln` | Exactly `powerlevels` entries | By `save` |
| `set vtx_band = N`, `vtx_channel`, `vtx_power` | Drafts a selection, 1-based. Nothing goes to the VTX yet | |
| `get vtx`, `diff`, `dump` | Prints the drafts; `diff` and `dump` print the table too | |
| `save` | Stores the table and sends the drafts to the VTX, one readback at a time | Yes |
| `defaults` | Erases the table, protocol and `sa2-offset`, then restarts | Erases |
| `freq MHz` | Sets a frequency directly. Replies `COMMAND ... queued` or `rejected` | By the VTX |
| `power mW` | Tramp, or a SmartAudio v2.1 advertised level | By the VTX |
| `power-index N` | SmartAudio v2 or v2.1 only | By the VTX |
| `pit 0`, `1` | Tramp on and off; SmartAudio v2 exit only; v2.1 both | By the VTX |
| `button a`, `b`, `c`, `holdc`, `holdb` | The screen's button handlers | |

Lines from a Betaflight dump pass quietly: comments, `batch start`, `batch end`, `vtx` aux ranges, and other `set` lines. `defaults nosave` is ignored. Names are cut to 8 characters and labels to 3, in upper case, as Betaflight does.

## Report fields

`m5vtxctl status` returns these as JSON, parsed from the firmware's `STATUS`, `STATE`, `LINE`, `UI_STATE`, `MEDIA` and `MEDIA_STATE` lines.

### `status`: the VTX and the raster

| Field | Meaning |
| --- | --- |
| `protocol` | What replied; `Unknown` until something does |
| `connected`, `probing` | A validated settings reply has come; discovery is walking its variants |
| `baud`, `inverted` | The transport variant in use or being tried |
| `frequency` | What the VTX reports, in MHz |
| `band`, `channel` | The vtxtable entry for that, 1-based, 0 if none. R7 and F8 are the same 5880 MHz, so aliases exist |
| `channel_raw` | SmartAudio's byte, band * 8 + channel; 255 if unknown, and always on Tramp |
| `power_index` | SmartAudio level after `sa2_offset`; -1 unknown |
| `power_raw` | The exact byte the VTX sent |
| `power_value` | The power as a vtxtable value: dBm on v2.1, index on v2 and v1, mW on Tramp |
| `power_mw` | 0 means not reported, not RF off |
| `power_levels` | Levels offered: the table's, else what the protocol advertises |
| `sa2_offset`, `pit` | As stored; as reported |
| `queries`, `rx`, `valid` | Queries sent, bytes heard including the board's own echo, validated replies |
| `crc_errors`, `echoes` | Damaged frames; the board's own frames heard back |
| `video`, `standard`, `frames`, `pattern` | Raster running, NTSC or PAL, frames sent, current image |
| `heap` | Free RAM |

### `state`

The last VTX state message, such as `Settings read at N ms` or `No validated VTX response. Hold B to retry.`

### `line`: the control wire

| Field | Meaning |
| --- | --- |
| `pin` | The control GPIO |
| `pullup`, `pulldown`, `floating` | Line level under each pull, sampled at the start of discovery, -1 if not yet read. A level that follows the pull means high impedance |
| `settled` | Line level 20 ms after the last transmission |
| `rx_edges` | Edges after the last transmission, excluding its first 1 ms. 0 means no reply, even an undecodable one |

### `ui`: the drafts and setup

| Field | Meaning |
| --- | --- |
| `vtx_band`, `vtx_channel`, `vtx_power`, `draft_mhz` | The drafted selection, 1-based. It follows the VTX unless edited |
| `edited` | Bits of unsent or unconfirmed rows: 1 Band, 2 Channel, 4 Power, 8 Pit, 16 Protocol |
| `pending`, `queued` | A setting is awaiting readback; bits of what `save` still has to send |
| `control_pin`, `draft_pin`, `protocol_mode`, `draft_protocol`, `selected` | Screen state |
| `setup_bands`, `setup_power`, `setup_protocol`, `setup_done` | The setup list |

### `media`: the SD movie player

| Field | Meaning |
| --- | --- |
| `mounted`, `scanning`, `files`, `rejected` | The card, and how many files were kept or refused |
| `playing`, `loading`, `selected`, `frame`, `fps`, `format` | The current movie; `frame` and `fps` are fractions, as text |
| `presented`, `underruns` | Frames shown, and late frames that repeated the last one |
| `read_ms`, `max_read_us` | SD read times |
| `errors` | SD errors |
| `image`, `flips`, `header_bytes` | Selected image, frames swapped in, the file's header size |

## Other lines the board prints

| Line | Meaning |
| --- | --- |
| `M5VTX boot: ...`, `VIDEO ...` | The board just started; `rebooted` is true in that command's report |
| `UI text` | The notice shown on the screen, such as `UI Setting confirmed by VTX readback` |
| `COMMAND line queued (requires readback)` or `rejected` | The result of a direct `freq`, `power`, `power-index` or `pit` |
| `###ERROR: text` | A `vtxtable` or `set` line the firmware refused |
| `vtx_band = 5` | A `set` line it took |
| `VTX PROBE n/m SmartAudio baud=...` | Discovery moved to the next variant |
| `VTX DETECTED ...` | Discovery found the VTX |
| `VTX NO_REPLY level=0 edges_after_1ms=0` | A query timed out, after 0.7 s |
| `VTX LINE ...`, `VTX LINE_RX settled=...` | Line diagnostics |
| `SD_VIDEO skip NAME: reason` | A card file was refused at scan |

## Reading the VTX traffic

`m5vtxctl send -seconds 5 -grep "VTX "` shows every byte on the control wire. In a SmartAudio exchange, the query and its echo come first, then the reply, one byte per `RX` line at 4800 baud:

```
VTX TX t=3171654 [7] 00 AA 55 03 00 9F 00      the query, with dummy zeros
VTX RX t=3171672 [7] 00 AA 55 03 00 9F 00      the board's own echo
VTX RX ... AA 55 09 06 20 02 00 16 1A 27       the reply, here joined up
VTX STATUS protocol=SmartAudio version=2 freq=5658 channel=32 ...
```

The reply reads `AA 55`, then the response code (`01` v1, `09` v2, `11` v2.1), then the length, then channel, power and mode bytes, the frequency high and low (`16 1A` = 5658), and a CRC. A SET is logged the same way, followed by `VTX SET_ACK` if the VTX acknowledges. The acknowledgement is not confirmation, and only the next `VTX STATUS` is.
