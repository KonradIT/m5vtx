These tests exercise the video engine's portable parts on the host, without I2S,
the audio PLL or the ISR's timing. `make host-tests` from the repository root
builds and runs them; the commands below do the same by hand.

`test_frame_exchange.cpp` is the framebuffer ownership state machine the ISR
uses. It queues a frame at every raster position and checks that each active
image keeps a single source buffer, queued buffers cannot be reused early, and
late frames wait for the next blanking boundary. It also checks palette and
frame bank coherence, arbitrary RGB888 palettes, cache hits, and exact
restoration of the RGB332 waveform through a frozen table fingerprint. That
fingerprint changes with the DAC levels, on purpose: the Go tests in
`tools/reference` check those against the standards.

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -Ilib/esp8bit_video/src tests/video_host/test_frame_exchange.cpp -o tests/video_host/test_frame_exchange
tests/video_host/test_frame_exchange
```

`test_cards.cpp` runs the real `src/composite_video.cpp` and PAL line renderer
over a stand-in for the engine's hardware half. It checks that the standard
follows the pattern, that the built-ins' exact palette returns after PAL, every
pulse of the PAL vertical interval, that the alternating DMA buffers are fully
written, and that four frames return to their starting phase. Given a directory,
it writes each card's pixels and DAC table and four PAL frames, which
`tools/reference`'s firmware test compares with its own rendering, sample for
sample; `go test -C tools ./reference` compiles and runs it itself when g++ is
on PATH.

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -Iinclude -Ilib/esp8bit_video/src -Itests/video_host/stubs tests/video_host/test_cards.cpp src/composite_video.cpp -o tests/video_host/test_cards
tests/video_host/test_cards
```
