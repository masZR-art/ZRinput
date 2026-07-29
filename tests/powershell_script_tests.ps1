[CmdletBinding()]
param(
  [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build')
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$scriptDirectory = Join-Path $repository 'scripts'

function Assert-ZRTrue {
  param([bool]$Condition, [string]$Message)
  if (-not $Condition) {
    throw $Message
  }
}

function Assert-ZREqual {
  param($Actual, $Expected, [string]$Message)
  if ($Actual -ne $Expected) {
    throw "$Message (expected '$Expected', got '$Actual')"
  }
}

$scripts = @(
  'package_common.ps1', 'install.ps1', 'update.ps1', 'uninstall.ps1')
foreach ($name in $scripts) {
  $path = Join-Path $scriptDirectory $name
  $tokens = $null
  $errors = $null
  [System.Management.Automation.Language.Parser]::ParseFile(
    $path, [ref]$tokens, [ref]$errors) | Out-Null
  if ($errors.Count -ne 0) {
    throw "$name has parser errors: $($errors -join '; ')"
  }
}

. (Join-Path $scriptDirectory 'package_common.ps1')
$registrationBefore = Get-ZRCurrentUserComRegistration
$tipBefore = Test-ZRMachineTipRegistration
$profileBefore = Test-ZRMachineProfileRegistration

$installPlan = & (Join-Path $scriptDirectory 'install.ps1') `
  -BuildDirectory $BuildDirectory -PlanOnly
$updatePlan = & (Join-Path $scriptDirectory 'update.ps1') `
  -BuildDirectory $BuildDirectory -PlanOnly
$uninstallPlan = & (Join-Path $scriptDirectory 'uninstall.ps1') -PlanOnly

Assert-ZREqual $installPlan.Operation 'Install' 'install plan operation is wrong'
Assert-ZREqual $updatePlan.Operation 'Update' 'update plan operation is wrong'
Assert-ZREqual $uninstallPlan.Operation 'Uninstall' 'uninstall plan operation is wrong'
foreach ($path in @($installPlan.SourceDll, $installPlan.LexiconSource,
    $installPlan.ThemeSource, $updatePlan.SourceDll,
    $updatePlan.LexiconSource, $updatePlan.ThemeSource)) {
  Assert-ZRTrue (Test-Path -LiteralPath $path -PathType Leaf) `
    "planned package source is missing: $path"
}
$expectedPath = Get-ZRVersionedDllPath $installPlan.SourceDll `
  $installPlan.ProgramInstallDirectory
$expectedHash = [IO.Path]::GetFileNameWithoutExtension($expectedPath).Substring(11)
Assert-ZREqual (Split-Path -Leaf $installPlan.DestinationDll) `
  "ZRinputTSF-$expectedHash.dll" 'install destination is not content-versioned'
Assert-ZREqual (Split-Path -Leaf $updatePlan.DestinationDll) `
  "ZRinputTSF-$expectedHash.dll" 'update destination is not content-versioned'
Assert-ZRTrue $installPlan.MachinePhaseRequiresElevation `
  'install plan lost its elevated machine phase'
Assert-ZRTrue $updatePlan.RequiresExistingMachineTip `
  'update plan must require a previous machine registration'
Assert-ZRTrue $updatePlan.RequiresExistingMachineProfile `
  'update plan must require a Simplified Chinese profile'
Assert-ZRTrue $updatePlan.RequiresExistingUserComRegistration `
  'update plan must require a previous per-user registration'
Assert-ZRTrue $uninstallPlan.PreservesPersonalData `
  'uninstall plan must preserve memory and saved themes'
Assert-ZRTrue ($uninstallPlan.UserAppDirectory -ne
  $uninstallPlan.PersonalDataDirectory) `
  'uninstall plan would remove the complete personal-data directory'

$registrationAfter = Get-ZRCurrentUserComRegistration
$tipAfter = Test-ZRMachineTipRegistration
$profileAfter = Test-ZRMachineProfileRegistration
Assert-ZREqual $registrationAfter $registrationBefore `
  'PlanOnly changed the current-user COM registration'
Assert-ZREqual $tipAfter $tipBefore `
  'PlanOnly changed the machine TIP registration'
Assert-ZREqual $profileAfter $profileBefore `
  'PlanOnly changed the machine language profile'

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) `
  ("zrinput-package-script-test-{0}" -f [guid]::NewGuid().ToString('N'))
$fakeProgram = Join-Path $temporaryRoot 'program'
$fakeApp = Join-Path $temporaryRoot 'app'
try {
  New-Item -ItemType Directory -Path $fakeProgram, $fakeApp -Force | Out-Null
  $currentDll = Join-Path $fakeApp 'ZRinputTSF-CURRENT.dll'
  $oldDll = Join-Path $fakeApp 'ZRinputTSF-OLD.dll'
  $registeredDll = Join-Path $fakeProgram 'ZRinputTSF-REGISTERED.dll'
  [IO.File]::WriteAllText($currentDll, 'current')
  [IO.File]::WriteAllText($oldDll, 'old')
  [IO.File]::WriteAllText($registeredDll, 'registered')

  $obsolete = @(Get-ZRObsoleteDlls $fakeApp $currentDll)
  Assert-ZREqual $obsolete.Count 1 'obsolete DLL selection is wrong'
  Assert-ZREqual $obsolete[0].FullName $oldDll `
    'obsolete DLL selection included the active version'
  $candidate = Get-ZRUnregisterCandidate $registeredDll $fakeProgram `
    $fakeApp $null
  Assert-ZREqual $candidate $registeredDll `
    'uninstall did not prioritize the currently registered DLL'

  $copySource = Join-Path $temporaryRoot 'copy-source.txt'
  $copyDestination = Join-Path $temporaryRoot 'copy-destination.txt'
  [IO.File]::WriteAllText($copySource, 'same')
  [IO.File]::WriteAllText($copyDestination, 'same')
  Assert-ZRTrue (-not (Copy-ZRFileIfDifferent $copySource $copyDestination)) `
    'an identical in-use destination would be overwritten'
  [IO.File]::WriteAllText($copySource, 'new')
  Assert-ZRTrue (Copy-ZRFileIfDifferent $copySource $copyDestination) `
    'a changed package file was not copied'
  Assert-ZREqual ([IO.File]::ReadAllText($copyDestination)) 'new' `
    'copied package content is wrong'
} finally {
  if (Test-Path -LiteralPath $temporaryRoot) {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
  }
}

Write-Output 'PowerShell package plans, paths, cleanup selection, and no-write mode passed.'
