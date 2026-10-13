# Builds ffvfw.dll + vfwtest.exe (32-bit) using MSYS2 MINGW32. Pass -Ffmpeg to (re)build the static FFmpeg first.
param([switch]$Ffmpeg, [string]$Msys = 'C:\msys64')
$root = $PSScriptRoot
$env:MSYSTEM = 'MINGW32'
$bash = Join-Path $Msys 'usr\bin\bash.exe'
if ($Ffmpeg) { & $bash -l "$root/scripts/build-ffmpeg.sh"; if ($LASTEXITCODE) { exit $LASTEXITCODE } }
$posix = ($root -replace '\','/') -replace '^([A-Za-z]):','/$1'
& $bash -lc "cd '$posix' && make -j4"
exit $LASTEXITCODE
