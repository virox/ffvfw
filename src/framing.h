#ifndef FFVFW_FRAMING_H
#define FFVFW_FRAMING_H
#include <stddef.h>
#include <stdint.h>
/* Packet framing for H.264 in VfW.
 *
 * VfW's ICM_DECOMPRESS carries NO input length: ICDECOMPRESS has only lpbiInput
 * (whose biSizeImage is the *stream-header constant* for Recoil) and lpInput.
 * Recoil also reuses one calloc'ed buffer for every frame, so bytes beyond the
 * current frame are stale data of earlier, longer frames.
 *
 * Therefore each AVI sample produced by tools/vfwmux.py begins with a private
 * "frame header" NAL (nal_unit_type 30, unspecified, ignored by every H.264
 * decoder) holding the exact length of the Annex-B access unit that follows,
 * a sequence number and a keyframe flag.  See docs/TECHNICAL_NOTES.md.
 */
typedef struct {
    const uint8_t *data;   /* Annex-B access unit (marker NAL stripped)      */
    size_t         size;
    int            has_marker;
    int            keyframe;   /* only meaningful if has_marker              */
    uint32_t       seq;        /* only meaningful if has_marker              */
} FramedPacket;

enum { FRAME_OK = 0, FRAME_ERR_NOLEN = -1, FRAME_ERR_BADMARKER = -2, FRAME_ERR_UNREADABLE = -3,
       FRAME_ERR_TOOBIG = -4 };

/* hint = lpbiInput->biSizeImage (0 if unknown). Returns FRAME_OK or an error. */
int framing_extract(const uint8_t *in, uint32_t hint, FramedPacket *out);

/* Scans an Annex-B buffer for IDR slices (nal type 5). */
int framing_has_idr(const uint8_t *d, size_t n);
#endif
