#!/usr/bin/env python3
"""Mux an Annex-B H.264 elementary stream (+ optional raw PCM) into a VfW-friendly AVI.

Every video sample is
    [frame-header NAL, type 30]  VFWH, ver, flags(bit0=IDR), seq, body_len
    [Annex-B access unit]        AUD, (SPS, PPS), slice(s)
so that a VfW decoder that is NOT told the packet length (Recoil's ICDecompress call)
can find the exact end of the access unit.  Stock decoders (ffmpeg, ffplay) ignore
NAL type 30, so the file stays playable everywhere.

The elementary stream must contain access-unit delimiters (use
  -bsf:v h264_metadata=aud=insert  or  x264 --aud).
usage: vfwmux.py in.h264 out.avi --fps 30 --width 320 --height 240 [--pcm in.raw --rate 22050 --channels 1]
(normally driven by tools/make-recoil-video.ps1). With --pcm the audio is trimmed/padded to the video length and written in the retail
Recoil layout: 1-second chunks, ~1 s ahead of the video, audio InitialFrames 23 (see docs/RECOIL_TEST.md, "Audio layout").
"""
import argparse, struct, sys

def split_nals(data):
    """yield (nal_type, start, end) with start at the first byte of the 3/4 byte start code."""
    i, n, starts = 0, len(data), []
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        s = j - 1 if j > 0 and data[j - 1] == 0 else j
        starts.append((s, j + 3))
        i = j + 3
    for k, (s, p) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else n
        yield data[p] & 0x1F, s, end

def access_units(data):
    cur, ranges = None, []
    for t, s, e in split_nals(data):
        if t == 9:                      # AUD begins a new access unit
            if cur is not None:
                ranges.append((cur, s))
            cur = s
        elif cur is None:
            raise SystemExit("stream does not start with an access unit delimiter (add aud=insert)")
    ranges.append((cur, len(data)))
    out = []
    for s, e in ranges:
        au = data[s:e].rstrip(b"\x00") if False else data[s:e]
        types = {t for t, _, _ in split_nals(au)}
        out.append((au, 5 in types))
    return out

def escape(b):
    out, z = bytearray(), 0
    for x in b:
        if z >= 2 and x <= 3:
            out.append(3); z = 0
        out.append(x)
        z = z + 1 if x == 0 else 0
    return bytes(out)

def marker(seq, key, body_len):
    pay = b"VFWH" + bytes([1, 1 if key else 0]) + struct.pack("<II", seq, body_len) + b"\xff"
    return b"\x00\x00\x00\x01\x1e" + escape(pay)

def chunk(fcc, payload):
    return fcc + struct.pack("<I", len(payload)) + payload + (b"\x00" if len(payload) & 1 else b"")

def lst(kind, payload):
    return b"LIST" + struct.pack("<I", len(payload) + 4) + kind + payload

def riff_chunks(d, start, end):
    """flat iterator of (fourcc, payload_offset, size) over a RIFF region, descending into LISTs."""
    p = start
    while p + 8 <= end:
        cid, sz = d[p:p + 4], struct.unpack("<I", d[p + 4:p + 8])[0]
        if cid == b"LIST":
            yield (b"LIST", d[p + 8:p + 12], p + 12, sz - 4)
            yield from riff_chunks(d, p + 12, min(p + 8 + sz, end))
        else:
            yield (cid, None, p + 8, sz)
        p += 8 + sz + (sz & 1)

def mux_like(a, samples, sugg, n):
    d = open(a.like, "rb").read()
    strh, strf, avih, movi = [], [], None, None
    for cid, kind, off, sz in riff_chunks(d, 12, len(d)):
        if cid == b"avih": avih = bytearray(d[off:off + sz])
        elif cid == b"strh": strh.append(bytearray(d[off:off + sz]))
        elif cid == b"strf": strf.append(d[off:off + sz])
        elif cid == b"LIST" and kind == b"movi": movi = (off, sz)
    assert movi and len(strh) == 2 and strh[1][:4] == b"auds", "unexpected layout in --like file"
    out, idx, pos, v = bytearray(b"movi"), [], 4, 0
    for cid, kind, off, sz in riff_chunks(d, movi[0], movi[0] + movi[1]):
        if cid == b"LIST" or cid == b"JUNK": continue
        if cid.endswith(b"dc"):
            if v >= n: continue
            payload, key = samples[v]; v += 1
            c = chunk(b"00dc", payload); idx.append((b"00dc", 0x10 if key else 0, pos, len(payload)))
        elif cid.endswith(b"wb"):
            payload = d[off:off + sz]; c = chunk(b"01wb", payload); idx.append((b"01wb", 0x10, pos, sz))
        else: continue
        out += c; pos += len(c)
    assert v == n, "like-file has fewer video frames (%d) than the new stream (%d)" % (v, n)
    sv = bytearray(b"vids" + a.fourcc.encode() + struct.pack("<IHHIIIIIIIIhhhh", 0, 0, 0, 0, 1, a.fps, 0, n, sugg, 10000, 0, 0, 0, a.width, a.height))
    bih = struct.pack("<IiiHHIIiiII", 40, a.width, a.height, 1, 24, struct.unpack("<I", a.fourcc.encode())[0], sugg, 0, 0, 0, 0)
    struct.pack_into("<I", avih, 28, max(struct.unpack_from("<I", avih, 28)[0], sugg))
    hdrl = lst(b"hdrl", chunk(b"avih", bytes(avih)) + lst(b"strl", chunk(b"strh", bytes(sv)) + chunk(b"strf", bih))
               + lst(b"strl", chunk(b"strh", bytes(strh[1])) + chunk(b"strf", strf[1])))
    idx1 = b"".join(f + struct.pack("<III", fl, off, sz) for f, fl, off, sz in idx)
    body = b"AVI " + hdrl + lst(b"movi", bytes(out[4:])) + chunk(b"idx1", idx1)
    open(a.avi, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
    print(f"{a.avi}: {n} frames in retail layout of {a.like}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("h264"); ap.add_argument("avi")
    ap.add_argument("--fps", type=int, required=True)
    ap.add_argument("--width", type=int, required=True); ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--fourcc", default="H264")
    ap.add_argument("--pcm"); ap.add_argument("--rate", type=int, default=22050); ap.add_argument("--channels", type=int, default=1)
    ap.add_argument("--like", help="retail AVI whose interleave, audio chunks, audio headers and avih are copied verbatim (only the video is replaced)")
    ap.add_argument("--no-marker", action="store_true", help="plain Annex-B samples (for negative tests)")
    a = ap.parse_args()

    aus = access_units(open(a.h264, "rb").read())
    samples = []
    for seq, (au, key) in enumerate(aus):
        samples.append((marker(seq, key, len(au)) + au if not a.no_marker else au, key))
    maxv = max(len(s) for s, _ in samples)
    sugg = (maxv + 1023) & ~1023
    n = len(samples)

    pcm = open(a.pcm, "rb").read() if a.pcm else None
    ba = 2 * a.channels
    if pcm is not None:                          # audio length == video length (trim, or pad with silence)
        want = n * a.rate // a.fps * ba
        pcm = pcm[:want] + b"\x00" * max(0, want - len(pcm))

    if a.like:
        return mux_like(a, samples, sugg, n)
    # ---- movi ----
    # Layout copied from the retail Recoil files (an earlier "one small audio chunk per frame" layout
    # played the audio garbled in Recoil): audio in 1-second chunks, the first one BEFORE frame 0 and
    # one more every `fps` frames, i.e. the audio runs ~1 s ahead of the video.
    movi, idx, pos = bytearray(b"movi"), [], 4
    asec = a.rate * ba if pcm is not None else 0
    apos = 0
    def put_audio(cnt):
        nonlocal pos, apos
        p = pcm[apos:apos + cnt]
        if not p: return
        apos += len(p); c = chunk(b"01wb", p)
        idx.append((b"01wb", 0x10, pos, len(p))); movi.extend(c); pos += len(c)
    for i, (s_, key) in enumerate(samples):
        if pcm is not None and i % a.fps == 0:
            put_audio(asec)
        c = chunk(b"00dc", s_)
        idx.append((b"00dc", 0x10 if key else 0, pos, len(s_))); movi.extend(c); pos += len(c)
    if pcm is not None:
        put_audio(len(pcm))               # whatever is left
    idx1 = b"".join(f + struct.pack("<III", fl, off, sz) for f, fl, off, sz in idx)

    # ---- headers ----
    # Recoil (streaming mode) sizes its two DirectSound half-buffers from the audio stream's
    # dwSuggestedBufferSize and divides by dwSampleSize: it must be non-zero (retail files: 1 s of audio + 8).
    strh_v = b"vids" + a.fourcc.encode() + struct.pack("<IHHIIIIIIIIhhhh", 0, 0, 0, 0, 1, a.fps, 0, n, sugg, 0xFFFFFFFF, 0, 0, 0, a.width, a.height)
    bih = struct.pack("<IiiHHIIiiII", 40, a.width, a.height, 1, 24, struct.unpack("<I", a.fourcc.encode())[0], sugg, 0, 0, 0, 0)
    strl = lst(b"strl", chunk(b"strh", strh_v) + chunk(b"strf", bih))
    nstreams = 1
    if pcm is not None:
        nsamp = len(pcm) // ba
        strh_a = b"auds" + b"\x00\x00\x00\x00" + struct.pack("<IHHIIIIIIIIhhhh", 0, 0, 0, 23, ba, a.rate * ba, 0, nsamp, a.rate * ba + 8, 10000, ba, 0, 0, 0, 0)
        wfx = struct.pack("<HHIIHH", 1, a.channels, a.rate, a.rate * ba, ba, 16)  # 16-byte PCMWAVEFORMAT, like the retail files
        strl += lst(b"strl", chunk(b"strh", strh_a) + chunk(b"strf", wfx))
        nstreams = 2
    avih = struct.pack("<IIIIIIIIIIIIII", 1000000 // a.fps, (a.rate * ba if pcm is not None else 0) + sum(len(x) for x, _ in samples) * a.fps // n, 0, 0x10, n, 23 if pcm is not None else 0, nstreams, max(sugg, asec + 8), a.width, a.height, 0, 0, 0, 0)
    hdrl = lst(b"hdrl", chunk(b"avih", avih) + strl)
    body = b"AVI " + hdrl + lst(b"movi", bytes(movi[4:])) + chunk(b"idx1", idx1)
    open(a.avi, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
    print(f"{a.avi}: {n} frames, max sample {maxv} B, suggested buffer {sugg} B, keyframes {sum(k for _, k in samples)}")

if __name__ == "__main__":
    main()
