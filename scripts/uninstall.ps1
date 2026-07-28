[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new(
  [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  $process = Start-Process powershell.exe -Verb RunAs `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath)) `
    -Wait -PassThru
  exit $process.ExitCode
}

$installDirectory = Join-Path $env:ProgramFiles 'ZRinput'
$dll = Join-Path $installDirectory 'ZRinputTSF.dll'
if (Test-Path $dll) {
  $process = Start-Process "$env:SystemRoot\System32\regsvr32.exe" `
    -ArgumentList @('/s', '/u', ('"{0}"' -f $dll)) -Wait -PassThru
  if ($process.ExitCode -ne 0) {
    throw "Unable to unregister ZRinput (exit $($process.ExitCode))."
  }
  Remove-Item -LiteralPath $dll -Force
}
if (Test-Path $installDirectory) {
  Remove-Item -LiteralPath $installDirectory -Recurse -Force
}
Write-Host 'ZRinput uninstalled. Personal memory was preserved.'
