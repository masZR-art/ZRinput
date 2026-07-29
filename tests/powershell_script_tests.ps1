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
$originalProgramFiles = $env:ProgramFiles
$originalLocalAppData = $env:LOCALAPPDATA
$originalSystemRoot = $env:SystemRoot
$env:ProgramFiles = 'C:\ZRinput-Untrusted-ProgramFiles'
$env:LOCALAPPDATA = 'C:\ZRinput-Untrusted-LocalAppData'
$env:SystemRoot = 'C:\ZRinput-Untrusted-Windows'

try {
  $installPlan = & (Join-Path $scriptDirectory 'install.ps1') `
    -BuildDirectory $BuildDirectory -PlanOnly
  $updatePlan = & (Join-Path $scriptDirectory 'update.ps1') `
    -BuildDirectory $BuildDirectory -PlanOnly
  $uninstallPlan = & (Join-Path $scriptDirectory 'uninstall.ps1') -PlanOnly
} finally {
  $env:ProgramFiles = $originalProgramFiles
  $env:LOCALAPPDATA = $originalLocalAppData
  $env:SystemRoot = $originalSystemRoot
}

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
Assert-ZREqual $installPlan.ProgramInstallDirectory `
  (Get-ZRProgramInstallDirectory) `
  'install trusted an overridden ProgramFiles environment variable'
Assert-ZREqual $updatePlan.UserAppDirectory `
  (Join-Path (Get-ZRLocalDataDirectory) 'app') `
  'update trusted an overridden LOCALAPPDATA environment variable'
Assert-ZREqual $uninstallPlan.ProgramInstallDirectory `
  (Get-ZRProgramInstallDirectory) `
  'uninstall trusted an overridden ProgramFiles environment variable'
Assert-ZREqual $uninstallPlan.UserAppDirectory `
  (Join-Path (Get-ZRLocalDataDirectory) 'app') `
  'uninstall trusted an overridden LOCALAPPDATA environment variable'
Assert-ZRTrue ((Get-ZRNativePowerShell) -notlike
    'C:\ZRinput-Untrusted-Windows*') `
  'package scripts trusted an overridden SystemRoot for PowerShell'
Assert-ZRTrue ((Get-ZRNativeRegsvr32) -notlike
    'C:\ZRinput-Untrusted-Windows*') `
  'package scripts trusted an overridden SystemRoot for regsvr32'

$dangerousParameterRejected = $false
try {
  & (Join-Path $scriptDirectory 'uninstall.ps1') -PlanOnly `
    -UserAppDirectory 'C:\Windows' | Out-Null
} catch {
  $dangerousParameterRejected = $true
}
Assert-ZRTrue $dangerousParameterRejected `
  'uninstall still accepts a caller-controlled recursive-delete path'
$dangerousDllRejected = $false
try {
  & (Join-Path $scriptDirectory 'uninstall.ps1') -PlanOnly `
    -RegisteredDll 'C:\Users\Public\untrusted.dll' | Out-Null
} catch {
  $dangerousDllRejected = $true
}
Assert-ZRTrue $dangerousDllRejected `
  'uninstall still accepts a caller-controlled elevated DLL path'

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
  [IO.File]::WriteAllText($currentDll, 'current')
  [IO.File]::WriteAllText($oldDll, 'old')
  $machineDll = Join-Path $fakeProgram 'ZRinputTSF-A1B2C3D4E5F6.dll'
  [IO.File]::WriteAllText($machineDll, 'machine')

  $obsolete = @(Get-ZRObsoleteDlls $fakeApp $currentDll)
  Assert-ZREqual $obsolete.Count 1 'obsolete DLL selection is wrong'
  Assert-ZREqual $obsolete[0].FullName $oldDll `
    'obsolete DLL selection included the active version'
  $candidate = Get-ZRMachineUnregisterCandidate $fakeProgram
  Assert-ZREqual $candidate $machineDll `
    'uninstall did not select the trusted machine DLL'
  Assert-ZRTrue ($candidate -ne $currentDll -and $candidate -ne $oldDll) `
    'uninstall selected a DLL from the user-writable update directory'

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
