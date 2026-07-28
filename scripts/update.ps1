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
  $process = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments `
    -Wait -PassThru
  exit $process.ExitCode
}

$source = (Resolve-Path (Join-Path $BuildDirectory 'Release\ZRinputTSF.dll')).Path
$installDirectory = Join-Path $env:ProgramFiles 'ZRinput'
New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
$hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.Substring(0, 12)
$destination = Join-Path $installDirectory "ZRinputTSF-$hash.dll"
$comKey = 'HKCU:\Software\Classes\CLSID\{BFD6C220-320C-46F4-94D0-78C4779AE70C}\InprocServer32'
$registered = (Get-ItemProperty $comKey -ErrorAction SilentlyContinue).'(default)'

if ($registered -and (Test-Path $registered)) {
  $process = Start-Process "$env:SystemRoot\System32\regsvr32.exe" `
    -ArgumentList @('/s', '/u', ('"{0}"' -f $registered)) -Wait -PassThru
  if ($process.ExitCode -ne 0) {
    throw "Unable to unregister existing ZRinput (exit $($process.ExitCode))."
  }
}

Copy-Item -LiteralPath $source -Destination $destination -Force
$process = Start-Process "$env:SystemRoot\System32\regsvr32.exe" `
  -ArgumentList @('/s', ('"{0}"' -f $destination)) -Wait -PassThru
if ($process.ExitCode -ne 0) {
  throw "Unable to register updated ZRinput (exit $($process.ExitCode))."
}
$actual = (Get-ItemProperty $comKey).'(default)'
if ($actual -ne $destination) {
  throw "COM registration points to an unexpected DLL: $actual"
}
Write-Host "ZRinput updated to $destination"
