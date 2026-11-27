<#
.SYNOPSIS
    Builds, and optionally configures and tests, a CMake preset from a plain
    PowerShell or IDE terminal.

.DESCRIPTION
    The Ninja presets need an MSVC environment (cl.exe, MIDL, ATL headers).
    When the current shell does not already provide one for the preset's
    architecture, this script locates Visual Studio 2026 with vswhere, imports
    its vcvars environment into this process, and then runs CMake and CTest.

    An existing build tree is driven by the CMake that configured it, as
    recorded in its CMakeCache.txt, so trees configured by an IDE-bundled
    CMake keep working. A new tree is configured with the CMake on PATH,
    which the Visual Studio environment provides.

.EXAMPLE
    pwsh -File build\scripts\Invoke-CMakeBuild.ps1 -Configure -Test

.EXAMPLE
    pwsh -File build\scripts\Invoke-CMakeBuild.ps1 -Preset x86-debug -Target MapWinGIS.Core
#>
[CmdletBinding()]
param(
    # Build and test preset name. Its configure preset is looked up in
    # CMakePresets.json (for example vs2026-x64-debug configures vs2026-x64).
    [string] $Preset = 'x64-debug',

    # Runs 'cmake --preset' before building; required for a new build tree.
    [switch] $Configure,

    # Runs 'ctest --preset' after a successful build.
    [switch] $Test,

    # Builds only the named targets.
    [string[]] $Target = @(),

    # Visual Studio installation to use instead of the vswhere result.
    [string] $VsInstallPath = $env:MWG_VS_INSTALL_PATH
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# x86 presets build Win32 with the x64-hosted cross compiler.
$vcvarsArch = if ($Preset -like 'x86-*' -or $Preset -like '*-x86*') { 'amd64_x86' } else { 'amd64' }
$targetArch = if ($vcvarsArch -eq 'amd64_x86') { 'x86' } else { 'x64' }

function Find-VisualStudio {
    if ($VsInstallPath) {
        if (-not (Test-Path -LiteralPath $VsInstallPath)) {
            throw "Visual Studio was not found at '$VsInstallPath'."
        }
        return $VsInstallPath
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'vswhere.exe was not found. Install Visual Studio 2026 with the C++ workload.'
    }
    $path = & $vswhere -latest -products '*' -version '[18.0,19.0)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if (-not $path) {
        throw 'Visual Studio 2026 with the C++ x64/x86 build tools was not found.'
    }
    return $path
}

function Import-MsvcEnvironment([string] $installPath) {
    $vcvars = Join-Path $installPath 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) {
        throw "vcvarsall.bat was not found under '$installPath'."
    }
    $environment = & cmd.exe /d /c "call `"$vcvars`" $vcvarsArch >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "vcvarsall.bat $vcvarsArch failed."
    }
    foreach ($line in $environment) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            [Environment]::SetEnvironmentVariable(
                $line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
        }
    }
}

$presets = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'CMakePresets.json') | ConvertFrom-Json
$configurePreset = $Preset
$buildPreset = @($presets.buildPresets) | Where-Object { $_.name -eq $Preset } | Select-Object -First 1
if ($buildPreset -and $buildPreset.configurePreset) {
    $configurePreset = $buildPreset.configurePreset
}

function Get-PresetBinaryDirectory([string] $configureName) {
    $pending = [System.Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($configureName)
    while ($pending.Count -gt 0) {
        $name = $pending.Dequeue()
        $configure = @($presets.configurePresets) | Where-Object { $_.name -eq $name } | Select-Object -First 1
        if (-not $configure) { continue }
        if ($configure.binaryDir) {
            return [System.IO.Path]::GetFullPath(
                ($configure.binaryDir -replace '\$\{sourceDir\}', $repoRoot.Replace('\', '/')))
        }
        foreach ($parent in @($configure.inherits)) {
            if ($parent) { $pending.Enqueue($parent) }
        }
    }
    return $null
}

function Resolve-CMakeTools {
    $cmake = 'cmake'
    $binaryDirectory = Get-PresetBinaryDirectory $configurePreset
    $cache = if ($binaryDirectory) { Join-Path $binaryDirectory 'CMakeCache.txt' }
    if ($cache -and (Test-Path -LiteralPath $cache)) {
        $entry = Select-String -LiteralPath $cache -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$' |
            Select-Object -First 1
        if ($entry -and (Test-Path -LiteralPath $entry.Matches[0].Groups[1].Value)) {
            $cmake = $entry.Matches[0].Groups[1].Value
        }
    }
    $ctest = 'ctest'
    if ($cmake -ne 'cmake') {
        $candidate = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
        if (Test-Path -LiteralPath $candidate) { $ctest = $candidate }
    }
    return $cmake, $ctest
}

function Invoke-Tool([string] $tool, [string[]] $arguments) {
    Write-Host "> $tool $($arguments -join ' ')"
    & $tool @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$tool failed with exit code $LASTEXITCODE."
    }
}

$hasMsvc = $env:VSCMD_ARG_TGT_ARCH -eq $targetArch -and
    (Get-Command cl.exe -ErrorAction SilentlyContinue)
if (-not $hasMsvc) {
    $installPath = Find-VisualStudio
    Write-Host "Using the MSVC $targetArch environment from $installPath"
    Import-MsvcEnvironment $installPath
}

Push-Location $repoRoot
try {
    $cmake, $ctest = Resolve-CMakeTools
    if ($Configure) {
        Invoke-Tool $cmake @('--preset', $configurePreset)
    }
    $buildArguments = @('--build', '--preset', $Preset)
    if ($Target.Count -gt 0) {
        $buildArguments += @('--target') + $Target
    }
    Invoke-Tool $cmake $buildArguments
    if ($Test) {
        Invoke-Tool $ctest @('--preset', $Preset)
    }
}
finally {
    Pop-Location
}
