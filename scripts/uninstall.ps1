[CmdletBinding()]
param(
  [switch]$PlanOnly,
  [switch]$MachinePhase
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package_common.ps1')

$programInstallDirectory = Get-ZRProgramInstallDirectory
$personalDataDirectory = Get-ZRLocalDataDirectory
$userAppDirectory = Join-Path $personalDataDirectory 'app'
$machineUnregisterCandidate = Get-ZRMachineUnregisterCandidate `
  $programInstallDirectory

$plan = [pscustomobject]@{
  Operation = 'Uninstall'
  MachineUnregisterCandidate = $machineUnregisterCandidate
  ProgramInstallDirectory = $programInstallDirectory
  UserAppDirectory = $userAppDirectory
  PersonalDataDirectory = $personalDataDirectory
  PreservesPersonalData = $true
  MachinePhaseRequiresElevation = $true
}
if ($PlanOnly) {
  return $plan
}

function Initialize-ZRDeleteOnReboot {
  if ('ZRinput.PackageNativeMethods' -as [type]) {
    return
  }
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace ZRinput {
  public static class PackageNativeMethods {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool MoveFileEx(
      string existingName, string newName, int flags);
  }
}
'@
}

function Remove-ZRDirectoryOrSchedule {
  param([Parameter(Mandatory = $true)][string]$Path)
  if (-not (Test-Path -LiteralPath $Path)) {
    return $false
  }
  try {
    Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
    return $false
  } catch {
    Initialize-ZRDeleteOnReboot
    $remaining = @(Get-ChildItem -LiteralPath $Path -Recurse -Force `
      -ErrorAction SilentlyContinue | Sort-Object { $_.FullName.Length } -Descending)
    $scheduled = $true
    foreach ($item in $remaining) {
      if (-not [ZRinput.PackageNativeMethods]::MoveFileEx(
          $item.FullName, $null, 0x4)) {
        $scheduled = $false
      }
    }
    if (-not [ZRinput.PackageNativeMethods]::MoveFileEx($Path, $null, 0x4)) {
      $scheduled = $false
    }
    if (-not $scheduled) {
      throw "Unable to remove or schedule every file under $Path. Close text applications and run uninstall.ps1 again."
    }
    Write-Warning "Files in use under $Path will be deleted after Windows restarts."
    return $true
  }
}

function Invoke-ZRMachineUninstall {
  if (-not (Test-ZRAdministrator)) {
    throw 'The machine uninstall phase requires administrator privileges.'
  }

  $candidate = Get-ZRMachineUnregisterCandidate $programInstallDirectory
  $unregistered = $false
  if ($candidate) {
    $process = Start-Process (Get-ZRNativeRegsvr32) `
      -ArgumentList @('/s', '/u', ('"{0}"' -f $candidate)) -Wait -PassThru
    $unregistered = $process.ExitCode -eq 0
    if (-not $unregistered) {
      Write-Warning "DLL unregistration failed (exit $($process.ExitCode)); removing the stale TIP registration directly."
    }
  }
  if (-not $unregistered -or (Test-ZRMachineTipRegistration)) {
    Remove-ZRMachineTipRegistration
  }
  $restartForProgramFiles = Remove-ZRDirectoryOrSchedule $programInstallDirectory
  return $restartForProgramFiles
}

$restartRequired = $false
if (-not $MachinePhase -and -not (Test-ZRAdministrator)) {
  $arguments = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
    ('"{0}"' -f $PSCommandPath), '-MachinePhase')
  $process = Start-Process (Get-ZRNativePowerShell) -Verb RunAs `
    -ArgumentList $arguments -Wait -PassThru
  if ($process.ExitCode -ne 0) {
    throw "Elevated uninstall failed (exit $($process.ExitCode))."
  }
  $restartRequired = Test-Path -LiteralPath $programInstallDirectory
} else {
  $restartRequired = Invoke-ZRMachineUninstall
}

if (-not $MachinePhase) {
  # Remove the requesting user's registration even if another administrator
  # supplied the UAC credentials for the machine phase.
  Remove-ZRCurrentUserComRegistration
  if (Test-Path -LiteralPath $userAppDirectory) {
    try {
      Remove-Item -LiteralPath $userAppDirectory -Recurse -Force `
        -ErrorAction Stop
    } catch {
      Write-Warning 'Some user update files remain in use. Close text applications or sign out, then run uninstall.ps1 again.'
    }
  }
  Write-Host 'ZRinput was unregistered. Personal memory and saved themes were preserved.'
  if ($restartRequired) {
    Write-Host 'Restart Windows to finish deleting DLLs that were still loaded.'
  } else {
    Write-Host 'Close and reopen text applications, or sign out and back in, to clear cached TSF state.'
  }
}
