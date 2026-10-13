# Notice and licensing

## This project's code (src/, test/, tools/, install/, scripts/)
MIT License. Copyright (c) 2026 the ffvfw authors. Permission is granted, free of charge, to use, copy, modify, merge, publish, distribute,
sublicense and sell copies, subject to the inclusion of this notice. The software is provided "as is", without warranty of any kind.

## FFmpeg (statically linked into ffvfw.dll)
* Version n7.1.1, configured with `--disable-everything --enable-avcodec --enable-swscale --enable-decoder=h264 --enable-parser=h264`
  and **without** `--enable-gpl`, `--enable-version3`, `--enable-nonfree`. `configure` reports: *License: LGPL version 2.1 or later*.
  No external libraries are linked (no x264/x265; encoding in `tools/` uses a separate, user-supplied ffmpeg and is not distributed).
* LGPL-2.1 obligations when you distribute `ffvfw.dll`:
  1. include the LGPL-2.1 text (`refs/ffmpeg/COPYING.LGPLv2.1`) and FFmpeg's copyright notice (`refs/ffmpeg/LICENSE.md`);
  2. make the FFmpeg source you built from available (n7.1.1 tag of https://github.com/FFmpeg/FFmpeg, unmodified; `scripts/build-ffmpeg.sh`
     holds the exact configure line) or include a written offer;
  3. allow relinking with a modified FFmpeg: because the link is static, ship the project sources plus `Makefile`/`build.ps1`
     (or the object files `out/obj/*.o` and FFmpeg's static libraries) so a user can rebuild the DLL;
  4. do not forbid reverse engineering for debugging such modifications.
  FFmpeg's H.264 decoder may be subject to patents in some jurisdictions; this is not legal advice.
* `ff_aom_uninit_film_grain_params` is defined as an empty stub in `src/decoder.c` (see comment there): an upstream link-time quirk of
  H.264-only builds; it frees state that only the HEVC path allocates.

## References consulted (not copied)
* RecoilRebuild (Vorta) - read for the FMV call sequence only; no code taken.
* sKyissKy/iv50-ffmpeg-vfw - read for the message map; no code taken.
* ffdshow-tryouts - installation convention only.
