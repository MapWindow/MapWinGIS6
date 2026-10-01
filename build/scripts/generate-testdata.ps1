<#
.SYNOPSIS
    Regenerates the binary test data in tests\data from the sources in
    tests\data\source.

.DESCRIPTION
    Only needs to be run when the CSV/VRT sources change. The generated files
    are committed, so a normal build and test run does not need GDAL tools.

    Requires ogr2ogr on PATH, or -GdalBin pointing at a folder that has it.
#>
[CmdletBinding()]
param(
    [string] $GdalBin
)

$ErrorActionPreference = 'Stop'

$dataDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..\tests\data')).Path
$sourceDir = Join-Path $dataDir 'source'
$vrt = Join-Path $sourceDir 'provinces.vrt'

$ogr2ogr = if ($GdalBin) { Join-Path $GdalBin 'ogr2ogr.exe' } else { 'ogr2ogr' }
if (-not (Get-Command $ogr2ogr -ErrorAction SilentlyContinue)) {
    throw "ogr2ogr was not found. Put it on PATH or pass -GdalBin <folder>."
}

function Convert-Layer {
    param([string] $Format, [string] $Target, [string[]] $Options = @())

    if (Test-Path $Target) { Remove-Item $Target -Recurse -Force }

    Write-Host "Writing $Target"
    & $ogr2ogr -f $Format $Target $vrt @Options
    if ($LASTEXITCODE -ne 0) { throw "ogr2ogr failed with exit code $LASTEXITCODE" }
}

# UTF-8 so that non-ASCII attribute values survive the round trip. The driver
# records the encoding in provinces.cpg.
Convert-Layer -Format 'ESRI Shapefile' -Target (Join-Path $dataDir 'provinces.shp') `
    -Options @('-lco', 'ENCODING=UTF-8')

Convert-Layer -Format 'GPKG' -Target (Join-Path $dataDir 'provinces.gpkg')

Write-Host 'Done.'
