<#
.SYNOPSIS
    Makes a vcpkg installation available for building MapWinGIS6.

.DESCRIPTION
    Resolves a vcpkg root in this order:
      1. -VcpkgRoot
      2. the MWG_VCPKG_ROOT environment variable
      3. <repo>\..\vcpkg   (cloned here by default)
      4. the VCPKG_ROOT environment variable

    Clones and bootstraps vcpkg when needed, and enables the local binary cache.
    MSBuild resolves the same locations, so after running this once the solution
    builds without further setup.
#>
[CmdletBinding()]
param(
    [string] $VcpkgRoot,
    [switch] $Update
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Resolve-VcpkgRoot {
    if ($VcpkgRoot) { return $VcpkgRoot }
    if ($env:MWG_VCPKG_ROOT) { return $env:MWG_VCPKG_ROOT }

    $sibling = Join-Path (Split-Path $repoRoot -Parent) 'vcpkg'
    if (Test-Path (Join-Path $sibling '.vcpkg-root')) { return $sibling }
    if ($env:VCPKG_ROOT -and (Test-Path (Join-Path $env:VCPKG_ROOT '.vcpkg-root'))) { return $env:VCPKG_ROOT }

    return $sibling
}

$root = Resolve-VcpkgRoot
Write-Host "vcpkg root: $root"

if (-not (Test-Path (Join-Path $root '.vcpkg-root'))) {
    Write-Host 'Cloning vcpkg...'
    git clone https://github.com/microsoft/vcpkg.git $root
}
elseif ($Update) {
    Write-Host 'Updating vcpkg...'
    git -C $root pull --ff-only
}

$exe = Join-Path $root 'vcpkg.exe'
if (-not (Test-Path $exe) -or $Update) {
    Write-Host 'Bootstrapping vcpkg...'
    & (Join-Path $root 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "bootstrap-vcpkg.bat failed with exit code $LASTEXITCODE" }
}

& $exe version

Write-Host ''
Write-Host 'Done. MapWinGIS6 will find this vcpkg automatically.'
Write-Host 'To use a different location, set MWG_VCPKG_ROOT or pass /p:MwgVcpkgRoot=<path> to MSBuild.'
