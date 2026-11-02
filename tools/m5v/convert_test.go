package m5v

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestExistingDestinationIsPreservedWithoutForce(t *testing.T) {
	dir := t.TempDir()
	source := filepath.Join(dir, "input.mp4")
	target := filepath.Join(dir, "movie.m5v")
	os.WriteFile(source, []byte("source"), 0o666)
	os.WriteFile(target, []byte("keep existing"), 0o666)
	_, err := Convert(source, target, Options{})
	if err == nil || !strings.Contains(err.Error(), "already exists") {
		t.Fatalf("got %v", err)
	}
	if kept, _ := os.ReadFile(target); string(kept) != "keep existing" {
		t.Fatal("existing output was changed")
	}
	if _, err := Convert(source, filepath.Join(dir, "movie.avi"), Options{}); err == nil || !strings.Contains(err.Error(), ".m5v") {
		t.Fatalf("wrong extension: %v", err)
	}
}

func TestRealFFmpegConversionValidatesPayloadAndReadback(t *testing.T) {
	ffmpeg, err := exec.LookPath("ffmpeg")
	if err != nil {
		t.Skip("ffmpeg is not on PATH")
	}
	if _, err := exec.LookPath("ffprobe"); err != nil {
		t.Skip("ffprobe is not on PATH")
	}
	dir := t.TempDir()
	source := filepath.Join(dir, "square.mkv")
	make := exec.Command(ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
		"testsrc2=size=64x64:rate=15:duration=0.2", "-an", "-c:v", "ffv1", source)
	if output, err := make.CombinedOutput(); err != nil {
		t.Fatalf("ffmpeg: %v %s", err, output)
	}

	target := filepath.Join(dir, "movie.m5v")
	report, err := Convert(source, target, Options{Palette: RGB332})
	if err != nil {
		t.Fatal(err)
	}
	if report.Format != MagicRGB332 || report.Output.Header.FrameCount != 3 || report.Output.Bytes != 512+3*61440 {
		t.Fatalf("report %+v", report.Output)
	}
	if hash, _ := SHA256File(target); hash != report.Output.SHA256 {
		t.Fatal("report hash differs from the file")
	}
	file, _ := os.ReadFile(target)
	frame := file[512 : 512+61440]
	for y := 0; y < Height; y++ {
		for x := 0; x < Width; x++ {
			if (x < 30 || x >= 226) && frame[y*Width+x] != 0 {
				t.Fatalf("pillarbox at %d,%d is %d", x, y, frame[y*Width+x])
			}
		}
	}
	nonzero := 0
	for y := 0; y < Height; y++ {
		for x := 40; x < 216; x++ {
			if frame[y*Width+x] != 0 {
				nonzero++
			}
		}
	}
	if nonzero < 1000 {
		t.Fatalf("only %d picture pixels", nonzero)
	}
	if _, err := Convert(source, target, Options{Palette: RGB332}); err == nil {
		t.Fatal("second conversion replaced the output without -force")
	}
	if _, err := Convert(source, target, Options{Palette: RGB332, Force: true}); err != nil {
		t.Fatal(err)
	}

	adaptive := filepath.Join(dir, "palette.m5v")
	report, err = Convert(source, adaptive, Options{Palette: Adaptive})
	if err != nil {
		t.Fatal(err)
	}
	header := report.Output.Header
	if report.Format != MagicIndexed || header.Version != 2 || header.FrameCount != 3 || report.Output.Bytes != 1024+3*61440 {
		t.Fatalf("report %+v", report.Output)
	}
	if report.PaletteTraining == nil || report.PaletteTraining.FramesSampled != 3 {
		t.Fatalf("training %+v", report.PaletteTraining)
	}
	file, _ = os.ReadFile(adaptive)
	colors := header.Colors()
	for y := 0; y < Height; y++ {
		for x := 0; x < 30; x++ {
			if index := file[1024+y*Width+x]; index != 0 || colors[index] != [3]uint8{} {
				t.Fatalf("pillarbox at %d,%d is index %d", x, y, index)
			}
		}
	}
	previews, err := WritePreviews(adaptive, filepath.Join(dir, "previews"))
	if err != nil {
		t.Fatal(err)
	}
	if len(previews) != 3 {
		t.Fatalf("%d previews of a 3-frame movie", len(previews))
	}
	for _, preview := range previews {
		for _, path := range []string{preview.RasterPNG, preview.Display4x3PNG} {
			if stat, err := os.Stat(path); err != nil || stat.Size() == 0 {
				t.Fatalf("preview %s: %v", path, err)
			}
		}
	}
}
