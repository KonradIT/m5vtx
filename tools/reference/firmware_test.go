package reference

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
)

// The real card and PAL line renderers of the firmware on the host, sample for
// sample against this package. tests/video_host/test_cards.cpp writes them.
func TestFirmwareRendersWhatThisPackageRenders(t *testing.T) {
	compiler, err := exec.LookPath("g++")
	if err != nil {
		t.Skip("needs a host C++ compiler, g++, on PATH")
	}
	root, err := filepath.Abs(filepath.Join("..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	directory := t.TempDir()
	binary := filepath.Join(directory, "test_cards.exe")
	arguments := []string{"-std=c++11", "-O2", "-Iinclude", "-Ilib/esp8bit_video/src", "-Itests/video_host/stubs",
		"tests/video_host/test_cards.cpp", "src/composite_video.cpp", "-o", binary}
	if runtime.GOOS == "windows" {
		arguments = append([]string{"-static"}, arguments...)
	}
	compile := exec.Command(compiler, arguments...)
	compile.Dir = root
	if output, err := compile.CombinedOutput(); err != nil {
		t.Fatalf("compiling the firmware's renderers: %v\n%s", err, output)
	}
	if output, err := exec.Command(binary, directory).CombinedOutput(); err != nil {
		t.Fatalf("running the firmware's renderers: %v\n%s", err, output)
	}
	read := func(name string) []uint8 {
		data, err := os.ReadFile(filepath.Join(directory, name))
		if err != nil {
			t.Fatal(err)
		}
		return data
	}

	for pattern, used := range map[Pattern]int{SMPTE: SmpteColorCount, Philips: PhilipsColorCount} {
		pixels := Card(pattern)
		firmware := read("pattern" + itoa(int(pattern)) + ".pixels")
		if len(firmware) != len(pixels) {
			t.Fatalf("pattern %d: firmware wrote %d pixels, this package %d", pattern, len(firmware), len(pixels))
		}
		for i := range pixels {
			if firmware[i] != pixels[i] {
				t.Fatalf("pattern %d: pixel %d,%d is %d in the firmware and %d here", pattern, i%Width, i/Width, firmware[i], pixels[i])
			}
		}
		table := read("pattern" + itoa(int(pattern)) + ".dac")
		palette := DACPalette(pattern)
		for color := 0; color < used; color++ {
			for s := 0; s < 4; s++ {
				// The float of the firmware and the double here may round apart by a code.
				if d := int(table[color*4+s]) - int(palette[color][s]); d > 1 || d < -1 {
					t.Fatalf("pattern %d: color %d sample %d is %d in the firmware and %d here", pattern, color, s, table[color*4+s], palette[color][s])
				}
			}
		}
	}

	frames := read("pal.frames")
	frameSamples := PALLinesPerFrame * PALSamplesPerLine
	if len(frames) != 4*frameSamples {
		t.Fatalf("firmware wrote %d PAL samples, want 4 frames of %d", len(frames), frameSamples)
	}
	pixels := Card(Philips)
	for number := 0; number < 4; number++ {
		firmware := frames[number*frameSamples : (number+1)*frameSamples]
		mirror := PALFrame(pixels, number&1 == 1, (number*frameSamples)&3)
		for i := range mirror {
			d := int(firmware[i]) - int(mirror[i])
			// Sync and blanking are integers on both sides, so timing is exact.
			if d > 1 || d < -1 || (firmware[i] == Sync) != (mirror[i] == Sync) {
				t.Fatalf("frame %d line %d sample %d is %d in the firmware and %d here", number, i/PALSamplesPerLine+1, i%PALSamplesPerLine, firmware[i], mirror[i])
			}
		}
	}
}
