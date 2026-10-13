<#
.SYNOPSIS  Converts ANY video file into a Recoil-playable H.264 AVI (for use with ffvfw.dll).
.DESCRIPTION
  Produces what the game's FMV player accepts: integer fps, all-IDR H.264 in the frame-header
  framing ffvfw needs, uncompressed 16-bit PCM audio, AVI laid out like the retail files.
  Needs ffmpeg (with libx264), ffprobe and python on PATH. The source file is only read.

  Resolution and frame rate adapt to what you pass:
    -Resolution 1920x1080      target frame size (also -Width / -Height); default 320x240 like the retail videos
    -Resolution source         keep the source size (rounded down to a multiple of 4 wide / 2 high)
    -Fps 0 (default 30)        keep the source frame rate, rounded to an integer (Recoil uses an integer dwRate/dwScale)
  -Fit decides what happens when the source aspect ratio differs from the target: pad (black bars), stretch, crop.
.EXAMPLE
  .\make-recoil-video.ps1 -Source C:\clips\trailer.mp4 -Dest "C:\Games\Recoil copy\video\intro_01.avi"
.EXAMPLE
  .\make-recoil-video.ps1 -Source in.mkv -Dest out.avi -Resolution 1280x960 -Fps 24 -Crf 20
.EXAMPLE
  .\make-recoil-video.ps1 -Source in.mp4 -Dest out.avi -Resolution source -Fps 0 -AudioRate 44100 -AudioChannels 2
#>
param(
  [Parameter(Mandatory)][string]$Source,
  [Parameter(Mandatory)][string]$Dest,
  [string]$Resolution,                         # "WxH" or "source"; overrides -Width/-Height
  [int]$Width = 320, [int]$Height = 240,       # retail files are 320x240
  [int]$Fps = 30,                              # integer fps; 0 = same as source (rounded). Never 29.97 - Recoil truncates dwRate/dwScale
  [ValidateSet('pad', 'stretch', 'crop')][string]$Fit = 'pad',
  [int]$AudioRate = 22050, [int]$AudioChannels = 1,   # retail intro/missions: 22050 Hz mono (zipperlogo: 44100 stereo)
  [switch]$NoAudio,
  [double]$Seconds = 0,                        # 0 = whole file
  [ValidateRange(0, 51)][int]$Crf = 18,        # x264 quality: lower = better/larger (all-IDR files are big at high resolution)
  [switch]$PFrames                             # GOP 30 with P-frames: much smaller, but freezes on frames Recoil skips
)
$ErrorActionPreference = 'Stop'
foreach ($tool in 'ffmpeg', 'ffprobe', 'python') { if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found on PATH" } }
if (-not (Test-Path $Source)) { throw "Source not found: $Source" }
if ($AudioChannels -notin 1, 2) { throw 'AudioChannels must be 1 or 2' }

# ---- probe the source -----------------------------------------------------------------------------
$probe = & ffprobe -v error -select_streams v:0 -show_entries stream=width,height,avg_frame_rate,r_frame_rate -of csv=p=0 $Source
if (-not $probe) { throw "No video stream found in $Source" }
$sw, $sh, $afr, $rfr = ($probe | Select-Object -First 1).Split(',')
function Get-Rate($s) { $p = $s.Split('/'); if ($p.Count -eq 2 -and [double]$p[1] -ne 0) { [double]$p[0] / [double]$p[1] } else { 0 } }
$srcFps = Get-Rate $afr; if ($srcFps -le 0) { $srcFps = Get-Rate $rfr }

# ---- resolve resolution and frame rate ------------------------------------------------------------
if ($Resolution) {
  if ($Resolution -eq 'source') { $Width = [int]$sw; $Height = [int]$sh }
  elseif ($Resolution -match '^(\d+)[xX](\d+)$') { $Width = [int]$Matches[1]; $Height = [int]$Matches[2] }
  else { throw "-Resolution must look like 1920x1080 or 'source'" }
}
if ($Resolution -eq 'source') { $Width -= $Width % 4; $Height -= $Height % 2 }   # make the source size legal
if ($Width % 4) { throw "Width $Width must be a multiple of 4 (Recoil's row-size calculation)." }
if ($Width % 2 -or $Height % 2) { throw 'Width and height must be even (4:2:0 chroma).' }
if ($Width -lt 16 -or $Height -lt 16 -or $Width -gt 8192 -or $Height -gt 8192) { throw 'Resolution out of range (16..8192).' }
if ($Fps -eq 0) { $Fps = [int][Math]::Round($srcFps); if ($Fps -lt 1) { throw 'Could not determine the source frame rate; pass -Fps.' } }
if ($Fps -lt 1 -or $Fps -gt 120) { throw '-Fps must be an integer from 1 to 120' }
Write-Host ("source: {0}x{1} @ {2:N3} fps  ->  target: {3}x{4} @ {5} fps, fit={6}, crf={7}{8}" -f $sw, $sh, $srcFps, $Width, $Height, $Fps, $Fit, $Crf, $(if ($PFrames) { ', P-frames' } else { ', all-IDR' }))

$work = Join-Path $env:TEMP ('ffvfw-' + [guid]::NewGuid().ToString('N').Substring(0, 8)); New-Item -ItemType Directory $work | Out-Null
try {
  $t = if ($Seconds) { @('-t', $Seconds) } else { @() }
  # explicit BT.601/limited-range output: the stream is tagged SD (smpte170m) below, so HD/BT.709 sources must be converted, not just relabelled
  $vf = switch ($Fit) {
    'pad'     { "scale=${Width}:${Height}:force_original_aspect_ratio=decrease:flags=bicubic:out_color_matrix=bt601:out_range=tv,pad=${Width}:${Height}:(ow-iw)/2:(oh-ih)/2:black" }
    'stretch' { "scale=${Width}:${Height}:flags=bicubic:out_color_matrix=bt601:out_range=tv" }
    'crop'    { "scale=${Width}:${Height}:force_original_aspect_ratio=increase:flags=bicubic:out_color_matrix=bt601:out_range=tv,crop=${Width}:${Height}" }
  }
  $vf += ",fps=$Fps,format=yuv420p"
  $x = if ($PFrames) { "keyint=30:min-keyint=30:bframes=0:scenecut=0:slices=1:aud=1:repeat-headers=1:threads=1:ref=1:crf=$Crf" }
       else          { "keyint=1:min-keyint=1:bframes=0:scenecut=0:slices=1:aud=1:repeat-headers=1:threads=1:crf=$Crf" }
  $es = Join-Path $work 'v.h264'
  & ffmpeg -v error -y @t -i $Source -an -vf $vf -c:v libx264 -preset medium -profile:v main `
      -colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m -color_range tv -x264-params $x -f h264 $es
  if ($LASTEXITCODE) { throw 'video encode failed' }
  $mux = @($es, $Dest, '--fps', $Fps, '--width', $Width, '--height', $Height)
  $hasAudio = -not $NoAudio -and ((& ffprobe -v error -select_streams a:0 -show_entries stream=codec_type -of csv=p=0 $Source) -match 'audio')
  if ($hasAudio) {
    $pcm = Join-Path $work 'a.raw'
    & ffmpeg -v error -y @t -i $Source -vn -f s16le -ar $AudioRate -ac $AudioChannels $pcm   # PCM only: Recoil gives it straight to DirectSound
    if ($LASTEXITCODE) { throw 'audio extract failed' }
    $mux += @('--pcm', $pcm, '--rate', $AudioRate, '--channels', $AudioChannels)
  } else { Write-Host 'no audio stream written' }
  & python -I (Join-Path $PSScriptRoot 'vfwmux.py') @mux
  if ($LASTEXITCODE) { throw 'mux failed' }
} finally { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }
