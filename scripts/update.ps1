[CmdletBinding()]
param(
  [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build')
)

$ErrorActionPreference = 'Stop'
$releaseRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$packagedDll = Join-Path $releaseRoot 'ZRinputTSF.dll'
if (Test-Path $packagedDll) {
  $source = $packagedDll
  $editorSource = Join-Path $releaseRoot 'zrinput_theme_editor.exe'
  $profileToolSource = Join-Path $releaseRoot 'zrinput_profile_tool.exe'
} else {
  $source = (Resolve-Path (Join-Path $BuildDirectory 'Release\ZRinputTSF.dll')).Path
  $editorSource = (Resolve-Path (Join-Path $BuildDirectory 'Release\zrinput_theme_editor.exe')).Path
  $profileToolSource = (Resolve-Path (Join-Path $BuildDirectory 'Release\zrinput_profile_tool.exe')).Path
}
$installDirectory = Join-Path $env:LOCALAPPDATA 'ZRinput\app'
New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
$hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.Substring(0, 12)
$destination = Join-Path $installDirectory "ZRinputTSF-$hash.dll"
$comKey = 'HKCU:\Software\Classes\CLSID\{BFD6C220-320C-46F4-94D0-78C4779AE70C}\InprocServer32'
Copy-Item -LiteralPath $source -Destination $destination -Force
Copy-Item -LiteralPath $editorSource `
  -Destination (Join-Path $installDirectory 'ZRinputThemeEditor.exe') -Force
Copy-Item -LiteralPath $profileToolSource `
  -Destination (Join-Path $installDirectory 'ZRinputProfileTool.exe') -Force
$dataDirectory = Join-Path $installDirectory 'data'
$themeDirectory = Join-Path $installDirectory 'themes'
New-Item -ItemType Directory -Path $dataDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $themeDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $releaseRoot 'data\default_lexicon.tsv') `
  -Destination (Join-Path $dataDirectory 'default_lexicon.tsv') -Force
Copy-Item -LiteralPath (Join-Path $releaseRoot 'themes\microsoft-dark.ini') `
  -Destination (Join-Path $themeDirectory 'microsoft-dark.ini') -Force
New-Item -Path $comKey -Force | Out-Null
Set-Item -LiteralPath $comKey -Value $destination
New-ItemProperty -LiteralPath $comKey -Name ThreadingModel -Value Apartment `
  -PropertyType String -Force | Out-Null
$actual = (Get-ItemProperty $comKey).'(default)'
if ($actual -ne $destination) {
  throw "COM registration points to an unexpected DLL: $actual"
}
Write-Host "ZRinput updated to $destination"
