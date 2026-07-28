[CmdletBinding()]
param(
  [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build')
)

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new(
  [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
    ('"{0}"' -f $PSCommandPath), '-BuildDirectory', ('"{0}"' -f $BuildDirectory))
  $process = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
  exit $process.ExitCode
}

$source = (Resolve-Path (Join-Path $BuildDirectory 'Release\ZRinputTSF.dll')).Path
$installDirectory = Join-Path $env:ProgramFiles 'ZRinput'
New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
$destination = Join-Path $installDirectory 'ZRinputTSF.dll'

$existing = Get-Item $destination -ErrorAction SilentlyContinue
if ($existing) {
  $unregister = Start-Process "$env:SystemRoot\System32\regsvr32.exe" `
    -ArgumentList @('/s', '/u', ('"{0}"' -f $destination)) -Wait -PassThru
  if ($unregister.ExitCode -ne 0) {
    throw "Unable to unregister existing ZRinput (exit $($unregister.ExitCode))."
  }
}

Copy-Item -LiteralPath $source -Destination $destination -Force
$register = Start-Process "$env:SystemRoot\System32\regsvr32.exe" `
  -ArgumentList @('/s', ('"{0}"' -f $destination)) -Wait -PassThru
if ($register.ExitCode -ne 0) {
  throw "Unable to register ZRinput (exit $($register.ExitCode))."
}

$tipKey = 'HKLM:\Software\Microsoft\CTF\TIP\{BFD6C220-320C-46F4-94D0-78C4779AE70C}'
if (-not (Test-Path $tipKey)) {
  throw 'TSF registration returned success but the TIP registry entry is missing.'
}
Write-Host "ZRinput installed to $destination"
