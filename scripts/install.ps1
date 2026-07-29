[CmdletBinding()]
param(
  [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'),
  [switch]$PlanOnly,
  [switch]$MachinePhase
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package_common.ps1')

$layout = Get-ZRPackageLayout $PSScriptRoot $BuildDirectory
$installDirectory = Get-ZRProgramInstallDirectory
$destination = Get-ZRVersionedDllPath $layout.Dll $installDirectory
$plan = [pscustomobject]@{
  Operation = 'Install'
  SourceDll = $layout.Dll
  DestinationDll = $destination
  LexiconSource = $layout.Lexicon
  ThemeSource = $layout.Theme
  ProgramInstallDirectory = $installDirectory
  CurrentUserRegistryView = 'Registry64'
  MachinePhaseRequiresElevation = $true
}
if ($PlanOnly) {
  return $plan
}

function Invoke-ZRMachineInstall {
  if (-not (Test-ZRAdministrator)) {
    throw 'The machine installation phase requires administrator privileges.'
  }

  New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
  $previousRegistration = Get-ZRCurrentUserComRegistration

  # Stage every required file before changing the working registration.
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

  $regsvr32 = Get-ZRNativeRegsvr32
  if ($previousRegistration -or (Test-ZRMachineTipRegistration)) {
    $unregisterDll = if ($previousRegistration -and
      (Test-Path -LiteralPath $previousRegistration -PathType Leaf)) {
      $previousRegistration
    } else {
      $destination
    }
    $unregister = Start-Process $regsvr32 `
      -ArgumentList @('/s', '/u', ('"{0}"' -f $unregisterDll)) -Wait -PassThru
    if ($unregister.ExitCode -ne 0 -and $unregisterDll -ne $destination) {
      $unregister = Start-Process $regsvr32 `
        -ArgumentList @('/s', '/u', ('"{0}"' -f $destination)) -Wait -PassThru
    }
    if ($unregister.ExitCode -ne 0) {
      throw "Unable to unregister the previous ZRinput (exit $($unregister.ExitCode))."
    }
  }

  $register = Start-Process $regsvr32 `
    -ArgumentList @('/s', ('"{0}"' -f $destination)) -Wait -PassThru
  if ($register.ExitCode -ne 0) {
    if ($previousRegistration -and
        (Test-Path -LiteralPath $previousRegistration -PathType Leaf)) {
      Start-Process $regsvr32 `
        -ArgumentList @('/s', ('"{0}"' -f $previousRegistration)) `
        -Wait | Out-Null
    }
    throw "Unable to register ZRinput (exit $($register.ExitCode))."
  }
  if (-not (Test-ZRMachineTipRegistration)) {
    throw 'TSF registration returned success but the machine TIP entry is missing.'
  }
  if (-not (Test-ZRMachineProfileRegistration)) {
    throw 'TSF registration returned success but the Simplified Chinese profile is missing.'
  }
  $actual = Get-ZRCurrentUserComRegistration
  if (-not [string]::Equals($actual, $destination,
      [StringComparison]::OrdinalIgnoreCase)) {
    throw "COM registration points to an unexpected DLL: $actual"
  }
  Remove-ZRObsoleteDlls $installDirectory $destination
}

if (-not $MachinePhase -and -not (Test-ZRAdministrator)) {
  $arguments = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
    ('"{0}"' -f $PSCommandPath), '-BuildDirectory',
    ('"{0}"' -f $BuildDirectory), '-MachinePhase')
  $process = Start-Process (Get-ZRNativePowerShell) -Verb RunAs `
    -ArgumentList $arguments -Wait -PassThru
  if ($process.ExitCode -ne 0) {
    throw "Elevated installation failed (exit $($process.ExitCode))."
  }
  if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
    throw "The elevated installer did not create the expected DLL: $destination"
  }
} else {
  Invoke-ZRMachineInstall
}

if (-not $MachinePhase) {
  # This runs in the requesting user's token even when another administrator
  # supplied the UAC credentials for the machine phase.
  Set-ZRCurrentUserComRegistration $destination
  $actual = Get-ZRCurrentUserComRegistration
  if (-not [string]::Equals($actual, $destination,
      [StringComparison]::OrdinalIgnoreCase)) {
    throw "Current-user COM registration points to an unexpected DLL: $actual"
  }
  Write-Host "ZRinput installed to $destination"
  Write-Host 'Close and reopen text applications, or sign out and back in, before testing the new DLL.'
}
