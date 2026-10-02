#pragma once
#include <stdint.h>
#include <stddef.h>

/* Minimal 8-bit grayscale PNG writer (no palette, no interlace). */
int png_write_gray8(const char *path, const uint8_t *pixels, int w, int h);

/* Minimal 24-bit RGB PNG writer. pixels is w*h*3 bytes, row-major, no padding. */
int png_write_rgb8(const char *path, const uint8_t *pixels, int w, int h);
