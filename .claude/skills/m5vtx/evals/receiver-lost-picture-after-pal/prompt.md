---
tags: [skill-creator]
max_turns: 20
timeout_seconds: 600
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

my analog receiver lost the picture a few minutes ago, right after another agent was poking at the bench. M5VTX is wired into the VTX and the receiver only does NTSC. I'm away from the bench so don't touch any serial ports, just tell me what's going on from this and give me the exact command to fix it. also is the VTX definitely transmitting on 5658 right now? here's what m5vtxctl status printed:

{"time":"2026-11-02T15:19:25+01:00","status":{"band":5,"baud":4800,"channel":1,"channel_raw":32,"connected":1,"crc_errors":0,"echoes":1918,"frames":92059,"frequency":5658,"heap":96304,"inverted":0,"pattern":4,"pit":0,"power_index":1,"power_levels":3,"power_mw":0,"power_raw":2,"power_value":1,"probing":0,"protocol":"SmartAudio","queries":1917,"rx":36438,"sa2_offset":1,"standard":"PAL","valid":1914,"video":1},"state":"Settings read at 3070398 ms","line":{"floating":1,"pin":16,"pulldown":0,"pullup":1,"rx_edges":54,"settled":1},"ui":{"control_pin":16,"draft_mhz":5658,"edited":0,"pending":0,"queued":0,"setup_done":1,"vtx_band":5,"vtx_channel":1,"vtx_power":2},"media":{"playing":0},"media_state":"Playback stopped","rebooted":false}
