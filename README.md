# ffvfw - FFmpeg-backed H.264 decoder for Video for Windows (32-bit)

`ffvfw.dll` is a 32-bit VfW *decompressor* (`DriverProc`, `vidc.H264` / `vidc.AVC1`) that lets unmodified legacy Windows
software - first target: the 1999 game **Recoil** - play H.264 AVI files through its original `AVIStream*` / `ICLocate` /
`ICDecompress` calls. libavcodec's H.264 decoder and libswscale are **statically linked**: one self-contained DLL, no FFmpeg
install, no `ffmpeg.exe`, only system DLLs (`kernel32`, `msvcrt`, `advapi32`, `bcrypt`, `winmm`).

Status (details: `docs/TEST_RESULTS.md`)
* standalone VfW test suite: 52/52 checks, bit-exact against FFmpeg, run against the registry-installed codec **verified**
* unmodified `Recoil.exe` (started directly): selects the codec, plays the 71 s intro at 320x240 and a 30 s 1920x1080 @ 30 fps test video, video and audio correct **verified by the game's owner**; decode+convert+pack averaged 17 ms/frame at 1080p
* not tested in-game: 24/32-bit display modes, looping, frames skipped on a slow machine, other resolutions than the two above
* does **not** work through `rcrun.exe` (its `rcfix` hook replaces the VfW lookup and has no `H264` handler); aspect-ratio fitting must happen in the video or in the game's blit (see `docs/TECHNICAL_NOTES.md` section 11)
* H.265 / VP9 / AV1: not implemented (architecture prepared, see `docs/TECHNICAL_NOTES.md` section 10)

## Layout
```
src/            ffvfw.c (DriverProc/ICM), decoder.c (libav wrapper, codec table), framing.c, pixout.c, log.c, ffvfw.def
test/vfwtest.c  32-bit test app using the real avifil32 + msvfw32 stack with Recoil's exact call sequence
tools/          make-recoil-video.ps1 (any video -> Recoil AVI), vfwmux.py (AVI muxer), make-test-videos.ps1 (test set), package.ps1
install/        install.ps1, uninstall.ps1 (+ common.ps1)
scripts/        build-ffmpeg.sh  (static 32-bit LGPL FFmpeg, H.264 decoder + swscale only)
Makefile, build.ps1
docs/           TECHNICAL_NOTES.md, TEST_RESULTS.md, RECOIL_TEST.md    NOTICE.md
```

## Build (Windows, MSYS2 with the MINGW32 environment)
Needs: MSYS2 (`pacman -S make diffutils mingw-w64-i686-gcc mingw-w64-i686-nasm mingw-w64-i686-pkgconf`), git, python 3.
```powershell
git clone --depth 1 --branch n7.1.1 https://github.com/FFmpeg/FFmpeg.git refs/ffmpeg
.\build.ps1 -Ffmpeg      # builds static FFmpeg (once, several minutes) then ffvfw.dll + vfwtest.exe into out\
.\build.ps1              # later builds
```
Result: `out\ffvfw.dll` (about 2.5 MB, PE32/x86) and `out\vfwtest.exe`.

## Make any video Recoil-compatible
```powershell
.\tools\make-recoil-video.ps1 -Source C:\clips\anything.mp4 -Dest "<COPY OF GAME>\video\intro_01.avi"
.\tools\make-recoil-video.ps1 -Source in.mkv -Dest out.avi -Resolution 1280x960 -Fps 24 -Crf 20 -Fit stretch
.\tools\make-recoil-video.ps1 -Source in.mp4 -Dest out.avi -Resolution source -Fps 0     # keep size and frame rate
```
Defaults: 320x240 (like the retail videos), 30 fps, all-IDR H.264, 16-bit PCM audio at 22050 Hz mono, black bars to keep the aspect ratio.
Parameters: `-Resolution WxH|source` (or `-Width/-Height`), `-Fps N` (`0` = source rate, rounded to an integer), `-Fit pad|stretch|crop`,
`-Crf 0..51` (quality; lower = bigger), `-AudioRate/-AudioChannels`, `-NoAudio`, `-Seconds`, `-PFrames`. Width must be a multiple of 4, width and height even.
Give the output the *same file name* as the video you replace (and work on a copy of the game).
Tested in-game: 320x240 and 1920x1080, both at 30 fps and 22050 Hz mono. Everything else is untested in-game. Standalone: bit-exact 16/24/32-bit output at 320x240, bit-exact 16/24-bit at 640x480; 1080p and 1280x960/720 were only decoded and benchmarked (and one 1080p frame compared: max delta 4/255).
Large all-IDR files get big (1080p, crf 18: about 1.8 MB/s of video); use a higher `-Crf` or, if you accept freezes on skipped frames, `-PFrames`.

## Make the test set (needs ffmpeg with libx264 and python on PATH)
```powershell
.\tools\make-test-videos.ps1 -RecoilVideo <any video file>   # testdata\*.avi + FFmpeg reference decodes (about 220 MB of raw references)
```
Encoder settings used (all-IDR, deterministic): `-c:v libx264 -preset medium -profile:v main -pix_fmt yuv420p
-x264-params keyint=1:min-keyint=1:bframes=0:scenecut=0:slices=1:aud=1:repeat-headers=1:threads=1:crf=18 -f h264`, then
`tools/vfwmux.py` wraps each access unit in the AVI with the length/sequence header (why: `docs/TECHNICAL_NOTES.md` section 1).
**Plain `ffmpeg -c:v libx264 out.avi` files lack that header** and only work if the host passes the frame size in `biSizeImage`.
Recoil does not.

## Install / uninstall (administrator; UAC prompt appears)
```powershell
powershell -ExecutionPolicy Bypass -File install\install.ps1 [-WhatIf]
powershell -ExecutionPolicy Bypass -File install\uninstall.ps1
```
* copies `ffvfw.dll` to `C:\Windows\SysWOW64\` and adds `vidc.H264`, `vidc.AVC1` to
  `HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Drivers32` (not COM; `regsvr32` does not apply)
* never overwrites an existing value (conflicts are listed and skipped), never touches `vidc.iv50` etc.
* records exactly what it added under `HKLM\SOFTWARE\WOW6432Node\FFmpegVfW`; uninstall removes only that
* verifies through `msvfw32` with `vfwtest probe H264` if `vfwtest.exe` is next to the script or in `out\`
* the DLL path must not contain spaces (see technical notes section 5)

## Test
```powershell
out\vfwtest.exe probe H264                       # is it registered and discovered?
out\vfwtest.exe selftest testdata                # full suite through the registry-installed codec
out\vfwtest.exe selftest testdata --dll out\ffvfw.dll   # same, without installing (process-local ICInstall)
out\vfwtest.exe dump testdata\x.avi 16 out.raw [--bottomup] [--fmt 555|rgb555|rgb32]
```

## Logging / settings
Environment variable or `HKCU\Software\FFmpegVfW` string value (environment wins):
`FFVFW_LOG` = log file path, `FFVFW_LOGLEVEL` = 0 off, 1 errors (default), 2 per-call, 3 per-frame, `FFVFW_GAP_POLICY` = `hold` (default) | `error`, `FFVFW_QUALITY` = `auto` (default) | `exact` | `fast` (colour-conversion mode; auto = exact up to 640x480 pixels).
Level 2 also logs, at the end of each stream, the frame/drop/error counts and the average decode / colour-convert / pack time per frame.
Everything is also sent to `OutputDebugStringA` (DebugView). No dialogs are ever shown. Remove the registry key to switch logging off.

## Recoil
See `docs/RECOIL_TEST.md`. Important: launch `Recoil.exe` directly. The `rcrun.exe` launcher in the tested install injects `rcfix.dll`, which
replaces the game's VfW lookup with a built-in handler list that does not know `H264`.

## Licence
Our code: MIT (see `NOTICE.md`). FFmpeg 7.1.1 (LGPL-2.1-or-later, built without GPL components) statically linked; see `NOTICE.md` for what that requires.
