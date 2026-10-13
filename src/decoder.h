#ifndef FFVFW_DECODER_H
#define FFVFW_DECODER_H
/* Thin per-instance wrapper around libavcodec/libswscale.  Everything FFmpeg
 * related lives behind this header so the VfW layer stays codec-agnostic:
 * adding H.265/VP9/AV1 means adding a CodecDesc entry (see codecs.c) and, if
 * the packet framing differs, a framing routine - not touching DriverProc. */
#include <stddef.h>
#include <stdint.h>

typedef struct FfDec FfDec;

typedef struct {
    const char *name;
    int         av_codec_id;      /* enum AVCodecID, kept as int to avoid leaking FFmpeg headers */
    uint32_t    fourcc[4];        /* biCompression values accepted (0 terminated) */
    int         (*has_keyframe)(const uint8_t *d, size_t n);
} CodecDesc;

const CodecDesc *codec_find_by_fourcc(uint32_t fourcc);
const CodecDesc *codec_by_index(int i);

void ffdec_global_init(void);
void ffdec_global_shutdown(void);
FfDec *ffdec_create(const CodecDesc *desc);
void   ffdec_destroy(FfDec *d);
void   ffdec_flush(FfDec *d);

/* Decode one complete access unit. Returns 0 and a ready frame, or <0 on error
 * (no output is produced for corrupt/truncated input). */
int ffdec_decode(FfDec *d, const uint8_t *data, size_t size);
int ffdec_frame_size(const FfDec *d, int *w, int *h);

/* Convert the last decoded frame to packed BGR24 (top-down, stride = w*3 rounded up to 4)
 * or BGR0 (32 bit). dst must hold stride*h bytes. */
int ffdec_to_bgr(FfDec *d, int bpp32, uint8_t *dst, int stride);
#endif
