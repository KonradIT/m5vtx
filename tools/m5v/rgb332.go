package m5v

import "fmt"

// PackRGB332 is the firmware's RRRGGGBB byte: bit truncation.
func PackRGB332(r, g, b uint8) uint8 {
	return (r & 0xe0) | ((g >> 3) & 0x1c) | (b >> 6)
}

// UnpackRGB332 is the color the player's RGB332 palette gives an index: red and
// green on eight evenly spaced levels, blue on four.
func UnpackRGB332(index uint8) [3]uint8 {
	return [3]uint8{
		uint8((int(index>>5)*255 + 3) / 7),
		uint8((int(index>>2&7)*255 + 3) / 7),
		uint8(int(index&3) * 85),
	}
}

// RGB332Palette is the fixed palette of a version 1 movie.
func RGB332Palette() Palette {
	var p Palette
	for i := range p {
		p[i] = UnpackRGB332(uint8(i))
	}
	return p
}

// Quantization is how RGB888 becomes RGB332.
type Quantization string

const (
	// Ordered mixes neighboring palette levels with a fixed 4x4 Bayer matrix: the
	// same pattern on every frame, so nothing flickers.
	Ordered Quantization = "ordered"
	// Nearest picks the closest level on each channel.
	Nearest Quantization = "nearest"
	// Truncate is the original bit truncation, PackRGB332.
	Truncate Quantization = "truncate"
)

var bayer4 = [4][4]uint32{{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}}

// QuantizeRGB332 converts a width*height RGB24 image into RGB332 bytes.
func QuantizeRGB332(rgb []uint8, width, height int, method Quantization, out []uint8) error {
	if len(rgb) != width*height*3 || len(out) != width*height {
		return fmt.Errorf("quantize: need %d RGB bytes and %d output bytes", width*height*3, width*height)
	}
	levels := [3]uint32{7, 7, 3}
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			i := y*width + x
			r, g, b := rgb[i*3], rgb[i*3+1], rgb[i*3+2]
			var codes [3]uint32
			switch method {
			case Truncate:
				out[i] = PackRGB332(r, g, b)
				continue
			case Nearest:
				for c, value := range [3]uint8{r, g, b} {
					codes[c] = (uint32(value)*levels[c] + 127) / 255
				}
			case Ordered:
				// floor(value * levels / 255 + 1 - (Bayer + .5) / 16), in integers so
				// that both endpoints stay exact and nothing clips.
				threshold := bayer4[y&3][x&3]
				for c, value := range [3]uint8{r, g, b} {
					codes[c] = (uint32(value)*levels[c]*32 + 255*(31-2*threshold)) / (255 * 32)
				}
			default:
				return fmt.Errorf("quantization must be ordered, nearest or truncate, not %q", method)
			}
			out[i] = uint8(codes[0]<<5 | codes[1]<<2 | codes[2])
		}
	}
	return nil
}
