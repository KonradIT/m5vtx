package m5v

import (
	"errors"
	"math"
	"sort"
)

// Rect is a crop, as x0, y0, x1, y1 with x1 and y1 not included.
type Rect struct{ X0, Y0, X1, Y1 int }

// Trainer gathers color statistics from every frame of a clip, in a 5-bit per
// channel histogram with exact RGB sums: about 1 MiB, however long the movie.
// Index 0 of the palette it makes is exact black, for letterboxes and borders.
type Trainer struct {
	crop   Rect
	stride int
	counts [32768]uint64
	sums   [3][32768]uint64
	frames int
	pixels int
}

// Training describes how a palette was made, for a conversion report.
type Training struct {
	Method              string `json:"method"`
	FramesSampled       int    `json:"frames_sampled"`
	PixelStride         int    `json:"pixel_stride"`
	Crop                Rect   `json:"crop_x0_y0_x1_y1"`
	SampledPixels       int    `json:"sampled_pixels"`
	NonblackSamples     uint64 `json:"nonblack_samples"`
	OccupiedColorBins   int    `json:"occupied_color_bins"`
	DistinctColors      int    `json:"distinct_palette_colors"`
	ReservedBlackIndex  int    `json:"reserved_black_index"`
	PaletteCRC32        uint32 `json:"palette_crc32"`
	PaletteChangesBetweenFrames bool `json:"palette_changes_between_frames"`
}

// NewTrainer samples every stride'th pixel inside crop.
func NewTrainer(crop Rect, stride int) *Trainer {
	if stride < 1 {
		stride = 1
	}
	return &Trainer{crop: crop, stride: stride}
}

// AddFrame adds a Width x Height RGB24 frame.
func (t *Trainer) AddFrame(rgb []uint8) {
	t.frames++
	for y := t.crop.Y0; y < t.crop.Y1; y += t.stride {
		for x := t.crop.X0; x < t.crop.X1; x += t.stride {
			i := (y*Width + x) * 3
			r, g, b := rgb[i], rgb[i+1], rgb[i+2]
			t.pixels++
			if r == 0 && g == 0 && b == 0 {
				continue
			}
			bin := uint32(r>>3)<<10 | uint32(g>>3)<<5 | uint32(b>>3)
			t.counts[bin]++
			t.sums[0][bin] += uint64(r)
			t.sums[1][bin] += uint64(g)
			t.sums[2][bin] += uint64(b)
		}
	}
}

type colorBin struct {
	mean   [3]float64
	weight uint64
}

// Finish makes the palette: black, then up to 255 colors from a median cut of
// the occupied histogram bins, each bin at its exact mean color.
func (t *Trainer) Finish() (Palette, Training, error) {
	if t.frames == 0 {
		return Palette{}, Training{}, errors.New("no frames were available for palette training")
	}
	var bins []colorBin
	var total uint64
	for i, count := range t.counts {
		if count == 0 {
			continue
		}
		total += count
		bins = append(bins, colorBin{weight: count, mean: [3]float64{
			float64(t.sums[0][i]) / float64(count),
			float64(t.sums[1][i]) / float64(count),
			float64(t.sums[2][i]) / float64(count)}})
	}
	var palette Palette
	colors := medianCut(bins, 255)
	for i, color := range colors {
		palette[1+i] = color
	}
	distinct := map[[3]uint8]bool{}
	for _, color := range palette {
		distinct[color] = true
	}
	report := Training{Method: "clip-global median cut over a 5-bit histogram, nearest RGB mapping",
		FramesSampled: t.frames, PixelStride: t.stride, Crop: t.crop, SampledPixels: t.pixels,
		NonblackSamples: total, OccupiedColorBins: len(bins), DistinctColors: len(distinct),
		ReservedBlackIndex: 0, PaletteCRC32: palette.CRC32()}
	return palette, report, nil
}

// medianCut splits the bins into at most count boxes, always the heaviest box
// along its widest axis at its weight's median, and returns each box's weighted
// mean color. It is deterministic for a given histogram.
func medianCut(bins []colorBin, count int) [][3]uint8 {
	if len(bins) == 0 {
		return nil
	}
	boxes := [][]colorBin{bins}
	for len(boxes) < count {
		chosen, heaviest := -1, uint64(0)
		for i, box := range boxes {
			if len(box) < 2 {
				continue
			}
			if weight := boxWeight(box); weight > heaviest {
				chosen, heaviest = i, weight
			}
		}
		if chosen < 0 {
			break
		}
		box := boxes[chosen]
		axis := widestAxis(box)
		sort.SliceStable(box, func(i, j int) bool {
			for k := 0; k < 3; k++ {
				a := (axis + k) % 3
				if box[i].mean[a] != box[j].mean[a] {
					return box[i].mean[a] < box[j].mean[a]
				}
			}
			return false
		})
		half, cut := heaviest/2, 1
		for sum := box[0].weight; cut < len(box)-1 && sum < half; cut++ {
			sum += box[cut].weight
		}
		boxes[chosen] = box[:cut]
		boxes = append(boxes, box[cut:])
	}
	colors := make([][3]uint8, 0, len(boxes))
	for _, box := range boxes {
		var sum [3]float64
		weight := boxWeight(box)
		for _, bin := range box {
			for c := range sum {
				sum[c] += bin.mean[c] * float64(bin.weight)
			}
		}
		var color [3]uint8
		for c := range color {
			color[c] = uint8(math.Min(255, math.Floor(sum[c]/float64(weight)+.5)))
		}
		colors = append(colors, color)
	}
	return colors
}

func boxWeight(box []colorBin) uint64 {
	var weight uint64
	for _, bin := range box {
		weight += bin.weight
	}
	return weight
}

func widestAxis(box []colorBin) int {
	low, high := [3]float64{255, 255, 255}, [3]float64{}
	for _, bin := range box {
		for c := range low {
			low[c] = math.Min(low[c], bin.mean[c])
			high[c] = math.Max(high[c], bin.mean[c])
		}
	}
	axis := 0
	for c := 1; c < 3; c++ {
		if high[c]-low[c] > high[axis]-low[axis] {
			axis = c
		}
	}
	return axis
}

// Mapper finds each RGB888 color's nearest palette entry, by Euclidean distance,
// the lowest index winning a tie. Results are remembered per color.
type Mapper struct {
	palette Palette
	cache   []uint8
	known   []uint64
}

// NewMapper prepares a mapper for one clip palette.
func NewMapper(palette Palette) *Mapper {
	return &Mapper{palette: palette, cache: make([]uint8, 1<<24), known: make([]uint64, 1<<24/64)}
}

// Index is the nearest entry for one color.
func (m *Mapper) Index(r, g, b uint8) uint8 {
	key := uint32(r)<<16 | uint32(g)<<8 | uint32(b)
	if m.known[key/64]&(1<<(key%64)) != 0 {
		return m.cache[key]
	}
	best, bestDistance := 0, math.MaxInt
	for i, color := range m.palette {
		dr, dg, db := int(r)-int(color[0]), int(g)-int(color[1]), int(b)-int(color[2])
		if distance := dr*dr + dg*dg + db*db; distance < bestDistance {
			best, bestDistance = i, distance
			if distance == 0 {
				break
			}
		}
	}
	m.cache[key] = uint8(best)
	m.known[key/64] |= 1 << (key % 64)
	return uint8(best)
}

// Map converts an RGB24 image into palette indices.
func (m *Mapper) Map(rgb []uint8, out []uint8) {
	for i := range out {
		out[i] = m.Index(rgb[i*3], rgb[i*3+1], rgb[i*3+2])
	}
}
