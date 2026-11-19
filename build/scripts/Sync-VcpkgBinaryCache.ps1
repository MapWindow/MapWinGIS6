<#
.SYNOPSIS
    Decides whether a vcpkg binary cache must be saved, and prunes it first.

.DESCRIPTION
    Used by the vcpkg dependency CI job around "vcpkg install" with a files
    binary source (VCPKG_BINARY_SOURCES=files,<CacheDir>,readwrite).

    -Record writes the archive names present after the cache restore to
    -SnapshotFile.

    -Finish compares the cache with that snapshot. The cache needs saving when
    vcpkg added archives or when it was restored from a key that does not start
    with -KeyPrefix (an older manifest, triplet or runner image). Before saving,
    archives that no package in -InstalledDir uses are removed, so the cache
    does not grow with every dependency update. Pruning is skipped with a
    warning if an installed package has no archive, which would mean the
    ABI-to-archive mapping no longer holds.

    Writes "changed=true|false" and "added=true|false" to GITHUB_OUTPUT when set.

.PARAMETER CacheDir
    The files binary source directory (<abi[0:2]>\<abi>.zip).

.PARAMETER SnapshotFile
    File that stores the archive names between -Record and -Finish.

.PARAMETER InstalledDir
    One or more vcpkg install roots populated from this cache. Every package
    writes share\<port>\vcpkg_abi_info.txt; its SHA-256 is the package ABI and
    the archive name.

.PARAMETER RestoredKey
    The cache key that was restored ("cache-matched-key"); empty on a miss.

.PARAMETER KeyPrefix
    Prefix shared by all cache keys for the current inputs.

.PARAMETER SkipPrune
    Keep all restored and newly built archives when the vcpkg install failed.
#>
[CmdletBinding(DefaultParameterSetName = 'Finish')]
param(
    [Parameter(Mandatory)]
    [string] $CacheDir,

    [Parameter(Mandatory)]
    [string] $SnapshotFile,

    [Parameter(Mandatory, ParameterSetName = 'Record')]
    [switch] $Record,

    [Parameter(Mandatory, ParameterSetName = 'Finish')]
    [switch] $Finish,

    [Parameter(Mandatory, ParameterSetName = 'Finish')]
    [string[]] $InstalledDir,

    [Parameter(ParameterSetName = 'Finish')]
    [AllowEmptyString()]
    [string] $RestoredKey = '',

    [Parameter(Mandatory, ParameterSetName = 'Finish')]
    [string] $KeyPrefix,

    [Parameter(ParameterSetName = 'Finish')]
    [switch] $SkipPrune
)

$ErrorActionPreference = 'Stop'

function Get-Archives {
    if (-not (Test-Path -LiteralPath $CacheDir)) { return @() }
    return @(Get-ChildItem -LiteralPath $CacheDir -Recurse -File -Filter *.zip)
}

function Format-Size([long] $bytes) { '{0:N0} MB' -f ($bytes / 1MB) }

if ($Record) {
    $names = @(Get-Archives | ForEach-Object { $_.BaseName.ToLowerInvariant() } | Sort-Object)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent ([IO.Path]::GetFullPath($SnapshotFile))) | Out-Null
    Set-Content -LiteralPath $SnapshotFile -Value $names -Encoding utf8
    Write-Host "Recorded $($names.Count) restored archives."
    return
}

$before = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
if (Test-Path -LiteralPath $SnapshotFile) {
    foreach ($name in Get-Content -LiteralPath $SnapshotFile) { if ($name) { [void]$before.Add($name) } }
}

$archives = Get-Archives
$added = @($archives | Where-Object { -not $before.Contains($_.BaseName) })
$staleKey = -not $RestoredKey.StartsWith($KeyPrefix, [StringComparison]::Ordinal)

Write-Host "Archives: $($archives.Count) ($(Format-Size (($archives | Measure-Object Length -Sum).Sum))), new: $($added.Count)."
if ($RestoredKey) { Write-Host "Restored key: $RestoredKey" } else { Write-Host 'No cache was restored.' }

$changed = $archives.Count -gt 0 -and ($added.Count -gt 0 -or $staleKey)
if ($changed) {
    if ($SkipPrune) {
        Write-Host 'Install did not succeed; keeping all archives.'
    }
    else {
        $used = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($root in $InstalledDir) {
            if (-not (Test-Path -LiteralPath $root)) { throw "Install root '$root' does not exist." }
            foreach ($info in Get-ChildItem -Path (Join-Path $root '*\share\*\vcpkg_abi_info.txt') -File) {
                [void]$used.Add((Get-FileHash -LiteralPath $info.FullName -Algorithm SHA256).Hash)
            }
        }

        $present = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($archive in $archives) { [void]$present.Add($archive.BaseName) }
        $missing = @($used | Where-Object { -not $present.Contains($_) })

        if ($used.Count -eq 0 -or $missing.Count -gt 0) {
            Write-Warning (("Not pruning the cache: {0} installed packages, {1} without an archive. " -f $used.Count, $missing.Count) +
                'vcpkg may have changed how archives are named.')
        }
        else {
            $unused = @($archives | Where-Object { -not $used.Contains($_.BaseName) })
            $unused | Remove-Item -Force
            Write-Host "Pruned $($unused.Count) unused archives ($(Format-Size (($unused | Measure-Object Length -Sum).Sum))); keeping $($used.Count)."
        }
    }
}

Write-Host "Save cache: $changed"
if ($env:GITHUB_OUTPUT) {
    $hasAdded = $added.Count -gt 0
    Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value "changed=$($changed.ToString().ToLowerInvariant())"
    Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value "added=$($hasAdded.ToString().ToLowerInvariant())"
}
