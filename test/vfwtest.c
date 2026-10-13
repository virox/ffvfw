/* Standalone 32-bit VfW test application for ffvfw.dll.
 *
 * Exercises the *Windows* VfW stack (avifil32 + msvfw32), never our decoder directly, and reproduces
 * Recoil's zFMV_Stream call sequence (RecoilRebuild fmv_stream.cpp 0x463EF0 / 0x4643A0):
 *   AVIStreamOpenFromFileA -> AVIStreamReadFormat -> V4 dest header from srcFormat -> ICLocate
 *   -> calloc(dwSuggestedBufferSize) -> ICM_DECOMPRESS_BEGIN (positive height) -> height = -height
 *   -> loop { AVIStreamRead(NULL byte counts) ; ICDecompress(flags 0) }.
 *
 *   vfwtest selftest <testdata-dir> [--dll path\ffvfw.dll]
 *   vfwtest dump <avi> <bpp:16|24|32> <out.raw> [--bottomup] [--fmt 565|555|rgb555|rgb32]
 *   vfwtest bmp  <avi> <frame> <out.bmp>
 *   vfwtest bench <avi> <16|24|32>   decode-speed benchmark through VfW (no output files)
 *   vfwtest probe <FOURCC>           is a VfW decompressor registered for this FourCC? (exit 0 = ours)
 * Without --dll the codec is located through the registry (i.e. it must be installed).
 */
#include <windows.h>
#include <mmsystem.h>
#include <vfw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static int g_fail, g_pass;
static FILE *g_log;

static void say(const char *fmt, ...)
{
    char b[1024]; va_list ap;
    va_start(ap, fmt); _vsnprintf(b, sizeof b - 2, fmt, ap); va_end(ap);
    b[sizeof b - 2] = 0; strcat(b, "\n");
    fputs(b, stdout); OutputDebugStringA(b);
    if (g_log) { fputs(b, g_log); fflush(g_log); }
}
#define CHECK(c, ...) do { if (c) { g_pass++; } else { g_fail++; say("  FAIL %s:%d: %s", __FILE__, __LINE__, #c); say("       " __VA_ARGS__); } } while (0)
#define INFO(...) say("  info: " __VA_ARGS__)

/* ---- Recoil-style player ------------------------------------------------ */
typedef enum { F_RGB24, F_RGB32, F_565, F_555, F_555RGB, F_RGB32BF } Fmt;
typedef struct {
    PAVISTREAM vs; AVISTREAMINFOA info; LONG nframes;
    BITMAPINFOHEADER *src; BITMAPV4HEADER *dst; LONG fmtBytes;
    HIC hic; void *cbuf; LONG cbytes; void *pixels; size_t pixbytes;
    int w, h, bpp, bottomup; Fmt fmt; int begun;
} Rc;

static const char *g_dll;
static int g_installed_fn;

static void ensure_driver(void)
{
    if (g_dll && !g_installed_fn) {
        HMODULE h = LoadLibraryA(g_dll);
        FARPROC dp = h ? GetProcAddress(h, "DriverProc") : NULL;
        if (!dp) { say("cannot load %s (err %lu)", g_dll, GetLastError()); exit(2); }
        if (!ICInstall(ICTYPE_VIDEO, mmioFOURCC('H','2','6','4'), (LPARAM)dp, NULL, ICINSTALL_FUNCTION)) { say("ICInstall(FUNCTION) failed"); exit(2); }
        g_installed_fn = 1;
    }
}

static void fill_dst(BITMAPV4HEADER *d, Fmt f)
{
    d->bV4BitCount = f == F_565 || f == F_555 || f == F_555RGB ? 16 : f == F_RGB24 ? 24 : 32;
    d->bV4V4Compression = (f == F_RGB24 || f == F_555RGB || f == F_RGB32) ? BI_RGB : BI_BITFIELDS;
    d->bV4ClrUsed = 0;
    d->bV4RedMask = f == F_565 ? 0xF800 : f == F_555 || f == F_555RGB ? 0x7C00 : 0xFF0000;
    d->bV4GreenMask = f == F_565 ? 0x07E0 : f == F_555 || f == F_555RGB ? 0x03E0 : 0xFF00;
    d->bV4BlueMask = f == F_565 || f == F_555 || f == F_555RGB ? 0x001F : 0xFF;
    d->bV4AlphaMask = 0;
    d->bV4SizeImage = d->bV4Height * ((d->bV4Width + 3) & ~3) * (d->bV4BitCount >> 3);   /* verbatim from Recoil */
}

/* Returns 0 on success. Mirrors zFMV_Stream::Constructor. */
static void rc_close(Rc *r);
static int rc_open(Rc *r, const char *path, Fmt fmt, int bottomup)
{
    LONG bytes = 0; HRESULT hr; DWORD sz;
    memset(r, 0, sizeof *r);
    ensure_driver();
    AVIFileInit();
    hr = AVIStreamOpenFromFileA(&r->vs, path, streamtypeVIDEO, 0, 0x10, 0);
    if (hr) { say("AVIStreamOpenFromFile(%s) = %08lx", path, hr); AVIFileExit(); free(r->src); memset(r, 0, sizeof *r); return -1; }
    if (AVIStreamReadFormat(r->vs, 0, 0, &bytes)) return -2;
    r->fmtBytes = bytes;
    r->src = (BITMAPINFOHEADER *)calloc(1, bytes);
    r->dst = (BITMAPV4HEADER *)calloc(1, bytes > (LONG)sizeof(BITMAPV4HEADER) ? bytes : sizeof(BITMAPV4HEADER));
    if (AVIStreamReadFormat(r->vs, 0, r->src, &bytes)) return -3;
    r->nframes = AVIStreamLength(r->vs);
    if (AVIStreamInfoA(r->vs, &r->info, sizeof r->info)) return -4;
    memcpy(r->dst, r->src, bytes);
    sz = bytes > (LONG)sizeof(BITMAPV4HEADER) ? bytes : sizeof(BITMAPV4HEADER);
    r->dst->bV4Size = sz;
    fill_dst(r->dst, fmt);
    r->fmt = fmt; r->bpp = r->dst->bV4BitCount; r->w = r->dst->bV4Width; r->h = r->dst->bV4Height; r->bottomup = bottomup;
    r->cbytes = (r->src->biBitCount >> 3) * r->src->biWidth * r->src->biHeight;
    if (r->info.dwSuggestedBufferSize) r->cbytes = r->info.dwSuggestedBufferSize;
    r->hic = ICLocate(ICTYPE_VIDEO, r->info.fccHandler, r->src, (LPBITMAPINFOHEADER)r->dst, ICMODE_DECOMPRESS);
    if (!r->hic) { say("ICLocate found no decompressor for fcc %.4s", (char *)&r->info.fccHandler); rc_close(r); return -5; }
    r->cbuf = calloc(r->cbytes, 1);
    ICSendMessage(r->hic, ICM_DECOMPRESS_BEGIN, (DWORD_PTR)r->src, (DWORD_PTR)r->dst);
    r->pixbytes = r->dst->bV4SizeImage;
    r->pixels = calloc(r->pixbytes, 1);
    if (!bottomup) r->dst->bV4Height = -r->dst->bV4Height;       /* Recoil negates AFTER BEGIN */
    r->begun = 1;
    return 0;
}

static LRESULT rc_frame(Rc *r, LONG idx)
{
    if (AVIStreamRead(r->vs, idx, 1, r->cbuf, r->cbytes, 0, 0)) return -9999;
    return ICDecompress(r->hic, 0, r->src, r->cbuf, (LPBITMAPINFOHEADER)r->dst, r->pixels);
}

static void rc_close(Rc *r)
{
    if (r->hic) { ICSendMessage(r->hic, ICM_DECOMPRESS_END, 0, 0); ICClose(r->hic); }
    if (r->vs) AVIStreamRelease(r->vs);
    AVIFileExit();
    free(r->src); free(r->dst); free(r->cbuf); free(r->pixels);
    memset(r, 0, sizeof *r);
}

/* ---- reference data & independent packer --------------------------------- */
static unsigned char *load_file(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb"); unsigned char *b; long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc(len); if (fread(b, 1, len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f); if (n) *n = len; return b;
}

static unsigned scale_c(unsigned v, int bits) { return (v * ((1u << bits) - 1) + 127) / 255; }

/* expected DIB bytes for one frame, written independently of src/pixout.c */
static void expect_frame(const Rc *r, const unsigned char *bgr, unsigned char *out)
{
    int w = r->w, h = r->h, bpp = r->bpp, stride = ((w * bpp + 31) / 32) * 4, x, y;
    memset(out, 0, (size_t)stride * h);
    for (y = 0; y < h; y++) {
        const unsigned char *s = bgr + (size_t)y * w * 3;
        unsigned char *d = out + (size_t)(r->bottomup ? h - 1 - y : y) * stride;
        for (x = 0; x < w; x++, s += 3) {
            unsigned B = s[0], G = s[1], R = s[2];
            if (bpp == 24) { d[3*x] = B; d[3*x+1] = G; d[3*x+2] = R; }
            else if (bpp == 32) ((unsigned *)d)[x] = R << 16 | G << 8 | B;
            else if (r->fmt == F_565) ((unsigned short *)d)[x] = (unsigned short)(scale_c(R,5) << 11 | scale_c(G,6) << 5 | scale_c(B,5));
            else ((unsigned short *)d)[x] = (unsigned short)(scale_c(R,5) << 10 | scale_c(G,5) << 5 | scale_c(B,5));
        }
    }
}

static int diff_stats(const unsigned char *a, const unsigned char *b, size_t n, int *maxd)
{
    size_t i; int bad = 0; *maxd = 0;
    for (i = 0; i < n; i++) { int d = abs((int)a[i] - (int)b[i]); if (d) bad++; if (d > *maxd) *maxd = d; }
    return bad;
}

/* ---- tests ---------------------------------------------------------------- */
static char g_dir[MAX_PATH];
static const char *P(const char *name) { static char b[4][MAX_PATH]; static int k; k = (k + 1) & 3; _snprintf(b[k], MAX_PATH, "%s\\%s", g_dir, name); return b[k]; }

static void t_discovery(void)
{
    Rc r; ICINFO ic; char nm[64];
    say("[1] ICLocate discovery (%s)", g_dll ? "process-local ICINSTALL_FUNCTION" : "registry-installed driver");
    CHECK(rc_open(&r, P("intra_640x480_15.avi"), F_565, 0) == 0, "open failed");
    if (r.hic) {
        memset(&ic, 0, sizeof ic);
        ICGetInfo(r.hic, &ic, sizeof ic);
        WideCharToMultiByte(CP_ACP, 0, ic.szName, -1, nm, sizeof nm, 0, 0);
        INFO("stream fccHandler=%.4s biCompression=%.4s  driver='%s' ver=%08lx", (char *)&r.info.fccHandler, (char *)&r.src->biCompression, nm, ic.dwVersion);
        CHECK(strstr(nm, "FFmpeg") != NULL, "ICLocate returned a different decoder: %s", nm);
        INFO("dwSuggestedBufferSize=%lu src.biSizeImage=%lu frames=%ld", r.info.dwSuggestedBufferSize, r.src->biSizeImage, r.nframes);
        rc_close(&r);
    }
}

static void t_format(void)
{
    BITMAPINFOHEADER in, out; BITMAPV4HEADER v4; HIC h; LRESULT lr; Rc r;
    say("[2] format negotiation");
    memset(&in, 0, sizeof in); in.biSize = 40; in.biWidth = 320; in.biHeight = 240; in.biPlanes = 1; in.biBitCount = 24;
    in.biCompression = mmioFOURCC('H','2','6','4'); in.biSizeImage = 50000;
    ensure_driver();
    h = ICLocate(ICTYPE_VIDEO, in.biCompression, &in, NULL, ICMODE_DECOMPRESS);
    CHECK(h != NULL, "ICLocate with no output format failed");
    if (!h) return;
    CHECK((lr = ICDecompressQuery(h, &in, NULL)) == ICERR_OK, "query(in,NULL)=%ld", (long)lr);
    out = in; out.biCompression = BI_RGB; out.biBitCount = 24;
    CHECK(ICDecompressQuery(h, &in, &out) == ICERR_OK, "24-bit RGB must be accepted");
    out.biBitCount = 8;  CHECK(ICDecompressQuery(h, &in, &out) != ICERR_OK, "8-bit must be refused");
    out.biBitCount = 24; out.biWidth = 100; CHECK(ICDecompressQuery(h, &in, &out) != ICERR_OK, "size mismatch must be refused");
    out.biWidth = 320; out.biCompression = mmioFOURCC('Y','U','Y','2'); CHECK(ICDecompressQuery(h, &in, &out) != ICERR_OK, "YUY2 is not offered");
    in.biCompression = mmioFOURCC('X','V','I','D'); CHECK(ICDecompressQuery(h, &in, NULL) != ICERR_OK, "foreign fourcc must be refused");
    in.biCompression = mmioFOURCC('H','2','6','4');
    CHECK((lr = ICDecompressGetFormatSize(h, &in)) >= (LRESULT)sizeof(BITMAPINFOHEADER), "GetFormatSize=%ld", (long)lr);
    memset(&out, 0, sizeof out);
    CHECK(ICDecompressGetFormat(h, &in, &out) == ICERR_OK && out.biBitCount == 24 && out.biCompression == BI_RGB && out.biWidth == 320, "GetFormat bpp=%d", out.biBitCount);
    /* V4 / BITFIELDS 16-bit with arbitrary (non-565) masks: accepted when contiguous, refused when not */
    memset(&v4, 0, sizeof v4); v4.bV4Size = sizeof v4; v4.bV4Width = 320; v4.bV4Height = -240; v4.bV4Planes = 1; v4.bV4BitCount = 16;
    v4.bV4V4Compression = BI_BITFIELDS; v4.bV4RedMask = 0x0F00; v4.bV4GreenMask = 0x00F0; v4.bV4BlueMask = 0x000F; v4.bV4AlphaMask = 0xF000;
    CHECK(ICDecompressQuery(h, &in, (LPBITMAPINFOHEADER)&v4) == ICERR_OK, "RGB444+A accepted");
    v4.bV4RedMask = 0xF81F; CHECK(ICDecompressQuery(h, &in, (LPBITMAPINFOHEADER)&v4) != ICERR_OK, "non-contiguous mask must be refused");
    ICClose(h);
    (void)r;
}

static unsigned char *load_ref(const char *name, size_t *n) { return load_file(P(name), n); }

/* decode every frame of `name` in `fmt`, compare to reference BGR24 (exact), skipping via `step` */
static void run_compare(const char *avi, const char *refname, Fmt fmt, int bottomup, int step, const char *label)
{
    Rc r; size_t refn = 0; unsigned char *ref = load_ref(refname, &refn), *exp; int i, bad = 0, worst = 0, frames = 0, maxd;
    if (!ref) { INFO("missing reference %s", refname); g_fail++; return; }
    if (rc_open(&r, P(avi), fmt, bottomup)) { g_fail++; free(ref); return; }
    exp = (unsigned char *)malloc(r.pixbytes + 64);
    for (i = 0; i < r.nframes; i += step) {
        LRESULT lr = rc_frame(&r, i);
        size_t fsz = (size_t)r.w * r.h * 3;
        int bytes = ((r.w * r.bpp + 31) / 32) * 4 * r.h;
        if (lr != 0) { bad++; say("  frame %d: ICDecompress=%ld", i, (long)lr); continue; }
        if ((size_t)(i + 1) * fsz > refn) break;
        expect_frame(&r, ref + (size_t)i * fsz, exp);
        if (diff_stats(exp, (unsigned char *)r.pixels, bytes, &maxd)) { bad++; if (maxd > worst) worst = maxd; }
        frames++;
    }
    CHECK(bad == 0 && frames > 0, "%s: %d/%d frames differ from FFmpeg reference (max byte delta %d)", label, bad, frames, worst);
    INFO("%s: %d frames compared, %d mismatching", label, frames, bad);
    free(exp); free(ref); rc_close(&r);
}

static void t_pixel_formats(void)
{
    static const struct { Fmt f; const char *n; } F[] = { { F_RGB24, "24 BI_RGB" }, { F_565, "16 BITFIELDS 565" }, { F_555, "16 BITFIELDS 555" },
        { F_555RGB, "16 BI_RGB (555)" }, { F_RGB32, "32 BI_RGB" }, { F_RGB32BF, "32 BITFIELDS" } };
    size_t i; char lab[96];
    say("[3] pixel formats, top-down (Recoil) and bottom-up, vs FFmpeg reference");
    for (i = 0; i < sizeof F / sizeof *F; i++) {
        _snprintf(lab, sizeof lab, "%s top-down", F[i].n);  run_compare("recoil_intra_320x240_30.avi", "recoil_intra_320x240_30.bgr24", F[i].f, 0, 1, lab);
        _snprintf(lab, sizeof lab, "%s bottom-up", F[i].n); run_compare("recoil_intra_320x240_30.avi", "recoil_intra_320x240_30.bgr24", F[i].f, 1, 7, lab);
    }
    run_compare("intra_640x480_15.avi", "intra_640x480_15.bgr24", F_565, 0, 1, "640x480 565 top-down");
}

static void t_nonsequential(void)
{
    static const int order[] = { 0, 5, 3, 100, 7, 239, 0, 128, 129, 2, 200, 1 };
    Rc r; size_t n; unsigned char *ref = load_ref("recoil_intra_320x240_30.bgr24", &n), *exp; size_t i; int maxd, bad = 0;
    say("[4] non-sequential requests on all-intra stream");
    if (!ref || rc_open(&r, P("recoil_intra_320x240_30.avi"), F_565, 0)) { g_fail++; return; }
    exp = (unsigned char *)malloc(r.pixbytes);
    for (i = 0; i < sizeof order / sizeof *order; i++) {
        LONG f = order[i]; size_t fsz = (size_t)r.w * r.h * 3;
        if (rc_frame(&r, f) != 0) { bad++; continue; }
        expect_frame(&r, ref + f * fsz, exp);
        if (diff_stats(exp, (unsigned char *)r.pixels, r.pixbytes, &maxd)) bad++;
    }
    CHECK(bad == 0, "%d of %d random-access frames wrong", bad, (int)(sizeof order / sizeof *order));
    free(exp); free(ref); rc_close(&r);
}

/* P-frame stream, Recoil skipping frames: gap must be detected, never decoded onto the wrong reference */
static void t_pframes(void)
{
    Rc r; size_t n; unsigned char *ref = load_ref("recoil_pframes_320x240_30.bgr24", &n), *exp, *prev; int i, maxd, seqbad = 0, gaps_ok = 0, gaps = 0, keys_ok = 0, keys = 0;
    say("[5] P-frame stream: sequential, then Recoil-style frame skipping");
    if (!ref || rc_open(&r, P("recoil_pframes_320x240_30.avi"), F_RGB24, 0)) { g_fail++; return; }
    exp = (unsigned char *)malloc(r.pixbytes); prev = (unsigned char *)malloc(r.pixbytes);
    for (i = 0; i < r.nframes; i++) {                                   /* sequential: exact */
        expect_frame(&r, ref + (size_t)i * r.w * r.h * 3, exp);
        if (rc_frame(&r, i) != 0 || diff_stats(exp, (unsigned char *)r.pixels, r.pixbytes, &maxd)) seqbad++;
    }
    CHECK(seqbad == 0, "%d sequential P-stream frames wrong (stream has 1 keyframe per 30)", seqbad);
    rc_close(&r);
    if (rc_open(&r, P("recoil_pframes_320x240_30.avi"), F_RGB24, 0)) { g_fail++; return; }
    for (i = 0; i < r.nframes; i += 2) {                                /* skip every other frame */
        int iskey = (i % 30) == 0;
        memcpy(prev, r.pixels, r.pixbytes);
        LRESULT lr = rc_frame(&r, i);
        expect_frame(&r, ref + (size_t)i * r.w * r.h * 3, exp);
        if (i == 0 || iskey) { keys++; if (lr == 0 && !diff_stats(exp, (unsigned char *)r.pixels, r.pixbytes, &maxd)) keys_ok++; }
        else if (i > 0) { gaps++; if (lr == 0 && !diff_stats(prev, (unsigned char *)r.pixels, r.pixbytes, &maxd)) gaps_ok++; }
    }
    CHECK(keys == keys_ok, "keyframes after a skip must decode exactly: %d/%d", keys_ok, keys);
    CHECK(gaps == gaps_ok, "skipped-reference P frames must be refused (output held, not corrupted): %d/%d", gaps_ok, gaps);
    INFO("skip-every-2nd: %d keyframes exact, %d dependent frames held (no corrupt output)", keys_ok, gaps_ok);
    free(exp); free(prev); free(ref); rc_close(&r);
}

static void t_multi(void)
{
    Rc a, b, c; size_t na, nb; unsigned char *ra = load_ref("recoil_intra_320x240_30.bgr24", &na), *rb = load_ref("intra_640x480_15.bgr24", &nb), *rc = load_ref("recoil_pframes_320x240_30.bgr24", &na), *ea, *eb, *ec; int i, bad = 0, maxd;
    say("[6] three simultaneous decoder instances, interleaved");
    if (!ra || !rb || !rc || rc_open(&a, P("recoil_intra_320x240_30.avi"), F_565, 0) || rc_open(&b, P("intra_640x480_15.avi"), F_RGB24, 0) || rc_open(&c, P("recoil_pframes_320x240_30.avi"), F_RGB32, 1)) { g_fail++; return; }
    CHECK(a.hic != b.hic && b.hic != c.hic, "instances share a handle");
    ea = (unsigned char *)malloc(a.pixbytes); eb = (unsigned char *)malloc(b.pixbytes); ec = (unsigned char *)malloc(c.pixbytes);
    for (i = 0; i < 60; i++) {
        if (rc_frame(&a, i) || rc_frame(&b, i) || rc_frame(&c, i)) { bad++; continue; }
        expect_frame(&a, ra + (size_t)i * 320 * 240 * 3, ea); if (diff_stats(ea, (unsigned char *)a.pixels, a.pixbytes, &maxd)) bad++;
        expect_frame(&b, rb + (size_t)i * 640 * 480 * 3, eb); if (diff_stats(eb, (unsigned char *)b.pixels, b.pixbytes, &maxd)) bad++;
        expect_frame(&c, rc + (size_t)i * 320 * 240 * 3, ec); if (diff_stats(ec, (unsigned char *)c.pixels, c.pixbytes, &maxd)) bad++;
    }
    CHECK(bad == 0, "%d interleaved frames wrong", bad);
    rc_close(&a); rc_close(&b); rc_close(&c); free(ea); free(eb); free(ec); free(ra); free(rb); free(rc);
}

static SIZE_T mem_now(void)
{
    MEMORY_BASIC_INFORMATION mbi; SIZE_T total = 0; char *p = 0;
    while (VirtualQuery(p, &mbi, sizeof mbi)) { if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) total += mbi.RegionSize; p = (char *)mbi.BaseAddress + mbi.RegionSize; if ((SIZE_T)p > 0x7FFE0000) break; }
    return total;
}

static void t_repeat(void)
{
    int i, bad = 0, cycles; SIZE_T m0 = 0, m1 = 0; Rc r;
    { char *e = getenv("VFWTEST_CYCLES"); cycles = e ? atoi(e) : 100; }
    say("[7] repeated init/shutdown (%d cycles)", cycles);
    for (i = 0; i < cycles; i++) {
        if (i == 10) m0 = mem_now();
        if (rc_open(&r, P("intra_640x480_15.avi"), i & 1 ? F_565 : F_RGB24, 0)) { bad++; break; }
        if (rc_frame(&r, i % r.nframes) != 0) bad++;
        rc_close(&r);
    }
    m1 = mem_now();
    CHECK(bad == 0, "%d failures", bad);
    CHECK(m1 < m0 + 4u * 1024 * 1024, "private memory grew %ld KB over %d cycles", (long)((m1 - m0) / 1024), cycles - 10);
    INFO("private committed memory cycle10=%lu KB, last=%lu KB", (unsigned long)(m0 / 1024), (unsigned long)(m1 / 1024));
}

static void t_invalid(void)
{
    Rc r; unsigned char *guard, *page; LRESULT lr; size_t i, sz; unsigned char *sent;
    say("[8] invalid / truncated input (output must stay untouched, decoder must recover)");
    if (rc_open(&r, P("intra_640x480_15.avi"), F_565, 0)) { g_fail++; return; }
    sent = (unsigned char *)malloc(r.pixbytes); memset(r.pixels, 0xA5, r.pixbytes); memset(sent, 0xA5, r.pixbytes);
    /* get a good packet */
    CHECK(AVIStreamRead(r.vs, 3, 1, r.cbuf, r.cbytes, 0, 0) == 0, "read");
    memcpy(sent, r.cbuf, 0); /* silence */
    {
        unsigned char *good = (unsigned char *)malloc(r.cbytes); memcpy(good, r.cbuf, r.cbytes);
        guard = (unsigned char *)VirtualAlloc(0, 65536 + 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        { DWORD old; VirtualProtect(guard + 65536, 4096, PAGE_NOACCESS, &old); }
        page = guard + 65536;
        /* a) garbage */
        memset(r.cbuf, 0x5A, r.cbytes);
        lr = ICDecompress(r.hic, 0, r.src, r.cbuf, (LPBITMAPINFOHEADER)r.dst, r.pixels);
        CHECK(lr != 0, "garbage accepted");
        /* b) all zeros */
        memset(r.cbuf, 0, r.cbytes);
        lr = ICDecompress(r.hic, 0, r.src, r.cbuf, (LPBITMAPINFOHEADER)r.dst, r.pixels);
        CHECK(lr != 0, "zeros accepted");
        /* c) marker valid, but body claims to run into an unreadable page (truncated at the buffer end) */
        sz = 0; for (i = 0; i < r.cbytes; i++) if (good[i]) sz = i + 1;   /* sz ~ marker+body length */
        memcpy(page - (sz - 100), good, sz - 100);
        lr = ICDecompress(r.hic, 0, r.src, page - (sz - 100), (LPBITMAPINFOHEADER)r.dst, r.pixels);
        CHECK(lr != 0, "packet truncated at page boundary accepted (lr=%ld)", (long)lr);
        /* d) marker intact, body bytes cut but readable (zero filled) -> decoder error, never a picture */
        memset(r.cbuf, 0, r.cbytes); memcpy(r.cbuf, good, sz / 2);
        lr = ICDecompress(r.hic, 0, r.src, r.cbuf, (LPBITMAPINFOHEADER)r.dst, r.pixels);
        CHECK(lr != 0, "half packet accepted (lr=%ld)", (long)lr);
        { int maxd; CHECK(diff_stats(sent, (unsigned char *)r.pixels, r.pixbytes, &maxd) == 0, "destination written despite error (maxd %d)", maxd); }
        /* e) NULL buffers / bad pointers */
        lr = ICDecompress(r.hic, 0, r.src, NULL, (LPBITMAPINFOHEADER)r.dst, r.pixels); CHECK(lr != 0, "NULL input accepted");
        lr = ICDecompress(r.hic, 0, r.src, good, (LPBITMAPINFOHEADER)r.dst, (void *)(guard + 65536 - 16)); CHECK(lr != 0, "unwritable destination accepted");
        /* f) recovery: next valid frame decodes fine */
        memcpy(r.cbuf, good, r.cbytes);
        lr = ICDecompress(r.hic, 0, r.src, r.cbuf, (LPBITMAPINFOHEADER)r.dst, r.pixels);
        CHECK(lr == 0, "valid frame after errors failed (%ld)", (long)lr);
        VirtualFree(guard, 0, MEM_RELEASE); free(good);
    }
    free(sent); rc_close(&r);
}

static void t_varying(void)
{
    Rc r; LONG i; DWORD mn = 0xFFFFFFFF, mx = 0; LONG got; int bad = 0;
    say("[9] variable packet sizes + stale reused buffer (Recoil never clears its calloc'd buffer)");
    if (rc_open(&r, P("recoil_intra_320x240_30.avi"), F_RGB24, 0)) { g_fail++; return; }
    for (i = 0; i < r.nframes; i++) {
        if (AVIStreamRead(r.vs, i, 1, NULL, 0, &got, NULL) == 0) { if ((DWORD)got < mn) mn = got; if ((DWORD)got > mx) mx = got; }
    }
    INFO("compressed frame sizes vary %lu .. %lu bytes (buffer %ld, biSizeImage %lu)", mn, mx, r.cbytes, r.src->biSizeImage);
    CHECK(mx > mn, "test stream does not vary in size");
    /* largest frame first, then every smaller one into the SAME buffer */
    for (i = 0; i < r.nframes; i++) { if (rc_frame(&r, i) != 0) bad++; }
    CHECK(bad == 0, "%d frames failed with a stale buffer", bad);
    rc_close(&r);
    /* same without the length marker: length must come from biSizeImage, which Recoil leaves constant */
    {
        Rc q; int ok = 0, err = 0, wrong = 0, maxd; size_t rn; unsigned char *ref = load_ref("intra_640x480_15.bgr24", &rn), *ex;
        if (ref && rc_open(&q, P("intra_640x480_15_nomarker.avi"), F_RGB24, 0) == 0) {
            ex = (unsigned char *)malloc(q.pixbytes);
            for (i = 0; i < q.nframes; i++) {
                if (rc_frame(&q, i) == 0) { ok++; expect_frame(&q, ref + (size_t)i * q.w * q.h * 3, ex); if (diff_stats(ex, (unsigned char *)q.pixels, q.pixbytes, &maxd)) wrong++; } else err++;
            }
            INFO("UNMARKED stream, Recoil convention (length = constant biSizeImage, stale buffer tail): %d decoded (%d with wrong pixels), %d refused", ok, wrong, err);
            free(ex); free(ref); rc_close(&q);
        }
    }
}

static void t_ex(void)
{
    Rc r; ICDECOMPRESSEX ex; LRESULT lr;
    say("[10] ICDecompressEx path");
    if (rc_open(&r, P("intra_640x480_15.avi"), F_RGB24, 1)) { g_fail++; return; }
    AVIStreamRead(r.vs, 0, 1, r.cbuf, r.cbytes, 0, 0);
    memset(&ex, 0, sizeof ex);
    r.dst->bV4Height = 480;
    lr = ICDecompressExBegin(r.hic, 0, r.src, r.cbuf, 0, 0, 640, 480, (LPBITMAPINFOHEADER)r.dst, r.pixels, 0, 0, 640, 480);
    CHECK(lr == ICERR_OK, "ExBegin=%ld", (long)lr);
    lr = ICDecompressEx(r.hic, 0, r.src, r.cbuf, 0, 0, 640, 480, (LPBITMAPINFOHEADER)r.dst, r.pixels, 0, 0, 640, 480);
    CHECK(lr == ICERR_OK, "Ex=%ld", (long)lr);
    lr = ICDecompressEx(r.hic, 0, r.src, r.cbuf, 0, 0, 640, 480, (LPBITMAPINFOHEADER)r.dst, r.pixels, 0, 0, 320, 240);
    CHECK(lr != ICERR_OK, "scaling request must be refused");
    ICDecompressExEnd(r.hic);
    rc_close(&r);
}

static void t_nodriver(void)
{
    HIC h; BITMAPINFOHEADER in; LRESULT lr;
    say("[11] driver-level open/close, GETINFO, unknown messages");
    ensure_driver();
    h = ICOpen(ICTYPE_VIDEO, mmioFOURCC('H','2','6','4'), ICMODE_DECOMPRESS);
    CHECK(h != NULL, "ICOpen failed");
    if (!h) return;
    CHECK(ICSendMessage(h, 0x7FFF + 0x1234, 0, 0) != ICERR_OK, "unknown message must not claim success");
    memset(&in, 0, sizeof in); in.biSize = 40; in.biWidth = 64; in.biHeight = 64; in.biPlanes = 1; in.biBitCount = 24; in.biCompression = mmioFOURCC('H','2','6','4');
    lr = ICSendMessage(h, ICM_DECOMPRESS_END, 0, 0);               /* END without BEGIN must be harmless */
    CHECK(lr == ICERR_OK, "END without BEGIN=%ld", (long)lr);
    ICClose(h);
    h = ICOpen(ICTYPE_VIDEO, mmioFOURCC('H','2','6','4'), ICMODE_COMPRESS);
    CHECK(h == NULL, "compress mode must be refused (decoder only)");
    if (h) ICClose(h);
}

static int cmd_dump(int argc, char **argv)
{
    Rc r; FILE *f; Fmt fmt = F_RGB24; int bottomup = 0, i; LONG n;
    if (argc < 5) return 2;
    switch (atoi(argv[3])) { case 16: fmt = F_565; break; case 32: fmt = F_RGB32; break; default: break; }
    for (i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--bottomup")) bottomup = 1;
        else if (!strcmp(argv[i], "--fmt") && i + 1 < argc) { i++; fmt = !strcmp(argv[i], "555") ? F_555 : !strcmp(argv[i], "rgb555") ? F_555RGB : !strcmp(argv[i], "rgb32") ? F_RGB32 : F_565; }
    }
    if (rc_open(&r, argv[2], fmt, bottomup)) return 1;
    f = fopen(argv[4], "wb");
    for (n = 0; n < r.nframes; n++) { if (rc_frame(&r, n)) { say("frame %ld failed", n); break; } fwrite(r.pixels, 1, r.pixbytes, f); }
    fclose(f); rc_close(&r); return 0;
}

static int cmd_bench(int argc, char **argv)
{
    Rc r; LARGE_INTEGER f, a, b; LONG n; int bpp, pass; double ms;
    if (argc < 4) return 2;
    bpp = atoi(argv[3]);
    if (rc_open(&r, argv[2], bpp == 16 ? F_565 : bpp == 32 ? F_RGB32 : F_RGB24, 0)) return 1;
    QueryPerformanceFrequency(&f);
    for (pass = 0; pass < 2; pass++) {
        QueryPerformanceCounter(&a);
        for (n = 0; n < r.nframes; n++) if (rc_frame(&r, n)) { say("frame %ld failed", n); return 1; }
        QueryPerformanceCounter(&b);
        ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        say("pass %d: %ld frames %dx%d @%d bpp: %.1f ms total, %.2f ms/frame (%.1f fps; real-time needs %.1f ms/frame at %lu fps)", pass, r.nframes, r.w, r.h, bpp, ms, ms / r.nframes, r.nframes * 1000.0 / ms,
            1000.0 / (r.info.dwRate / r.info.dwScale), r.info.dwRate / r.info.dwScale);
    }
    rc_close(&r); return 0;
}

static int cmd_probe(int argc, char **argv)
{
    HIC h; ICINFO ic; char nm[64], desc[128]; DWORD fcc = 0; int i;
    if (argc < 3 || strlen(argv[2]) != 4) { fputs("usage: vfwtest probe <FOURCC>\n", stderr); return 2; }
    for (i = 3; i >= 0; i--) fcc = (fcc << 8) | (unsigned char)argv[2][i];
    ensure_driver();
    h = ICOpen(ICTYPE_VIDEO, fcc, ICMODE_DECOMPRESS);
    if (!h) { printf("ICOpen('%s') failed: no VfW decompressor registered\n", argv[2]); return 1; }
    memset(&ic, 0, sizeof ic);
    ICGetInfo(h, &ic, sizeof ic);
    WideCharToMultiByte(CP_ACP, 0, ic.szName, -1, nm, sizeof nm, 0, 0);
    WideCharToMultiByte(CP_ACP, 0, ic.szDescription, -1, desc, sizeof desc, 0, 0);
    ICClose(h);
    printf("ICOpen('%s') -> '%s' / '%s' v%lu.%lu\n", argv[2], nm, desc, ic.dwVersion >> 16, ic.dwVersion & 0xFFFF);
    return strstr(nm, "FFmpeg") ? 0 : 1;
}

static int cmd_bmp(int argc, char **argv)
{
    Rc r; FILE *f; BITMAPFILEHEADER bf; BITMAPINFOHEADER bi;
    if (argc < 5 || rc_open(&r, argv[2], F_RGB24, 1)) return 1;
    if (rc_frame(&r, atoi(argv[3]))) return 1;
    memset(&bf, 0, sizeof bf); bf.bfType = 0x4D42; bf.bfOffBits = sizeof bf + sizeof bi; bf.bfSize = bf.bfOffBits + r.pixbytes;
    bi = *r.src; bi.biSize = sizeof bi; bi.biBitCount = 24; bi.biCompression = BI_RGB; bi.biSizeImage = (DWORD)r.pixbytes;
    f = fopen(argv[4], "wb"); fwrite(&bf, sizeof bf, 1, f); fwrite(&bi, sizeof bi, 1, f); fwrite(r.pixels, 1, r.pixbytes, f); fclose(f);
    rc_close(&r); return 0;
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dll") && i + 1 < argc) { g_dll = argv[++i]; memmove(argv + i - 1, argv + i + 1, (argc - i - 1) * sizeof *argv); argc -= 2; i -= 2; }
    }
    if (argc >= 2 && !strcmp(argv[1], "dump")) return cmd_dump(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "bmp")) return cmd_bmp(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "bench")) return cmd_bench(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "probe")) return cmd_probe(argc, argv);
    if (argc < 3 || strcmp(argv[1], "selftest")) { fputs("usage: vfwtest selftest <testdata> [--dll ffvfw.dll] | dump <avi> <bpp> <out> | bmp <avi> <n> <out>\n", stderr); return 2; }
    strncpy(g_dir, argv[2], MAX_PATH - 1);
    { char lp[MAX_PATH]; _snprintf(lp, sizeof lp, "%s\\vfwtest.log", g_dir); g_log = fopen(lp, "w"); }
    say("vfwtest: codec source = %s", g_dll ? g_dll : "registry");
    t_discovery(); t_format(); t_pixel_formats(); t_nonsequential(); t_pframes(); t_multi(); t_repeat(); t_invalid(); t_varying(); t_ex(); t_nodriver();
    say("\nRESULT: %d checks passed, %d FAILED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
