<#
.SYNOPSIS
    Verifies the C# sample CMake file configures when the ActiveX target is absent.
#>
[CmdletBinding()]
param(
    [string] $CMakeExecutable = 'cmake'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$samplePath = (Join-Path $repoRoot 'samples\CSharp') -replace '\\', '/'
$temp = Join-Path ([System.IO.Path]::GetTempPath()) "MapWinGIS CSharp core-only check $([guid]::NewGuid())"
$source = Join-Path $temp 'source'
$build = Join-Path $temp 'build'

try {
    New-Item -ItemType Directory -Force -Path $source | Out-Null
    $project = @'
cmake_minimum_required(VERSION 3.28)
project(MwgCoreOnlySample NONE)
set(MWG_RUNTIME_DIR "${CMAKE_BINARY_DIR}/runtime")
set(MWG_PLATFORM x64)
add_subdirectory("@CSHARP_SAMPLE@" samples/CSharp)
'@
    $project = $project.Replace('@CSHARP_SAMPLE@', $samplePath)
    [System.IO.File]::WriteAllText((Join-Path $source 'CMakeLists.txt'), $project)

    & $CMakeExecutable '-S' $source '-B' $build
    if ($LASTEXITCODE -ne 0) {
        throw 'The C# sample failed to configure without the ActiveX target.'
    }
    Write-Host 'The C# sample configures without an ActiveX target.'
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}