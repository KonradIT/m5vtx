package m5v

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"math/big"
	"os/exec"
	"strconv"
	"strings"
)

// The 4:3 display the raster is shown on. The source is fitted to it with square
// pixels, then squeezed to 256 columns; the television stretches it back.
const (
	DisplayWidth  = 320
	DisplayHeight = 240
)

// Stream is what ffprobe reports about the first video stream.
type Stream struct {
	Width             int               `json:"width"`
	Height            int               `json:"height"`
	SampleAspectRatio string            `json:"sample_aspect_ratio"`
	Duration          string            `json:"duration"`
	Tags              map[string]string `json:"tags"`
	SideDataList      []map[string]any  `json:"side_data_list"`
}

// Probe is ffprobe's JSON for the source.
type Probe struct {
	Streams []Stream `json:"streams"`
	Format  struct {
		Duration string `json:"duration"`
	} `json:"format"`
	Raw json.RawMessage `json:"-"`
}

// ProbeSource runs ffprobe on the first video stream.
func ProbeSource(ffprobe, path string) (Probe, error) {
	command := exec.Command(ffprobe, "-v", "error", "-select_streams", "v:0",
		"-show_streams", "-show_format", "-of", "json", path)
	var stderr bytes.Buffer
	command.Stderr = &stderr
	output, err := command.Output()
	if err != nil {
		return Probe{}, fmt.Errorf("ffprobe failed: %v: %s", err, strings.TrimSpace(stderr.String()))
	}
	var probe Probe
	if err := json.Unmarshal(output, &probe); err != nil {
		return Probe{}, fmt.Errorf("ffprobe output: %w", err)
	}
	if len(probe.Streams) == 0 {
		return Probe{}, errors.New("source contains no video stream")
	}
	probe.Raw = output
	return probe, nil
}

// DurationSeconds is the stream's duration, or the container's.
func (p Probe) DurationSeconds() float64 {
	for _, text := range []string{p.Streams[0].Duration, p.Format.Duration} {
		if seconds, err := strconv.ParseFloat(text, 64); err == nil && seconds > 0 {
			return seconds
		}
	}
	return 0
}

// SourceAspect is the stream's display aspect ratio, after its pixel aspect and
// any rotation metadata.
func SourceAspect(s Stream) (float64, error) {
	if s.Width <= 0 || s.Height <= 0 {
		return 0, errors.New("source has no dimensions")
	}
	sar := 1.0
	if ratio, ok := new(big.Rat).SetString(strings.Replace(s.SampleAspectRatio, ":", "/", 1)); ok && ratio.Sign() > 0 {
		sar, _ = ratio.Float64()
	}
	aspect := float64(s.Width) * sar / float64(s.Height)
	rotation := 0.0
	if text, ok := s.Tags["rotate"]; ok {
		rotation, _ = strconv.ParseFloat(text, 64)
	}
	for _, side := range s.SideDataList {
		if value, ok := side["rotation"].(float64); ok {
			rotation = value
		}
	}
	if int(math.Round(rotation/90))%2 != 0 {
		aspect = 1 / aspect
	}
	if math.IsInf(aspect, 0) || math.IsNaN(aspect) || aspect <= 0 {
		return 0, errors.New("source display aspect ratio is invalid")
	}
	return aspect, nil
}

// Geometry is where the source lands on the display and in the raster.
type Geometry struct {
	DisplayCanvas       [2]int     `json:"display_canvas"`
	DisplayContentRect  [4]int     `json:"display_content_rect"`
	RasterContentRect   [4]float64 `json:"raster_content_rect"`
	Raster              [2]int     `json:"raster"`
	PixelAspectRatio    string     `json:"pixel_aspect_ratio"`
	DisplayAspectRatio  string     `json:"display_aspect_ratio"`
	SourceDisplayAspect float64    `json:"source_display_aspect"`
}

// FitGeometry letterboxes or pillarboxes the source on the 4:3 display.
func FitGeometry(aspect float64) (Geometry, error) {
	if math.IsInf(aspect, 0) || math.IsNaN(aspect) || aspect <= 0 {
		return Geometry{}, errors.New("display aspect must be positive and finite")
	}
	var w, h int
	if aspect >= float64(DisplayWidth)/DisplayHeight {
		w, h = DisplayWidth, clamp(int(math.Round(DisplayWidth/aspect)), 1, DisplayHeight)
	} else {
		w, h = clamp(int(math.Round(DisplayHeight*aspect)), 1, DisplayWidth), DisplayHeight
	}
	x, y := (DisplayWidth-w)/2, (DisplayHeight-h)/2
	return Geometry{
		DisplayCanvas:      [2]int{DisplayWidth, DisplayHeight},
		DisplayContentRect: [4]int{x, y, w, h},
		RasterContentRect: [4]float64{float64(x) * Width / DisplayWidth, float64(y),
			float64(w) * Width / DisplayWidth, float64(h)},
		Raster: [2]int{Width, Height}, PixelAspectRatio: "5:4", DisplayAspectRatio: "4:3",
		SourceDisplayAspect: aspect}, nil
}

// RasterCrop is the raster's columns and rows that carry the picture.
func (g Geometry) RasterCrop() Rect {
	x, y, w, h := g.RasterContentRect[0], g.RasterContentRect[1], g.RasterContentRect[2], g.RasterContentRect[3]
	return Rect{clamp(int(math.Ceil(x)), 0, Width), clamp(int(math.Ceil(y)), 0, Height),
		clamp(int(math.Floor(x+w)), 0, Width), clamp(int(math.Floor(y+h)), 0, Height)}
}

func clamp(value, low, high int) int {
	return max(low, min(high, value))
}

// FilterGraph is the ffmpeg video filter that produces 256x240 RGB24 frames.
func FilterGraph(fps FPS, g Geometry) string {
	r := g.DisplayContentRect
	return fmt.Sprintf("fps=%d/%d,scale=%d:%d:flags=lanczos,setsar=1,pad=%d:%d:%d:%d:color=black,"+
		"scale=%d:%d:flags=lanczos,setsar=5/4,format=rgb24",
		fps.Num, fps.Den, r[2], r[3], DisplayWidth, DisplayHeight, r[0], r[1], Width, Height)
}

// FFmpegArguments decodes the source's first video stream to raw RGB24 on stdout.
func FFmpegArguments(ffmpeg, source string, filters string) []string {
	return []string{ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-i", source,
		"-map", "0:v:0", "-an", "-sn", "-dn", "-vf", filters, "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"}
}

// Frames reads RGB24 frames from a running ffmpeg.
type Frames struct {
	command *exec.Cmd
	stdout  io.ReadCloser
	stderr  bytes.Buffer
	frame   []byte
}

// StartFrames runs the ffmpeg command line.
func StartFrames(arguments []string) (*Frames, error) {
	command := exec.Command(arguments[0], arguments[1:]...)
	f := &Frames{command: command, frame: make([]byte, FrameBytes*3)}
	command.Stderr = &f.stderr
	stdout, err := command.StdoutPipe()
	if err != nil {
		return nil, err
	}
	f.stdout = stdout
	if err := command.Start(); err != nil {
		return nil, fmt.Errorf("ffmpeg: %w", err)
	}
	return f, nil
}

// Next returns the next frame, valid until the following call, or io.EOF.
func (f *Frames) Next() ([]byte, error) {
	n, err := io.ReadFull(f.stdout, f.frame)
	if err == io.EOF {
		return nil, io.EOF
	}
	if err != nil {
		return nil, fmt.Errorf("ffmpeg produced a partial RGB24 frame (%d/%d bytes)", n, len(f.frame))
	}
	return f.frame, nil
}

// Close waits for ffmpeg and reports its failure, if any.
func (f *Frames) Close() error {
	f.stdout.Close()
	if err := f.command.Wait(); err != nil {
		return fmt.Errorf("ffmpeg failed: %v: %s", err, strings.TrimSpace(f.stderr.String()))
	}
	return nil
}

// Kill stops a still-running ffmpeg, for an abandoned conversion.
func (f *Frames) Kill() {
	if f.command.ProcessState == nil {
		f.command.Process.Kill()
		f.command.Wait()
	}
}
