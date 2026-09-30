#!/usr/bin/env python3
"""
Generates a fixed-width 1-bit bitmap font from a real TTF, at an
arbitrary target size and ASCII range - the same method already used
for components/gfx/gfx_font8x8.h (that file's own header comment):
render each glyph against the font's own ascent/descent metrics (not a
per-glyph bounding box - inconsistent cap/digit height on the first
pass), supersample at a higher resolution, box-filter downsample to the
target size, threshold to 1-bit. No antialiasing/grayscale in the
output - project chat 2026-09-30: "kein Rasterizer zur Laufzeit, kein
Antialiasing, nur größere Glyphen im Flash".

Usage:
    python3 gen_bitmap_font.py <ttf-path> <width> <height> <out.h> <table-name> [first] [last]

first/last (optional, hex or decimal, default 0x20/0x7e - the full
printable ASCII range) restrict the generated range - e.g. 0x30 0x3a
for "0123456789:" only. gfx_font_t (components/gfx/include/gfx/text.h)
indexes glyphs as `c - first`, so this only works for a *contiguous*
range - not an arbitrary character subset. Digits+colon happen to be
contiguous in ASCII ('0'-'9' is 0x30-0x39, ':' is 0x3a right after),
which is exactly the restriction project chat 2026-09-30 asked for (a
large digits-only "time" size, to afford a bigger native font without
paying full-ASCII flash cost for glyphs a clock face never uses).

Emits a flat, glyph-major byte array: for each glyph in [first, last],
height rows of ceil(width/8) bytes each, MSB-first, unused high bits in
the last byte of a row are 0. Same font source (Liberation Mono
Regular) and same visual-verification discipline as the 8x8 font -
PNG_DUMP=1 writes one sanity-check PNG per invocation before committing
a generated header, don't skip that step - and re-check the digits
specifically (1/4/7 broke silently on the original 8x8 font's first
pass, at the packed-bit level, not just the antialiased preview).
"""
import sys
from PIL import Image, ImageDraw, ImageFont

def render_glyph(font, ch, width, height, supersample=4):
    sw, sh = width * supersample, height * supersample
    img = Image.new("L", (sw, sh), 0)
    draw = ImageDraw.Draw(img)
    ascent, descent = font.getmetrics()
    # Scale the supersampled canvas to the font's own ascent+descent, not
    # a per-glyph bbox - same reasoning as the 8x8 font's own comment.
    scale = sh / (ascent + descent)
    draw_font = ImageFont.truetype(font.path, size=int(font.size * scale))
    ascent2, descent2 = draw_font.getmetrics()
    bbox = draw_font.getbbox(ch)
    cw = draw_font.getlength(ch)
    x = (sw - cw) / 2 if cw < sw else 0
    y = (sh - (ascent2 + descent2)) / 2
    draw.text((x, y), ch, font=draw_font, fill=255)
    img = img.resize((width, height), Image.BOX)
    return img

def pack_rows(img, width, height):
    row_bytes = (width + 7) // 8
    out = bytearray(row_bytes * height)
    px = img.load()
    for yy in range(height):
        for xx in range(width):
            if px[xx, yy] >= 128:
                out[yy * row_bytes + xx // 8] |= 0x80 >> (xx % 8)
    return bytes(out)

def parse_code(s):
    return int(s, 0)  # accepts "0x30" or "48"

def main():
    argv = sys.argv[1:]
    ttf_path, width, height, out_path, table_name = argv[0:5]
    width, height = int(width), int(height)
    first = parse_code(argv[5]) if len(argv) > 5 else 0x20
    last = parse_code(argv[6]) if len(argv) > 6 else 0x7e
    n_glyphs = last - first + 1
    row_bytes = (width + 7) // 8
    base_font = ImageFont.truetype(ttf_path, size=height)

    glyphs = []
    for code in range(first, last + 1):
        ch = chr(code)
        img = render_glyph(base_font, ch, width, height)
        glyphs.append(pack_rows(img, width, height))

    import os
    if os.environ.get("PNG_DUMP"):
        cols = min(n_glyphs, 16)
        rows = (n_glyphs + cols - 1) // cols
        sheet = Image.new("L", (width * cols, height * rows), 0)
        for i, code in enumerate(range(first, last + 1)):
            gx, gy = (i % cols) * width, (i // cols) * height
            g = render_glyph(base_font, chr(code), width, height)
            sheet.paste(g, (gx, gy))
        png_path = out_path.rsplit(".", 1)[0] + "_sheet.png"
        sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(png_path)
        print(f"wrote {png_path}")

    range_comment = (f"ASCII 0x{first:02x}-0x{last:02x} "
                      f"({chr(first)}..{chr(last)})")
    with open(out_path, "w") as f:
        f.write(f"""/**
 * Built-in {width}x{height} bitmap font, {range_comment}.
 * Generated from Liberation Mono Regular by tools/fontgen/gen_bitmap_font.py
 * - same method as gfx_font8x8.h (render against the font's own ascent/
 * descent metrics, box-filter downsample, threshold to 1-bit, no runtime
 * rasterizer/antialiasing). Flat glyph-major byte array: {height} rows
 * of {row_bytes} bytes each per glyph, MSB-first, unused high bits in a
 * row's last byte are 0. Range restricted to {n_glyphs} glyphs (not the
 * full 95) to afford a larger native size at a fraction of the flash
 * cost - see gfx/text.h's gfx_font_t.first/.last.
 */
#pragma once
#include <stdint.h>

#define {table_name.upper()}_W {width}
#define {table_name.upper()}_H {height}
#define {table_name.upper()}_ROW_BYTES {row_bytes}
#define {table_name.upper()}_FIRST 0x{first:02x}
#define {table_name.upper()}_LAST 0x{last:02x}

static const uint8_t {table_name}[{n_glyphs} * {height} * {row_bytes}] = {{
""")
        for i, g in enumerate(glyphs):
            code = first + i
            ch_comment = chr(code) if 0x21 <= code <= 0x7e and chr(code) not in ('\\', "'") else " "
            f.write(f"    /* 0x{code:02x} {ch_comment} */\n    ")
            f.write(", ".join(f"0x{b:02x}" for b in g))
            f.write(",\n")
        f.write("};\n")
    print(f"wrote {out_path} ({len(glyphs)} glyphs, {row_bytes*height} B/glyph, {row_bytes*height*n_glyphs} B total)")

if __name__ == "__main__":
    main()
