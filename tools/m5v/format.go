// Package m5v reads and writes the M5VTX player's .m5v movies: a little-endian
// header followed by uncompressed 256x240 frames of one byte per pixel, either
// RGB332 (version 1) or indices into a 256-color palette carried in the header
// (version 2). The layout is in tools/M5V_FORMAT.md and the firmware's decoder in
// include/sd_video_format.h; this package applies the same rules.
package m5v

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"math/big"
	"os"
	"unicode/utf8"
)

const (
	Width      = 256
	Height     = 240
	FrameBytes = Width * Height
	MaxFPS     = 30

	MagicRGB332    = "M5RGB332"
	VersionRGB332  = 1
	HeaderRGB332   = 512
	MagicIndexed   = "M5PAL256"
	VersionIndexed = 2
	HeaderIndexed  = 1024

	titleOffset   = 32
	TitleBytes    = 64 // including the terminating NUL
	PaletteOffset = 96
	PaletteBytes  = 256 * 3

	// FAT32MaxBytes is the largest file a FAT32 card holds.
	FAT32MaxBytes = 0xFFFFFFFF
)

// Palette is 256 RGB888 colors, as a version 2 header carries them.
type Palette [256][3]uint8

// Bytes is the palette as the header stores it.
func (p *Palette) Bytes() []byte {
	out := make([]byte, PaletteBytes)
	for i, color := range p {
		copy(out[i*3:], color[:])
	}
	return out
}

// CRC32 identifies a palette, as the firmware's MEDIA_FILE report does.
func (p *Palette) CRC32() uint32 { return crc32.ChecksumIEEE(p.Bytes()) }

// FPS is a frame rate as a positive rational, 1..30 frames per second.
type FPS struct {
	Num int `json:"numerator"`
	Den int `json:"denominator"`
}

// ParseFPS accepts an integer, a fraction such as 30000/1001, or a decimal.
func ParseFPS(text string) (FPS, error) {
	rational, ok := new(big.Rat).SetString(text)
	if !ok {
		return FPS{}, errors.New("FPS must be a number or fraction between 1 and 30")
	}
	if rational.Cmp(big.NewRat(1, 1)) < 0 || rational.Cmp(big.NewRat(MaxFPS, 1)) > 0 {
		return FPS{}, errors.New("FPS must be between 1 and 30")
	}
	if !rational.Num().IsInt64() || rational.Num().Int64() > 0xFFFF || rational.Denom().Int64() > 0xFFFF {
		return FPS{}, errors.New("FPS numerator and denominator must fit unsigned 16-bit fields")
	}
	return FPS{int(rational.Num().Int64()), int(rational.Denom().Int64())}, nil
}

// Float is the rate in frames per second.
func (f FPS) Float() float64 { return float64(f.Num) / float64(f.Den) }

func (f FPS) String() string {
	if f.Den == 1 {
		return fmt.Sprint(f.Num)
	}
	return fmt.Sprintf("%d/%d", f.Num, f.Den)
}

// Header is a decoded movie header. Palette is nil for version 1.
type Header struct {
	Version     int      `json:"version"`
	HeaderBytes int      `json:"header_bytes"`
	FPS         FPS      `json:"fps"`
	FrameCount  uint32   `json:"frame_count"`
	Title       string   `json:"title"`
	CRC32       uint32   `json:"header_crc32"`
	Palette     *Palette `json:"palette_rgb,omitempty"`
}

// Indexed is true for a version 2 movie with its own palette.
func (h Header) Indexed() bool { return h.Palette != nil }

// Duration is the movie's length in seconds.
func (h Header) Duration() float64 { return float64(h.FrameCount) / h.FPS.Float() }

// FileBytes is the exact size a file with this header must have.
func (h Header) FileBytes() int64 { return int64(h.HeaderBytes) + int64(h.FrameCount)*FrameBytes }

// Colors is the palette the player shows: the header's, or RGB332 for version 1.
func (h Header) Colors() Palette {
	if h.Palette != nil {
		return *h.Palette
	}
	return RGB332Palette()
}

// checkTitle applies the firmware's rules: UTF-8 without control characters,
// at most 63 bytes. Longer titles are cut at a character boundary.
func checkTitle(title string) (string, error) {
	if !utf8.ValidString(title) {
		return "", errors.New("title is not valid UTF-8")
	}
	for _, r := range title {
		if r < 0x20 || r == 0x7f {
			return "", errors.New("title cannot contain control characters")
		}
	}
	for len(title) >= TitleBytes {
		_, size := utf8.DecodeLastRuneInString(title)
		title = title[:len(title)-size]
	}
	return title, nil
}

// MakeHeader builds a version 1 header, or a version 2 one when a palette is given.
func MakeHeader(frames uint32, fps FPS, title string, palette *Palette) ([]byte, error) {
	if frames == 0 {
		return nil, errors.New("frame count must be 1..4294967295")
	}
	if fps.Den <= 0 || fps.Num < fps.Den || fps.Num > MaxFPS*fps.Den || fps.Num > 0xFFFF || fps.Den > 0xFFFF {
		return nil, errors.New("FPS must be between 1 and 30 with 16-bit numerator and denominator")
	}
	title, err := checkTitle(title)
	if err != nil {
		return nil, err
	}
	size, magic, version := HeaderRGB332, MagicRGB332, VersionRGB332
	if palette != nil {
		size, magic, version = HeaderIndexed, MagicIndexed, VersionIndexed
	}
	header := make([]byte, size)
	copy(header, magic)
	le := binary.LittleEndian
	le.PutUint16(header[8:], uint16(version))
	le.PutUint16(header[10:], uint16(size))
	le.PutUint16(header[12:], Width)
	le.PutUint16(header[14:], Height)
	le.PutUint16(header[16:], uint16(fps.Num))
	le.PutUint16(header[18:], uint16(fps.Den))
	le.PutUint32(header[20:], frames)
	le.PutUint32(header[24:], FrameBytes)
	copy(header[titleOffset:], title)
	if palette != nil {
		copy(header[PaletteOffset:], palette.Bytes())
	}
	le.PutUint32(header[size-4:], crc32.ChecksumIEEE(header[:size-4]))
	return header, nil
}

// ParseHeader decodes and checks a complete header. A non-negative fileSize is
// checked against the frame count as the firmware checks it.
func ParseHeader(raw []byte, fileSize int64) (Header, error) {
	var h Header
	if len(raw) < 16 {
		return h, errors.New("header is truncated")
	}
	le := binary.LittleEndian
	switch {
	case bytes.Equal(raw[:8], []byte(MagicRGB332)):
		h.Version, h.HeaderBytes = VersionRGB332, HeaderRGB332
	case bytes.Equal(raw[:8], []byte(MagicIndexed)):
		h.Version, h.HeaderBytes = VersionIndexed, HeaderIndexed
	default:
		return h, errors.New("not an M5RGB332 or M5PAL256 movie")
	}
	if int(le.Uint16(raw[8:])) != h.Version {
		return h, errors.New("unsupported format version")
	}
	if int(le.Uint16(raw[10:])) != h.HeaderBytes {
		return h, errors.New("unsupported header size")
	}
	if len(raw) != h.HeaderBytes {
		return h, fmt.Errorf("header must contain exactly %d bytes", h.HeaderBytes)
	}
	if le.Uint16(raw[12:]) != Width || le.Uint16(raw[14:]) != Height {
		return h, errors.New("only 256x240 frames are supported")
	}
	h.FPS = FPS{int(le.Uint16(raw[16:])), int(le.Uint16(raw[18:]))}
	if h.FPS.Den == 0 || h.FPS.Num < h.FPS.Den || h.FPS.Num > MaxFPS*h.FPS.Den {
		return h, errors.New("frame rate must be 1..30 fps")
	}
	h.FrameCount = le.Uint32(raw[20:])
	if h.FrameCount == 0 {
		return h, errors.New("frame count must be nonzero")
	}
	if le.Uint32(raw[24:]) != FrameBytes {
		return h, errors.New("frames must be 61440 bytes")
	}
	if le.Uint32(raw[28:]) != 0 {
		return h, errors.New("unsupported flags")
	}
	titleField := raw[titleOffset : titleOffset+TitleBytes]
	end := bytes.IndexByte(titleField, 0)
	if end < 0 || !allZero(titleField[end:]) {
		return h, errors.New("title must be NUL terminated and padded with zeros")
	}
	h.Title = string(titleField[:end])
	if _, err := checkTitle(h.Title); err != nil {
		return h, err
	}
	reservedStart := PaletteOffset
	if h.Version == VersionIndexed {
		reservedStart = PaletteOffset + PaletteBytes
	}
	if !allZero(raw[reservedStart : h.HeaderBytes-4]) {
		return h, errors.New("nonzero reserved header bytes")
	}
	h.CRC32 = le.Uint32(raw[h.HeaderBytes-4:])
	if h.CRC32 != crc32.ChecksumIEEE(raw[:h.HeaderBytes-4]) {
		return h, errors.New("header CRC32 mismatch")
	}
	if h.Version == VersionIndexed {
		var palette Palette
		for i := range palette {
			copy(palette[i][:], raw[PaletteOffset+i*3:])
		}
		h.Palette = &palette
	}
	if fileSize >= 0 && fileSize != h.FileBytes() {
		return h, fmt.Errorf("payload size mismatch: expected %d bytes, got %d", h.FileBytes(), fileSize)
	}
	return h, nil
}

func allZero(b []byte) bool {
	for _, v := range b {
		if v != 0 {
			return false
		}
	}
	return true
}

// ReadHeader reads whichever header the stream starts with and leaves it at the
// first frame.
func ReadHeader(r io.Reader, fileSize int64) (Header, error) {
	raw := make([]byte, HeaderRGB332)
	if _, err := io.ReadFull(r, raw); err != nil {
		return Header{}, errors.New("header is truncated")
	}
	if bytes.Equal(raw[:8], []byte(MagicIndexed)) {
		rest := make([]byte, HeaderIndexed-HeaderRGB332)
		if _, err := io.ReadFull(r, rest); err != nil {
			return Header{}, errors.New("header is truncated")
		}
		raw = append(raw, rest...)
	}
	return ParseHeader(raw, fileSize)
}

// Info describes a validated movie file.
type Info struct {
	Path            string  `json:"path"`
	Bytes           int64   `json:"bytes"`
	SHA256          string  `json:"sha256"`
	Header          Header  `json:"header"`
	DurationSeconds float64 `json:"duration_seconds"`
}

// Validate checks a movie file's header against its size and hashes it.
func Validate(path string) (Info, error) {
	file, err := os.Open(path)
	if err != nil {
		return Info{}, err
	}
	defer file.Close()
	stat, err := file.Stat()
	if err != nil {
		return Info{}, err
	}
	header, err := ReadHeader(file, stat.Size())
	if err != nil {
		return Info{}, fmt.Errorf("%s: %w", path, err)
	}
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		return Info{}, err
	}
	digest := sha256.New()
	if _, err := io.Copy(digest, file); err != nil {
		return Info{}, err
	}
	return Info{Path: path, Bytes: stat.Size(), SHA256: hex.EncodeToString(digest.Sum(nil)),
		Header: header, DurationSeconds: header.Duration()}, nil
}

// SHA256File hashes a file.
func SHA256File(path string) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	digest := sha256.New()
	if _, err := io.Copy(digest, file); err != nil {
		return "", err
	}
	return hex.EncodeToString(digest.Sum(nil)), nil
}
