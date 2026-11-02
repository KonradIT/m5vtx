// Package reference renders the firmware's built-in images and the DAC waveform
// it sends for them, as the firmware does. It mirrors src/composite_video.cpp
// and lib/esp8bit_video; a test compiles those and compares, sample for sample.
// The PNG is what a receiver should show, and the waveform exercises a decoder
// without the board. The analog output voltage is not modeled.
package reference

// Pattern is a built-in image, numbered as the firmware's Image menu.
type Pattern int

const (
	TestCard Pattern = iota
	Colors256
	GrayRamp
	SMPTE
	Philips
	PatternCount
)

var patternNames = [PatternCount]string{"Test card", "256 colors", "Gray ramp", "SMPTE bars NTSC", "Philips PM5544 PAL"}

func (p Pattern) String() string {
	if p < 0 || p >= PatternCount {
		return "invalid pattern"
	}
	return patternNames[p]
}

// PAL is true for the one image the firmware sends as PAL.
func (p Pattern) PAL() bool { return p == Philips }

// Rows is the framebuffer height the image uses.
func (p Pattern) Rows() int {
	if p.PAL() {
		return PALHeight
	}
	return Height
}

// Framebuffer geometry.
const (
	Width     = 256
	Height    = 240
	PALHeight = 288
)

// SMPTE card colors, in the firmware's SmpteColor order. The last three have no
// RGB: they are set in signal terms, see DACPalette.
const (
	SmpteGray = iota
	SmpteYellow
	SmpteCyan
	SmpteGreen
	SmpteMagenta
	SmpteRed
	SmpteBlue
	SmpteBlack
	SmpteWhite
	SmpteAboveBlack
	SmpteBelowBlack
	SmpteMinusI
	SmptePlusQ
	SmpteColorCount
)

// SmpteRGB is the SMPTE card's palette for the colors that have RGB.
var SmpteRGB = [10][3]uint8{{191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0}, {191, 0, 191}, {191, 0, 0}, {0, 0, 191},
	{0, 0, 0}, {255, 255, 255}, {11, 11, 11}}

// PM5544 colors, in the firmware's PhilipsColor order.
const (
	PhilipsBlack = iota
	PhilipsWhite
	PhilipsBackground
	PhilipsSquare
	PhilipsYellow
	PhilipsCyan
	PhilipsGreen
	PhilipsMagenta
	PhilipsRed
	PhilipsBlue
	Philips270
	Philips90
	Philips326
	Philips146
	Philips180
	Philips0
	PhilipsBurstA
	PhilipsBurstB
	PhilipsStep1
	PhilipsStep2
	PhilipsStep3
	PhilipsStep4
	PhilipsColorCount
)

// PhilipsRGB is the PM5544's palette.
var PhilipsRGB = [PhilipsColorCount][3]uint8{{0, 0, 0}, {255, 255, 255}, {122, 122, 122}, {191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
	{191, 0, 191}, {191, 0, 0}, {0, 0, 191}, {55, 157, 122}, {190, 88, 122}, {85, 122, 222}, {160, 122, 23},
	{122, 146, 2}, {122, 99, 243}, {68, 0, 0}, {0, 35, 0}, {51, 51, 51}, {102, 102, 102}, {153, 153, 153}, {204, 204, 204}}

// RGB332 is the firmware's RRRGGGBB index of a color.
func RGB332(r, g, b uint8) uint8 { return (r & 0xe0) | ((g >> 3) & 0x1c) | (b >> 6) }

var glyphs = map[rune][5]uint8{
	'M': {0x7f, 0x02, 0x0c, 0x02, 0x7f}, '5': {0x27, 0x45, 0x45, 0x45, 0x39}, 'V': {0x1f, 0x20, 0x40, 0x20, 0x1f},
	'T': {0x01, 0x01, 0x7f, 0x01, 0x01}, 'X': {0x63, 0x14, 0x08, 0x14, 0x63}, 'R': {0x7f, 0x09, 0x19, 0x29, 0x46},
	'G': {0x3e, 0x41, 0x49, 0x49, 0x7a}, 'B': {0x7f, 0x49, 0x49, 0x49, 0x36}, '3': {0x22, 0x41, 0x49, 0x49, 0x36},
	'2': {0x42, 0x61, 0x51, 0x49, 0x46}, 'N': {0x7f, 0x04, 0x08, 0x10, 0x7f}, 'S': {0x46, 0x49, 0x49, 0x49, 0x31},
	'C': {0x3e, 0x41, 0x41, 0x41, 0x22}, 'P': {0x7f, 0x09, 0x09, 0x09, 0x06}, 'A': {0x7e, 0x11, 0x11, 0x11, 0x7e},
	'L': {0x7f, 0x40, 0x40, 0x40, 0x40}, '6': {0x3e, 0x49, 0x49, 0x49, 0x32},
}

type canvas struct {
	pixels []uint8
	rows   int
}

func (c *canvas) rect(x, y, w, h int, color uint8) {
	for row := max(0, y); row < min(c.rows, y+h); row++ {
		for col := max(0, x); col < min(Width, x+w); col++ {
			c.pixels[row*Width+col] = color
		}
	}
}

// Tiny 5x7 glyphs; bits run top to bottom by column.
func (c *canvas) text(x, y int, s string, scale int, color uint8, rowScale int) {
	if rowScale == 0 {
		rowScale = scale
	}
	for _, letter := range s {
		bitmap := glyphs[letter]
		for col := 0; col < 5; col++ {
			for row := 0; row < 7; row++ {
				if bitmap[col]&(1<<row) != 0 {
					c.rect(x+col*scale, y+row*rowScale, scale, rowScale, color)
				}
			}
		}
		x += 6 * scale
	}
}

// Card is the framebuffer of a pattern: Rows() rows of Width indices.
func Card(p Pattern) []uint8 {
	c := &canvas{pixels: make([]uint8, Width*p.Rows()), rows: p.Rows()}
	switch p {
	case Colors256:
		for y := 0; y < Height; y++ {
			for x := 0; x < Width; x++ {
				c.pixels[y*Width+x] = uint8((y/15)*16 + x/16)
			}
		}
	case GrayRamp:
		// RGB332's exactly neutral values are black and white only: a spatial ramp
		// over a small Bayer-like threshold matrix.
		for y := 0; y < Height; y++ {
			for x := 0; x < Width; x++ {
				level := x
				if y < 40 {
					level = 255
				} else if y > 199 {
					level = 0
				}
				threshold := ((x&7)*17 + (y&7)*29) & 63
				if level > threshold*4 {
					c.pixels[y*Width+x] = 255
				}
			}
		}
		c.rect(53, 99, 150, 42, 0)
		c.text(57, 106, "M5 VTX", 3, 255, 0)
	case SMPTE:
		smpteCard(c)
	case Philips:
		philipsCard(c)
	default:
		testCard(c)
	}
	return c.pixels
}

func testCard(c *canvas) {
	bars := [8]uint8{255, 252, 31, 28, 227, 224, 3, 0}
	for y := 0; y < 76; y++ {
		for x := 0; x < Width; x++ {
			c.pixels[y*Width+x] = bars[x/32]
		}
	}
	c.rect(0, 76, Width, 42, 0)
	c.text(24, 83, "M5 VTX", 4, 255, 0)
	// A simple 8-bit mountain scene makes vertical and frame alignment visible.
	c.rect(0, 118, Width, 75, RGB332(45, 145, 255))
	for y := 124; y < 144; y++ {
		for x := 196; x < 216; x++ {
			if (x-206)*(x-206)+(y-134)*(y-134) < 100 {
				c.pixels[y*Width+x] = RGB332(255, 255, 0)
			}
		}
	}
	for x := 0; x < Width; x++ {
		ridge := 130 + abs(x-83)/2
		for y := ridge; y < 194; y++ {
			if y < ridge+8 {
				c.pixels[y*Width+x] = 255
			} else {
				c.pixels[y*Width+x] = RGB332(55, 75, 110)
			}
		}
		front := 157 + abs(x-184)/3
		for y := front; y < 194; y++ {
			if y < front+5 {
				c.pixels[y*Width+x] = RGB332(170, 220, 220)
			} else {
				c.pixels[y*Width+x] = RGB332(0, 110, 55)
			}
		}
	}
	c.rect(0, 194, Width, 24, 0)
	c.text(19, 200, "RGB332 NTSC", 2, 255, 0)
	for y := 218; y < 236; y++ {
		for x := 0; x < Width; x++ {
			c.pixels[y*Width+x] = RGB332(uint8(x), uint8(x), uint8(x))
		}
	}
	c.rect(0, 236, Width, 4, 255)
}

// SmpteEdge is column n/d of the picture, whose first sent column is 1, and 253 are sent.
func SmpteEdge(n, d int) int {
	switch {
	case n == 0:
		return 0
	case n == d:
		return Width
	default:
		return 1 + n*253/d
	}
}

// SMPTE EG 1: 75% bars over two thirds of the height, a twelfth of reversed bars,
// and -I, white, +Q and PLUGE.
func smpteCard(c *canvas) {
	barsEnd := Height * 2 / 3
	reversedEnd := barsEnd + Height/12
	reversed := [7]uint8{SmpteBlue, SmpteBlack, SmpteMagenta, SmpteBlack, SmpteCyan, SmpteBlack, SmpteGray}
	for bar := 0; bar < 7; bar++ {
		left := SmpteEdge(bar, 7)
		c.rect(left, 0, SmpteEdge(bar+1, 7)-left, barsEnd, uint8(bar))
		c.rect(left, barsEnd, SmpteEdge(bar+1, 7)-left, reversedEnd-barsEnd, reversed[bar])
	}
	blocks := [4]uint8{SmpteMinusI, SmpteWhite, SmptePlusQ, SmpteBlack}
	for block := 0; block < 4; block++ {
		left := SmpteEdge(block*5, 28)
		c.rect(left, reversedEnd, SmpteEdge((block+1)*5, 28)-left, Height-reversedEnd, blocks[block])
	}
	pluge := [3]uint8{SmpteBelowBlack, SmpteBlack, SmpteAboveBlack}
	for third := 0; third < 3; third++ {
		left := SmpteEdge(15+third, 21)
		c.rect(left, reversedEnd, SmpteEdge(16+third, 21)-left, Height-reversedEnd, pluge[third])
	}
	c.rect(SmpteEdge(6, 7), reversedEnd, Width-SmpteEdge(6, 7), Height-reversedEnd, SmpteBlack)
}

// The bars' and the gray scale's edges, 28 columns apart around the center.
func philipsStep(x int) int {
	switch {
	case x < 72:
		return 0
	case x < 100:
		return 1
	case x < 128:
		return 2
	case x < 156:
		return 3
	case x < 184:
		return 4
	default:
		return 5
	}
}

// Inside the circle, by row.
func philipsCircle(x, y int) uint8 {
	bars := [6]uint8{PhilipsYellow, PhilipsCyan, PhilipsGreen, PhilipsMagenta, PhilipsRed, PhilipsBlue}
	steps := [6]uint8{PhilipsBlack, PhilipsStep1, PhilipsStep2, PhilipsStep3, PhilipsStep4, PhilipsWhite}
	center := x >= 122 && x <= 134
	pick := func(condition bool, a, b uint8) uint8 {
		if condition {
			return a
		}
		return b
	}
	switch {
	case center && y >= 112 && y < 175:
		return pick(x == 128 || y == 144, PhilipsWhite, PhilipsBlack)
	case y < 49:
		return pick(y >= 28 && x >= 100 && x < 156, PhilipsBlack, PhilipsWhite)
	case y < 70: // a black edge and a needle on white: echoes show to their right
		return pick(x >= 86 && x < 170 && x != 92, PhilipsWhite, PhilipsBlack)
	case y < 91: // 250 kHz
		return pick(((x+72)/10)&1 != 0, PhilipsSquare, PhilipsBlack)
	case y < 134:
		return bars[philipsStep(x)]
	case y < 154:
		return pick((x-9)%14 == 0 || y == 144, PhilipsWhite, PhilipsBlack)
	case y < 196: // the gratings, between the band's columns
		return PhilipsBlack
	case y < 217:
		return steps[philipsStep(x)]
	case y < 238:
		return pick(x >= 86 && x < 170 && x != 92, PhilipsBlack, PhilipsWhite)
	default:
		return pick(center, PhilipsRed, PhilipsYellow)
	}
}

// Philips PM5544, from a 768x576 picture at a third of its columns and half its rows.
func philipsCard(c *canvas) {
	fill := func(y, from, to int, color uint8) {
		for x := from; x < to; x++ {
			c.pixels[y*Width+x] = color
		}
	}
	for y := 0; y < PALHeight; y++ {
		for x := 0; x < Width; x++ {
			dx, dy := (x-128)*3, (y-144)*2
			if dx*dx+dy*dy <= 252*252 {
				c.pixels[y*Width+x] = philipsCircle(x, y)
			} else if (x-9)%14 == 0 || (y-7)%21 == 0 {
				c.pixels[y*Width+x] = PhilipsWhite
			} else {
				c.pixels[y*Width+x] = PhilipsBackground
			}
		}
		// Outside the circle, on the grid: every other row goes the other way around
		// the color axis, for which PAL's mean of two lines is gray.
		if y < 8 || y >= 280 {
			continue
		}
		if (y-7)%21 != 0 {
			if y&1 != 0 {
				fill(y, 10, 23, Philips270)
				fill(y, 234, 247, Philips180)
			} else {
				fill(y, 10, 23, Philips90)
				fill(y, 234, 247, Philips0)
			}
		}
		upper, lower := y >= 29 && y < 70, y >= 218 && y < 259
		if y >= 29 && y < 259 {
			if y < 144 {
				fill(y, 24, 37, Philips270)
				fill(y, 219, 233, Philips180)
			} else {
				fill(y, 24, 37, Philips90)
				fill(y, 219, 233, Philips0)
			}
		}
		if upper || lower {
			color := uint8(Philips146)
			if upper {
				color = Philips326
			}
			fill(y, 37, 51, color)
			fill(y, 206, 219, color)
		}
	}
	// Castellations: the border's black cells, between white ones.
	c.rect(0, 0, Width, 7, PhilipsWhite)
	c.rect(0, 281, Width, 7, PhilipsWhite)
	for cell := 0; cell < 9; cell++ {
		c.rect(10+cell*28, 0, 13, 7, PhilipsBlack)
		c.rect(10+cell*28, 281, 13, 7, PhilipsBlack)
	}
	c.rect(0, 0, 9, PALHeight, PhilipsWhite)
	c.rect(248, 0, 8, PALHeight, PhilipsWhite)
	for cell := 0; cell < 7; cell++ {
		c.rect(0, 8+cell*42, 9, 20, PhilipsBlack)
		c.rect(248, 8+cell*42, 8, 20, PhilipsBlack)
	}
	// Burst gate check: little luma, much chroma, the other way on every other row.
	for y := 50; y < 70; y++ {
		if y&1 != 0 {
			fill(y, 0, 9, PhilipsBurstB)
		} else {
			fill(y, 0, 9, PhilipsBurstA)
		}
	}
	c.text(110, 31, "M5 VTX", 1, PhilipsWhite, 2)
	c.text(107, 221, "PAL 625", 1, PhilipsWhite, 2)
}

func abs(v int) int {
	if v < 0 {
		return -v
	}
	return v
}
