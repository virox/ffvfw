#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
#include "decoder.h"
#include "framing.h"
#include "log.h"

#define FOURCC(a,b,c,d) ((uint32_t)(a)|(uint32_t)(b)<<8|(uint32_t)(c)<<16|(uint32_t)(d)<<24)

static const CodecDesc g_codecs[] = {
    { "H.264", AV_CODEC_ID_H264,
      { FOURCC('H','2','6','4'), FOURCC('h','2','6','4'), FOURCC('A','V','C','1'), FOURCC('a','v','c','1') },
      framing_has_idr },
};

const CodecDesc *codec_by_index(int i) { return i >= 0 && i < (int)(sizeof g_codecs / sizeof *g_codecs) ? &g_codecs[i] : NULL; }

const CodecDesc *codec_find_by_fourcc(uint32_t fcc)
{
    size_t i; int j;
    for (i = 0; i < sizeof g_codecs / sizeof *g_codecs; i++)
        for (j = 0; j < 4 && g_codecs[i].fourcc[j]; j++)
            if (g_codecs[i].fourcc[j] == fcc) return &g_codecs[i];
    return NULL;
}

/* This FFmpeg is built without threading support (so the DLL pulls in no pthread runtime). Such a build
 * is not safe for concurrent use, hence every libav* call is serialised by one process-wide lock. */
/* Colour-conversion quality.  "exact" (accurate rounding + full chroma interpolation) is bit-exact with FFmpeg's
 * own bicubic RGB output and is what the test-suite checks; it costs ~35-45 ms for a 1080p frame, which does
 * not fit a 30 fps budget on one thread.  "fast" drops those two flags.  Default "auto": exact up to 640x480 pixels
 * (all Recoil-sized content), fast above.  Override: FFVFW_QUALITY=exact|fast|auto (env or HKCU\Software\FFmpegVfW). */
int ffvfw_cfg_string(const char *name, char *buf, unsigned cb);
static int sws_flags(int w, int h)
{
    char q[16]; int exact;
    if (ffvfw_cfg_string("FFVFW_QUALITY", q, sizeof q) && !_stricmp(q, "exact")) exact = 1;
    else if (q[0] && !_stricmp(q, "fast")) exact = 0;
    else exact = (long)w * h <= 640L * 480L;
    return exact ? SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT : SWS_BICUBIC;
}

static CRITICAL_SECTION g_ff_lock;
static volatile LONG g_ff_ready;
void ffdec_global_init(void) { InitializeCriticalSection(&g_ff_lock); g_ff_ready = 1; }
void ffdec_global_shutdown(void) { g_ff_ready = 0; DeleteCriticalSection(&g_ff_lock); }
#define FF_LOCK()   EnterCriticalSection(&g_ff_lock)
#define FF_UNLOCK() LeaveCriticalSection(&g_ff_lock)

struct FfDec {
    const CodecDesc *desc;
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *pkt;
    uint8_t *pbuf; size_t pcap;
    struct SwsContext *sws;
    int sw, sh, sfmt, scs, srange, dfmt;
    int have_frame;
};

static void av_log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    char line[512];
    if (level > AV_LOG_WARNING || ffvfw_log_level() < 2) return;
    vsnprintf(line, sizeof line, fmt, vl);
    { size_t n = strlen(line); while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0; }
    ffvfw_log(LOG_INFO, "libav(%d): %s", level, line);
}

static void ffdec_destroy_locked(FfDec *d);
static FfDec *ffdec_create_locked(const CodecDesc *desc)
{
    const AVCodec *c; FfDec *d;
    av_log_set_callback(av_log_cb);
    c = avcodec_find_decoder((enum AVCodecID)desc->av_codec_id);
    if (!c) { ffvfw_log(LOG_ERR, "decoder %s not compiled into FFmpeg", desc->name); return NULL; }
    d = (FfDec *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->desc = desc;
    d->ctx = avcodec_alloc_context3(c);
    d->frame = av_frame_alloc();
    d->pkt = av_packet_alloc();
    if (!d->ctx || !d->frame || !d->pkt) goto fail;
    d->ctx->thread_count = 1;                     /* deterministic: no frame/slice threads  */
    d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;     /* 1 packet in -> 1 frame out (no B-frames) */
    d->ctx->err_recognition = AV_EF_EXPLODE | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_CRCCHECK;
    if (avcodec_open2(d->ctx, c, NULL) < 0) goto fail;
    return d;
fail:
    ffdec_destroy_locked(d);
    return NULL;
}

static void ffdec_destroy_locked(FfDec *d)
{
    if (!d) return;
    if (d->sws) sws_freeContext(d->sws);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    avcodec_free_context(&d->ctx);
    free(d->pbuf);
    free(d);
}

static void ffdec_flush_locked(FfDec *d)
{
    if (d && d->ctx) avcodec_flush_buffers(d->ctx);
    if (d) { av_frame_unref(d->frame); d->have_frame = 0; }
}

static int ffdec_decode_locked(FfDec *d, const uint8_t *data, size_t size)
{
    int r;
    d->have_frame = 0;
    if (size + AV_INPUT_BUFFER_PADDING_SIZE > d->pcap) {
        uint8_t *n = (uint8_t *)realloc(d->pbuf, size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!n) return AVERROR(ENOMEM);
        d->pbuf = n; d->pcap = size + AV_INPUT_BUFFER_PADDING_SIZE;
    }
    memcpy(d->pbuf, data, size);
    memset(d->pbuf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    av_packet_unref(d->pkt);
    d->pkt->data = d->pbuf; d->pkt->size = (int)size;
    r = avcodec_send_packet(d->ctx, d->pkt);
    d->pkt->data = NULL; d->pkt->size = 0;
    if (r < 0) { ffvfw_log(LOG_ERR, "send_packet failed: %d", r); return r; }
    av_frame_unref(d->frame);
    r = avcodec_receive_frame(d->ctx, d->frame);
    if (r < 0) { ffvfw_log(LOG_ERR, "receive_frame failed: %d (no picture in access unit)", r); return r; }
    if ((d->frame->flags & AV_FRAME_FLAG_CORRUPT) || d->frame->decode_error_flags) {
        ffvfw_log(LOG_ERR, "frame flagged corrupt (flags=%x err=%x), discarded", d->frame->flags, d->frame->decode_error_flags);
        av_frame_unref(d->frame);
        return AVERROR_INVALIDDATA;
    }
    d->have_frame = 1;
    return 0;
}

int ffdec_frame_size(const FfDec *d, int *w, int *h)
{
    if (!d->have_frame) return -1;
    *w = d->frame->width; *h = d->frame->height; return 0;
}

static int ffdec_to_bgr_locked(FfDec *d, int bpp32, uint8_t *dst, int stride)
{
    AVFrame *f = d->frame;
    enum AVPixelFormat dfmt = bpp32 ? AV_PIX_FMT_BGRA : AV_PIX_FMT_BGR24;
    uint8_t *dp[4] = { dst, 0, 0, 0 }; int dl[4] = { stride, 0, 0, 0 };
    int cs, range;
    if (!d->have_frame) return -1;
    if (!d->sws || d->sw != f->width || d->sh != f->height || d->sfmt != f->format || d->dfmt != (int)dfmt) {
        if (d->sws) sws_freeContext(d->sws);
        d->sws = sws_getContext(f->width, f->height, (enum AVPixelFormat)f->format,
                                f->width, f->height, dfmt, sws_flags(f->width, f->height), NULL, NULL, NULL);
        if (!d->sws) return -1;
        d->sw = f->width; d->sh = f->height; d->sfmt = f->format; d->dfmt = dfmt; d->scs = -1;
    }
    /* honour the stream's colour description; untagged SD => BT.601 (what FFmpeg itself assumes) */
    cs = f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 :
         f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M ? SWS_CS_ITU601 :
         f->colorspace == AVCOL_SPC_SMPTE240M ? SWS_CS_SMPTE240M :
         f->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 :
         (f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601);
    range = f->color_range == AVCOL_RANGE_JPEG;
    if (d->scs != cs || d->srange != range) {
        const int *inv, *tab; int srcR, dstR, b, c, s;
        sws_getColorspaceDetails(d->sws, (int **)&inv, &srcR, (int **)&tab, &dstR, &b, &c, &s);
        sws_setColorspaceDetails(d->sws, sws_getCoefficients(cs), range, sws_getCoefficients(SWS_CS_DEFAULT), 1, b, c, s);
        d->scs = cs; d->srange = range;
    }
    return sws_scale(d->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dp, dl) == f->height ? 0 : -1;
}

FfDec *ffdec_create(const CodecDesc *d) { FfDec *r; FF_LOCK(); r = ffdec_create_locked(d); FF_UNLOCK(); return r; }
void ffdec_destroy(FfDec *d) { FF_LOCK(); ffdec_destroy_locked(d); FF_UNLOCK(); }
void ffdec_flush(FfDec *d) { FF_LOCK(); ffdec_flush_locked(d); FF_UNLOCK(); }
int ffdec_decode(FfDec *d, const uint8_t *data, size_t size) { int r; FF_LOCK(); r = ffdec_decode_locked(d, data, size); FF_UNLOCK(); return r; }
int ffdec_to_bgr(FfDec *d, int b32, uint8_t *dst, int stride) { int r; FF_LOCK(); r = ffdec_to_bgr_locked(d, b32, dst, stride); FF_UNLOCK(); return r; }

/* FFmpeg 7.1 quirk: h2645_sei.o (needed by the H.264 decoder) references this function, but its
 * implementation (aom_film_grain.o) is only linked when the HEVC decoder is enabled. The state it
 * frees is only ever allocated while parsing HEVC film-grain SEI, so for an H.264-only build there
 * is nothing to release. */
void ff_aom_uninit_film_grain_params(void *s) { (void)s; }
