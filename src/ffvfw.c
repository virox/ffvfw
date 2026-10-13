/* FFmpeg-backed Video for Windows decompressor: DriverProc and ICM message handling. */
#include <windows.h>
#include <mmsystem.h>
#include <vfw.h>
#include <stdio.h>
#include <string.h>
#include "decoder.h"
#include "framing.h"
#include "pixout.h"
#include "log.h"

#define INST_MAGIC 0x56464631u /* 'VFF1' */
#define FFVFW_VERSION 0x00010000u

int ffvfw_cfg_string(const char *name, char *buf, unsigned cb);

enum { GAP_HOLD = 0, GAP_ERROR = 1 };

typedef struct Inst {
    DWORD magic, fccHandler;
    CRITICAL_SECTION cs;
    const CodecDesc *desc;
    FfDec *dec;
    int begun, w, h;
    int have_seq, need_key, policy, warned_unmarked;
    uint32_t last_seq;
    unsigned long frames, dropped, errors;
    double t_dec, t_conv, t_pack;   /* accumulated ms, for the end-of-stream log */
    uint8_t *scratch;
    size_t scratch_cap;
} Inst;

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID res)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        ffvfw_log_init();
        ffdec_global_init();
    } else if (reason == DLL_PROCESS_DETACH && !res) {
        ffdec_global_shutdown();
        ffvfw_log_shutdown();
    }
    return TRUE;
}

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

static Inst *inst_of(DWORD_PTR id)
{
    Inst *i = (Inst *)id;
    if (!i || i == (Inst *)(DWORD_PTR)0xFFFF0000u || IsBadReadPtr(i, sizeof *i) || i->magic != INST_MAGIC)
        return NULL;
    return i;
}

/* ---- format checks --------------------------------------------------- */

static const CodecDesc *check_input(const BITMAPINFOHEADER *bi)
{
    if (!bi || IsBadReadPtr(bi, sizeof *bi) || bi->biSize < sizeof *bi) return NULL;
    if (bi->biWidth <= 0 || bi->biHeight == 0 || bi->biWidth > 16384) return NULL;
    return codec_find_by_fourcc(bi->biCompression);
}

static int abs_h(const BITMAPINFOHEADER *bi) { return bi->biHeight < 0 ? -bi->biHeight : bi->biHeight; }

static DWORD query(const BITMAPINFOHEADER *in, const BITMAPINFOHEADER *out)
{
    OutFmt o;
    int r;
    if (!check_input(in)) return ICERR_BADFORMAT;
    if (!out) return ICERR_OK;
    if (IsBadReadPtr(out, sizeof *out + 16)) return ICERR_BADPARAM;
    r = outfmt_parse(out, &o);
    if (r) return (DWORD)r;
    if (o.w != in->biWidth || o.h != abs_h(in)) return ICERR_BADFORMAT;
    return ICERR_OK;
}

static void inst_end(Inst *p)
{
    if (p->dec) {
        ffvfw_log(LOG_INFO, "end: frames=%lu dropped=%lu errors=%lu  avg ms/frame: decode %.2f, colour-convert %.2f, pack %.2f", p->frames, p->dropped, p->errors,
                  p->frames ? p->t_dec / p->frames : 0.0, p->frames ? p->t_conv / p->frames : 0.0, p->frames ? p->t_pack / p->frames : 0.0);
        ffdec_destroy(p->dec);
    }
    p->dec = NULL;
    p->begun = 0;
    free(p->scratch);
    p->scratch = NULL;
    p->scratch_cap = 0;
}

static DWORD inst_begin(Inst *p, const BITMAPINFOHEADER *in, const BITMAPINFOHEADER *out)
{
    DWORD r = query(in, out);
    if (r) {
        ffvfw_log(LOG_ERR, "BEGIN rejected (%ld)", (long)(int)r);
        return r;
    }
    inst_end(p);
    p->desc = check_input(in);
    p->dec = ffdec_create(p->desc);
    if (!p->dec) return ICERR_MEMORY;
    p->begun = 1;
    p->w = in->biWidth;
    p->h = abs_h(in);
    p->have_seq = 0;
    p->need_key = 0;
    p->frames = p->dropped = p->errors = 0;
    p->t_dec = p->t_conv = p->t_pack = 0;
    ffvfw_log(LOG_INFO, "BEGIN %s %dx%d in.fcc=%08lx in.sizeImage=%lu out: bpp=%d comp=%lu h=%ld", p->desc->name,
              p->w, p->h, in->biCompression, in->biSizeImage, out ? out->biBitCount : 0,
              out ? out->biCompression : 0, out ? out->biHeight : 0);
    return ICERR_OK;
}

/* ---- the actual decode ----------------------------------------------- */

static DWORD decompress(Inst *p, DWORD flags, const BITMAPINFOHEADER *bin, const void *src,
                        const BITMAPINFOHEADER *bout, void *dst)
{
    OutFmt o;
    FramedPacket pk;
    int r, w, h, bgr32;
    size_t need, sstride;
    double t0, t1;
    DWORD ret;

    if (flags & ICDECOMPRESS_NULLFRAME) return ICERR_OK; /* "repeat previous frame" */
    if (!bin || !bout || !src || !dst) return ICERR_BADPARAM;
    if (!p->begun) { /* lenient: some callers skip BEGIN */
        ffvfw_log(LOG_INFO, "DECOMPRESS without BEGIN - beginning implicitly");
        ret = inst_begin(p, bin, bout);
        if (ret != ICERR_OK) return ret;
    }
    if (!check_input(bin)) return ICERR_BADFORMAT;
    r = outfmt_parse(bout, &o);
    if (r) return (DWORD)r;
    if (o.w != p->w || o.h != p->h) {
        ffvfw_log(LOG_ERR, "frame size %dx%d != begun %dx%d", o.w, o.h, p->w, p->h);
        return ICERR_BADFORMAT;
    }
    need = outfmt_bytes(&o);
    if (o.size_hint && o.size_hint < need) {
        ffvfw_log(LOG_ERR, "dest biSizeImage %lu < needed %lu", (unsigned long)o.size_hint, (unsigned long)need);
        return ICERR_BADSIZE;
    }
    if (IsBadWritePtr(dst, need)) {
        ffvfw_log(LOG_ERR, "dest buffer not writable for %lu bytes", (unsigned long)need);
        return ICERR_BADPARAM;
    }

    r = framing_extract((const uint8_t *)src, bin->biSizeImage, &pk);
    if (r) {
        p->errors++;
        ffvfw_log(LOG_ERR, "framing error %d (biSizeImage=%lu)", r, bin->biSizeImage);
        return ICERR_ERROR;
    }

    if (pk.has_marker) {
        int contiguous = p->have_seq && pk.seq == p->last_seq + 1;
        if (pk.keyframe) {
            if (p->have_seq && !contiguous) ffdec_flush(p->dec);
            p->need_key = 0;
        } else if (!contiguous || p->need_key) {
            p->need_key = 1;
            p->dropped++;
            ffvfw_log(LOG_ERR,
                      "frame-dependency gap: got seq %lu after %lu, not a keyframe -> %s (dropped=%lu)",
                      (unsigned long)pk.seq, p->have_seq ? (unsigned long)p->last_seq : 0ul,
                      p->policy == GAP_ERROR ? "ICERR_GOTOKEYFRAME" : "holding previous output until next keyframe",
                      p->dropped);
            return p->policy == GAP_ERROR ? ICERR_GOTOKEYFRAME : ICERR_OK;
        }
    } else if (!p->warned_unmarked) {
        p->warned_unmarked = 1;
        ffvfw_log(LOG_INFO, "unmarked packets: size taken from biSizeImage, no frame-gap detection");
    }

    t0 = now_ms();
    if (ffdec_decode(p->dec, pk.data, pk.size) < 0) {
        p->errors++;
        if (pk.has_marker && !pk.keyframe) p->need_key = 1;
        return ICERR_ERROR;
    }
    if (pk.has_marker) {
        p->last_seq = pk.seq;
        p->have_seq = 1;
    }
    p->frames++;
    p->t_dec += now_ms() - t0;
    if (flags & ICDECOMPRESS_PREROLL) return ICERR_OK; /* decoded for reference only */

    if (ffdec_frame_size(p->dec, &w, &h) || w != p->w || h != p->h) {
        ffvfw_log(LOG_ERR, "decoded size differs from header %dx%d", p->w, p->h);
        return ICERR_BADFORMAT;
    }
    bgr32 = o.bpp == 32 ? 1 : 0;
    sstride = (size_t)w * (bgr32 ? 4 : 3);
    if (p->scratch_cap < sstride * h) {
        free(p->scratch);
        p->scratch = (uint8_t *)malloc(sstride * h);
        p->scratch_cap = p->scratch ? sstride * h : 0;
        if (!p->scratch) return ICERR_MEMORY;
    }
    t0 = now_ms();
    if (ffdec_to_bgr(p->dec, bgr32, p->scratch, (int)sstride)) return ICERR_ERROR;
    t1 = now_ms();
    outfmt_write(&o, p->scratch, (int)sstride, bgr32, (uint8_t *)dst);
    p->t_conv += t1 - t0;
    p->t_pack += now_ms() - t1;
    ffvfw_log(LOG_TRACE, "frame ok seq=%lu key=%d bytes=%lu", pk.has_marker ? (unsigned long)pk.seq : 0ul,
              pk.keyframe, (unsigned long)pk.size);
    return ICERR_OK;
}

static DWORD ex_check(const ICDECOMPRESSEX *e, int w, int h)
{
    int dh = e->dyDst < 0 ? -e->dyDst : e->dyDst, sh = e->dySrc < 0 ? -e->dySrc : e->dySrc;
    if (e->xDst || e->yDst || e->xSrc || e->ySrc) return ICERR_BADPARAM;
    if ((e->dxDst && e->dxDst != w) || (e->dxSrc && e->dxSrc != w)) return ICERR_UNSUPPORTED; /* no scaling */
    if ((dh && dh != h) || (sh && sh != h)) return ICERR_UNSUPPORTED;
    return ICERR_OK;
}

/* ---- DriverProc ------------------------------------------------------- */

static LRESULT get_info(Inst *p, ICINFO *ic, DWORD cb)
{
    ICINFO t;
    if (!ic || cb < 16 || IsBadWritePtr(ic, cb)) return 0;
    memset(&t, 0, sizeof t);
    t.dwSize = sizeof t;
    t.fccType = ICTYPE_VIDEO;
    t.fccHandler = p ? p->fccHandler : mmioFOURCC('H', '2', '6', '4');
    t.dwVersion = FFVFW_VERSION;
    t.dwVersionICM = ICVERSION;
    wcscpy(t.szName, L"FFmpeg H.264");
    wcscpy(t.szDescription, L"FFmpeg-backed H.264 decoder (VfW)");
    wcscpy(t.szDriver, L"ffvfw.dll");
    if (cb > sizeof t) cb = sizeof t;
    memcpy(ic, &t, cb);
    return cb;
}

LRESULT CALLBACK DriverProc(DWORD_PTR id, HDRVR hdrvr, UINT msg, LPARAM l1, LPARAM l2)
{
    Inst *p = inst_of(id);
    LRESULT r;

    switch (msg) {
    case DRV_LOAD:
        ffvfw_log(LOG_INFO, "DRV_LOAD");
        return 1;
    case DRV_FREE:
        ffvfw_log(LOG_INFO, "DRV_FREE");
        return 1;
    case DRV_ENABLE:
    case DRV_DISABLE:
        return 1;
    case DRV_INSTALL:
    case DRV_REMOVE:
        return DRVCNF_OK;
    case DRV_QUERYCONFIGURE:
        return 0;
    case DRV_CONFIGURE:
        return DRVCNF_CANCEL;

    case DRV_OPEN: {
        ICOPEN *io = (ICOPEN *)l2;
        char pol[16];
        if (io && IsBadReadPtr(io, sizeof *io)) io = NULL;
        if (io) {
            if (io->fccType != ICTYPE_VIDEO) {
                io->dwError = ICERR_UNSUPPORTED;
                return 0;
            }
            if (io->dwFlags != 0 && io->dwFlags != ICMODE_DECOMPRESS && io->dwFlags != ICMODE_FASTDECOMPRESS &&
                io->dwFlags != ICMODE_QUERY && io->dwFlags != ICMODE_DRAW) {
                ffvfw_log(LOG_INFO, "DRV_OPEN refused: mode %lu not supported (decoder only)", io->dwFlags);
                io->dwError = ICERR_UNSUPPORTED;
                return 0;
            }
        }
        p = (Inst *)calloc(1, sizeof *p);
        if (!p) {
            if (io) io->dwError = ICERR_MEMORY;
            return 0;
        }
        p->magic = INST_MAGIC;
        p->fccHandler = io ? io->fccHandler : 0;
        InitializeCriticalSection(&p->cs);
        if (ffvfw_cfg_string("FFVFW_GAP_POLICY", pol, sizeof pol) && !_stricmp(pol, "error")) p->policy = GAP_ERROR;
        if (io) io->dwError = ICERR_OK;
        ffvfw_log(LOG_INFO, "DRV_OPEN inst=%p handler=%08lx mode=%lu", (void *)p, p->fccHandler, io ? io->dwFlags : 0);
        return (LRESULT)p;
    }
    case DRV_CLOSE:
        if (p) {
            ffvfw_log(LOG_INFO, "DRV_CLOSE inst=%p", (void *)p);
            EnterCriticalSection(&p->cs);
            inst_end(p);
            LeaveCriticalSection(&p->cs);
            DeleteCriticalSection(&p->cs);
            p->magic = 0;
            free(p);
        }
        return 1;

    case ICM_GETINFO:
        return get_info(p, (ICINFO *)l1, (DWORD)l2);
    case ICM_ABOUT:
    case ICM_CONFIGURE:
    case ICM_GETSTATE:
    case ICM_SETSTATE:
        return ICERR_UNSUPPORTED;

    case ICM_DECOMPRESS_QUERY:
    case ICM_DECOMPRESSEX_QUERY: {
        const BITMAPINFOHEADER *in = (const BITMAPINFOHEADER *)l1, *out = (const BITMAPINFOHEADER *)l2;
        if (msg == ICM_DECOMPRESSEX_QUERY) {
            const ICDECOMPRESSEX *e = (const ICDECOMPRESSEX *)l1;
            if (!e || IsBadReadPtr(e, sizeof *e)) return ICERR_BADPARAM;
            in = e->lpbiSrc;
            out = e->lpbiDst;
        }
        r = (LRESULT)query(in, out);
        ffvfw_log(LOG_INFO, "DECOMPRESS_QUERY in.fcc=%08lx out.bpp=%d -> %ld",
                  in && !IsBadReadPtr(in, sizeof *in) ? in->biCompression : 0,
                  out && !IsBadReadPtr(out, sizeof *out) ? out->biBitCount : -1, (long)r);
        return r;
    }
    case ICM_DECOMPRESS_GET_FORMAT: {
        const BITMAPINFOHEADER *in = (const BITMAPINFOHEADER *)l1;
        BITMAPINFOHEADER *out = (BITMAPINFOHEADER *)l2;
        if (!check_input(in)) return ICERR_BADFORMAT;
        if (!out) return sizeof(BITMAPINFOHEADER);
        if (IsBadWritePtr(out, sizeof *out)) return ICERR_BADPARAM;
        memset(out, 0, sizeof *out);
        out->biSize = sizeof *out;
        out->biWidth = in->biWidth;
        out->biHeight = in->biHeight;
        out->biPlanes = 1;
        out->biBitCount = 24;
        out->biCompression = BI_RGB;
        out->biSizeImage = ((in->biWidth * 24 + 31) / 32) * 4 * abs_h(in);
        return ICERR_OK;
    }
    }

    if (!p) return msg >= DRV_USER ? ICERR_UNSUPPORTED : DefDriverProc(id, hdrvr, msg, l1, l2);

    switch (msg) {
    case ICM_DECOMPRESS_BEGIN:
        EnterCriticalSection(&p->cs);
        r = (LRESULT)inst_begin(p, (const BITMAPINFOHEADER *)l1, (const BITMAPINFOHEADER *)l2);
        LeaveCriticalSection(&p->cs);
        return r;
    case ICM_DECOMPRESSEX_BEGIN: {
        const ICDECOMPRESSEX *e = (const ICDECOMPRESSEX *)l1;
        if (!e || IsBadReadPtr(e, sizeof *e) || !e->lpbiSrc) return ICERR_BADPARAM;
        r = (LRESULT)ex_check(e, e->lpbiSrc->biWidth, abs_h(e->lpbiSrc));
        if (r) return r;
        EnterCriticalSection(&p->cs);
        r = (LRESULT)inst_begin(p, e->lpbiSrc, e->lpbiDst);
        LeaveCriticalSection(&p->cs);
        return r;
    }
    case ICM_DECOMPRESS: {
        const ICDECOMPRESS *d = (const ICDECOMPRESS *)l1;
        if (!d || IsBadReadPtr(d, sizeof *d)) return ICERR_BADPARAM;
        EnterCriticalSection(&p->cs);
        r = (LRESULT)decompress(p, d->dwFlags, d->lpbiInput, d->lpInput, d->lpbiOutput, d->lpOutput);
        LeaveCriticalSection(&p->cs);
        return r;
    }
    case ICM_DECOMPRESSEX: {
        const ICDECOMPRESSEX *e = (const ICDECOMPRESSEX *)l1;
        if (!e || IsBadReadPtr(e, sizeof *e) || !e->lpbiSrc) return ICERR_BADPARAM;
        r = (LRESULT)ex_check(e, e->lpbiSrc->biWidth, abs_h(e->lpbiSrc));
        if (r) return r;
        EnterCriticalSection(&p->cs);
        r = (LRESULT)decompress(p, e->dwFlags, e->lpbiSrc, e->lpSrc, e->lpbiDst, e->lpDst);
        LeaveCriticalSection(&p->cs);
        return r;
    }
    case ICM_DECOMPRESS_END:
    case ICM_DECOMPRESSEX_END:
        EnterCriticalSection(&p->cs);
        inst_end(p);
        LeaveCriticalSection(&p->cs);
        ffvfw_log(LOG_INFO, "DECOMPRESS_END");
        return ICERR_OK;
    case ICM_DECOMPRESS_GET_PALETTE:
    case ICM_DECOMPRESS_SET_PALETTE:
        return ICERR_UNSUPPORTED;
    }
    return msg >= DRV_USER ? ICERR_UNSUPPORTED : DefDriverProc(id, hdrvr, msg, l1, l2);
}
