function Get-ZRClsidPath {
  return 'Software\Classes\CLSID\{BFD6C220-320C-46F4-94D0-78C4779AE70C}'
}

function Get-ZRTipPath {
  return 'Software\Microsoft\CTF\TIP\{BFD6C220-320C-46F4-94D0-78C4779AE70C}'
}

function Get-ZRProfilePath {
  return "$(Get-ZRTipPath)\LanguageProfile\0x00000804\{97313B73-4F48-48E4-BC7E-10DF2538892C}"
}

function Get-ZRProgramInstallDirectory {
  $programFiles = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::ProgramFiles)
  if (-not $programFiles) {
    throw 'Windows did not provide the Program Files directory.'
  }
  return (Join-Path $programFiles 'ZRinput')
}

function Get-ZRLocalDataDirectory {
  $localData = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::LocalApplicationData)
  if (-not $localData) {
    throw 'Windows did not provide the local application-data directory.'
  }
  return (Join-Path $localData 'ZRinput')
}

function Test-ZRAdministrator {
  $principal = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent())
  return $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-ZRNativePowerShell {
  $systemDirectory = [Environment]::SystemDirectory
  if ([Environment]::Is64BitOperatingSystem -and
      -not [Environment]::Is64BitProcess) {
    $windowsDirectory = Split-Path -Parent $systemDirectory
    return (Join-Path $windowsDirectory `
      'Sysnative\WindowsPowerShell\v1.0\powershell.exe')
  }
  return (Join-Path $systemDirectory 'WindowsPowerShell\v1.0\powershell.exe')
}

function Get-ZRNativeRegsvr32 {
  $systemDirectory = [Environment]::SystemDirectory
  if ([Environment]::Is64BitOperatingSystem -and
      -not [Environment]::Is64BitProcess) {
    return (Join-Path (Split-Path -Parent $systemDirectory) `
      'Sysnative\regsvr32.exe')
  }
  return (Join-Path $systemDirectory 'regsvr32.exe')
}

function Get-ZRPackageLayout {
  param(
    [Parameter(Mandatory = $true)][string]$ScriptRoot,
    [Parameter(Mandatory = $true)][string]$BuildDirectory
  )

  $releaseRoot = (Resolve-Path -LiteralPath (Join-Path $ScriptRoot '..')).Path
  $packagedDll = Join-Path $releaseRoot 'ZRinputTSF.dll'
  if (Test-Path -LiteralPath $packagedDll -PathType Leaf) {
    $dll = $packagedDll
    $editor = Join-Path $releaseRoot 'zrinput_theme_editor.exe'
    $profileTool = Join-Path $releaseRoot 'zrinput_profile_tool.exe'
  } else {
    $releaseDirectory = Join-Path $BuildDirectory 'Release'
    $dll = Join-Path $releaseDirectory 'ZRinputTSF.dll'
    $editor = Join-Path $releaseDirectory 'zrinput_theme_editor.exe'
    $profileTool = Join-Path $releaseDirectory 'zrinput_profile_tool.exe'
  }

  $layout = [ordered]@{
    ReleaseRoot = $releaseRoot
    Dll = $dll
    ThemeEditor = $editor
    ProfileTool = $profileTool
    Lexicon = (Join-Path $releaseRoot 'data\default_lexicon.tsv')
    Theme = (Join-Path $releaseRoot 'themes\microsoft-dark.ini')
  }
  foreach ($key in @($layout.Keys)) {
    if ($key -eq 'ReleaseRoot') {
      continue
    }
    $value = $layout[$key]
    if (-not (Test-Path -LiteralPath $value -PathType Leaf)) {
      throw "Required package file is missing: $value"
    }
    $layout[$key] = (Resolve-Path -LiteralPath $value).Path
  }
  return [pscustomobject]$layout
}

function Get-ZRFileSha256 {
  param([Parameter(Mandatory = $true)][string]$Path)
  $stream = [IO.File]::OpenRead($Path)
  $algorithm = [Security.Cryptography.SHA256]::Create()
  try {
    $hash = -join @($algorithm.ComputeHash($stream) |
      ForEach-Object { $_.ToString('X2') })
  } finally {
    $algorithm.Dispose()
    $stream.Dispose()
  }
  return $hash
}

function Get-ZRVersionedDllPath {
  param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$InstallDirectory
  )
  $hash = (Get-ZRFileSha256 $Source).Substring(0, 12)
  return (Join-Path $InstallDirectory "ZRinputTSF-$hash.dll")
}

function Copy-ZRFileIfDifferent {
  param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Destination
  )
  if (Test-Path -LiteralPath $Destination -PathType Leaf) {
    if ((Get-ZRFileSha256 $Source) -eq (Get-ZRFileSha256 $Destination)) {
      return $false
    }
  }
  Copy-Item -LiteralPath $Source -Destination $Destination -Force
  return $true
}

function Open-ZRRegistryBase {
  param(
    [Parameter(Mandatory = $true)][Microsoft.Win32.RegistryHive]$Hive,
    [Parameter(Mandatory = $true)][Microsoft.Win32.RegistryView]$View
  )
  return [Microsoft.Win32.RegistryKey]::OpenBaseKey($Hive, $View)
}

function Get-ZRCurrentUserComRegistration {
  $base = Open-ZRRegistryBase CurrentUser Registry64
  try {
    $key = $base.OpenSubKey("$(Get-ZRClsidPath)\InprocServer32")
    if (-not $key) {
      return $null
    }
    try {
      return [string]$key.GetValue('', $null,
        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    } finally {
      $key.Dispose()
    }
  } finally {
    $base.Dispose()
  }
}

function Set-ZRCurrentUserComRegistration {
  param([Parameter(Mandatory = $true)][string]$DllPath)

  $base = Open-ZRRegistryBase CurrentUser Registry64
  try {
    $key = $base.CreateSubKey("$(Get-ZRClsidPath)\InprocServer32", $true)
    if (-not $key) {
      throw 'Unable to create the current-user COM registration.'
    }
    try {
      $key.SetValue('', $DllPath, [Microsoft.Win32.RegistryValueKind]::String)
      $key.SetValue('ThreadingModel', 'Apartment',
        [Microsoft.Win32.RegistryValueKind]::String)
    } finally {
      $key.Dispose()
    }
  } finally {
    $base.Dispose()
  }
}

function Remove-ZRCurrentUserComRegistration {
  $base = Open-ZRRegistryBase CurrentUser Registry64
  try {
    $base.DeleteSubKeyTree((Get-ZRClsidPath), $false)
  } finally {
    $base.Dispose()
  }
}

function Test-ZRMachineTipRegistration {
  $base = Open-ZRRegistryBase LocalMachine Registry64
  try {
    $key = $base.OpenSubKey((Get-ZRTipPath))
    if (-not $key) {
      return $false
    }
    $key.Dispose()
    return $true
  } finally {
    $base.Dispose()
  }
}

function Test-ZRMachineProfileRegistration {
  $base = Open-ZRRegistryBase LocalMachine Registry64
  try {
    $key = $base.OpenSubKey((Get-ZRProfilePath))
    if (-not $key) {
      return $false
    }
    $key.Dispose()
    return $true
  } finally {
    $base.Dispose()
  }
}

function Remove-ZRMachineTipRegistration {
  $base = Open-ZRRegistryBase LocalMachine Registry64
  try {
    $base.DeleteSubKeyTree((Get-ZRTipPath), $false)
  } finally {
    $base.Dispose()
  }
}

function Get-ZRObsoleteDlls {
  param(
    [Parameter(Mandatory = $true)][string]$Directory,
    [Parameter(Mandatory = $true)][string]$CurrentDll
  )
  if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
    return @()
  }
  $currentFullPath = [IO.Path]::GetFullPath($CurrentDll)
  return @(Get-ChildItem -LiteralPath $Directory -Filter 'ZRinputTSF-*.dll' -File |
    Where-Object {
      -not [string]::Equals($_.FullName, $currentFullPath,
        [StringComparison]::OrdinalIgnoreCase)
    })
}

function Remove-ZRObsoleteDlls {
  param(
    [Parameter(Mandatory = $true)][string]$Directory,
    [Parameter(Mandatory = $true)][string]$CurrentDll
  )
  foreach ($file in @(Get-ZRObsoleteDlls $Directory $CurrentDll)) {
    try {
      Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
    } catch {
      Write-Warning "Old DLL is still in use and was preserved: $($file.FullName)"
    }
  }
}

function Get-ZRMachineUnregisterCandidate {
  param([Parameter(Mandatory = $true)][string]$ProgramInstallDirectory)

  if (-not (Test-Path -LiteralPath $ProgramInstallDirectory -PathType Container)) {
    return $null
  }
  $trustedRoot = [IO.Path]::GetFullPath($ProgramInstallDirectory).TrimEnd(
    [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
  $candidates = @(
    Get-ChildItem -LiteralPath $trustedRoot -File -ErrorAction SilentlyContinue |
      Where-Object {
        ($_.Name -eq 'ZRinputTSF.dll' -or $_.Name -match
          '^ZRinputTSF-[0-9A-Fa-f]{12}\.dll$') -and
        -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
        [string]::Equals($_.DirectoryName, $trustedRoot,
          [StringComparison]::OrdinalIgnoreCase)
      } |
      Sort-Object LastWriteTime -Descending)
  if ($candidates.Count -eq 0) {
    return $null
  }
  return $candidates[0].FullName
}
