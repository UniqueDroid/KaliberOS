#!/usr/bin/env python3
"""
Generates a fixed-width 1-bit bitmap font (ASCII 0x20-0x7e) from a real
TTF, at an arbitrary target size - the same method already used for
components/gfx/gfx_font8x8.h (that file's own header comment): render
each glyph against the font's own ascent/descent metrics (not a per-
glyph bounding box - inconsistent cap/digit height on the first pass),
supersample at a higher resolution, box-filter downsample to the target
size, threshold to 1-bit. No antialiasing/grayscale in the output -
project chat 2026-09-30: "kein Rasterizer zur Laufzeit, kein
Antialiasing, nur größere Glyphen im Flash".

Usage:
    python3 gen_bitmap_font.py <ttf-path> <width> <height> <out.h> <table-name>

Emits a flat, glyph-major byte array: for each of the 95 glyphs (0x20
space .. 0x7e ~), height rows of ceil(width/8) bytes each, MSB-first,
unused high bits in the last byte of a row are 0. Same font source
(Liberation Mono Regular) and same visual-verification discipline as
the 8x8 font - PNG_DUMP=1 writes one sanity-check PNG per invocation
before committing a generated header, don't skip that step.
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

def main():
    ttf_path, width, height, out_path, table_name = sys.argv[1:6]
    width, height = int(width), int(height)
    row_bytes = (width + 7) // 8
    base_font = ImageFont.truetype(ttf_path, size=height)

    glyphs = []
    for code in range(0x20, 0x7f):
        ch = chr(code)
        img = render_glyph(base_font, ch, width, height)
        glyphs.append(pack_rows(img, width, height))

    import os
    if os.environ.get("PNG_DUMP"):
        sheet = Image.new("L", (width * 16, height * 6), 0)
        for i, code in enumerate(range(0x20, 0x7f)):
            gx, gy = (i % 16) * width, (i // 16) * height
            g = render_glyph(base_font, chr(code), width, height)
            sheet.paste(g, (gx, gy))
        png_path = out_path.rsplit(".", 1)[0] + "_sheet.png"
        sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(png_path)
        print(f"wrote {png_path}")

    with open(out_path, "w") as f:
        f.write(f"""/**
 * Built-in {width}x{height} bitmap font, ASCII 0x20-0x7e (space..~).
 * Generated from Liberation Mono Regular by tools/fontgen/gen_bitmap_font.py
 * - same method as gfx_font8x8.h (render against the font's own ascent/
 * descent metrics, box-filter downsample, threshold to 1-bit, no runtime
 * rasterizer/antialiasing). Flat glyph-major byte array: {height} rows
 * of {row_bytes} bytes each per glyph, MSB-first, unused high bits in a
 * row's last byte are 0.
 */
#pragma once
#include <stdint.h>

#define {table_name.upper()}_W {width}
#define {table_name.upper()}_H {height}
#define {table_name.upper()}_ROW_BYTES {row_bytes}

static const uint8_t {table_name}[95 * {height} * {row_bytes}] = {{
""")
        for i, g in enumerate(glyphs):
            code = 0x20 + i
            ch_comment = chr(code) if 0x21 <= code <= 0x7e and chr(code) not in ('\\', "'") else " "
            f.write(f"    /* 0x{code:02x} {ch_comment} */\n    ")
            f.write(", ".join(f"0x{b:02x}" for b in g))
            f.write(",\n")
        f.write("};\n")
    print(f"wrote {out_path} ({len(glyphs)} glyphs, {row_bytes*height} B/glyph, {row_bytes*height*95} B total)")

if __name__ == "__main__":
    main()
