# Creates dist\ffvfw-<date>\ : the redistributable DLL, installer, test tool, docs and licence files.
$root = Split-Path $PSScriptRoot -Parent
$d = Join-Path $root ('dist\ffvfw-' + (Get-Date -Format yyyyMMdd)); Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $d | Out-Null
Copy-Item "$root\out\ffvfw.dll", "$root\out\vfwtest.exe", "$root\install\*.ps1", "$root\README.md", "$root\NOTICE.md" $d
Copy-Item "$root\docs" $d -Recurse
Copy-Item "$root\refs\ffmpeg\COPYING.LGPLv2.1", "$root\refs\ffmpeg\LICENSE.md" $d
Copy-Item "$root\src", "$root\test", "$root\tools", "$root\scripts", "$root\Makefile", "$root\build.ps1" $d -Recurse   # LGPL: sources to rebuild/relink
Write-Host "package: $d"
