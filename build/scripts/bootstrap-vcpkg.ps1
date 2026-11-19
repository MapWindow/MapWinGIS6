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

.PARAMETER Baseline
    Checks out the vcpkg commit named by "builtin-baseline" in vcpkg.json
    (detached HEAD) instead of the default branch. Package ABI hashes include
    vcpkg's scripts, so CI uses this to keep binary caches valid until the
    baseline is changed deliberately. The clone keeps full history because
    vcpkg resolves older port versions from it.
#>
[CmdletBinding()]
param(
    [string] $VcpkgRoot,
    [switch] $Update,
    [switch] $Baseline
)

$ErrorActionPreference = 'Stop'

if ($Update -and $Baseline) { throw '-Update and -Baseline cannot be combined.' }

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Invoke-Git {
    & git @args
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed with exit code $LASTEXITCODE" }
}

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

$vcpkgUrl = 'https://github.com/microsoft/vcpkg.git'
$checkedOut = $false

if ($Baseline) {
    $manifest = Get-Content -Raw (Join-Path $repoRoot 'vcpkg.json') | ConvertFrom-Json
    $commit = $manifest.'builtin-baseline'
    if ($commit -notmatch '^[0-9a-f]{40}$') { throw "vcpkg.json has no valid builtin-baseline: '$commit'." }

    # vcpkg reads older port versions from git history, so the clone must not be shallow.
    $cloned = $false
    if (-not (Test-Path (Join-Path $root '.vcpkg-root'))) {
        Write-Host 'Cloning vcpkg...'
        Invoke-Git clone --quiet --no-checkout $vcpkgUrl $root
        $cloned = $true
    }
    elseif ((git -C $root rev-parse --is-shallow-repository) -eq 'true') {
        Write-Host 'Unshallowing vcpkg...'
        Invoke-Git -C $root fetch --quiet --unshallow origin
    }

    if ($cloned -or (git -C $root rev-parse HEAD 2>$null) -ne $commit) {
        Write-Host "Checking out vcpkg $commit..."
        git -C $root cat-file -e "$commit^{commit}" 2>$null
        if ($LASTEXITCODE -ne 0) { Invoke-Git -C $root fetch --quiet origin $commit }
        Invoke-Git -C $root checkout --quiet --detach $commit
        $checkedOut = $true
    }
}
elseif (-not (Test-Path (Join-Path $root '.vcpkg-root'))) {
    Write-Host 'Cloning vcpkg...'
    git clone $vcpkgUrl $root
}
elseif ($Update) {
    Write-Host 'Updating vcpkg...'
    git -C $root pull --ff-only
}

$exe = Join-Path $root 'vcpkg.exe'
# A different checkout may require a different vcpkg tool version.
if (-not (Test-Path $exe) -or $Update -or $checkedOut) {
    Write-Host 'Bootstrapping vcpkg...'
    & (Join-Path $root 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "bootstrap-vcpkg.bat failed with exit code $LASTEXITCODE" }
}

& $exe version

Write-Host ''
Write-Host 'Done. MapWinGIS6 will find this vcpkg automatically.'
Write-Host 'To use a different location, set MWG_VCPKG_ROOT or pass /p:MwgVcpkgRoot=<path> to MSBuild.'
