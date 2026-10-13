<#
.SYNOPSIS  Removes exactly what install.ps1 added (and nothing else). Other codecs are never touched.
#>
[CmdletBinding(SupportsShouldProcess)]
param([switch]$KeepFiles)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
if (-not (Test-IsAdmin) -and -not $WhatIfPreference) { Invoke-Elevated $PSCommandPath $PSBoundParameters; exit $LASTEXITCODE }

$ledger = Get-LedgerKey
if (-not (Test-Path $ledger)) { Write-Host "Nothing to do: ffvfw is not installed (no ledger at $ledger)."; exit 0 }
$props = Get-ItemProperty $ledger
$drv = Get-Drivers32Key
foreach ($name in @($props.Values)) {
  $cur = (Get-ItemProperty -Path $drv -Name $name -ErrorAction SilentlyContinue).$name
  if ($null -eq $cur) { Write-Host "  $name already absent" }
  elseif ($cur -ine $props.Dll) { Write-Warning "  $name now points to '$cur' (not ours) - left untouched" }
  elseif ($PSCmdlet.ShouldProcess($name, 'remove registration')) { Remove-ItemProperty -Path $drv -Name $name; Write-Host "  removed $name" }
}
if ($PSCmdlet.ShouldProcess($ledger, 'remove ledger and installed files')) {
  Remove-Item $ledger -Recurse -Force
  if (-not $KeepFiles) {
    if (Test-Path $props.Dll) { Remove-Item $props.Dll -Force -ErrorAction Continue }
    if (Test-Path $props.Dll) { Write-Warning "Could not delete $($props.Dll) (still loaded by a running program?)." }
    if (($props.OwnsDir -eq 1 -or $null -eq $props.OwnsDir) -and (Test-Path $props.InstallDir) -and -not (Get-ChildItem $props.InstallDir -Force)) { Remove-Item $props.InstallDir -Force }
  }
}
Write-Host 'Done. Existing codecs (vidc.iv50, ...) were not touched.'
