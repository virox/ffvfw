#include <string.h>
#include "pixout.h"

#ifndef BI_BITFIELDS
#define BI_BITFIELDS 3
#endif

static int popc(uint32_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }
static int ctz(uint32_t v) { int n = 0; while (!(v & 1)) { v >>= 1; n++; } return n; }
static int contiguous(uint32_t m) { return m && ((m >> ctz(m)) & ((m >> ctz(m)) + 1)) == 0; }

int outfmt_parse(const BITMAPINFOHEADER *bi, OutFmt *o)
{
    const uint32_t *m = NULL;
    memset(o, 0, sizeof *o);
    if (!bi || bi->biSize < sizeof(BITMAPINFOHEADER) || bi->biPlanes != 1 || bi->biWidth <= 0 || bi->biHeight == 0)
        return ICERR_BADFORMAT;
    if (bi->biWidth > 16384 || bi->biHeight > 16384 || bi->biHeight < -16384) return ICERR_BADFORMAT;
    o->w = bi->biWidth;
    o->topdown = bi->biHeight < 0;
    o->h = o->topdown ? -bi->biHeight : bi->biHeight;
    o->bpp = bi->biBitCount;
    o->size_hint = bi->biSizeImage;
    if (o->bpp != 16 && o->bpp != 24 && o->bpp != 32) return ICERR_BADBITDEPTH;
    o->stride = ((o->w * o->bpp + 31) / 32) * 4;

    if (bi->biCompression == BI_RGB) {
        if (o->bpp == 16) { o->mask[0] = 0x7C00; o->mask[1] = 0x03E0; o->mask[2] = 0x001F; }
        else if (o->bpp == 32) { o->mask[0] = 0xFF0000; o->mask[1] = 0xFF00; o->mask[2] = 0xFF; }
    } else if (bi->biCompression == BI_BITFIELDS) {
        if (o->bpp == 24) return ICERR_BADFORMAT;
        /* masks live in the V4/V5 header, or directly after a plain 40-byte header */
        m = (const uint32_t *)((const uint8_t *)bi + sizeof(BITMAPINFOHEADER));
        o->mask[0] = m[0]; o->mask[1] = m[1]; o->mask[2] = m[2];
        if (bi->biSize >= 56) o->mask[3] = m[3];   /* BITMAPV3INFOHEADER+ has an alpha mask */
    } else return ICERR_BADFORMAT;

    if (o->bpp != 24) {
        uint32_t limit = o->bpp == 32 ? 0xFFFFFFFFu : 0xFFFFu, all = 0; int i;
        for (i = 0; i < 4; i++) {
            if (i == 3 && !o->mask[3]) break;
            if (!contiguous(o->mask[i]) || (o->mask[i] & ~limit) || (all & o->mask[i])) return ICERR_BADFORMAT;
            all |= o->mask[i];
        }
    }
    return 0;
}

size_t outfmt_bytes(const OutFmt *o) { return (size_t)o->stride * o->h; }

void outfmt_write(const OutFmt *o, const uint8_t *bgr, int bgr_stride, int bgr32, uint8_t *dst)
{
    int y, x, sp = bgr32 ? 4 : 3;
    uint32_t lut[3][256], alpha = 0; int sh[3], c;
    if (o->bpp != 24) {
        for (c = 0; c < 3; c++) {
            int bits = popc(o->mask[c]), v; sh[c] = ctz(o->mask[c]);
            for (v = 0; v < 256; v++) lut[c][v] = (uint32_t)((v * ((1u << bits) - 1) + 127) / 255) << sh[c];
        }
        alpha = o->mask[3];
    }
    for (y = 0; y < o->h; y++) {
        const uint8_t *s = bgr + (size_t)y * bgr_stride;
        uint8_t *d = dst + (size_t)(o->topdown ? y : o->h - 1 - y) * o->stride;
        if (o->bpp == 24) {
            if (!bgr32) memcpy(d, s, (size_t)o->w * 3);
            else for (x = 0; x < o->w; x++) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d += 3; s += 4; }
        } else if (o->bpp == 16) {
            uint16_t *p = (uint16_t *)d;
            for (x = 0; x < o->w; x++, s += sp)
                p[x] = (uint16_t)(lut[0][s[2]] | lut[1][s[1]] | lut[2][s[0]] | alpha);
            memset(d + o->w * 2, 0, o->stride - o->w * 2);
        } else {
            uint32_t *p = (uint32_t *)d;
            for (x = 0; x < o->w; x++, s += sp) p[x] = lut[0][s[2]] | lut[1][s[1]] | lut[2][s[0]] | alpha;
        }
    }
}
