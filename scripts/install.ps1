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

$releaseRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$packagedDll = Join-Path $releaseRoot 'ZRinputTSF.dll'
if (Test-Path $packagedDll) {
  $source = $packagedDll
  $editorSource = Join-Path $releaseRoot 'zrinput_theme_editor.exe'
  $profileToolSource = Join-Path $releaseRoot 'zrinput_profile_tool.exe'
  $assetRoot = $releaseRoot
} else {
  $source = (Resolve-Path (Join-Path $BuildDirectory 'Release\ZRinputTSF.dll')).Path
  $editorSource = (Resolve-Path (Join-Path $BuildDirectory 'Release\zrinput_theme_editor.exe')).Path
  $profileToolSource = (Resolve-Path (Join-Path $BuildDirectory 'Release\zrinput_profile_tool.exe')).Path
  $assetRoot = $releaseRoot
}
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
Copy-Item -LiteralPath $editorSource `
  -Destination (Join-Path $installDirectory 'ZRinputThemeEditor.exe') -Force
Copy-Item -LiteralPath $profileToolSource `
  -Destination (Join-Path $installDirectory 'ZRinputProfileTool.exe') -Force
$lexiconSource = Join-Path $assetRoot 'data\default_lexicon.tsv'
$dataDirectory = Join-Path $installDirectory 'data'
New-Item -ItemType Directory -Path $dataDirectory -Force | Out-Null
Copy-Item -LiteralPath $lexiconSource `
  -Destination (Join-Path $dataDirectory 'default_lexicon.tsv') -Force
$themeSource = Join-Path $assetRoot 'themes\microsoft-dark.ini'
$themeDirectory = Join-Path $installDirectory 'themes'
New-Item -ItemType Directory -Path $themeDirectory -Force | Out-Null
Copy-Item -LiteralPath $themeSource `
  -Destination (Join-Path $themeDirectory 'microsoft-dark.ini') -Force
$register = Start-Process "$env:SystemRoot\System32\regsvr32.exe" `
  -ArgumentList @('/s', ('"{0}"' -f $destination)) -Wait -PassThru
if ($register.ExitCode -ne 0) {
  throw "Unable to register ZRinput (exit $($register.ExitCode))."
}

$tipKey = 'HKLM:\Software\Microsoft\CTF\TIP\{BFD6C220-320C-46F4-94D0-78C4779AE70C}'
if (-not (Test-Path $tipKey)) {
  throw 'TSF registration returned success but the TIP registry entry is missing.'
}
$comKey = 'HKCU:\Software\Classes\CLSID\{BFD6C220-320C-46F4-94D0-78C4779AE70C}\InprocServer32'
if (-not (Test-Path $comKey)) {
  throw 'TSF registration returned success but the COM registration is missing.'
}
$profileKey = Join-Path $tipKey 'LanguageProfile\0x00000804\{97313B73-4F48-48E4-BC7E-10DF2538892C}'
if (-not (Test-Path $profileKey)) {
  throw 'TSF registration returned success but the Simplified Chinese profile is missing.'
}
Write-Host "ZRinput installed to $destination"
