<#
.SYNOPSIS
    Tests dependency argument quoting and single-owner runtime deployment.
#>
[CmdletBinding()]
param(
    [string] $MSBuild = 'msbuild'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) "MapWinGIS build checks $([guid]::NewGuid())"
try {
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    Copy-Item "$repoRoot\Directory.Build.props", "$repoRoot\Directory.Build.targets" $temp
    Copy-Item "$repoRoot\build" "$temp\build" -Recurse
    New-Item -ItemType Directory -Path "$temp\packages\share\gdal", "$temp\packages\share\proj" -Force | Out-Null
    Set-Content "$temp\packages\share\gdal\test.csv" 'GDAL fixture'
    Set-Content "$temp\packages\share\proj\proj.db" 'PROJ fixture'
    Set-Content "$temp\capture.ps1" @'
$commandLine = [Environment]::GetCommandLineArgs()
$start = [array]::IndexOf($commandLine, $PSCommandPath) + 1
$commandLine | Select-Object -Skip $start | Set-Content (Join-Path $PSScriptRoot 'arguments.txt')
'@

    $project = @'
<Project>
  <PropertyGroup>
    <Configuration>Debug</Configuration>
    <Platform>x64</Platform>
    <MwgVcpkgRoot>$(MSBuildThisFileDirectory)no-vcpkg</MwgVcpkgRoot>
  </PropertyGroup>
  <Import Project="Directory.Build.props" />
  <PropertyGroup>
    <MwgVcpkgTripletDir>$(MSBuildThisFileDirectory)packages\</MwgVcpkgTripletDir>
    <OutDir>$(MSBuildThisFileDirectory)output\</OutDir>
  </PropertyGroup>
  <Import Project="Directory.Build.targets" />
  <Target Name="CaptureArguments">
    <Exec Command="&quot;$(MwgTestPowerShell)&quot; -NoProfile -File &quot;$(MSBuildThisFileDirectory)capture.ps1&quot; $(VcpkgAdditionalInstallOptions)" />
  </Target>
</Project>
'@
    Set-Content "$temp\RuntimeDataOwner.proj" $project
    Set-Content "$temp\MapWinGIS.ActiveX.proj" $project
    $powerShell = (Get-Process -Id $PID).Path
    & $MSBuild "$temp\RuntimeDataOwner.proj" -nologo -v:q -t:CaptureArguments "-p:MwgTestPowerShell=$powerShell" '-p:MwgVcpkgFeatures=tests%3Bfull-drivers'
    if ($LASTEXITCODE -ne 0) { throw 'Dependency argument capture failed.' }
    $arguments = @(Get-Content "$temp\arguments.txt")
    if ($arguments.Count -ne 3 -or
        $arguments[0] -ne "--overlay-triplets=$temp\build\triplets" -or
        $arguments[1] -ne '--x-feature=tests' -or
        $arguments[2] -ne '--x-feature=full-drivers') {
        throw "Dependency arguments were split or changed: $arguments"
    }

    # A consumer must not copy, even when the shared output is empty.
    & $MSBuild "$temp\MapWinGIS.ActiveX.proj" -nologo -v:q -t:MwgDeployGdalRuntimeData
    if ($LASTEXITCODE -ne 0) { throw 'Consumer deployment check failed.' }
    if (Test-Path "$temp\output") { throw 'A consumer copied shared runtime data.' }

    & $MSBuild "$temp\RuntimeDataOwner.proj" -nologo -v:q -t:MwgDeployGdalRuntimeData -p:MwgDeployRuntimeData=true
    if ($LASTEXITCODE -ne 0) { throw 'Core deployment failed.' }
    foreach ($relative in @('gdal-data\test.csv', 'proj\proj.db')) {
        if (-not (Test-Path "$temp\output\$relative")) { throw "Runtime data missing: $relative" }
    }

    Set-Content "$temp\packages\share\proj\proj.db" 'Updated PROJ fixture'
    & $MSBuild "$temp\MapWinGIS.ActiveX.proj" -nologo -v:q -t:MwgDeployGdalRuntimeData
    if ($LASTEXITCODE -ne 0) { throw 'Consumer redeployment check failed.' }
    if ((Get-Content "$temp\output\proj\proj.db") -ne 'PROJ fixture') {
        throw 'A consumer overwrote shared runtime data.'
    }
    Write-Host 'Build configuration checks passed.'
}
finally {
    if (Test-Path $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}
