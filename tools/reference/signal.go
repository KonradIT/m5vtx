package reference

import "math"

// NTSC, SMPTE 170M at four samples per color cycle. DAC codes are the firmware's
// measured ones, see lib/esp8bit_video/src/esp8bit_video.h.
const (
	SampleRate     = 14318186.442057292
	SamplesPerLine = 910
	LinesPerFrame  = 525
	Sync           = 14
	Blank          = 33
	Black          = 36
	White          = 79
	Burst          = 16
	ChromaBoost    = 1.74
	ChromaLeadDeg  = 8.0
	SyncSamples    = 67
	Equalizing     = 34
	Broad          = 387
	BurstStart     = 76
	BurstEnd       = 112
	ActiveStart    = 130
	ActivePixels   = 253
	FirstPixel     = 1
	Field1First    = 23
	Field2First    = 286
)

// PAL, ITU-R BT.470 system B/G at four samples per color cycle.
const (
	PALSampleRate       = 17734476.725260417
	PALSamplesPerLine   = 1135
	PALLinesPerFrame    = 625
	PALWhite            = Blank + 44
	PALBurst            = 11
	PALBurstEdge        = 6
	PALHalfLine         = 568
	PALSyncSamples      = 83
	PALEqualizing       = 42
	PALBroad            = 484
	PALBurstStart       = 99
	PALBurstEnd         = 139
	PALActiveStart      = 186
	PALActiveSamples    = 922
	PALField1First      = 23
	PALField2First      = 336
	PALBandFirstColumn  = 60
	PALBandColumns      = 135
	PALBandSamples      = 486
	GratingColumns      = 27
	GratingFirstRow     = 154
	GratingBlockEnd     = 175
	GratingLastRow      = 196
)

// GratingMHz are the PM5544's definition lines.
var GratingMHz = [5]float64{0.8, 1.8, 2.8, 3.8, 4.8}

// PatternRGB is what each palette index of a pattern looks like, 0..1.
func PatternRGB(p Pattern) [256][3]float64 {
	var rgb [256][3]float64
	switch p {
	case SMPTE:
		for i, color := range SmpteRGB {
			rgb[i] = [3]float64{float64(color[0]) / 255, float64(color[1]) / 255, float64(color[2]) / 255}
		}
	case Philips:
		for i, color := range PhilipsRGB {
			rgb[i] = [3]float64{float64(color[0]) / 255, float64(color[1]) / 255, float64(color[2]) / 255}
		}
	default:
		for i := range rgb {
			rgb[i] = [3]float64{float64(i>>5) / 7, float64(i>>2&7) / 7, float64(i&3) / 3}
		}
	}
	return rgb
}

// DACPalette is each index's four subcarrier samples, as the firmware's table.
func DACPalette(p Pattern) [256][4]uint8 {
	var y, u, v [256]float64
	for i, c := range PatternRGB(p) {
		y[i] = .299*c[0] + .587*c[1] + .114*c[2]
		u[i] = .492 * (c[2] - y[i])
		v[i] = .877 * (c[0] - y[i])
	}
	if p == SMPTE {
		// Out of RGB: IRE around the 7.5 of black, of which 92.5 reach white.
		pluge, carrier := 4/92.5, 20/92.5
		y[SmpteBelowBlack], u[SmpteBelowBlack], v[SmpteBelowBlack] = -pluge, 0, 0
		y[SmpteMinusI], u[SmpteMinusI], v[SmpteMinusI] = 0, carrier*math.Cos(303*math.Pi/180), carrier*math.Sin(303*math.Pi/180)
		y[SmptePlusQ], u[SmptePlusQ], v[SmptePlusQ] = 0, carrier*math.Cos(33*math.Pi/180), carrier*math.Sin(33*math.Pi/180)
	}
	return encode(y, u, v, p.PAL())
}

// encode is the firmware's PaletteBank: SMPTE 170M with a negative-cosine burst,
// U*cos(t)-V*sin(t), chroma sent 8 degrees ahead on NTSC and as it is on PAL.
func encode(y, u, v [256]float64, pal bool) [256][4]uint8 {
	cosLead, sinLead := math.Cos(ChromaLeadDeg*math.Pi/180), math.Sin(ChromaLeadDeg*math.Pi/180)
	black, white := float64(Black), float64(White)
	if pal {
		cosLead, sinLead = 1, 0
		black, white = Blank, PALWhite
	}
	var table [256][4]uint8
	for i := range table {
		ur, vr := u[i]*cosLead-v[i]*sinLead, u[i]*sinLead+v[i]*cosLead
		for s, phase := range [4]float64{ur, -vr, -ur, vr} {
			code := math.Floor(black + (white-black)*(y[i]+ChromaBoost*phase) + .5)
			table[i][s] = uint8(math.Max(0, math.Min(255, code)))
		}
	}
	return table
}

func fill(out []uint8, from, to int, value uint8) {
	for i := from; i < to; i++ {
		out[i] = value
	}
}

// NTSCFrame is one 525-line frame: row k of the result is SMPTE 170M line k + 1.
// pixels is a 256x240 framebuffer and palette its DAC table.
func NTSCFrame(pixels []uint8, palette [256][4]uint8, startPhase int) []uint8 {
	signal := make([]uint8, LinesPerFrame*SamplesPerLine)
	half := SamplesPerLine / 2
	for line := 1; line <= LinesPerFrame; line++ {
		out := signal[(line-1)*SamplesPerLine : line*SamplesPerLine]
		fill(out, 0, SamplesPerLine, Blank)
		phase := func(i int) int { return (i + 2*(line-1) + startPhase) & 3 }
		// The vertical intervals: lines 1..9, and from the middle of 263 to that of 272.
		var first, second string
		switch {
		case line <= 9:
			first, second = "e", "e"
			if line >= 4 && line <= 6 {
				first, second = "b", "b"
			}
		case line >= 264 && line <= 272:
			first, second = "e", "e"
			if line >= 267 && line <= 269 {
				first = "b"
			}
			if line >= 266 && line <= 268 {
				second = "b"
			}
			if line == 272 {
				second = "x"
			}
		}
		if first != "" {
			ntscHalf(out[:half], first)
			ntscHalf(out[half:], second)
			continue
		}
		fill(out, 0, SyncSamples, Sync)
		for i := BurstStart; i < BurstEnd; i++ {
			amplitude := Burst
			if i < BurstStart+4 || i >= BurstEnd-4 {
				amplitude = Burst / 2
			}
			switch phase(i) {
			case 0:
				out[i] = Blank - uint8(amplitude)
			case 2:
				out[i] = Blank + uint8(amplitude)
			}
		}
		if line == 263 {
			ntscHalf(out[half:], "e")
			continue
		}
		row := line - Field1First
		if line >= Field2First {
			row = line - Field2First
		}
		if row < 0 || row >= Height {
			continue
		}
		for x := 0; x < ActivePixels; x++ {
			wave := palette[pixels[row*Width+FirstPixel+x]]
			for k := 0; k < 3; k++ {
				i := ActiveStart + 3*x + k
				out[i] = wave[phase(i)]
			}
		}
	}
	return signal
}

func ntscHalf(out []uint8, kind string) {
	fill(out, 0, len(out), Blank)
	fill(out, 0, map[string]int{"e": Equalizing, "b": Broad, "x": 0}[kind], Sync)
}

// PALColumnStart is where column x starts within the picture's samples: 18 samples
// per five columns, as 4, 4, 3, 4, 3.
func PALColumnStart(x int) int {
	start := x/5*18 + x%5*4
	if x%5 >= 3 {
		start--
	}
	return start
}

// GratingSamples are the DAC codes of the band's two kinds of row: with the
// center block, then without.
func GratingSamples() (block, plain [PALBandSamples]uint8) {
	origin := PALColumnStart(PALBandFirstColumn)
	for burst, megahertz := range GratingMHz {
		start := PALColumnStart(PALBandFirstColumn+burst*GratingColumns) - origin
		stop := PALColumnStart(PALBandFirstColumn+(burst+1)*GratingColumns) - origin
		for i := start; i < stop; i++ {
			level := .5 - .5*math.Cos(2*math.Pi*megahertz*1e6*float64(i-start)/PALSampleRate)
			plain[i] = uint8(math.Floor(Blank + (PALWhite-Blank)*level + .5))
		}
	}
	block = plain
	for i := PALColumnStart(122) - origin; i < PALColumnStart(135)-origin; i++ {
		block[i] = Blank
	}
	for i := PALColumnStart(128) - origin; i < PALColumnStart(129)-origin; i++ {
		block[i] = PALWhite
	}
	return block, plain
}

// palInterval is each vertical-interval line's two halves: broad, equalizing or none.
var palInterval = map[int][2]byte{1: {'b', 'b'}, 2: {'b', 'b'}, 3: {'b', 'e'}, 4: {'e', 'e'}, 5: {'e', 'e'},
	311: {'e', 'e'}, 312: {'e', 'e'}, 313: {'e', 'b'}, 314: {'b', 'b'}, 315: {'b', 'b'}, 316: {'e', 'e'}, 317: {'e', 'e'},
	318: {'e', 'x'}, 624: {'e', 'e'}, 625: {'e', 'e'}}

func palHalf(out []uint8, kind byte) {
	fill(out, 0, len(out), Blank)
	fill(out, 0, map[byte]int{'e': PALEqualizing, 'b': PALBroad, 'x': 0}[kind], Sync)
}

// PALBurstLine is whether a line carries burst: Bruch blanking leaves nine lines
// around each interval without it, moved by a line every other field.
func PALBurstLine(line int, secondFrame bool) bool {
	if secondFrame {
		return line > 5 && line < 623 && (line < 311 || line > 319)
	}
	return line > 6 && line < 622 && (line < 310 || line > 318)
}

// PALFrame is one 625-line frame of the PM5544: row k of the result is line k + 1.
// pixels is a 256x288 framebuffer.
func PALFrame(pixels []uint8, secondFrame bool, startPhase int) []uint8 {
	palette := DACPalette(Philips)
	block, plain := GratingSamples()
	band := PALColumnStart(PALBandFirstColumn)
	var columnOfSample [PALActiveSamples]int
	for x := 0; x < Width; x++ {
		for i := PALColumnStart(x); i < PALColumnStart(x+1) && i < PALActiveSamples; i++ {
			columnOfSample[i] = x
		}
	}
	signal := make([]uint8, PALLinesPerFrame*PALSamplesPerLine)
	for line := 1; line <= PALLinesPerFrame; line++ {
		out := signal[(line-1)*PALSamplesPerLine : line*PALSamplesPerLine]
		fill(out, 0, PALSamplesPerLine, Blank)
		phase := (startPhase + (line-1)*PALSamplesPerLine) & 3
		if halves, ok := palInterval[line]; ok {
			palHalf(out[:PALHalfLine], halves[0])
			palHalf(out[PALHalfLine:], halves[1])
			continue
		}
		second := 0
		if secondFrame {
			second = 1
		}
		inverted := (line+second)&1 == 0
		fill(out, 0, PALSyncSamples, Sync)
		if PALBurstLine(line, secondFrame) {
			for i := PALBurstStart; i < PALBurstEnd; i++ {
				p := (i + phase) & 3
				amplitude := PALBurst
				if i < PALBurstStart+4 || i >= PALBurstEnd-4 {
					amplitude = PALBurstEdge
				}
				high := p >= 2
				if inverted {
					high = p == 1 || p == 2
				}
				if high {
					out[i] = Blank + uint8(amplitude)
				} else {
					out[i] = Blank - uint8(amplitude)
				}
			}
		}
		row := line - PALField1First
		if line >= PALField2First {
			row = line - PALField2First
		}
		if row < 0 || row >= PALHeight {
			continue
		}
		active := out[PALActiveStart : PALActiveStart+PALActiveSamples]
		for i := range active {
			index := (PALActiveStart + i + phase) & 3
			if inverted {
				index = -index & 3
			}
			active[i] = palette[pixels[row*Width+columnOfSample[i]]][index]
		}
		if row >= GratingFirstRow && row < GratingBlockEnd {
			copy(active[band:band+PALBandSamples], block[:])
		} else if row >= GratingBlockEnd && row < GratingLastRow {
			copy(active[band:band+PALBandSamples], plain[:])
		}
		// Half lines at the ends of the picture.
		if line == PALField1First {
			fill(out, PALActiveStart, PALHalfLine, Blank)
		} else if line == PALField2First+PALHeight-1 {
			palHalf(out[PALHalfLine:], 'e')
		}
	}
	return signal
}

// Waveform is one frame of the pattern's DAC samples and its sample rate.
func Waveform(p Pattern) (samples []uint8, sampleRate float64) {
	pixels := Card(p)
	if p.PAL() {
		return PALFrame(pixels, false, 0), PALSampleRate
	}
	return NTSCFrame(pixels, DACPalette(p), 0), SampleRate
}
