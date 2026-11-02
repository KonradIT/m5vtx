# M5V indexed video formats

Both formats hold silent, uncompressed 256x240 frames of one byte per pixel.
Version 1 uses the fixed RGB332 palette. Version 2 stores a clip-specific
256-color RGB palette in its header. Frame size and SD traffic are the same.

## Version 1: fixed RGB332

Files use the `.m5v` extension and live in the SD card's root. All integer
fields are unsigned little-endian. A 512-byte header is followed by complete
frames of exactly 256 x 240 = 61,440 bytes each, row-major from the top left. A
byte is `RRRGGGBB`, an index into the fixed palette: red and green have eight
evenly spaced levels from 0 to 255, blue four.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | ASCII `M5RGB332` |
| 8 | 2 | Version: 1 |
| 10 | 2 | Header size: 512 |
| 12 | 2 | Width: 256 |
| 14 | 2 | Height: 240 |
| 16 | 2 | Frame-rate numerator |
| 18 | 2 | Frame-rate denominator |
| 20 | 4 | Frame count, at least 1 |
| 24 | 4 | Frame size: 61,440 |
| 28 | 4 | Flags: 0 |
| 32 | 64 | UTF-8 title, NUL terminated, at most 63 bytes, zero padded |
| 96 | 412 | Reserved: zero |
| 508 | 4 | IEEE CRC32 of bytes 0 through 507 |

The file length must be exactly `512 + frame_count * 61440`. The frame rate is a
positive rational from 1 to 30 fps; 15/1 is the converter's default. There is no
audio, padding, compression or custom palette. The header and every frame start
on a 512-byte sector boundary. The converter also enforces FAT32's file size limit.

## Version 2: clip-specific 256-color palette

Version 2 shares offsets 12 through 95 with version 1. The rest is:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | ASCII `M5PAL256` |
| 8 | 2 | Version: 2 |
| 10 | 2 | Header size: 1024 |
| 96 | 768 | 256 RGB888 entries, three bytes R, G, B per entry |
| 864 | 156 | Reserved: zero |
| 1020 | 4 | IEEE CRC32 of bytes 0 through 1019 |

Each pixel byte indexes its RGB888 entry. The file length must be exactly
`1024 + frame_count * 61440`. The CRC covers the palette. The palette does not
change between frames. The firmware computes each palette's color-carrier
waveforms outside the scanline interrupt and caches a table per framebuffer, so
image and palette switch at the same vertical blanking; version 1 movies and the
built-in images restore the RGB332 palette.

## Conversion

```sh
convertvideo clip.mp4 E:/clip.m5v
convertvideo -palette rgb332 -quantization ordered clip.mp4 E:/clip_rgb332.m5v
convertvideo -validate E:/clip.m5v
```

`convertvideo` decodes with ffmpeg to RGB24 and packs every frame itself, so
pixel-format aliases cannot change channel order. The source is fitted inside a
320x240 display without cropping, padded with black, then scaled to the 256x240
raster: the display shows the raster at 4:3, so its pixels are 5:4. Square
footage occupies 192x240 raster pixels between 32-pixel side bars.

For version 2, a first pass through the clip gathers a 5-bit per channel color
histogram with exact sums, skipping exact black, from every frame; a median cut
over the occupied bins gives 255 colors and index 0 is black. The second pass
maps every pixel to its nearest palette color. For version 1, `-quantization`
picks ordered dithering (a fixed 4x4 Bayer matrix, the same on every frame, so
no flicker), nearest levels, or plain bit truncation.

The converter writes a temporary file in the destination directory, validates
the finished file with the firmware's rules, renames it into place, and reads
the destination back to compare hashes. Existing outputs need `-force`.
`-report FILE` saves a JSON record (source hash, ffprobe output, ffmpeg
arguments, geometry, palette training and the validated output) and
`-previews DIR` saves five frames as PNG.

## Validation

The firmware checks extension, magic, version, dimensions, rate, frame size,
flags, title encoding, reserved bytes, header CRC and exact file length before
listing a movie, and again when playback starts, including the version 2
palette's CRC against the catalog entry. It scans at most 512 root entries and
keeps the first 32 valid movies in name order. It does not checksum pixels.
