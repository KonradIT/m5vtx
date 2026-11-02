// convertvideo turns a video into the M5VTX player's silent .m5v movie: 256x240,
// one byte per pixel, with a palette trained on the clip or the fixed RGB332 one.
// It needs ffmpeg and ffprobe on PATH, or named with -ffmpeg and -ffprobe.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"

	"github.com/KonradIT/m5vtx/tools/m5v"
)

func main() {
	fps := flag.String("fps", "15", "frames per second, 1..30, an integer or a fraction such as 30000/1001")
	palette := flag.String("palette", "adaptive", "adaptive: a palette trained on the clip (version 2); rgb332: the fixed palette (version 1)")
	quantization := flag.String("quantization", "ordered", "rgb332 only: ordered dithering, nearest, or truncate")
	title := flag.String("title", "", "title stored in the header; the source's file name if empty")
	force := flag.Bool("force", false, "replace an existing output file")
	ffmpeg := flag.String("ffmpeg", "ffmpeg", "ffmpeg executable")
	ffprobe := flag.String("ffprobe", "ffprobe", "ffprobe executable")
	report := flag.String("report", "", "write a JSON record of the conversion to this file")
	previews := flag.String("previews", "", "write PNG previews of five frames into this directory")
	validate := flag.String("validate", "", "only validate this existing .m5v file and print its header and SHA-256")
	flag.Usage = func() {
		fmt.Fprintf(flag.CommandLine.Output(), "Usage: convertvideo [flags] SOURCE OUTPUT.m5v\n       convertvideo -validate FILE.m5v\n\n")
		flag.PrintDefaults()
	}
	flag.Parse()

	if *validate != "" {
		info, err := m5v.Validate(*validate)
		if err != nil {
			fail(err)
		}
		print(info)
		return
	}
	if flag.NArg() != 2 {
		flag.Usage()
		os.Exit(2)
	}
	rate, err := m5v.ParseFPS(*fps)
	if err != nil {
		fail(err)
	}
	options := m5v.Options{FPS: rate, Force: *force, Palette: m5v.PaletteMode(*palette),
		Quantization: m5v.Quantization(*quantization), FFmpeg: *ffmpeg, FFprobe: *ffprobe, Progress: os.Stderr}
	if *title != "" {
		options.Title = title
	}
	result, err := m5v.Convert(flag.Arg(0), flag.Arg(1), options)
	if err != nil {
		fail(err)
	}
	if *previews != "" {
		result.Previews, err = m5v.WritePreviews(flag.Arg(1), *previews)
		if err != nil {
			fail(err)
		}
	}
	if *report != "" {
		text, err := json.MarshalIndent(result, "", "  ")
		if err != nil {
			fail(err)
		}
		if err := os.WriteFile(*report, append(text, '\n'), 0o666); err != nil {
			fail(err)
		}
	}
	summary := result.Output
	summary.Header.Palette = nil
	print(struct {
		Output            m5v.Info `json:"output"`
		PaletteCRC32      *uint32  `json:"palette_crc32,omitempty"`
		ConversionSeconds float64  `json:"conversion_seconds"`
	}{summary, paletteCRC(result), result.ConversionSeconds})
}

func paletteCRC(result *m5v.Report) *uint32 {
	if result.PaletteTraining == nil {
		return nil
	}
	return &result.PaletteTraining.PaletteCRC32
}

func print(value any) {
	text, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		fail(err)
	}
	fmt.Println(string(text))
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "convertvideo:", err)
	os.Exit(1)
}
