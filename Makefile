# Builds the PC tools and runs the host tests. The firmware itself is built by
# PlatformIO: pio run. GNU make with a POSIX shell (Git Bash or MSYS2 on Windows).

GO ?= go
CXX ?= g++
CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra -Werror
PIO_LIBDEPS ?= .pio/libdeps/m5stack-basic-v27
TOOLS := convertvideo referencerender m5vtxctl
BIN := bin
EXE := $(if $(filter Windows_NT,$(OS)),.exe,)
# Windows looks for the compiler's runtime next to a test executable.
HOST_LINK := $(if $(filter Windows_NT,$(OS)),-static,)

.PHONY: all tools $(TOOLS) test go-test host-tests vtx-host-test clean

all: tools

tools: $(TOOLS)

$(TOOLS):
	$(GO) build -C tools -o $(CURDIR)/$(BIN)/$@$(EXE) ./cmd/$@

test: go-test host-tests

go-test:
	$(GO) test -C tools ./...

# The video engine's frame exchange and cards, the M5V decoder, the SD player and
# the vtxtable CLI.
host-tests:
	$(CXX) $(CXXFLAGS) $(HOST_LINK) -Iinclude tests/vtx_host/test_vtx_table.cpp src/vtx_table.cpp -o tests/vtx_host/test_vtx_table$(EXE)
	tests/vtx_host/test_vtx_table$(EXE)
	$(CXX) $(CXXFLAGS) $(HOST_LINK) -Ilib/esp8bit_video/src tests/video_host/test_frame_exchange.cpp -o tests/video_host/test_frame_exchange$(EXE)
	tests/video_host/test_frame_exchange$(EXE)
	$(CXX) $(CXXFLAGS) $(HOST_LINK) -Iinclude -Ilib/esp8bit_video/src -Itests/video_host/stubs tests/video_host/test_cards.cpp src/composite_video.cpp -o tests/video_host/test_cards$(EXE)
	tests/video_host/test_cards$(EXE)
	$(CXX) $(CXXFLAGS) $(HOST_LINK) -Iinclude tests/sd_video_host/test_format.cpp -o tests/sd_video_host/test_format$(EXE)
	tests/sd_video_host/test_format$(EXE)
	$(CXX) $(CXXFLAGS) $(HOST_LINK) -pthread -Itests/sd_video_host/stubs -Iinclude tests/sd_video_host/test_player.cpp src/sd_video_player.cpp -o tests/sd_video_host/test_player$(EXE)
	tests/sd_video_host/test_player$(EXE)

# Needs the pinned VTXControl_ESP32 headers, which the first pio run installs. Run
# for the Basic, then with the StickC Plus2's pins from platformio.ini.
PLUS2_DEFINES := -DM5VTX_BOARD=M5VTX_BOARD_STICKC_PLUS2 -DM5VTX_HAS_SD=0 -DM5VTX_VIDEO_DAC_GPIO=26 \
	-DM5VTX_CONTROL_PINS=0,32,33 -DM5VTX_CONTROL_PIN_DEFAULT=0

vtx-host-test:
	$(CXX) -std=c++11 $(HOST_LINK) -Itests/vtx_host/stubs -Iinclude -I$(PIO_LIBDEPS)/VTXControl_ESP32/src tests/vtx_host/test_vtx.cpp src/vtx_manager.cpp -o tests/vtx_host/test_vtx$(EXE)
	tests/vtx_host/test_vtx$(EXE)
	$(CXX) -std=c++11 $(HOST_LINK) -Itests/vtx_host/stubs -Iinclude -I$(PIO_LIBDEPS)/VTXControl_ESP32/src tests/vtx_host/test_vtx.cpp src/vtx_manager.cpp $(PLUS2_DEFINES) -o tests/vtx_host/test_vtx_plus2$(EXE)
	tests/vtx_host/test_vtx_plus2$(EXE)

clean:
	rm -rf $(BIN) tests/video_host/*$(EXE) tests/sd_video_host/*$(EXE) tests/vtx_host/*$(EXE)
