<#
.SYNOPSIS  Registers ffvfw.dll as a 32-bit VfW video decompressor (vidc.* entries in the 32-bit Drivers32 key).
.DESCRIPTION
  VfW codecs are NOT COM objects; msvfw32 finds them through "vidc.<FOURCC>" values under
  HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Drivers32 (64-bit Windows).
  Nothing else is touched:
    * existing registrations (Indeo vidc.iv50, a third-party vidc.H264, ...) are never overwritten - a
      FourCC that is already claimed by something else is skipped and reported;
    * every value we add is recorded in HKLM\SOFTWARE\WOW6432Node\FFmpegVfW so uninstall.ps1 removes
      exactly those.
  Needs administrator rights; the script re-launches itself elevated (UAC prompt) when necessary.
.PARAMETER Dll         32-bit ffvfw.dll (default: ffvfw.dll next to this script, else ..\out\ffvfw.dll)
.PARAMETER FourCC      FourCCs to register (default H264, AVC1)
.PARAMETER InstallDir  Where the DLL is copied (default: the 32-bit system directory, like Windows' own codecs).
                 The path MUST NOT contain spaces: msvfw32 parses a Drivers32 value as "driver [parameters]" and
                 cuts it at the first space, so e.g. "C:\Program Files (x86)\..." silently fails to load.
.PARAMETER WhatIf      Show what would be done without changing anything.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
  [string]$Dll,
  [string[]]$FourCC = @('H264', 'AVC1'),
  [string]$InstallDir = $(if ([Environment]::Is64BitOperatingSystem) { Join-Path $env:windir 'SysWOW64' } else { Join-Path $env:windir 'System32' })
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

if (-not $Dll) {
  $Dll = @((Join-Path $PSScriptRoot 'ffvfw.dll'), (Join-Path $PSScriptRoot '..\out\ffvfw.dll')) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Dll -or -not (Test-Path $Dll)) { throw 'ffvfw.dll not found; build it first or pass -Dll.' }
$Dll = (Resolve-Path $Dll).Path
if (-not (Test-IsAdmin) -and -not $WhatIfPreference) { Invoke-Elevated $PSCommandPath $PSBoundParameters; exit $LASTEXITCODE }

Assert-Pe32 $Dll
if ($InstallDir -match '\s') { throw "InstallDir '$InstallDir' contains a space; VfW cannot load drivers from such a path (see -?)." }
$ownsDir = -not ($InstallDir -ieq (Join-Path $env:windir 'SysWOW64') -or $InstallDir -ieq (Join-Path $env:windir 'System32'))
$target = Join-Path $InstallDir 'ffvfw.dll'
$drv = Get-Drivers32Key
Write-Host "Drivers32 key : $drv"
Write-Host "Install to    : $target"

# ---- conflict detection, before anything is changed -------------------------------------------------
$plan = foreach ($f in $FourCC) {
  $name = 'vidc.' + $f
  $cur = (Get-ItemProperty -Path $drv -Name $name -ErrorAction SilentlyContinue).$name
  if ($null -eq $cur) { [pscustomobject]@{ Name = $name; Action = 'add'; Current = $null } }
  elseif ($cur -ieq $target) { [pscustomobject]@{ Name = $name; Action = 'same'; Current = $cur } }
  else { [pscustomobject]@{ Name = $name; Action = 'CONFLICT'; Current = $cur } }
}
$plan | ForEach-Object { Write-Host ('  {0,-12} {1,-9} {2}' -f $_.Name, $_.Action, $_.Current) }
if ($plan | Where-Object Action -eq 'CONFLICT') {
  Write-Warning 'Some FourCCs are already registered to another driver and are left untouched (never overwritten).'
}
$todo = @($plan | Where-Object { $_.Action -ne 'CONFLICT' })
if ($todo.Count -eq 0) { throw 'Nothing to register: every requested FourCC is taken by another driver.' }

# ---- install ----------------------------------------------------------------------------------------
if ($PSCmdlet.ShouldProcess($target, 'copy DLL and register VfW codec')) {
  New-Item -ItemType Directory -Force $InstallDir | Out-Null
  Copy-Item $Dll $target -Force
  $added = @()
  foreach ($p in $todo) {
    if ($p.Action -eq 'add') { New-ItemProperty -Path $drv -Name $p.Name -Value $target -PropertyType String | Out-Null; $added += $p.Name }
  }
  $ledger = Get-LedgerKey
  New-Item -Path $ledger -Force | Out-Null
  Set-ItemProperty -Path $ledger -Name Dll -Value $target
  Set-ItemProperty -Path $ledger -Name InstallDir -Value $InstallDir
  Set-ItemProperty -Path $ledger -Name OwnsDir -Value ([int]$ownsDir) -Type DWord
  New-ItemProperty -Path $ledger -Name Values -Value ([string[]]$added) -PropertyType MultiString -Force | Out-Null
  Write-Host "Registered: $($added -join ', ')"

  # ---- verification through the real msvfw32 stack (32-bit probe) ------------------------------------
  $probe = @((Join-Path $PSScriptRoot 'vfwtest.exe'), (Join-Path $PSScriptRoot '..\out\vfwtest.exe')) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($probe) {
    $bad = $false
    foreach ($f in ($todo.Name -replace '^vidc\.')) {
      $out = & $probe probe $f 2>&1
      Write-Host "verify $f : $out"
      if ($LASTEXITCODE) { Write-Warning "ICOpen('$f') did not reach ffvfw.dll"; $bad = $true }
    }
    if ($bad) { exit 3 }
  } else {
    Write-Warning 'vfwtest.exe not found - skipped discovery verification (run: vfwtest probe H264).'
  }
}
