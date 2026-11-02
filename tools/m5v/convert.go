package m5v

import (
	"encoding/json"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"math"
	"os"
	"path/filepath"
	"strings"
	"time"

	"golang.org/x/image/draw"
)

// PaletteMode is which palette a movie gets.
type PaletteMode string

const (
	// Adaptive trains one 256-color palette on the whole clip: version 2.
	Adaptive PaletteMode = "adaptive"
	// RGB332 is the fixed palette: version 1.
	RGB332 PaletteMode = "rgb332"
)

// Options control a conversion.
type Options struct {
	FPS          FPS
	Title        *string // nil: the source's file name
	Force        bool    // replace an existing output
	Palette      PaletteMode
	Quantization Quantization // RGB332 only
	FFmpeg       string
	FFprobe      string
	Progress     io.Writer // nil for none
}

// Preview is one frame saved as PNG.
type Preview struct {
	FrameIndex   uint32  `json:"frame_index"`
	TimeSeconds  float64 `json:"time_seconds"`
	RasterPNG    string  `json:"raster_png"`
	Display4x3PNG string `json:"display_4x3_png"`
}

// Report records everything about a conversion.
type Report struct {
	Format            string          `json:"format"`
	Source            string          `json:"source"`
	SourceSHA256      string          `json:"source_sha256"`
	SourceProbe       json.RawMessage `json:"source_probe"`
	FFmpegArguments   []string        `json:"ffmpeg_arguments"`
	Geometry          Geometry        `json:"geometry"`
	Audio             string          `json:"audio"`
	PaletteMode       PaletteMode     `json:"palette_mode"`
	PaletteTraining   *Training       `json:"palette_training,omitempty"`
	Quantization      string          `json:"quantization"`
	Output            Info            `json:"output"`
	ConversionSeconds float64         `json:"conversion_seconds"`
	Previews          []Preview       `json:"previews,omitempty"`
}

// Convert decodes the source with ffmpeg, quantizes every frame and writes the
// movie through a temporary file in the output's directory, which it validates
// and reads back before publishing.
func Convert(source, output string, opt Options) (*Report, error) {
	started := time.Now()
	if opt.FFmpeg == "" {
		opt.FFmpeg = "ffmpeg"
	}
	if opt.FFprobe == "" {
		opt.FFprobe = "ffprobe"
	}
	if opt.Palette == "" {
		opt.Palette = Adaptive
	}
	if opt.Quantization == "" {
		opt.Quantization = Ordered
	}
	if opt.Palette != Adaptive && opt.Palette != RGB332 {
		return nil, fmt.Errorf("palette must be adaptive or rgb332, not %q", opt.Palette)
	}
	if opt.Quantization != Ordered && opt.Quantization != Nearest && opt.Quantization != Truncate {
		return nil, fmt.Errorf("quantization must be ordered, nearest or truncate, not %q", opt.Quantization)
	}
	if opt.FPS.Den == 0 {
		opt.FPS = FPS{15, 1}
	}
	source, err := filepath.Abs(source)
	if err != nil {
		return nil, err
	}
	output, err = filepath.Abs(output)
	if err != nil {
		return nil, err
	}
	if stat, err := os.Stat(source); err != nil || !stat.Mode().IsRegular() {
		return nil, fmt.Errorf("source file does not exist: %s", source)
	}
	if !strings.EqualFold(filepath.Ext(output), ".m5v") {
		return nil, errors.New("output extension must be .m5v")
	}
	if source == output {
		return nil, errors.New("source and output must be different paths")
	}
	if stat, err := os.Stat(output); err == nil {
		if !stat.Mode().IsRegular() {
			return nil, errors.New("output path is not a regular file")
		}
		if !opt.Force {
			return nil, fmt.Errorf("output already exists; use -force to replace it: %s", output)
		}
	}
	if stat, err := os.Stat(filepath.Dir(output)); err != nil || !stat.IsDir() {
		return nil, fmt.Errorf("output directory does not exist: %s", filepath.Dir(output))
	}
	title := strings.TrimSuffix(filepath.Base(source), filepath.Ext(source))
	if opt.Title != nil {
		title = *opt.Title
	}
	if _, err := MakeHeader(1, opt.FPS, title, nil); err != nil {
		return nil, err
	}

	probe, err := ProbeSource(opt.FFprobe, source)
	if err != nil {
		return nil, err
	}
	aspect, err := SourceAspect(probe.Streams[0])
	if err != nil {
		return nil, err
	}
	geometry, err := FitGeometry(aspect)
	if err != nil {
		return nil, err
	}
	arguments := FFmpegArguments(opt.FFmpeg, source, FilterGraph(opt.FPS, geometry))
	headerSize := HeaderRGB332
	if opt.Palette == Adaptive {
		headerSize = HeaderIndexed
	}
	if estimated := float64(headerSize) + math.Ceil(probe.DurationSeconds()*opt.FPS.Float()+1)*FrameBytes; estimated > FAT32MaxBytes {
		return nil, errors.New("estimated movie exceeds the FAT32 maximum file size")
	}

	var palette *Palette
	var training *Training
	var mapper *Mapper
	if opt.Palette == Adaptive {
		palette, training, err = trainPalette(arguments, geometry, opt.Progress)
		if err != nil {
			return nil, err
		}
		mapper = NewMapper(*palette)
	}

	temporary, err := os.CreateTemp(filepath.Dir(output), filepath.Base(output)+".*.tmp")
	if err != nil {
		return nil, err
	}
	temporaryPath := temporary.Name()
	published := false
	defer func() {
		temporary.Close()
		if !published {
			os.Remove(temporaryPath)
		}
	}()
	if _, err := temporary.Write(make([]byte, headerSize)); err != nil {
		return nil, err
	}
	frames, err := StartFrames(arguments)
	if err != nil {
		return nil, err
	}
	defer frames.Kill()
	indices := make([]byte, FrameBytes)
	var count uint32
	for {
		rgb, err := frames.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
		if mapper != nil {
			mapper.Map(rgb, indices)
		} else if err := QuantizeRGB332(rgb, Width, Height, opt.Quantization, indices); err != nil {
			return nil, err
		}
		if _, err := temporary.Write(indices); err != nil {
			return nil, err
		}
		count++
		if int64(headerSize)+int64(count)*FrameBytes > FAT32MaxBytes {
			return nil, errors.New("converted movie exceeds the FAT32 maximum file size")
		}
		if opt.Progress != nil && count%150 == 0 {
			fmt.Fprintf(opt.Progress, "Converted %d frames (%.1f s)\n", count, float64(count)/opt.FPS.Float())
		}
	}
	if err := frames.Close(); err != nil {
		return nil, err
	}
	if count == 0 {
		return nil, errors.New("ffmpeg produced no video frames")
	}
	if training != nil && int(count) != training.FramesSampled {
		return nil, errors.New("source frame count changed between palette training and encoding")
	}
	header, err := MakeHeader(count, opt.FPS, title, palette)
	if err != nil {
		return nil, err
	}
	if _, err := temporary.WriteAt(header, 0); err != nil {
		return nil, err
	}
	if err := temporary.Sync(); err != nil {
		return nil, err
	}
	if err := temporary.Close(); err != nil {
		return nil, err
	}
	validated, err := Validate(temporaryPath)
	if err != nil {
		return nil, err
	}
	if !opt.Force {
		if _, err := os.Stat(output); err == nil {
			return nil, fmt.Errorf("output appeared during conversion; use -force to replace it: %s", output)
		}
	}
	if err := os.Rename(temporaryPath, output); err != nil {
		return nil, err
	}
	published = true
	readback, err := Validate(output)
	if err != nil {
		return nil, err
	}
	if readback.SHA256 != validated.SHA256 {
		return nil, errors.New("destination readback hash differs from the validated converted file")
	}
	sourceHash, err := SHA256File(source)
	if err != nil {
		return nil, err
	}
	report := &Report{Format: MagicRGB332, Source: source, SourceSHA256: sourceHash, SourceProbe: probe.Raw,
		FFmpegArguments: arguments, Geometry: geometry, Audio: "omitted", PaletteMode: opt.Palette,
		PaletteTraining: training, Quantization: string(opt.Quantization), Output: readback,
		ConversionSeconds: time.Since(started).Seconds()}
	if palette != nil {
		report.Format, report.Quantization = MagicIndexed, "nearest-palette"
	}
	return report, nil
}

func trainPalette(arguments []string, geometry Geometry, progress io.Writer) (*Palette, *Training, error) {
	trainer := NewTrainer(geometry.RasterCrop(), 4)
	frames, err := StartFrames(arguments)
	if err != nil {
		return nil, nil, err
	}
	defer frames.Kill()
	for {
		rgb, err := frames.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, nil, err
		}
		trainer.AddFrame(rgb)
		if progress != nil && trainer.frames%150 == 0 {
			fmt.Fprintf(progress, "Sampled palette from %d frames\n", trainer.frames)
		}
	}
	if err := frames.Close(); err != nil {
		return nil, nil, fmt.Errorf("palette pass: %w", err)
	}
	palette, training, err := trainer.Finish()
	if err != nil {
		return nil, nil, err
	}
	return &palette, &training, nil
}

// WritePreviews saves the first, quarter, middle, three-quarter and last frames
// as PNG, at raster size and at the 4:3 the television shows.
func WritePreviews(movie, directory string) ([]Preview, error) {
	if err := os.MkdirAll(directory, 0o777); err != nil {
		return nil, err
	}
	file, err := os.Open(movie)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	stat, err := file.Stat()
	if err != nil {
		return nil, err
	}
	header, err := ReadHeader(file, stat.Size())
	if err != nil {
		return nil, err
	}
	colors := header.Colors()
	n := header.FrameCount
	chosen := []uint32{0, n / 4, n / 2, n * 3 / 4, n - 1}
	var previews []Preview
	seen := map[uint32]bool{}
	frame := make([]byte, FrameBytes)
	for _, index := range chosen {
		if seen[index] {
			continue
		}
		seen[index] = true
		if _, err := file.ReadAt(frame, int64(header.HeaderBytes)+int64(index)*FrameBytes); err != nil {
			return nil, err
		}
		raster := image.NewNRGBA(image.Rect(0, 0, Width, Height))
		for i, pixel := range frame {
			c := colors[pixel]
			raster.Set(i%Width, i/Width, color.NRGBA{c[0], c[1], c[2], 255})
		}
		display := image.NewNRGBA(image.Rect(0, 0, DisplayWidth, DisplayHeight))
		draw.CatmullRom.Scale(display, display.Bounds(), raster, raster.Bounds(), draw.Src, nil)
		rasterPath := filepath.Join(directory, fmt.Sprintf("frame_%06d.png", index))
		displayPath := filepath.Join(directory, fmt.Sprintf("frame_%06d_display4x3.png", index))
		if err := savePNG(rasterPath, raster); err != nil {
			return nil, err
		}
		if err := savePNG(displayPath, display); err != nil {
			return nil, err
		}
		previews = append(previews, Preview{FrameIndex: index, TimeSeconds: float64(index) / header.FPS.Float(),
			RasterPNG: rasterPath, Display4x3PNG: displayPath})
	}
	return previews, nil
}

func savePNG(path string, img image.Image) error {
	file, err := os.Create(path)
	if err != nil {
		return err
	}
	if err := png.Encode(file, img); err != nil {
		file.Close()
		return err
	}
	return file.Close()
}
