<#
.SYNOPSIS
    Fails when an ActiveX binary exports test-only hooks.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $OcxPath
)

$ErrorActionPreference = 'Stop'
$resolvedOcx = (Resolve-Path -LiteralPath $OcxPath).Path
$dumpbinPath = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue).Source
if (-not $dumpbinPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'dumpbin.exe and vswhere.exe were not found.'
    }
    $visualStudioPath = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $visualStudioPath) {
        throw 'Could not locate Visual Studio dumpbin.exe.'
    }
    $toolsetRoot = Join-Path $visualStudioPath 'VC\Tools\MSVC'
    $toolset = Get-ChildItem -LiteralPath $toolsetRoot -Directory |
        Sort-Object -Property Name -Descending |
        Select-Object -First 1
    if (-not $toolset) {
        throw 'Could not locate an installed MSVC toolset.'
    }
    $dumpbinPath = Join-Path $toolset.FullName 'bin\Hostx64\x64\dumpbin.exe'
    if (-not (Test-Path -LiteralPath $dumpbinPath)) {
        throw "Could not locate dumpbin.exe under '$($toolset.FullName)'."
    }
}

$exports = & $dumpbinPath /nologo /exports $resolvedOcx | Out-String
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin failed to inspect '$resolvedOcx'."
}
if ($exports -match '\bMapWinGISControlSet\w+ForTesting\b') {
    throw "The ActiveX binary '$resolvedOcx' exports a test-only hook."
}

Write-Host "No ActiveX test hooks are exported by '$resolvedOcx'."