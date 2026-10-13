# Generates the H.264 test AVIs into testdata\ (usage: make-test-videos.ps1 -RecoilVideo <any video file>).  Requires ffmpeg (with libx264) and python on PATH.
param(
  [Parameter(Mandatory)][string]$RecoilVideo,   # any 320x240-ish source clip of at least $Seconds s (read only); the Recoil intro/mission videos work
  [string]$Out = (Join-Path $PSScriptRoot '..\testdata'),
  [int]$Seconds = 8
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$mux = Join-Path $PSScriptRoot 'vfwmux.py'
# BT.601-tagged SD, one slice per picture, AUD in every access unit, SPS/PPS repeated, single thread: deterministic.
$common = @('-pix_fmt','yuv420p','-colorspace','smpte170m','-color_primaries','smpte170m','-color_trc','smpte170m','-color_range','tv','-c:v','libx264','-preset','medium','-profile:v','main')
$intra  = '-x264-params','keyint=1:min-keyint=1:bframes=0:scenecut=0:slices=1:aud=1:repeat-headers=1:threads=1:bitrate=0:crf=18' 
$inter  = '-x264-params','keyint=30:min-keyint=30:bframes=0:scenecut=0:slices=1:aud=1:repeat-headers=1:threads=1:ref=1:crf=20'

function Encode($srcArgs, $x264, $w, $h, $fps, $name, $audioArgs) {
  $es = Join-Path $Out "$name.h264"
  & ffmpeg -v error -y @srcArgs -vf "scale=${w}:${h}:flags=bicubic,fps=$fps,format=yuv420p" -an @common @x264 -f h264 $es
  if ($LASTEXITCODE) { throw "ffmpeg encode failed" }
  $m = @($es, (Join-Path $Out "$name.avi"), '--fps', $fps, '--width', $w, '--height', $h) + $audioArgs
  & python -I $mux @m
  if ($LASTEXITCODE) { throw "mux failed" }
}

# 1. Recoil-like: 320x240 @30, real footage, PCM 22050 mono (Recoil feeds audio straight to DirectSound)
$pcm = Join-Path $Out 'recoil_audio.raw'
& ffmpeg -v error -y -t $Seconds -i $RecoilVideo -vn -f s16le -ar 22050 -ac 1 $pcm
Encode @('-t',$Seconds,'-i',$RecoilVideo) $intra 320 240 30 'recoil_intra_320x240_30' @('--pcm',$pcm,'--rate','22050','--channels','1')
# 2. The brief's reference profile: 640x480 @15, no audio, all-IDR
Encode @('-f','lavfi','-t',$Seconds,'-i','testsrc2=size=640x480:rate=15') $intra 640 480 15 'intra_640x480_15' @()
# 3. Same footage with P-frames (GOP 30), for the frame-skipping investigation
Encode @('-t',$Seconds,'-i',$RecoilVideo) $inter 320 240 30 'recoil_pframes_320x240_30' @('--pcm',$pcm,'--rate','22050','--channels','1')
# 4. unmarked variant of (2) for negative tests (packet length unknown to decoder)
& python -I $mux (Join-Path $Out 'intra_640x480_15.h264') (Join-Path $Out 'intra_640x480_15_nomarker.avi') --fps 15 --width 640 --height 480 --no-marker
Get-ChildItem $Out -Filter *.avi | Select-Object Name, Length

# 5. Reference decodes by stock FFmpeg (native h264 decoder, same swscale settings) as top-down BGR24.
foreach ($n in 'recoil_intra_320x240_30','intra_640x480_15','recoil_pframes_320x240_30') {
  & ffmpeg -v error -y -i (Join-Path $Out "$n.avi") -map 0:v -vf 'scale=flags=bicubic+accurate_rnd+full_chroma_int,format=bgr24' -f rawvideo (Join-Path $Out "$n.bgr24")
  if ($LASTEXITCODE) { throw "reference decode failed" }
}
