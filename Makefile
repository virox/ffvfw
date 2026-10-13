# Build with MSYS2 MINGW32 (i686) after scripts/build-ffmpeg.sh.
#   MSYSTEM=MINGW32 make            -> out/ffvfw.dll, out/vfwtest.exe
CC      ?= gcc
FFINC   := build/ffmpeg-install/include
FFLIB   := build/ffmpeg-install/lib
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -I$(FFINC) -Isrc
LDSTAT  := -static -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fixup
SRCS    := src/ffvfw.c src/decoder.c src/framing.c src/pixout.c src/log.c
OBJS    := $(SRCS:src/%.c=out/obj/%.o)
LIBS    := -L$(FFLIB) -lavcodec -lswscale -lavutil -lm -latomic -lole32 -luser32 -lbcrypt -lwinmm

all: out/ffvfw.dll out/vfwtest.exe

out/obj/%.o: src/%.c | out/obj
	$(CC) $(CFLAGS) -c $< -o $@
out/obj:
	mkdir -p out/obj
out/ffvfw.dll: $(OBJS) src/ffvfw.def
	$(CC) -shared -o $@ $(OBJS) src/ffvfw.def $(LDSTAT) $(LIBS) -s
out/vfwtest.exe: test/vfwtest.c | out/obj
	$(CC) -O2 -Wall -Wextra -Wno-unused-parameter -o $@ $< -lvfw32 -lavifil32 -lmsvfw32 -lwinmm -static-libgcc -s
clean:
	rm -rf out
.PHONY: all clean
