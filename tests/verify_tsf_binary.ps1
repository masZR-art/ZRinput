param(
  [Parameter(Mandatory = $true)]
  [string]$DllPath,

  [Parameter(Mandatory = $true)]
  [string]$DumpbinPath,

  [Parameter(Mandatory = $true)]
  [ValidateSet('x64', 'ARM64')]
  [string]$ExpectedMachine
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$resolvedDll = (Resolve-Path -LiteralPath $DllPath).Path
$resolvedDumpbin = (Resolve-Path -LiteralPath $DumpbinPath).Path

function Invoke-Dumpbin {
  param([Parameter(Mandatory = $true)][string]$Argument)

  $output = & $resolvedDumpbin $Argument $resolvedDll 2>&1
  if ($LASTEXITCODE -ne 0) {
    throw "dumpbin $Argument failed: $($output -join [Environment]::NewLine)"
  }
  return @($output | ForEach-Object { $_.ToString() })
}

$headers = Invoke-Dumpbin '/headers'
$headerText = $headers -join "`n"
$machinePattern = if ($ExpectedMachine -eq 'x64') {
  'machine \(x64\)'
} else {
  'machine \(ARM64\)'
}
if ($headerText -notmatch $machinePattern) {
  throw "PE machine does not match $ExpectedMachine"
}
$requiredFlags = @('Dynamic base', 'NX compatible', 'Control Flow Guard')
if ($ExpectedMachine -in @('x64', 'ARM64')) {
  $requiredFlags += 'High Entropy Virtual Addresses'
}
foreach ($requiredFlag in $requiredFlags) {
  if ($headerText -notmatch [regex]::Escape($requiredFlag)) {
    throw "missing PE hardening flag: $requiredFlag"
  }
}

$loadConfigText = (Invoke-Dumpbin '/loadconfig') -join "`n"
if ($loadConfigText -notmatch 'CF instrumented' -or
    $loadConfigText -notmatch 'FID table present' -or
    $loadConfigText -notmatch '(?m)^\s*[1-9][0-9]*\s+Guard CF function count') {
  throw 'Control Flow Guard metadata is incomplete'
}

$exportText = (Invoke-Dumpbin '/exports') -join "`n"
$matches = [regex]::Matches(
  $exportText,
  '(?m)^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)(?:\s+=.*)?\s*$')
$actualExports = @($matches | ForEach-Object { $_.Groups[1].Value } |
  Sort-Object -Unique)
$expectedExports = @(
  'DllCanUnloadNow',
  'DllGetClassObject',
  'DllRegisterServer',
  'DllUnregisterServer'
) | Sort-Object
$exportDelta = @(Compare-Object $expectedExports $actualExports)
if ($exportDelta.Count -ne 0) {
  throw "unexpected COM export set: $($exportDelta | Out-String)"
}

$dependentText = (Invoke-Dumpbin '/dependents') -join "`n"
if ($dependentText -match '(?i)(vcruntime|msvcp|ucrtbase)[^\s]*\.dll') {
  throw 'TSF DLL unexpectedly depends on the dynamic VC/UCRT runtime'
}

$version = [Diagnostics.FileVersionInfo]::GetVersionInfo($resolvedDll)
if ($version.FileVersion -ne '0.2.0.0' -or
    $version.ProductVersion -ne '0.2.0.0' -or
    $version.ProductName -ne 'ZRinput' -or
    $version.OriginalFilename -ne 'ZRinputTsf.dll') {
  throw 'TSF DLL version metadata is missing or inconsistent'
}

Write-Output ("machine={0} exports={1} cfg=enabled signature=not-enforced" -f
  $ExpectedMachine, $actualExports.Count)
