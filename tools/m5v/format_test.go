package m5v

import (
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"strings"
	"testing"
)

func sealed(header []byte) []byte {
	binary.LittleEndian.PutUint32(header[len(header)-4:], crc32.ChecksumIEEE(header[:len(header)-4]))
	return header
}

func TestV1HeaderMatchesIndependentLayoutAndCRC(t *testing.T) {
	actual, err := MakeHeader(445, FPS{15, 1}, "reaction", nil)
	if err != nil {
		t.Fatal(err)
	}
	expected := make([]byte, 512)
	copy(expected, "M5RGB332")
	for offset, value := range map[int]uint16{8: 1, 10: 512, 12: 256, 14: 240, 16: 15, 18: 1} {
		binary.LittleEndian.PutUint16(expected[offset:], value)
	}
	binary.LittleEndian.PutUint32(expected[20:], 445)
	binary.LittleEndian.PutUint32(expected[24:], 61440)
	copy(expected[32:], "reaction")
	if !bytes.Equal(actual, sealed(expected)) {
		t.Fatal("header bytes differ from the documented layout")
	}
	parsed, err := ParseHeader(actual, 512+445*61440)
	if err != nil {
		t.Fatal(err)
	}
	if parsed.FrameCount != 445 || parsed.FPS != (FPS{15, 1}) || parsed.Title != "reaction" || parsed.Indexed() {
		t.Fatalf("parsed %+v", parsed)
	}
}

func TestV2HeaderCarriesThePalette(t *testing.T) {
	var palette Palette
	for i := range palette {
		palette[i] = [3]uint8{uint8(i), uint8(255 - i), uint8(i * 3)}
	}
	palette[0] = [3]uint8{}
	actual, err := MakeHeader(444, FPS{15, 1}, "palette", &palette)
	if err != nil {
		t.Fatal(err)
	}
	expected := make([]byte, 1024)
	copy(expected, "M5PAL256")
	for offset, value := range map[int]uint16{8: 2, 10: 1024, 12: 256, 14: 240, 16: 15, 18: 1} {
		binary.LittleEndian.PutUint16(expected[offset:], value)
	}
	binary.LittleEndian.PutUint32(expected[20:], 444)
	binary.LittleEndian.PutUint32(expected[24:], 61440)
	copy(expected[32:], "palette")
	copy(expected[96:], palette.Bytes())
	if !bytes.Equal(actual, sealed(expected)) {
		t.Fatal("version 2 header bytes differ from the documented layout")
	}
	stream := bytes.NewReader(append(actual, make([]byte, 61440*444)...))
	header, err := ReadHeader(stream, int64(stream.Len()))
	if err != nil {
		t.Fatal(err)
	}
	if stream.Len() != 61440*444 {
		t.Fatal("ReadHeader did not stop at the first frame")
	}
	if header.Version != 2 || header.HeaderBytes != 1024 || *header.Palette != palette || header.Colors() != palette {
		t.Fatalf("parsed %+v", header)
	}
}

func TestHeaderRejections(t *testing.T) {
	good, _ := MakeHeader(2, FPS{15, 1}, "", nil)
	damage := func(offset int, value byte) []byte {
		copy := append([]byte(nil), good...)
		copy[offset] = value
		return sealed(copy)
	}
	cases := map[string]struct {
		raw  []byte
		size int64
		want string
	}{
		"truncated":     {good[:511], -1, "exactly 512"},
		"crc":           {append(append([]byte(nil), good[:508]...), append([]byte{good[508] ^ 1}, good[509:]...)...), -1, "CRC32"},
		"short payload": {good, 512 + 61440, "payload size"},
		"long payload":  {good, 512 + 2*61440 + 1, "payload size"},
		"width":         {damage(12, 0x40), -1, "256x240"},
		"reserved":      {damage(96, 1), -1, "reserved"},
		"flags":         {damage(28, 1), -1, "flags"},
		"frame bytes":   {damage(24, 1), -1, "61440"},
		"fps zero":      {damage(16, 0), -1, "1..30"},
		"fps 31":        {damage(16, 31), -1, "1..30"},
		"no frames":     {damage(20, 0), -1, "nonzero"},
		"title padding": {damage(50, 'x'), -1, "NUL"},
		"control char":  {damage(32, 9), -1, "control"},
		"magic":         {damage(0, 'X'), -1, "not an"},
		"version":       {damage(8, 2), -1, "version"},
	}
	for name, c := range cases {
		if _, err := ParseHeader(c.raw, c.size); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: got %v, want %q", name, err, c.want)
		}
	}
	if _, err := ParseHeader(good, 512+2*61440); err != nil {
		t.Fatal(err)
	}
	var palette Palette
	v2, _ := MakeHeader(2, FPS{15, 1}, "", &palette)
	damaged := append([]byte(nil), v2...)
	damaged[864] = 1
	if _, err := ParseHeader(sealed(damaged), -1); err == nil || !strings.Contains(err.Error(), "reserved") {
		t.Errorf("version 2 reserved bytes: %v", err)
	}
	if _, err := ParseHeader(v2[:512], -1); err == nil || !strings.Contains(err.Error(), "1024") {
		t.Errorf("version 2 truncated: %v", err)
	}
}

func TestFPSAndTitles(t *testing.T) {
	for text, want := range map[string]FPS{"1": {1, 1}, "30": {30, 1}, "30000/1001": {30000, 1001}, "29.97": {2997, 100}, "12.5": {25, 2}} {
		got, err := ParseFPS(text)
		if err != nil || got != want {
			t.Errorf("%s: %v %v", text, got, err)
		}
	}
	for _, text := range []string{"0", "31", "1/2", "1/0", "not a rate", "65536/2185"} {
		if _, err := ParseFPS(text); err == nil {
			t.Errorf("%s accepted", text)
		}
	}
	long := strings.Repeat("é", 40)
	header, err := MakeHeader(1, FPS{1, 1}, long, nil)
	if err != nil {
		t.Fatal(err)
	}
	parsed, err := ParseHeader(header, -1)
	if err != nil || parsed.Title != strings.Repeat("é", 31) {
		t.Fatalf("title %q %v", parsed.Title, err)
	}
	for _, bad := range []string{"a\x00b", "tab\there", "\xff"} {
		if _, err := MakeHeader(1, FPS{1, 1}, bad, nil); err == nil {
			t.Errorf("title %q accepted", bad)
		}
	}
	if _, err := MakeHeader(0, FPS{1, 1}, "", nil); err == nil {
		t.Error("zero frames accepted")
	}
}
