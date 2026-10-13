# Test results (2026-10-12, Windows 11 x64, 32-bit ffvfw.dll built from this tree)

Legend: **V** = verified by a test or log in this repo, **A** = assumption / not tested.

## Standalone VfW suite - `out\vfwtest.exe selftest testdata` (codec installed in the registry): 52 passed, 0 failed
Same result with `--dll out\ffvfw.dll` (process-local `ICInstall`). Raw output: `testdata/selftest_installed.txt` (not in git).
Reference: stock FFmpeg decode of the same AVI to BGR24 (`scale=flags=bicubic+accurate_rnd+full_chroma_int`). The suite was run against the registry-installed DLL, and again with `--dll` after the colour-conversion quality switch (test content is at most 640x480, i.e. the exact path).

| # | Spec item | Result |
|---|---|---|
| 1 | `ICLocate` discovery | **V** stream `H264` -> driver "FFmpeg H.264" through the registry; `probe` for `H264` and `AVC1` OK |
| 2 | Format negotiation | **V** accepts 16/24/32; refuses 8 bit, size mismatch, YUY2, foreign FourCC, non-contiguous masks; GET_FORMAT returns 24-bit RGB; BITFIELDS RGB444+alpha accepted |
| 3 | init/cleanup | **V** END before BEGIN harmless; compress-mode open refused |
| 4 | Sequential decode | **V** 240/240 frames 320x240 and 120/120 frames 640x480 pixel-exact |
| 5 | Non-sequential requests | **V** 12 random-access frames exact (all-IDR). P stream: see 5b |
| 5b | P-frames + skipping | **V** sequential exact; skip-every-2nd: 8/8 keyframes exact, 112/112 dependent frames refused with output held (no corrupt picture) |
| 6 | Pixel formats | **V** exact (top-down and bottom-up): 24 BI_RGB, 16 BITFIELDS 565, 16 BITFIELDS 555, 16 BI_RGB (555), 32 BI_RGB, 32 BITFIELDS |
| 7 | Multiple instances | **V** three interleaved instances (320x240/16, 640x480/24, P-stream/32 bottom-up) exact |
| 8 | Repeated init/shutdown | **V** 100 cycles (and 1000 cycles: +200 KB private memory) |
| 9 | Invalid/truncated input | **V** garbage, zeros, packet cut at a no-access page, half packet, NULL input, unwritable destination: all rejected, destination untouched, next valid frame decodes |
| 10 | Variable packet sizes | **V** 5206..50385 bytes with Recoil's reused stale buffer: all exact. Unmarked plain-Annex-B stream under the same convention also exact (**luck, not a guarantee**) |
| - | `ICDecompressEx` | **V** begin/decode; scaling request refused |

## Large frames (standalone, through VfW, `vfwtest bench`, no disk output)
| Content | Output | Result |
|---|---|---|
| 1920x1080 @30, all-IDR | 16-bit | **V** 28.7 ms/frame (fast mode); 34-72 ms/frame with the previous exact-only conversion (runs varied) |
| 1920x1080 @30, all-IDR | 24-bit | **V** 24.3 ms/frame (fast mode) |
| 1280x720 @24, 1280x960 @24 | 24-bit | **V** about 15-16 ms/frame |
| 1920x1080 fast vs exact conversion | - | **V** max delta 4/255, mean 0.7, 0.9 % of pixels differ by more than 2 (one frame) |
Not bit-exact-tested against FFmpeg at these sizes (the fast path is intentionally not bit-exact).

## Recoil (unmodified retail `Recoil.exe`, directly started) - see `docs/RECOIL_TEST.md`
* **V** codec selected by the game's own `ICLocate`; 16-bit BITFIELDS destination; 625 frames at real-time pace, 0 errors, clean teardown.
* **V (tester)** video plays correctly; audio correct with the retail layout and with the muxer default (1-second chunks); garbled with per-frame chunks.
* **V** 1920x1080 @ 30 fps all-IDR plays fine in-game (17.3 ms/frame average); see `docs/RECOIL_TEST.md`.
* **V (tester)** 4:3 video is stretched by the game on wider display modes; not fixable inside the decoder (`TECHNICAL_NOTES.md` section 11).
* **A / untested** other display modes in-game, looping, frame skipping in-game, audio/video sync measurement, `rcrun.exe` path (known to fail: launcher hook).

## Known limitations
* Needs the `vfwmux.py` frame header for Recoil (no per-frame length otherwise).
* P-frame streams freeze (hold) on skipped frames until the next IDR; B-frames unsupported.
* Annex-B only (no `avcC`/length-prefixed); output RGB only; no scaling in `ICDecompressEx`; width should be a multiple of 4 (the script enforces it).
* Frames above 640x480 pixels use the fast (not bit-exact) colour conversion unless `FFVFW_QUALITY=exact`.
* Not usable through `rcrun.exe` until its VfW hook forwards unknown handlers.
* No 8-bit (palettised) output: if the game ever runs in one of its 8-bit display modes (per RecoilRebuild `zvid_init.c`, mode indices 0-1; unverified against the exe), `ICLocate` finds no decoder and the game reports "Cannot Decompress AVI Video Stream". Indeo handles that itself.
* Recoil asked for a 16-bit destination in every run; per the reconstructed source its display modes are only 8 or 16 bit, so 32-bit output (supported and tested standalone) is probably unreachable from the game.
* Driver path in the registry must contain no spaces.
