/* Minimal PNG encoder - just enough to dump a host-rendered framebuffer
 * to a file a human can look at (tools/host_render's whole point). Uses
 * zlib for the IDAT deflate stream and crc32, nothing else external. */
#include "png_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;

    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const unsigned char *)type, 4);
    if (len) crc = crc32(crc, data, len);
    uint8_t crcbuf[4];
    put_be32(crcbuf, crc);
    return fwrite(crcbuf, 1, 4, f) == 4 ? 0 : -1;
}

/* color_type: 0 = grayscale (1 byte/px), 2 = RGB (3 bytes/px). */
static int png_write(const char *path, const uint8_t *pixels, int w, int h,
                      int color_type, int bytes_per_px) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (fwrite(sig, 1, 8, f) != 8) { fclose(f); return -1; }

    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;            /* bit depth */
    ihdr[9] = (uint8_t)color_type;
    ihdr[10] = 0;           /* compression */
    ihdr[11] = 0;           /* filter */
    ihdr[12] = 0;           /* interlace */
    if (write_chunk(f, "IHDR", ihdr, sizeof ihdr) != 0) { fclose(f); return -1; }

    /* Filter byte 0 (none) prepended to every scanline, per spec. */
    size_t row_bytes = (size_t)w * bytes_per_px;
    size_t raw_len = (row_bytes + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) { fclose(f); return -1; }
    for (int y = 0; y < h; y++) {
        uint8_t *dst = raw + (size_t)y * (row_bytes + 1);
        dst[0] = 0;
        memcpy(dst + 1, pixels + (size_t)y * row_bytes, row_bytes);
    }

    uLongf comp_cap = compressBound((uLong)raw_len);
    uint8_t *comp = malloc(comp_cap);
    if (!comp) { free(raw); fclose(f); return -1; }
    if (compress2(comp, &comp_cap, raw, (uLong)raw_len, Z_BEST_COMPRESSION) != Z_OK) {
        free(raw); free(comp); fclose(f); return -1;
    }
    free(raw);

    int rc = write_chunk(f, "IDAT", comp, (uint32_t)comp_cap);
    free(comp);
    if (rc != 0) { fclose(f); return -1; }

    rc = write_chunk(f, "IEND", NULL, 0);
    fclose(f);
    return rc;
}

int png_write_gray8(const char *path, const uint8_t *pixels, int w, int h) {
    return png_write(path, pixels, w, h, 0, 1);
}

int png_write_rgb8(const char *path, const uint8_t *pixels, int w, int h) {
    return png_write(path, pixels, w, h, 2, 3);
}
