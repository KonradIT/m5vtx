// referencerender draws a built-in image of the firmware as a PNG, and can save
// the DAC waveform of one frame of it: raw unsigned bytes, one per sample, at the
// standard's sample rate (14.318186 MS/s for NTSC, 17.734477 MS/s for PAL).
package main

import (
	"flag"
	"fmt"
	"image/png"
	"os"
	"path/filepath"

	"github.com/KonradIT/m5vtx/tools/reference"
)

func main() {
	pattern := flag.Int("pattern", 0, "built-in image, 0..4: test card, 256 colors, gray ramp, SMPTE bars, Philips PM5544")
	output := flag.String("output", "reference.png", "where to write the PNG")
	waveform := flag.String("waveform", "", "also write one frame of DAC samples to this file")
	flag.Usage = func() {
		fmt.Fprintf(flag.CommandLine.Output(), "Usage: referencerender [-pattern N] [-output FILE.png] [-waveform FILE]\n\n")
		flag.PrintDefaults()
	}
	flag.Parse()
	p := reference.Pattern(*pattern)
	if p < 0 || p >= reference.PatternCount {
		fail(fmt.Errorf("pattern must be 0..%d", reference.PatternCount-1))
	}
	if err := os.MkdirAll(filepath.Dir(*output), 0o777); err != nil {
		fail(err)
	}
	file, err := os.Create(*output)
	if err != nil {
		fail(err)
	}
	if err := png.Encode(file, reference.Image(p)); err != nil {
		fail(err)
	}
	if err := file.Close(); err != nil {
		fail(err)
	}
	fmt.Printf("Pattern %d, %s: %s\n", p, p, *output)
	if *waveform != "" {
		samples, rate := reference.Waveform(p)
		if err := os.WriteFile(*waveform, samples, 0o666); err != nil {
			fail(err)
		}
		fmt.Printf("Waveform: %s, %d samples at %.3f MS/s\n", *waveform, len(samples), rate/1e6)
	}
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "referencerender:", err)
	os.Exit(1)
}
