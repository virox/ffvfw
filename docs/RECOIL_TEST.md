# Testing in Recoil

Use a **copy** of the game; the original install is never touched.

1. Install the codec (`install\install.ps1`) and check `out\vfwtest.exe probe H264`.
2. Copy the game folder (without `Reconstructed Decomp`), e.g. to `recoil_test\`.
3. Build the replacement video from any source (the original Indeo file works) and give it the name of the file it replaces:
   ```powershell
   .\tools\make-recoil-video.ps1 -Source "<game>\video\intro_01.avi" -Dest "<copy>\video\intro_01.avi"          # 320x240 @ 30, retail-like
   .\tools\make-recoil-video.ps1 -Source "<game>\video\intro_01.avi" -Dest "<copy>\video\intro_01.avi" -Resolution 1920x1080 -Seconds 30
   ```
4. Start **`<copy>\Recoil.exe` directly** (not `rcrun.exe`, whose rcfix hooks replace VfW). In this install the intro plays at start-up.
5. Optional evidence: set the string values `FFVFW_LOG` (`C:\path\recoil.log`) and `FFVFW_LOGLEVEL` (`2` = lifecycle + per-stream timing summary,
   `3` = one line per frame) under `HKCU\Software\FFmpegVfW`; the log then shows `DECOMPRESS_QUERY` (the format the game asks for), `BEGIN`, and at the end
   `end: frames=.. dropped=.. errors=.. avg ms/frame: decode, colour-convert, pack`. Delete the key afterwards.
6. Restore: delete the copy, or put the original `intro_01.avi` back.

## Observed (this machine, Windows 11 x64, DDrawCompat ddraw.dll in the game folder)

First run, 320x240 intro (`Recoil.exe` started directly), codec log:
`DRV_LOAD`, `DECOMPRESS_QUERY in.fcc=H264 out.bpp=16` -> OK (a 16-bit `BI_BITFIELDS` destination, requested by the game), `BEGIN H.264 320x240 ... out: bpp=16 comp=3`,
625 frames in 20.8 s (30.0 fps real time), 0 dropped, 0 errors, then `DECOMPRESS_END`, `DRV_CLOSE`, `DRV_FREE`. The run ended there; why (the tester leaving the intro,
or otherwise) was not recorded.

Second run, 1920x1080 @ 30 fps (all-IDR, 55 MB, 30 s of the intro pillarboxed): plays "just fine" (tester: picture, smoothness and audio). Log: 566 frames, 0 errors,
clean teardown, average **decode 13.2 ms + colour-convert 1.1 ms + pack 3.0 ms = 17.3 ms per frame** (budget 33.3 ms).

| Question | Result |
|---|---|
| Does Windows select our decoder? | **yes** (`ICLocate` -> `vidc.H264` -> `ffvfw.dll`), unmodified `Recoil.exe` |
| Does Recoil accept the stream format? | **yes**, at 320x240 and 1920x1080 |
| Video appears correctly? | **yes** (tester) |
| Timing? | 30 fps pacing and no drops in the logs; lip-sync not formally measured |
| Audio? | garbled with my first two AVI layouts; **correct** with the retail layout and with the muxer's current default (see below) |
| Clean exit? | decoder teardown logged both times; no crash event in the Application log |
| Seeking / frame skipping / looping | not exercised (all frames sequential). Skipped-frame behaviour is covered only by the standalone test |
| Other display modes (24/32 bit) | not tested in-game; standalone tests cover 16/24/32 |
| Aspect ratio | the game stretches the frame to the screen; 4:3 video is stretched on wider displays (tester); see `TECHNICAL_NOTES.md` section 11 |
| Under `rcrun.exe` | **fails**: rcfix logs `unknown IC handler 'H264'` and the game reports "Cannot Decompress AVI Video Stream" |

## Audio layout (important)
Recoil streams PCM straight into DirectSound, reading `dwSuggestedBufferSize` bytes at a time. Observed in-game:
* audio `dwSuggestedBufferSize = 0`: garbled ("slow motion") - a real bug in the first muxer;
* suggested buffer fixed (44108) but one small audio chunk (1470 B) per video frame, `InitialFrames` 0, avih flags 0x110: still garbled;
* retail layout copied verbatim (`vfwmux.py --like <retail.avi>`, kept as a diagnostic option): **correct**;
* the muxer's default layout (1-second audio chunks, first before frame 0 then one every `fps` frames, audio `InitialFrames` 23, avih flags 0x10,
  audio `dwSuggestedBufferSize` = 1 s + 8): **correct** (tester). Its second audio chunk sits after 30 frames, retail's after ~8, so the exact interleave
  position does not matter.
Not isolated: which of {1-second chunks, `InitialFrames` 23, avih flags/MaxBytesPerSec} is the decisive one versus the failing layout. Keep all of them.
