package reference

import (
	"image"
	"image/color"
	"math"
)

// Image is the pattern as a receiver should show it. The SMPTE card's colors
// without RGB are shown as their nearest colors, and the PM5544's gratings as
// each column's mean level: both only for looking at.
func Image(p Pattern) *image.NRGBA {
	pixels := Card(p)
	rgb := PatternRGB(p)
	img := image.NewNRGBA(image.Rect(0, 0, Width, p.Rows()))
	for i, index := range pixels {
		c := rgb[index]
		img.Set(i%Width, i/Width, color.NRGBA{round(c[0] * 255), round(c[1] * 255), round(c[2] * 255), 255})
	}
	if p == SMPTE {
		substitutes := map[uint8]color.NRGBA{SmpteBelowBlack: {0, 0, 0, 255}, SmpteMinusI: {0, 33, 76, 255}, SmptePlusQ: {50, 0, 106, 255}}
		for i, index := range pixels {
			if c, ok := substitutes[index]; ok {
				img.Set(i%Width, i/Width, c)
			}
		}
	}
	if p == Philips {
		block, plain := GratingSamples()
		origin := PALColumnStart(PALBandFirstColumn)
		for _, band := range []struct {
			first, last int
			samples     [PALBandSamples]uint8
		}{{GratingFirstRow, GratingBlockEnd, block}, {GratingBlockEnd, GratingLastRow, plain}} {
			for x := PALBandFirstColumn; x < PALBandFirstColumn+PALBandColumns; x++ {
				from, to := PALColumnStart(x)-origin, PALColumnStart(x+1)-origin
				sum := 0.0
				for i := from; i < to; i++ {
					sum += float64(band.samples[i])
				}
				level := round((sum/float64(to-from) - Blank) / (PALWhite - Blank) * 255)
				for y := band.first; y < band.last; y++ {
					img.Set(x, y, color.NRGBA{level, level, level, 255})
				}
			}
		}
	}
	return img
}

func round(v float64) uint8 {
	return uint8(math.Max(0, math.Min(255, math.Floor(v+.5))))
}
