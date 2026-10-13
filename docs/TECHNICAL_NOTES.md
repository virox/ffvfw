# Technical notes

Everything marked **[verified]** was observed in this repository's tests or in a logged run of the
unmodified `Recoil.exe`. Everything marked **[assumption]** was reasoned from sources, not observed.

## 1. The VfW contract Recoil expects (from RecoilRebuild `zFMV/fmv_stream.cpp`)

Source of truth: `zFMV_Stream::Constructor` (0x463EF0) and `ReadAndDecodeFrame` (0x4643A0).
RecoilRebuild is a reconstruction (not every function is verified), so the table is the *source's* contract. What the retail `Recoil.exe` was
actually seen doing (codec log of real runs): `ICLocate` -> `ICM_DECOMPRESS_QUERY` for a **16-bit `BI_BITFIELDS`** destination -> `ICM_DECOMPRESS_BEGIN`
-> one decode per frame in sequence at the video's frame rate -> `ICM_DECOMPRESS_END`/`ICClose`. The remaining rows (negated height after BEGIN,
NULL byte counters, frame skipping by elapsed time, error handling) come from the source only; the standalone test reproduces them.
**Not checked against the original executable or its decompilation** (no HLIL was consulted).

| Step | What Recoil does | What the codec must handle |
|---|---|---|
| Open | `AVIStreamOpenFromFileA(.., streamtypeVIDEO, 0, 0x10, 0)` | normal AVI; no OS-level codec involved yet |
| Source format | `AVIStreamReadFormat` -> `srcFormat` = the `strf` chunk, size unchanged | `biCompression` = our FourCC; 40-byte header is enough |
| Dest format | `calloc(max(formatBytes,108))` `BITMAPV4HEADER` copied from `srcFormat`; `bV4Size = max(formatBytes,108)`, `bV4BitCount = display bpp`, `BI_BITFIELDS` (or `BI_RGB` when 24), masks from `zVideo::PixelPackGetRgbMasks`, alpha 0, `SizeImage = h*((w+3)&~3)*bpp/8` | arbitrary contiguous 16/32-bit masks, 24-bit `BI_RGB`; do **not** assume 565 |
| Locate | `ICLocate(ICTYPE_VIDEO, fccHandler, src, dst, ICMODE_DECOMPRESS)` | answers `ICM_DECOMPRESS_QUERY(src,dst)` with `ICERR_OK` for exactly that pair |
| Buffer | `calloc(dwSuggestedBufferSize)` (fallback `bitcount/8*w*h`), **once, reused every frame** | stale tail of earlier frames beyond the current one |
| Begin | `ICM_DECOMPRESS_BEGIN(src, dst)` with a **positive** `bV4Height` | |
| Orientation | `bV4Height = -bV4Height` **after** BEGIN | `ICM_DECOMPRESS` arrives with a *negative* height (top-down) although BEGIN saw positive: orientation must be taken per call, not cached at BEGIN |
| Frame | `AVIStreamRead(vs, idx, 1, buf, bufBytes, 0, 0)` (NULL byte/sample counters), then `ICDecompress(hic, 0, src, buf, dst, pixels)` | flags = 0, `ckid` = 0 |
| End | `ICM_DECOMPRESS_END`, `ICClose` | |
| Timing | `idx = (int)((t - t0) * (int)dwRate/dwScale)` | integer fps only (use 15/24/25/30, not 29.97); frames **are** skipped when the game is slow; `idx >= nframes` repeats the last output |
| Errors | any non-zero `ICDecompress` result -> `zError::ReportOld(0x400, ...)`, returns 0 **without** `LeaveCriticalSection` | return non-zero only for real errors; "hold" gap policy returns `ICERR_OK` |
| Audio | separate stream, read as PCM straight into DirectSound | audio must be uncompressed PCM; the codec is not involved |

### The two buffer/length details you asked about

**No input length reaches the codec [verified in `vfwtest`, consistent with Recoil source].**
`ICM_DECOMPRESS` carries `ICDECOMPRESS{dwFlags, lpbiInput, lpInput, lpbiOutput, lpOutput, ckid}`: no byte
count. `lpbiInput->biSizeImage` is Recoil's `srcFormat`, i.e. the constant value from the stream's `strf`.
`AVIStreamRead(..., NULL, NULL)` discards the real chunk size, and because the buffer is reused,
**the bytes after the current frame are leftovers of earlier (possibly larger) frames, not zeros**.
For Indeo this is harmless (the bitstream is self-terminating); for Annex-B H.264 it means the decoder cannot tell
where the access unit ends.

**Chosen solution: a self-describing frame header.** `tools/vfwmux.py` writes every AVI sample as

```
00 00 00 01 1E  <escaped: "VFWH" ver=1 flags seq(u32) body_len(u32) FF>      <- NAL type 30 (unspecified)
00 00 00 01 09 ...                                                           <- the Annex-B access unit (AUD, SPS, PPS, slice)
```

* NAL type 30 is *unspecified* in H.264 and is ignored by every decoder, so the AVI still plays unchanged in
  ffplay/VLC/ffmpeg [verified with ffmpeg: decodes with no errors].
* The codec reads `body_len` and passes exactly that many bytes to libavcodec; stale bytes after it are never looked at.
* The range `[ptr, ptr+body_len)` is probed with `IsBadReadPtr` first, so a lying/truncated header cannot crash the host.
* `seq` + the IDR flag give the codec the frame number Recoil never passes (see section 4).
* **Fallback for plain Annex-B AVIs** (no marker): the length is `lpbiInput->biSizeImage`. That is the standard VfW
  convention and works for apps that fill it in per frame (VirtualDub, etc.). Under Recoil's constant-`biSizeImage`
  convention the decoder receives the real frame *plus stale tail*. **[verified]** In the one unmarked test file
  (single slice per picture, all-IDR) FFmpeg tolerated the tail and all 120 frames were pixel-exact, but that
  is luck, not a contract; multi-slice or P-frame content could break. Use the muxer.

### Sizes and dimensions
* Keep `strf.biSizeImage` equal to the largest frame (the muxer does) and the width a multiple of 4 (see 5).
* `dwSuggestedBufferSize` is recomputed by Windows' AVI reader from the real max chunk **[verified: 16072 vs strf 16384]**;
  the buffer Recoil allocates can therefore be *smaller* than `strf.biSizeImage`. The codec never trusts `biSizeImage`
  as a readable range when the marker is present.

## 2. H.264-in-AVI representation

* Stored: Annex-B byte stream (start codes), one access unit per `00dc` chunk, SPS/PPS repeated in every IDR
  (`repeat-headers=1`), AUD at the start of each AU. `biCompression`/`fccHandler` = `H264`.
* AVC-style (length-prefixed NALs + `avcC` extradata, FourCC `avc1`) is **not supported**: it needs the extradata from the
  `strf` tail, which Recoil's V4 conversion would corrupt (it copies `formatBytes` into the destination header and enlarges
  `bV4Size`). `AVC1` is registered as an alias *for Annex-B payloads only*. **[assumption]** other AVI tools may label
  length-prefixed streams `avc1`; those are rejected by the decoder (error, not garbage).
* Because Recoil copies the whole `strf` into the destination header (`bV4Size = formatBytes` when larger than 108), any
  `strf` extradata would make `bV4Size` > 108. The codec accepts `biSize >= 108` as V4 but the muxer emits a plain 40-byte `strf`.
* B-frames are unsupported: the decoder runs with `AV_CODEC_FLAG_LOW_DELAY` (1 packet in, 1 frame out).

## 3. Pixel output [verified, bit-exact vs FFmpeg up to 640x480; fast path above]
* decode -> `sws_scale` to BGR24/BGRA (bicubic; colourspace taken from the stream tags, untagged SD = BT.601) -> per-channel scaling
  into the destination masks, `(v*(2^n-1)+127)/255`, no dithering.
* Conversion quality (`FFVFW_QUALITY`): `exact` = `SWS_ACCURATE_RND|SWS_FULL_CHR_H_INT` (bit-exact with FFmpeg's own bicubic RGB output,
  what the test suite checks, 34-44 ms for a 1080p frame); `fast` = plain bicubic (about 2 ms at 1080p, max 4/255 and mean 0.7 difference on a
  1080p frame); `auto` (default) = exact for frames of at most 640x480 pixels, fast above. Without this switch 1080p30 was not sustainable
  on one thread.
* 24 BI_RGB; 16/32 `BI_BITFIELDS` with any contiguous, non-overlapping masks (incl. RGB555/RGB565/RGB444+alpha);
  16 `BI_RGB` = RGB555; 32 `BI_RGB` = 0x00RRGGBB; alpha mask non-zero -> alpha is written fully opaque.
* Rows: DWORD aligned stride `((w*bpp+31)/32)*4`; negative height = top-down, positive = bottom-up. All writes go to a scratch
  buffer first and are copied row by row into the destination; destination `biSizeImage` (if set) and writability are validated.
* **Recoil row-size quirk [from source, arithmetic only]:** Recoil sizes its destination with `((w+3)&~3)*bpp/8` per row (and keeps a third,
  unpadded value `bpp/8*w` in `decodedFrameStrideBytes`). The DIB stride the codec writes is `((w*bpp+31)/32)*4`. For 16 and 24 bit these differ
  whenever `w % 4 != 0` (e.g. w=322: 644 vs 648 bytes at 16 bit, 968 vs 972 at 24 bit); for 32 bit they never differ. Keep widths a multiple of
  4 (`make-recoil-video.ps1` enforces it). What the game's blit does with other widths was not tested.
* Not offered: palettised and YUV output. `ICLocate` therefore never selects us for those requests.

## 4. Frame dependencies and frame skipping [verified]
Recoil computes the frame index from wall-clock time and may skip frames; it never decodes the skipped ones and never
passes `ICDECOMPRESS_NOTKEYFRAME`/`PREROLL`. A P-frame decoded after a skip would reference a picture the decoder never
saw. FFmpeg would *silently conceal* this ("frame num gap"), producing smeared video, so the codec detects it itself:

* each marker carries `seq`; a non-IDR frame is accepted only if `seq == last_seq + 1` and no earlier gap is unresolved;
* otherwise the frame is **not decoded**; the decision is logged at error level with the counts;
* default policy `hold`: return `ICERR_OK` and leave the destination untouched (Recoil keeps showing the last good
  frame; nothing corrupt is ever written) until the next IDR, which decodes exactly. Policy `error` (`FFVFW_GAP_POLICY=error`)
  returns `ICERR_GOTOKEYFRAME` instead, which Recoil treats as a fatal error.
* Test: skipping every other frame of a GOP-30 stream: 8/8 keyframes exact, 112/112 dependent frames refused; sequential decode of the
  same stream is exact.
* Consequence: P-frame video works in Recoil only if the game never skips, otherwise it freezes until the next IDR. **The supported
  profile is all-IDR** (the encode command in `tools/make-test-videos.ps1` produces it). Short GOPs (e.g. 0.5 s) bound the freeze.

## 5. Registration [verified]
* VfW codecs are located through `vidc.<fourcc>` string values under `Drivers32`; the 32-bit view
  (`HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Drivers32`) is what a 32-bit game sees. No COM, no `regsvr32`, HKLM only.
* **The value is parsed as `driver [parameters]`: a path containing a space is cut at the first space.**
  My first install into `C:\Program Files (x86)\FFmpegVfW\` enumerated fine (`ICInfo` listed `h264`) but `ICOpen` silently failed
  and the DLL was never loaded. The installer now defaults to `C:\Windows\SysWOW64\` (like Windows' own `l3codeca.acm`; ffdshow also installs into the system directory [from memory, not checked])
  and rejects paths with spaces.
* Values are matched case-insensitively. `vidc.H264`/`vidc.AVC1` are registered; neither existed before. Conflicts are reported, never overwritten.

## 6. The launcher in this install hijacks VfW [verified]
Running the game through `rcrun.exe` injects `rcfix.dll`, whose `crates\fixup\src\vfw\ic.rs` replaces the system VfW lookup:
log `loaded IC handler 'cvid'` / `ERROR unknown IC handler 'H264'` -> `Cannot Decompress AVI Video Stream`.
Our codec is not consulted on that path (nothing to do with our DLL). The retail `Recoil.exe` launched directly uses real
`msvfw32` and our codec was selected (`vidc.H264` -> `ffvfw.dll`). Adding an `H264` handler to rcfix would be a separate
change to that launcher and was deliberately not made.

## 7. Threading and robustness
* FFmpeg is built without threads (no pthread runtime in the DLL) and is therefore serialised process-wide with one lock; each VfW
  instance has its own `AVCodecContext`/`SwsContext`/scratch buffers and its own critical section. Decoder thread count 1.
* `err_recognition = EXPLODE|BITSTREAM|BUFFER|CRCCHECK`, `AV_FRAME_FLAG_CORRUPT`/`decode_error_flags` => frame discarded, `ICERR_ERROR`,
  destination untouched **[verified: garbage, zero fill, half packet, page-boundary truncation, NULL pointers; decoder recovers on the next frame]**.
* `ICM_DECOMPRESS` without BEGIN is begun implicitly. END without BEGIN is harmless. Compress modes are refused at `DRV_OPEN`.
* 1000 open/decode/close cycles: private memory +200 KB (no leak) **[verified]**.

## 8. Reference implementations
* `sKyissKy/iv50-ffmpeg-vfw` (cloned under `refs/`; only its licence header and file layout were looked at, not studied in detail): useful as a map of messages an Indeo-oriented FFmpeg VfW
  must answer. Nothing was copied; the driver here is ~350 lines written from the msvfw32 documentation and observed behaviour.
* ffdshow-tryouts: only its *installation convention* was used (DLL in the system dir, bare `vidc.*` values). No code reviewed or copied (GPL).

## 9. Licensing
See `NOTICE.md`. FFmpeg 7.1.1 is configured **without** `--enable-gpl/--enable-version3/--enable-nonfree` (`License: LGPL version 2.1 or later`),
H.264 decoder + parser + swscale only, static link. LGPL requires that users can relink against a modified FFmpeg: the repo contains the
build scripts, our sources and `make` rules; `out/obj/*.o` plus `build/ffmpeg-install/lib/*.a` relink with `make`. Distributors must ship the
FFmpeg source (or a written offer) and the LGPL text, and must not impose terms that forbid reverse engineering for debugging such modifications.

## 10. Future codecs
`src/decoder.c` holds a codec table (`CodecDesc`: name, `AVCodecID`, FourCCs, keyframe detector). The VfW layer only needs a
FourCC match, a framing routine and a keyframe test. H.265 (Annex-B + hvcC differences, 2-byte NAL header, needs `--enable-decoder=hevc`),
VP9 (superframes/hidden frames: one packet can produce no picture) and AV1 (OBUs, temporal units, `--enable-decoder=av1` + dav1d/libaom
or the native decoder) each need their own framing check, their own "1 packet -> 1 picture" analysis (VP9/AV1 may output nothing or
delay), and a dependency/keyframe rule for the skipped-frame policy. None of this is implemented; each must be added and verified individually.

## 11. Things learned from the Recoil runs [verified unless noted]

**Audio layout matters.** Recoil reads the PCM stream in `dwSuggestedBufferSize` pieces straight into DirectSound, so the AVI's audio headers and
interleave must look like the retail files. Seen in-game:
* audio `dwSuggestedBufferSize = 0` -> audio garbled ("slow motion");
* suggested buffer fixed, but one 1470-byte audio chunk per video frame, `InitialFrames` 0, avih flags 0x110 -> still garbled;
* retail layout copied exactly, and the muxer's default layout (1-second audio chunks, first chunk before frame 0 and one more every `fps` frames,
  audio `InitialFrames` 23, avih flags 0x10, audio `dwSuggestedBufferSize` = 1 s of audio + 8, 16-byte PCM `strf`) -> correct.
Which of 1-second chunks / `InitialFrames` / avih fields is decisive was **not** isolated; the muxer keeps all of them. The audio must be uncompressed PCM
and exactly as long as the video (the muxer trims or pads with silence).

**Aspect ratio.** The game stretches the whole decoded frame to its target rectangle; a 4:3 video on a wider display is stretched (tester's report, and the
source: `CZFMVActionPlayAvi::Update` blits to `destRect` = the active region). The decoder cannot change the frame size (Recoil dictates the destination size
from the stream header) and does not know the display. Options: bake black bars into the video (`make-recoil-video.ps1 -Fit pad` with a canvas of the screen's
aspect), or fix the blit rectangle in the game launcher. A decoder-side "pre-compensation" (shrink the picture inside the same frame so the game's stretch restores
the aspect) is possible with a configured target aspect but was **not implemented**; under `rcrun.exe` it additionally requires rcfix to forward unknown handlers to the
system `ICLocate` (section 6).

**Performance (single thread, FFmpeg built without threads).** 1920x1080 all-IDR, crf 18: decode 13-19 ms, colour conversion 1-2 ms (fast mode), packing 1-5 ms; measured
in-game 17.3 ms/frame average (decode 13.2 + convert 1.1 + pack 3.0), standalone `vfwtest bench` 24-29 ms/frame including the AVI read. Timings varied noticeably between
runs on this machine (decode 13-20 ms), so treat them as ranges. Benchmark: `vfwtest bench <avi> <bpp>`.
Multi-slice or threaded decoding was not built; if 4K or slower CPUs matter, rebuild FFmpeg with `--enable-w32threads` and encode with `slices=4`.

