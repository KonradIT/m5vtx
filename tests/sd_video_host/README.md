These tests compile the production format decoder and SD player. The player
test uses a threaded host task, in-memory SD files, checked shared-bus locks,
and a simulated vblank buffer/palette swap. It covers v1 RGB332 and v2 indexed
palettes, changed-palette revalidation, cancellation, read failures, slow-card
underruns, each format's EOF offset, catalog validation and explicit rescanning.
Physical SD throughput and shared LCD transactions still need board testing.

```powershell
g++ -std=c++11 -I include tests/sd_video_host/test_format.cpp -o tests/sd_video_host/test_format.exe
./tests/sd_video_host/test_format.exe
g++ -std=c++11 -pthread -I tests/sd_video_host/stubs -I include tests/sd_video_host/test_player.cpp src/sd_video_player.cpp -o tests/sd_video_host/test_player.exe
./tests/sd_video_host/test_player.exe
```

The format test optionally accepts one or more converted video paths and
validates each against the actual firmware decoder. File-specific frame counts,
rates, titles and CRCs are reported rather than assumed to match the original
444-frame sample; its hash and exact metadata remain in the conversion record.
