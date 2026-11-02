package reference

// Checks against NTSC and PAL specifications and color vectors, not against the
// exporter's own coefficients: SMPTE 170M section 10, Tektronix's NTSC vector
// diagram (070763504, figure 2-8) and the PAL vector diagram.

import (
	"math"
	"math/cmplx"
	"testing"
)

type pulse struct{ start, length int }

// pulses is where a line goes to sync, as (start, length).
func pulses(line []uint8) []pulse {
	var found []pulse
	for i := 0; i < len(line); {
		if line[i] != Sync {
			i++
			continue
		}
		end := i
		for end < len(line) && line[end] == Sync {
			end++
		}
		found = append(found, pulse{i, end - i})
		i = end
	}
	return found
}

func samePulses(a, b []pulse) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// phasor is the samples against one continuous subcarrier, whole cycles, with the
// mean removed: U + jV for U*cos(t)-V*sin(t).
func phasor(samples []uint8, phase int) complex128 {
	mean := 0.0
	for _, s := range samples {
		mean += float64(s)
	}
	mean /= float64(len(samples))
	var sum complex128
	for i, s := range samples {
		sum += complex(float64(s)-mean, 0) * cmplx.Exp(complex(0, -2*math.Pi*float64(i+phase)/4))
	}
	return sum / complex(float64(len(samples))/2, 0)
}

func degrees(z complex128) float64 { return cmplx.Phase(z) * 180 / math.Pi }

// wrap folds an angle difference into -180..180.
func wrap(deg float64) float64 { return math.Mod(deg+540, 360) - 180 }

func near(t *testing.T, got, want, delta float64, what string) {
	t.Helper()
	if math.Abs(got-want) > delta {
		t.Errorf("%s: %v, want %v within %v", what, got, want, delta)
	}
}

func TestNTSCHueVectors(t *testing.T) {
	// A V-sign reversal would mirror hues and fail these standard targets.
	palette := DACPalette(TestCard)
	targets := map[int]float64{224: 104, 28: 241, 3: 347, 31: 284, 252: 167, 227: 61}
	for index, want := range targets {
		angle := degrees(phasor(palette[index][:], 0))
		near(t, wrap(angle-want-ChromaLeadDeg), 0, 1.6, "hue of RGB332 index "+itoa(index)) // 8-bit DAC phase quantization
	}
}

func TestLuminanceAndNeutralColors(t *testing.T) {
	palette := DACPalette(TestCard)
	// SMPTE setup 7.5 IRE, nominal white 100, and 100% primary luminances.
	targets := map[int]float64{0: 7.5, 255: 100, 224: 35.1575, 28: 61.7975, 3: 18.045}
	for index, ire := range targets {
		near(t, lumaIRE(palette[index], Blank, White), ire, 1.3, "luma of index "+itoa(index)) // a code is 2.2 IRE
	}
	for _, index := range []int{0, 255} {
		if ptp(palette[index][:]) != 0 {
			t.Errorf("index %d carries chroma", index)
		}
	}
}

func lumaIRE(wave [4]uint8, blank, white float64) float64 {
	return (mean(wave[:]) - blank) / (white - blank) * 100
}

func TestSyncBurstAndInterlacedTiming(t *testing.T) {
	raster := NTSCFrame(Card(TestCard), DACPalette(TestCard), 0)
	line := func(n int) []uint8 { return raster[(n-1)*SamplesPerLine : n*SamplesPerLine] }
	half := SamplesPerLine / 2
	eq2 := []pulse{{0, Equalizing}, {half, Equalizing}}
	broad2 := []pulse{{0, Broad}, {half, Broad}}
	expect := func(n int, want []pulse) {
		if got := pulses(line(n)); !samePulses(got, want) {
			t.Errorf("line %d pulses %v, want %v", n, got, want)
		}
	}
	// Field 1: six equalizing pulses, six broad, six equalizing, on the line.
	for _, n := range []int{1, 2, 3, 7, 8, 9} {
		expect(n, eq2)
	}
	for _, n := range []int{4, 5, 6} {
		expect(n, broad2)
	}
	// Field 2: the same, half a line later: from the middle of 263 to that of 272.
	expect(263, []pulse{{0, SyncSamples}, {half, Equalizing}})
	for _, n := range []int{264, 265, 270, 271} {
		expect(n, eq2)
	}
	expect(266, []pulse{{0, Equalizing}, {half, Broad}})
	expect(267, broad2)
	expect(268, broad2)
	expect(269, []pulse{{0, Broad}, {half, Equalizing}})
	expect(272, []pulse{{0, Equalizing}})
	// The rest is one sync pulse a line, 4.7 us: nominal, in samples.
	for n := 10; n <= 525; n++ {
		if n >= 263 && n <= 272 {
			continue
		}
		if got := pulses(line(n)); len(got) == 0 || got[0] != (pulse{0, SyncSamples}) {
			t.Errorf("line %d pulses %v", n, got)
		}
	}
	near(t, SyncSamples/SampleRate*1e6, 4.7, .05, "sync us")
	near(t, Equalizing/SampleRate*1e6, 2.3, .1, "equalizing us")
	near(t, Broad/SampleRate*1e6, 27.1, .1, "broad us")

	// Burst: from line 10 on, nine cycles from 19 cycles after the sync edge, its
	// phase opposite the U axis on line 10 and turning over from line to line.
	if BurstStart != 19*4 || BurstEnd != 28*4 {
		t.Error("burst is not 19..28 cycles after sync")
	}
	for _, n := range []int{1, 9, 264, 272} {
		for _, s := range line(n)[BurstStart:BurstEnd] {
			if s == Blank+Burst {
				t.Errorf("line %d has burst", n)
			}
		}
	}
	for n, want := range map[int]float64{11: 180, 12: 0, 263: 180, 273: 180, 274: 0} {
		burst := line(n)[BurstStart:BurstEnd]
		if mean(burst) != Blank {
			t.Errorf("line %d burst mean %v", n, mean(burst))
		}
		near(t, math.Abs(degrees(phasor(burst, 0))), want, .01, "burst phase of line "+itoa(n))
	}
	near(t, SampleRate/4, 315e6/88, 2, "subcarrier")
	near(t, SampleRate/float64(len(raster)), 30000.0/1001, .001, "frame rate")
	near(t, SamplesPerLine/SampleRate*1e6, 63.5556, .0005, "line us")

	// The picture: 240 lines a field, the same rows in both.
	var pictured []int
	for n := 1; n <= LinesPerFrame; n++ {
		if (n >= 1 && n <= 9) || (n >= 263 && n <= 272) {
			continue
		}
		for _, s := range line(n)[ActiveStart : ActiveStart+3*ActivePixels] {
			if s != Blank {
				pictured = append(pictured, n)
				break
			}
		}
	}
	var want []int
	for n := Field1First; n < Field1First+240; n++ {
		want = append(want, n)
	}
	for n := Field2First; n < Field2First+240; n++ {
		want = append(want, n)
	}
	if !equalInts(pictured, want) || Field2First-Field1First != 263 {
		t.Errorf("pictured lines %v", pictured)
	}
}

func TestPaletteAndImagesDoNotClip(t *testing.T) {
	palette := DACPalette(TestCard)
	low, high := 255, 0
	for _, wave := range palette {
		for _, s := range wave {
			low, high = min(low, int(s)), max(high, int(s))
		}
	}
	// Saturated colors go below black and above white, but never reach the DAC
	// rails, which would distort hue.
	if low <= 0 || high >= 255 || low >= Black || high <= White {
		t.Errorf("palette spans %d..%d", low, high)
	}
	for p := TestCard; p < Philips; p++ {
		pixels := Card(p)
		if len(pixels) != Width*Height {
			t.Fatalf("pattern %d has %d pixels", p, len(pixels))
		}
		frame := NTSCFrame(pixels, DACPalette(p), 0)
		end := ActiveStart + 3*ActivePixels
		for n := Field1First; n < Field1First+240; n++ {
			for _, s := range frame[(n-1)*SamplesPerLine+end : n*SamplesPerLine] {
				if s != Blank {
					t.Fatalf("pattern %d line %d writes into the front porch", p, n)
				}
			}
		}
		near(t, float64(SamplesPerLine-end)/SampleRate*1e6, 1.5, .05, "front porch us")
	}
	if len(distinct(Card(Colors256))) != 256 {
		t.Error("256 colors are not all there")
	}
	if values := distinct(Card(GrayRamp)); len(values) != 2 || !values[0] || !values[255] {
		t.Error("the gray ramp is not black and white only")
	}
}

// SMPTE EG 1 bars, against Tektronix's table of 75% bars with 7.5 IRE of setup.
func TestSmpteLevelsAndVectors(t *testing.T) {
	palette := DACPalette(SMPTE)
	// Luma IRE, chroma IRE peak to peak, degrees from +U.
	targets := map[int][3]float64{SmpteGray: {77, 0, math.NaN()}, SmpteYellow: {69, 62, 167.1}, SmpteCyan: {56, 88, 283.5},
		SmpteGreen: {48, 82, 240.7}, SmpteMagenta: {36, 82, 60.7}, SmpteRed: {28, 88, 103.5}, SmpteBlue: {15, 62, 347.1},
		SmpteMinusI: {7.5, 40, 303}, SmptePlusQ: {7.5, 40, 33}}
	for index, want := range targets {
		z := phasor(palette[index][:], 0)
		near(t, lumaIRE(palette[index], Blank, White), want[0], 2.2, "luma of SMPTE color "+itoa(index)) // a code
		near(t, 2*cmplx.Abs(z)/ChromaBoost/(White-Black)*92.5, want[1], 3, "chroma of SMPTE color "+itoa(index))
		if !math.IsNaN(want[2]) {
			near(t, wrap(degrees(z)-ChromaLeadDeg-want[2]), 0, 3.5, "hue of SMPTE color "+itoa(index))
		}
	}
}

func TestSmptePlugeStraddlesBlack(t *testing.T) {
	palette := DACPalette(SMPTE)
	below, black, above := palette[SmpteBelowBlack], palette[SmpteBlack], palette[SmpteAboveBlack]
	for _, level := range [][4]uint8{below, black, above} {
		if ptp(level[:]) != 0 {
			t.Errorf("PLUGE level %v carries chroma", level)
		}
	}
	// 4 IRE either way: below black is reachable only because it is not an RGB color.
	if below[0] != Black-2 || black[0] != Black || above[0] != Black+2 || below[0] <= Blank {
		t.Errorf("PLUGE %d %d %d", below[0], black[0], above[0])
	}
}

func TestSmpteGeometry(t *testing.T) {
	pixels := Card(SMPTE)
	at := func(x, y int) int { return int(pixels[y*Width+x]) }
	for y := 0; y < 160; y++ {
		if at(128, y) != at(128, 0) {
			t.Fatalf("column 128 changes at row %d", y)
		}
	}
	if at(128, 160) != SmpteBlack {
		t.Error("reversed bars do not start at row 160")
	}
	for k, want := range []int{SmpteGray, SmpteYellow, SmpteCyan, SmpteGreen, SmpteMagenta, SmpteRed, SmpteBlue} {
		if at(1+253*k/7+18, 0) != want {
			t.Errorf("bar %d is %d", k, at(1+253*k/7+18, 0))
		}
	}
	for k, want := range []int{SmpteBlue, SmpteBlack, SmpteMagenta, SmpteBlack, SmpteCyan, SmpteBlack, SmpteGray} {
		if at(1+253*k/7+18, 170) != want {
			t.Errorf("reversed bar %d is %d", k, at(1+253*k/7+18, 170))
		}
	}
	// Bars of equal width over the sent columns: 1..253.
	var edges []int
	for x := 1; x < Width; x++ {
		if at(x, 0) != at(x-1, 0) {
			edges = append(edges, x)
		}
	}
	if len(edges) != 6 {
		t.Fatalf("edges %v", edges)
	}
	for i := 1; i < len(edges); i++ {
		if math.Abs(float64(edges[i]-edges[i-1])-253.0/7) > 1 {
			t.Errorf("bar width %d", edges[i]-edges[i-1])
		}
	}
	// -I, white under yellow and cyan, +Q, black, and PLUGE under the red bar.
	for x, want := range map[int]int{20: SmpteMinusI, 60: SmpteWhite, 110: SmptePlusQ, 160: SmpteBlack, 250: SmpteBlack} {
		if at(x, 200) != want {
			t.Errorf("bottom row at %d is %d", x, at(x, 200))
		}
	}
	var pluge []int
	for x := 0; x < Width; x++ {
		if at(x, 0) == SmpteRed && (len(pluge) == 0 || pluge[len(pluge)-1] != at(x, 200)) {
			pluge = append(pluge, at(x, 200))
		}
	}
	if !equalInts(pluge, []int{SmpteBelowBlack, SmpteBlack, SmpteAboveBlack}) {
		t.Errorf("PLUGE order %v", pluge)
	}
	if len(distinct(pixels)) != 13 {
		t.Errorf("%d colors used", len(distinct(pixels)))
	}
}

// The PM5544 on PAL, against ITU-R BT.470 system B/G and the PAL vector diagram.
type palFrame struct {
	pixels []uint8
	frame  []uint8
}

func newPALFrame() palFrame {
	pixels := Card(Philips)
	return palFrame{pixels, PALFrame(pixels, false, 0)}
}

func (f palFrame) line(n int) []uint8 { return f.frame[(n-1)*PALSamplesPerLine : n*PALSamplesPerLine] }

func (f palFrame) phasor(line, start, stop int) complex128 {
	return phasor(f.line(line)[start:stop], ((line-1)*PALSamplesPerLine)&3+start)
}

func TestPALTimingConstants(t *testing.T) {
	us := func(samples int) float64 { return float64(samples) / PALSampleRate * 1e6 }
	near(t, PALSampleRate/4, 4433618.75, 2, "subcarrier")
	// 283.75 cycles a line without the standard's 25 Hz more: 5.7 ppm short of 64 us.
	if PALSamplesPerLine/4.0 != 283.75 {
		t.Error("line is not 283.75 cycles")
	}
	near(t, us(PALSamplesPerLine), 64*(1-5.7e-6), .0001, "line us")
	near(t, PALSampleRate/(PALSamplesPerLine*PALLinesPerFrame), 25, .0002, "frame rate")
	near(t, us(PALSyncSamples), 4.7, .05, "sync")
	near(t, us(PALEqualizing), 2.35, .05, "equalizing")
	near(t, us(PALBroad), 27.3, .05, "broad")
	near(t, us(PALBurstStart), 5.6, .05, "burst start")
	if PALBurstEnd-PALBurstStart != 40 {
		t.Error("burst is not 10 cycles")
	}
	near(t, us(PALActiveStart), 10.5, .05, "picture start")
	near(t, us(PALActiveSamples), 52, .05, "picture")
	near(t, us(PALSamplesPerLine-PALActiveStart-PALActiveSamples), 1.5, .05, "front porch")
}

func TestPALLevelsKeepTheRatioOfSyncToPicture(t *testing.T) {
	near(t, float64(Blank-Sync)/float64(PALWhite-Blank), 3.0/7, .01, "sync to picture")
	palette := DACPalette(Philips)
	if palette[PhilipsBlack] != [4]uint8{Blank, Blank, Blank, Blank} { // no setup
		t.Errorf("black %v", palette[PhilipsBlack])
	}
	if palette[PhilipsWhite] != [4]uint8{PALWhite, PALWhite, PALWhite, PALWhite} {
		t.Errorf("white %v", palette[PhilipsWhite])
	}
	for index := 0; index < PhilipsColorCount; index++ {
		for _, s := range palette[index] {
			if s == 0 || s == 255 {
				t.Errorf("color %d reaches the rail", index)
			}
		}
	}
}

func TestPALVerticalIntervalAndPictureLines(t *testing.T) {
	f := newPALFrame()
	eq := func(at int) pulse { return pulse{at, PALEqualizing} }
	broad := func(at int) pulse { return pulse{at, PALBroad} }
	half := PALHalfLine
	expected := map[int][]pulse{1: {broad(0), broad(half)}, 2: {broad(0), broad(half)}, 3: {broad(0), eq(half)},
		4: {eq(0), eq(half)}, 5: {eq(0), eq(half)}, 311: {eq(0), eq(half)}, 312: {eq(0), eq(half)},
		313: {eq(0), broad(half)}, 314: {broad(0), broad(half)}, 315: {broad(0), broad(half)},
		316: {eq(0), eq(half)}, 317: {eq(0), eq(half)}, 318: {eq(0)}, 623: {{0, PALSyncSamples}, eq(half)},
		624: {eq(0), eq(half)}, 625: {eq(0), eq(half)}}
	var pictured []int
	for line := 1; line <= PALLinesPerFrame; line++ {
		want, interval := expected[line]
		if !interval {
			want = []pulse{{0, PALSyncSamples}}
		}
		if got := pulses(f.line(line)); !samePulses(got, want) {
			t.Errorf("line %d pulses %v, want %v", line, got, want)
		}
		if interval {
			continue
		}
		for _, s := range f.line(line)[PALActiveStart : PALActiveStart+PALActiveSamples] {
			if s != Blank {
				pictured = append(pictured, line)
				break
			}
		}
	}
	var want []int
	for n := PALField1First; n < PALField1First+PALHeight; n++ {
		want = append(want, n)
	}
	for n := PALField2First; n < PALField2First+PALHeight-1; n++ {
		want = append(want, n)
	}
	if !equalInts(pictured, want) {
		t.Errorf("pictured lines %v", pictured)
	}
	// Half lines at the ends of the picture.
	for _, s := range f.line(PALField1First)[PALActiveStart:half] {
		if s != Blank {
			t.Fatal("line 23 has picture in its first half")
		}
	}
	if allBlank(f.line(PALField1First)[half:]) || allBlank(f.line(623)[PALActiveStart:half]) {
		t.Error("the half lines carry no picture")
	}
}

func TestPALBurstBlankingAndSwing(t *testing.T) {
	f := newPALFrame()
	off := func(frame []uint8) []int {
		var lines []int
		for line := 1; line <= PALLinesPerFrame; line++ {
			if ptp(frame[(line-1)*PALSamplesPerLine+PALBurstStart:(line-1)*PALSamplesPerLine+PALBurstEnd]) == 0 {
				lines = append(lines, line)
			}
		}
		return lines
	}
	if got := off(f.frame); !equalInts(got, concat(span(1, 7), span(310, 319), span(622, 626))) {
		t.Errorf("burst off on %v", got)
	}
	second := PALFrame(f.pixels, true, (PALLinesPerFrame*PALSamplesPerLine)&3)
	if got := off(second); !equalInts(got, concat(span(1, 6), span(311, 320), span(623, 626))) {
		t.Errorf("second frame's burst off on %v", got)
	}
	// +/-135 degrees from +U, turning over line by line, and full cycles between the edges.
	start, stop := PALBurstStart+4, PALBurstEnd-4
	for line := 7; line < 300; line++ {
		angle := degrees(f.phasor(line, start, stop))
		near(t, math.Abs(angle), 135, .01, "burst swing on line "+itoa(line))
		near(t, angle, -degrees(f.phasor(line+1, start, stop)), .01, "burst turning over after line "+itoa(line))
	}
	// The first burst of the field leaves V as it is.
	if degrees(f.phasor(7, start, stop)) <= 0 {
		t.Error("line 7's burst has V inverted")
	}
	// A burst the size of NTSC's: 300 mV peak to peak, as sync is, raised 1/.61.
	near(t, cmplx.Abs(f.phasor(7, start, stop))/float64(Blank-Sync)*2*.61, 1, .05, "burst size")
}

func TestPALVSwitchOnTheColorBars(t *testing.T) {
	f := newPALFrame()
	// Degrees from +U on a line with V as it is. V inverted mirrors them about the U axis.
	targets := map[int]float64{PhilipsYellow: 167, PhilipsCyan: 283, PhilipsGreen: 241, PhilipsMagenta: 61, PhilipsRed: 103, PhilipsBlue: 347}
	row := 100
	for _, line := range []int{PALField1First + row, PALField1First + row + 1, PALField2First + row, PALField2First + row + 1} {
		sign := 1.0
		if degrees(f.phasor(line, PALBurstStart+4, PALBurstEnd-4)) < 0 {
			sign = -1
		}
		for color, want := range targets {
			var columns []int
			for x := 0; x < Width; x++ {
				if int(f.pixels[row*Width+x]) == color {
					columns = append(columns, x)
				}
			}
			middle := columns[len(columns)/2-5]
			first := PALActiveStart + PALColumnStart(middle)
			angle := degrees(f.phasor(line, first, first+32))
			near(t, wrap(angle-sign*want), 0, 2, "hue of bar "+itoa(color)+" on line "+itoa(line))
		}
	}
}

func TestPALLumaAndChromaOfABar(t *testing.T) {
	f := newPALFrame()
	// A decoder's result, the mean of a line and the next: red at 75%.
	row, column := 100, 170
	if f.pixels[row*Width+column] != PhilipsRed {
		t.Fatal("column 170 of row 100 is not red")
	}
	first := PALActiveStart + PALColumnStart(column)
	y := .299 * .749
	var decoded complex128
	for _, line := range []int{PALField1First + row, PALField1First + row + 1} {
		samples := f.line(line)[first : first+32]
		near(t, (mean(samples)-Blank)/(PALWhite-Blank), y, .015, "luma on line "+itoa(line))
		z := f.phasor(line, first, first+32) / complex(ChromaBoost*(PALWhite-Blank), 0)
		if degrees(f.phasor(line, PALBurstStart+4, PALBurstEnd-4)) < 0 {
			z = cmplx.Conj(z)
		}
		decoded += z / 2
	}
	near(t, real(decoded), .492*-y, .015, "U of red")
	near(t, imag(decoded), .877*(.749-y), .015, "V of red")
}

func TestGratingsAreTheirFrequencies(t *testing.T) {
	f := newPALFrame()
	line := PALField1First + GratingLastRow - 5
	for burst, megahertz := range GratingMHz {
		start := PALActiveStart + PALColumnStart(PALBandFirstColumn+burst*GratingColumns)
		stop := PALActiveStart + PALColumnStart(PALBandFirstColumn+(burst+1)*GratingColumns)
		samples := f.line(line)[start:stop]
		peak := spectralPeakMHz(samples, PALSampleRate)
		near(t, peak, megahertz, .1, "grating frequency")
		if float64(ptp(samples)) <= .85*(PALWhite-Blank) {
			t.Errorf("%.1f MHz grating swings only %d codes", megahertz, ptp(samples))
		}
		for _, s := range samples {
			if s < Blank || s > PALWhite {
				t.Errorf("%.1f MHz grating leaves black..white", megahertz)
			}
		}
	}
}

// spectralPeakMHz is the strongest frequency of the Hann-windowed samples, from a
// DFT on a 4096-point grid.
func spectralPeakMHz(samples []uint8, rate float64) float64 {
	n := len(samples)
	m := mean(samples)
	windowed := make([]float64, n)
	for i, s := range samples {
		windowed[i] = (float64(s) - m) * (.5 - .5*math.Cos(2*math.Pi*float64(i)/float64(n-1)))
	}
	best, bestMagnitude := 0, -1.0
	for k := 0; k <= 2048; k++ {
		var sum complex128
		for i, w := range windowed {
			sum += complex(w, 0) * cmplx.Exp(complex(0, -2*math.Pi*float64(k*i)/4096))
		}
		if magnitude := cmplx.Abs(sum); magnitude > bestMagnitude {
			best, bestMagnitude = k, magnitude
		}
	}
	return float64(best) / 4096 * rate / 1e6
}

func TestPALCardLayout(t *testing.T) {
	pixels := Card(Philips)
	at := func(x, y int) int { return int(pixels[y*Width+x]) }
	if len(pixels) != Width*PALHeight {
		t.Fatal("the PAL card is not 256x288")
	}
	// A round circle at 4:3, its full height within the picture.
	radiusColumns, radiusRows := 84.0, 126.0
	near(t, radiusColumns/256*4, radiusRows/288*3, 1e-9, "circle roundness")
	if at(44, 144) != PhilipsWhite || at(45, 143) != PhilipsBlack || at(43, 143) == PhilipsBlack || at(128, 18) != PhilipsWhite {
		t.Error("the circle's edge is not where it should be")
	}
	// 19 x 14 grid: lines on the background every 14 columns and 21 rows.
	var whiteColumns []int
	for x := 9; x <= 247; x++ {
		if at(x, 15) == PhilipsWhite && at(x, 14) == PhilipsWhite {
			whiteColumns = append(whiteColumns, x)
		}
	}
	var wantColumns []int
	for x := 9; x < 248; x += 14 {
		wantColumns = append(wantColumns, x)
	}
	if !equalInts(whiteColumns, wantColumns) || len(wantColumns)+1 != 19 || len(span(7, 281))/21+1 != 14 {
		t.Errorf("grid columns %v", whiteColumns)
	}
	for _, y := range []int{7, 28, 49, 70, 217, 238, 259} { // the rest of this column is in the circle
		if at(55, y) != PhilipsWhite || at(55, y+1) == PhilipsWhite {
			t.Errorf("grid row at %d", y)
		}
	}
	if at(55, 280) != PhilipsWhite { // and then the castellations start
		t.Error("row 280 is not white")
	}
	// Bars in their order, and a gray scale of six steps.
	if got := runs(pixels[100*Width+60 : 100*Width+200]); !equalInts(got, []int{PhilipsYellow, PhilipsCyan, PhilipsGreen, PhilipsMagenta, PhilipsRed, PhilipsBlue}) {
		t.Errorf("bars %v", got)
	}
	palette := DACPalette(Philips)
	steps := runs(pixels[205*Width+60 : 205*Width+200])
	if len(steps) != 6 {
		t.Fatalf("gray steps %v", steps)
	}
	for i := 1; i < len(steps); i++ {
		if palette[steps[i]][0] <= palette[steps[i-1]][0] {
			t.Errorf("gray scale is not rising at step %d", i)
		}
	}
	if palette[steps[0]][0] != Blank || palette[steps[5]][0] != PALWhite {
		t.Error("gray scale does not run black to white")
	}
	// Castellations all the way around.
	if len(distinct(pixels[3*Width+10:3*Width+247])) != 2 || at(16, 3) != PhilipsBlack || at(30, 3) != PhilipsWhite ||
		at(4, 16) != PhilipsBlack || at(4, 40) != PhilipsWhite {
		t.Error("castellations are wrong")
	}
}

func mean(samples []uint8) float64 {
	sum := 0.0
	for _, s := range samples {
		sum += float64(s)
	}
	return sum / float64(len(samples))
}

func ptp(samples []uint8) int {
	low, high := 255, 0
	for _, s := range samples {
		low, high = min(low, int(s)), max(high, int(s))
	}
	return high - low
}

func allBlank(samples []uint8) bool {
	for _, s := range samples {
		if s != Blank {
			return false
		}
	}
	return true
}

func distinct(pixels []uint8) map[uint8]bool {
	values := map[uint8]bool{}
	for _, p := range pixels {
		values[p] = true
	}
	return values
}

// runs lists the values of a row in the order they first appear.
func runs(row []uint8) []int {
	var found []int
	seen := map[uint8]bool{}
	for _, v := range row {
		if !seen[v] {
			seen[v] = true
			found = append(found, int(v))
		}
	}
	return found
}

func span(from, to int) []int {
	var values []int
	for i := from; i < to; i++ {
		values = append(values, i)
	}
	return values
}

func concat(lists ...[]int) []int {
	var all []int
	for _, list := range lists {
		all = append(all, list...)
	}
	return all
}

func equalInts(a, b []int) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func itoa(v int) string {
	if v < 0 {
		return "-" + itoa(-v)
	}
	if v < 10 {
		return string(rune('0' + v))
	}
	return itoa(v/10) + string(rune('0'+v%10))
}
