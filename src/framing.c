#include <windows.h>
#include <string.h>
#include "framing.h"
#include "log.h"

#define MAX_PACKET (64u * 1024 * 1024)
#define MARKER_NAL_HDR 0x1E /* forbidden_zero=0, nal_ref_idc=0, type=30 */

static int readable(const void *p, size_t n) { return n == 0 || !IsBadReadPtr(p, n); }

int framing_has_idr(const uint8_t *d, size_t n)
{
    size_t i;
    for (i = 0; i + 3 < n; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if ((d[i + 3] & 0x1f) == 5) return 1;
            i += 2;
        }
    return 0;
}

int framing_extract(const uint8_t *in, uint32_t hint, FramedPacket *out)
{
    memset(out, 0, sizeof *out);
    if (!in || !readable(in, 8)) return FRAME_ERR_UNREADABLE;

    /* Marker NAL: [00] 00 00 01 1E <escaped payload> then next start code. */
    {
        const uint8_t *p = in; size_t sc = 0;
        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) sc = 4;
        else if (p[0] == 0 && p[1] == 0 && p[2] == 1) sc = 3;
        if (sc && p[sc] == MARKER_NAL_HDR) {
            uint8_t pay[32]; size_t np = 0, i = sc + 1, zeros = 0;
            const uint8_t *body;
            if (!readable(in, 64)) return FRAME_ERR_UNREADABLE;
            while (i < 64) {                      /* unescape until next start code */
                uint8_t b = p[i];
                if (zeros >= 2 && b == 1) { i -= 2; np -= 2; break; }
                if (zeros >= 2 && b == 3) { zeros = 0; i++; continue; }  /* emulation prevention */
                if (np >= sizeof pay) return FRAME_ERR_BADMARKER;
                pay[np++] = b; zeros = b ? 0 : zeros + 1; i++;
            }
            if (i >= 64 || np < 14 || memcmp(pay, "VFWH", 4) != 0 || pay[4] != 1) return FRAME_ERR_BADMARKER;
            /* pay: "VFWH" ver flags seq(4) bodylen(4) pad */
            out->has_marker = 1;
            out->keyframe = pay[5] & 1;
            out->seq = pay[6] | pay[7] << 8 | pay[8] << 16 | (uint32_t)pay[9] << 24;
            out->size = pay[10] | pay[11] << 8 | pay[12] << 16 | (uint32_t)pay[13] << 24;
            if (out->size == 0 || out->size > MAX_PACKET) return FRAME_ERR_TOOBIG;
            body = p + i;
            if (!(body[0] == 0 && body[1] == 0 && (body[2] == 1 || (body[2] == 0 && body[3] == 1))))
                return FRAME_ERR_BADMARKER;
            if (!readable(body, out->size)) { ffvfw_log(LOG_ERR, "framing: %u-byte body not readable", (unsigned)out->size); return FRAME_ERR_UNREADABLE; }
            out->data = body;
            return FRAME_OK;
        }
    }
    /* Unmarked: trust the VfW convention biSizeImage == bytes of this frame. */
    if (hint == 0) return FRAME_ERR_NOLEN;
    if (hint > MAX_PACKET) return FRAME_ERR_TOOBIG;
    if (!readable(in, hint)) return FRAME_ERR_UNREADABLE;
    out->data = in; out->size = hint;
    return FRAME_OK;
}
