#ifndef FFVFW_PIXOUT_H
#define FFVFW_PIXOUT_H
#include <windows.h>
#include <mmsystem.h>
#include <vfw.h>
#include <stdint.h>

/* Validated description of a destination DIB format. */
typedef struct {
    int w, h;            /* h > 0 always; orientation in topdown */
    int topdown;         /* biHeight < 0 */
    int bpp;             /* 16, 24 or 32 */
    uint32_t mask[4];    /* r,g,b,alpha (0 for 24-bit) */
    int stride;          /* DWORD aligned bytes per row */
    uint32_t size_hint;  /* biSizeImage as supplied (0 = unknown) */
} OutFmt;

/* Returns 0 on success, otherwise ICERR_BADFORMAT/ICERR_BADBITDEPTH. */
int outfmt_parse(const BITMAPINFOHEADER *bi, OutFmt *o);
size_t outfmt_bytes(const OutFmt *o);

/* bgr = top-down packed BGR24 (bgr32=0) or BGRA (bgr32=1) with given stride.
 * dst receives the full image; caller guarantees outfmt_bytes() writable bytes. */
void outfmt_write(const OutFmt *o, const uint8_t *bgr, int bgr_stride, int bgr32, uint8_t *dst);
#endif
