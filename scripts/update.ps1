[CmdletBinding()]
param(
  [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'),
  [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package_common.ps1')

$layout = Get-ZRPackageLayout $PSScriptRoot $BuildDirectory
$installDirectory = Join-Path (Get-ZRLocalDataDirectory) 'app'
$destination = Get-ZRVersionedDllPath $layout.Dll $installDirectory
$plan = [pscustomobject]@{
  Operation = 'Update'
  SourceDll = $layout.Dll
  DestinationDll = $destination
  LexiconSource = $layout.Lexicon
  ThemeSource = $layout.Theme
  UserAppDirectory = $installDirectory
  RequiresExistingMachineTip = $true
  RequiresExistingMachineProfile = $true
  RequiresExistingUserComRegistration = $true
  CurrentUserRegistryView = 'Registry64'
}
if ($PlanOnly) {
  return $plan
}

$registered = Get-ZRCurrentUserComRegistration
if (-not (Test-ZRMachineTipRegistration) -or
    -not (Test-ZRMachineProfileRegistration) -or -not $registered) {
  throw 'ZRinput is not installed for this user. Run install.ps1 before update.ps1.'
}

New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
Copy-ZRFileIfDifferent $layout.Dll $destination | Out-Null
Copy-ZRFileIfDifferent $layout.ThemeEditor `
  (Join-Path $installDirectory 'ZRinputThemeEditor.exe') | Out-Null
Copy-ZRFileIfDifferent $layout.ProfileTool `
  (Join-Path $installDirectory 'ZRinputProfileTool.exe') | Out-Null
$dataDirectory = Join-Path $installDirectory 'data'
$themeDirectory = Join-Path $installDirectory 'themes'
New-Item -ItemType Directory -Path $dataDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $themeDirectory -Force | Out-Null
Copy-ZRFileIfDifferent $layout.Lexicon `
  (Join-Path $dataDirectory 'default_lexicon.tsv') | Out-Null
Copy-ZRFileIfDifferent $layout.Theme `
  (Join-Path $themeDirectory 'microsoft-dark.ini') | Out-Null

# Switch only after the complete version has been staged.
Set-ZRCurrentUserComRegistration $destination
$actual = Get-ZRCurrentUserComRegistration
if (-not [string]::Equals($actual, $destination,
    [StringComparison]::OrdinalIgnoreCase)) {
  throw "COM registration points to an unexpected DLL: $actual"
}
Remove-ZRObsoleteDlls $installDirectory $destination
Write-Host "ZRinput updated to $destination"
Write-Host 'Already-running applications still have the old DLL loaded.'
Write-Host 'Close and reopen them, or sign out and back in, before checking the update.'
