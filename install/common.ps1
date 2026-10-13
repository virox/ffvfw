function Test-IsAdmin {
  ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-Elevated($script, $bound) {
  $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"")
  foreach ($k in $bound.Keys) {
    $v = $bound[$k]
    if ($v -is [switch]) { if ($v) { $argList += "-$k" } }
    elseif ($v -is [array]) { $argList += "-$k"; $argList += ($v -join ',') }
    else { $argList += "-$k"; $argList += "`"$v`"" }
  }
  Write-Host 'Administrator rights are required - requesting elevation (UAC)...'
  $p = Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $argList -Verb RunAs -Wait -PassThru
  exit $p.ExitCode
}

# VfW (msvfw32) looks codecs up as "vidc.<FOURCC>" values in Drivers32. A 32-bit process on 64-bit
# Windows sees the Wow6432Node view, which is where a 32-bit game's lookup lands.
function Get-Drivers32Key {
  if ([Environment]::Is64BitOperatingSystem) { 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Drivers32' }
  else { 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32' }
}

# Records what we added so uninstall removes exactly that.
function Get-LedgerKey {
  if ([Environment]::Is64BitOperatingSystem) { 'HKLM:\SOFTWARE\WOW6432Node\FFmpegVfW' } else { 'HKLM:\SOFTWARE\FFmpegVfW' }
}

function Assert-Pe32($path) {
  $b = [IO.File]::ReadAllBytes($path)
  $pe = [BitConverter]::ToInt32($b, 0x3C)
  $machine = [BitConverter]::ToUInt16($b, $pe + 4)
  if ($machine -ne 0x14C) {
    throw ('{0} is not a 32-bit x86 DLL (machine 0x{1:X}); VfW codecs for 32-bit games must be x86.' -f $path, $machine)
  }
}
