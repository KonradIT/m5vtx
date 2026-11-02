package m5v

import (
	"math"
	"math/rand"
	"testing"
)

func solid(w, h int, r, g, b uint8) []uint8 {
	rgb := make([]uint8, w*h*3)
	for i := 0; i < w*h; i++ {
		rgb[i*3], rgb[i*3+1], rgb[i*3+2] = r, g, b
	}
	return rgb
}

func TestRGB332UsesFirmwareBitPositions(t *testing.T) {
	cases := [][4]uint8{{255, 0, 0, 224}, {0, 255, 0, 28}, {0, 0, 255, 3}, {255, 255, 255, 255},
		{31, 31, 63, 0}, {32, 32, 64, 37}, {160, 80, 192, 171}}
	for _, c := range cases {
		if got := PackRGB332(c[0], c[1], c[2]); got != c[3] {
			t.Errorf("%v packed to %d", c[:3], got)
		}
	}
	for i := 0; i < 256; i++ {
		c := UnpackRGB332(uint8(i))
		if PackRGB332(c[0], c[1], c[2]) != uint8(i) {
			t.Errorf("index %d does not round trip through %v", i, c)
		}
	}
}

func TestNearestMinimizesEachChannelError(t *testing.T) {
	rgb := make([]uint8, 256*3)
	out := make([]uint8, 256)
	for v := 0; v < 256; v++ {
		rgb[v*3], rgb[v*3+1], rgb[v*3+2] = uint8(v), uint8(v), uint8(v)
	}
	if err := QuantizeRGB332(rgb, 256, 1, Nearest, out); err != nil {
		t.Fatal(err)
	}
	for v := 0; v < 256; v++ {
		decoded := UnpackRGB332(out[v])
		for channel, levels := range []int{8, 8, 4} {
			best := 255.0
			for level := 0; level < levels; level++ {
				best = math.Min(best, math.Abs(float64(v)-math.Round(float64(level)*255/float64(levels-1))))
			}
			if got := math.Abs(float64(decoded[channel]) - float64(v)); got != best {
				t.Errorf("value %d channel %d: error %v, best %v", v, channel, got, best)
			}
		}
	}
	truncated := make([]uint8, 256)
	QuantizeRGB332(rgb, 256, 1, Truncate, truncated)
	for v := range truncated {
		if truncated[v] != PackRGB332(uint8(v), uint8(v), uint8(v)) {
			t.Fatal("truncate is not bit truncation")
		}
	}
}

func TestOrderedDitherKeepsEndpointsAndTheMeanAndIsStable(t *testing.T) {
	out, again := make([]uint8, 256), make([]uint8, 256)
	for _, value := range []uint8{0, 49, 100, 145, 207, 255} {
		rgb := solid(16, 16, value, value, value)
		QuantizeRGB332(rgb, 16, 16, Ordered, out)
		QuantizeRGB332(rgb, 16, 16, Ordered, again)
		var sum [3]float64
		for i, index := range out {
			if index != again[i] {
				t.Fatal("dither changed between identical frames")
			}
			decoded := UnpackRGB332(index)
			for c := range sum {
				sum[c] += float64(decoded[c])
			}
			if (value == 0 || value == 255) && (decoded[0] != value || decoded[1] != value || decoded[2] != value) {
				t.Fatalf("endpoint %d dithered", value)
			}
		}
		for c := range sum {
			if math.Abs(sum[c]/256-float64(value)) > 3 {
				t.Errorf("value %d channel %d averages %v", value, c, sum[c]/256)
			}
		}
	}
	rgb := solid(16, 16, 100, 100, 100)
	QuantizeRGB332(rgb, 16, 16, Ordered, out)
	QuantizeRGB332(rgb, 16, 16, Truncate, again)
	dithered, truncated := 0.0, 0.0
	for i := range out {
		dithered += float64(UnpackRGB332(out[i])[0])
		truncated += float64(UnpackRGB332(again[i])[0])
	}
	if math.Abs(dithered/256-100) >= math.Abs(truncated/256-100) {
		t.Error("ordered dither is no closer than truncation")
	}
	if err := QuantizeRGB332(rgb, 16, 16, "unknown", out); err == nil {
		t.Error("unknown quantization accepted")
	}
}

func TestTrainerIsDeterministicCoversFramesAndReservesBlack(t *testing.T) {
	var palettes [2]Palette
	for round := range palettes {
		trainer := NewTrainer(Rect{0, 0, 8, 8}, 1)
		for _, color := range [][3]uint8{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0, 0, 0}} {
			trainer.AddFrame(solid(Width, Height, color[0], color[1], color[2]))
		}
		palette, report, err := trainer.Finish()
		if err != nil {
			t.Fatal(err)
		}
		if report.FramesSampled != 4 || report.PaletteChangesBetweenFrames || palette[0] != [3]uint8{} {
			t.Fatalf("report %+v palette[0] %v", report, palette[0])
		}
		for _, want := range [][3]uint8{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}} {
			found := false
			for _, color := range palette {
				found = found || color == want
			}
			if !found {
				t.Errorf("%v missing from the palette", want)
			}
		}
		palettes[round] = palette
	}
	if palettes[0] != palettes[1] {
		t.Fatal("training is not deterministic")
	}
	if _, _, err := NewTrainer(Rect{}, 1).Finish(); err == nil {
		t.Error("training on no frames succeeded")
	}
}

func TestMedianCutSplitsAGradientIntoDistinctColors(t *testing.T) {
	trainer := NewTrainer(Rect{0, 0, Width, Height}, 1)
	rgb := make([]uint8, FrameBytes*3)
	for i := 0; i < FrameBytes; i++ {
		x, y := i%Width, i/Width
		rgb[i*3], rgb[i*3+1], rgb[i*3+2] = uint8(x), uint8(y), uint8((x+y)/2)
	}
	trainer.AddFrame(rgb)
	palette, report, err := trainer.Finish()
	if err != nil {
		t.Fatal(err)
	}
	if report.DistinctColors < 200 {
		t.Fatalf("only %d distinct colors for a gradient", report.DistinctColors)
	}
	mapper := NewMapper(palette)
	indices := make([]uint8, FrameBytes)
	mapper.Map(rgb, indices)
	total := 0.0
	for i := range indices {
		c := palette[indices[i]]
		for k := 0; k < 3; k++ {
			total += math.Abs(float64(c[k]) - float64(rgb[i*3+k]))
		}
	}
	if mean := total / float64(FrameBytes*3); mean > 6 {
		t.Fatalf("mean error %.2f per channel", mean)
	}
}

func TestMapperIsExactNearestWithBlackAtZero(t *testing.T) {
	random := rand.New(rand.NewSource(71))
	var palette Palette
	for i := 1; i < 256; i++ {
		palette[i] = [3]uint8{uint8(random.Intn(256)), uint8(random.Intn(256)), uint8(random.Intn(256))}
	}
	mapper := NewMapper(palette)
	for n := 0; n < 2000; n++ {
		r, g, b := uint8(random.Intn(256)), uint8(random.Intn(256)), uint8(random.Intn(256))
		if n < 10 {
			r, g, b = 0, 0, 0
		}
		got := mapper.Index(r, g, b)
		best := math.MaxInt
		for _, c := range palette {
			d := sq(int(r)-int(c[0])) + sq(int(g)-int(c[1])) + sq(int(b)-int(c[2]))
			best = min(best, d)
		}
		c := palette[got]
		if d := sq(int(r)-int(c[0])) + sq(int(g)-int(c[1])) + sq(int(b)-int(c[2])); d != best {
			t.Fatalf("%v mapped to %v at distance %d, best %d", [3]uint8{r, g, b}, c, d, best)
		}
		if r == 0 && g == 0 && b == 0 && got != 0 {
			t.Fatal("black did not map to index 0")
		}
		if again := mapper.Index(r, g, b); again != got {
			t.Fatal("cached answer differs")
		}
	}
}

func sq(v int) int { return v * v }

func TestGeometryFitsSquareAndWideSourcesWithoutCropping(t *testing.T) {
	square, err := FitGeometry(1)
	if err != nil || square.DisplayContentRect != [4]int{40, 0, 240, 240} || square.RasterContentRect != [4]float64{32, 0, 192, 240} {
		t.Fatalf("square: %+v %v", square, err)
	}
	if square.RasterCrop() != (Rect{32, 0, 224, 240}) {
		t.Fatalf("square crop %+v", square.RasterCrop())
	}
	wide, err := FitGeometry(16.0 / 9)
	if err != nil || wide.DisplayContentRect != [4]int{0, 30, 320, 180} || wide.RasterContentRect != [4]float64{0, 30, 256, 180} {
		t.Fatalf("wide: %+v %v", wide, err)
	}
	aspect, err := SourceAspect(Stream{Width: 720, Height: 576, SampleAspectRatio: "16:15"})
	if err != nil || math.Abs(aspect-4.0/3) > 1e-9 {
		t.Fatalf("aspect %v %v", aspect, err)
	}
	rotated, _ := SourceAspect(Stream{Width: 1920, Height: 1080, SideDataList: []map[string]any{{"rotation": -90.0}}})
	if math.Abs(rotated-1080.0/1920) > 1e-9 {
		t.Fatalf("rotated aspect %v", rotated)
	}
	if _, err := FitGeometry(0); err == nil {
		t.Error("zero aspect accepted")
	}
	if got := FilterGraph(FPS{30000, 1001}, square); got != "fps=30000/1001,scale=240:240:flags=lanczos,setsar=1,pad=320:240:40:0:color=black,scale=256:240:flags=lanczos,setsar=5/4,format=rgb24" {
		t.Fatalf("filter %q", got)
	}
}
