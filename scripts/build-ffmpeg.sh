#!/bin/bash
# Builds a minimal static 32-bit LGPL FFmpeg (H.264 decoder + swscale) with MSYS2 MINGW32.
# Run via: C:\msys64\usr\bin\env.exe MSYSTEM=MINGW32 C:\msys64\usr\bin\bash.exe -l scripts/build-ffmpeg.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/refs/ffmpeg"; OUT="$ROOT/build/ffmpeg"; PREFIX="$ROOT/build/ffmpeg-install"
export TMPDIR=/tmp
mkdir -p "$OUT" && cd "$OUT"
"$SRC/configure" --prefix="$PREFIX" --arch=x86 --target-os=mingw32 \
  --enable-static --disable-shared --disable-programs --disable-doc \
  --disable-everything --disable-autodetect --disable-network --disable-debug \
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample \
  --disable-pthreads --disable-w32threads --disable-iconv \
  --enable-avcodec --enable-swscale --enable-decoder=h264 --enable-parser=h264 \
  --extra-cflags="-O2"
make -j8
make install
