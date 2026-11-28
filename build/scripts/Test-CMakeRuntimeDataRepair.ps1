<#
.SYNOPSIS
    Verifies incremental runtime-data repair.
#>
[CmdletBinding()]
param(
    [string] $CMakeExecutable = 'cmake'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$temp = Join-Path $repoRoot "out\cmake-runtime-data-repair-$([guid]::NewGuid())"
$source = Join-Path $temp 'runtime-source'
$build = Join-Path $temp 'runtime-build'
$installedRoot = Join-Path $source 'vcpkg_installed'
$packages = Join-Path $installedRoot 'test-triplet\share'
$gdalSource = Join-Path $packages 'gdal'
$projSource = Join-Path $packages 'proj'

function Invoke-CMakeChecked {
    param([string[]] $Arguments, [string] $FailureMessage)

    & $CMakeExecutable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw $FailureMessage
    }
}

try {
    New-Item -ItemType Directory -Force -Path $gdalSource, $projSource |
        Out-Null
    [System.IO.File]::WriteAllText((Join-Path $gdalSource 'test.csv'), 'GDAL fixture')
    [System.IO.File]::WriteAllText((Join-Path $projSource 'proj.db'), 'PROJ fixture')

    $modulePath = (Join-Path $repoRoot 'cmake\MwgRuntimeDeploy.cmake') -replace '\\', '/'
    $installedPath = $installedRoot -replace '\\', '/'
    $runtimeProject = @'
cmake_minimum_required(VERSION 3.28)
project(MwgRuntimeDataRepair NONE)
set(VCPKG_INSTALLED_DIR "@INSTALLED_ROOT@")
set(VCPKG_TARGET_TRIPLET "test-triplet")
set(MWG_RUNTIME_DIR "${CMAKE_BINARY_DIR}/runtime")
set(MWG_DEPLOY_RUNTIME_DATA ON)
include("@RUNTIME_MODULE@")
mwg_add_runtime_data_target()
'@
    $runtimeProject = $runtimeProject.Replace('@INSTALLED_ROOT@', $installedPath)
    $runtimeProject = $runtimeProject.Replace('@RUNTIME_MODULE@', $modulePath)
    [System.IO.File]::WriteAllText(
        (Join-Path $source 'CMakeLists.txt'),
        $runtimeProject)

    Invoke-CMakeChecked @('-S', $source, '-B', $build) `
        'The runtime-data fixture failed to configure.'
    $runtime = Join-Path $build 'runtime'
    Invoke-CMakeChecked @('--build', $build, '--target', 'MapWinGIS.RuntimeData') `
        'The initial runtime-data deployment failed.'
    $gdalOutput = Join-Path $runtime 'gdal-data\test.csv'
    $projOutput = Join-Path $runtime 'proj\proj.db'
    if ((Get-Content -LiteralPath $gdalOutput -Raw) -ne 'GDAL fixture' -or
        (Get-Content -LiteralPath $projOutput -Raw) -ne 'PROJ fixture') {
        throw 'The initial runtime-data deployment did not copy every file.'
    }

    $marker = Join-Path $runtime 'gdal-data\local-marker.txt'
    [System.IO.File]::WriteAllText($marker, 'preserve')
    Remove-Item -LiteralPath $projOutput
    Invoke-CMakeChecked @('--build', $build, '--target', 'MapWinGIS.RuntimeData') `
        'The runtime-data target did not repair a missing output.'
    if ((Get-Content -LiteralPath $projOutput -Raw) -ne 'PROJ fixture' -or
        (Get-Content -LiteralPath $marker -Raw) -ne 'preserve') {
        throw 'Repairing a missing runtime-data file removed unrelated data.'
    }

    $newSource = Join-Path $gdalSource 'added.csv'
    [System.IO.File]::WriteAllText($newSource, 'new GDAL fixture')
    Invoke-CMakeChecked @('-S', $source, '-B', $build) `
        'The runtime-data fixture failed to reconfigure after a new input was added.'
    Invoke-CMakeChecked @('--build', $build, '--target', 'MapWinGIS.RuntimeData') `
        'The runtime-data target did not notice a newly added input.'
    if ((Get-Content -LiteralPath (Join-Path $runtime 'gdal-data\added.csv') -Raw) -ne
        'new GDAL fixture') {
        throw 'The newly added runtime-data file was not deployed.'
    }

    Write-Host 'CMake runtime-data repair check passed.'
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}