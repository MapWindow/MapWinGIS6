<#
.SYNOPSIS
    Measures compile, link and binary parity between the MSBuild and CMake builds.

.DESCRIPTION
    Collects the effective compiler and linker command lines of a build,
    normalises them (paths relative to the repository, build-directory paths
    replaced by placeholders, option order sorted) and writes a JSON report.

    The MSBuild side is read from a detailed MSBuild file log. Without
    -MSBuildLog, the script builds MapWinGIS6.slnx itself and records that log.
    The CMake side is optional: pass -CMakeBuildDir to read its
    compile_commands.json and, with -CMakeBuildLog, the link lines of a verbose
    build (`cmake --build <dir> -- -v`). When both sides are present, each
    translation unit and each linked binary is diffed and every option that only
    one build uses is reported.

    The report also contains the warning count of each build and, with
    -Binaries, a dumpbin summary (dependents, exports, subsystem, DLL
    characteristics) of each binary in both output folders.

    MSBuild is located in this order: -MSBuild, the MWG_MSBUILD environment
    variable, msbuild on PATH, then vswhere.

.EXAMPLE
    pwsh -File build\scripts\Compare-CompileCommands.ps1 -Configuration Debug -Platform x64

    Builds the solution with MSBuild and writes the baseline report.

.EXAMPLE
    pwsh -File build\scripts\Compare-CompileCommands.ps1 -MSBuildLog out\parity\msbuild-Debug-x64.log `
        -CMakeBuildDir out\b\x64d -CMakeBuildLog out\parity\cmake-x64d.log -Binaries MapWinGIS.Core.dll

    Compares an existing MSBuild log with a CMake Ninja build.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Debug',

    [ValidateSet('x64', 'Win32')]
    [string] $Platform = 'x64',

    [string] $MSBuild,

    [string] $MSBuildLog,

    [string] $MSBuildBinDirectory,

    [string] $CMakeBuildDir,

    [string] $CMakeBuildLog,

    [string] $CMakeBinDirectory,

    [string[]] $Binaries = @(),

    [string] $OutputPath,

    [switch] $FailOnLostFlags
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$parityDir = Join-Path $repoRoot 'out\parity'

# Options that only control logging, parallelism or object/PDB file names, so
# they say nothing about the generated code.
$ignoredOptions = @(
    '/c', '/nologo', '/MP', '/FC', '/errorReport:queue', '/ERRORREPORT:QUEUE', '/NOLOGO',
    '/diagnostics:column', '/diagnostics:caret', '/showIncludes', '/FS'
)
$ignoredOptionPrefixes = @('/Fo', '/Fd', '/Fe', '/Fp', '/ILK:', '/PDB:', '/IMPLIB:', '/OUT:', '/MP')

# Link differences that are deliberate (docs/CMAKE-MIGRATION.md). They are
# reported under 'explained' and do not count as lost options.
$explainedLinkDifferences = [ordered]@{
    '^/manifest(:embed|uac:.*)?$'      = 'CMake''s vs_link wrapper generates and embeds the manifest with mt.exe; the embedded manifest is identical'
    '^/tlbid:1$'                       = 'Only used when a type library resource is embedded'
    '^/libpath:<vcpkg_installed>/'     = 'vcpkg auto-link replaced by imported targets with full library paths'
    '^lib:<vcpkg_installed>/.*\*\.lib$' = 'vcpkg auto-link replaced by imported targets with full library paths'
    '^lib:<vcpkg_installed>/'          = 'Library from an imported target (find_package)'
    '^lib:odbc(cp)?32\.lib$'           = 'MSBuild default library; unused, not in CMake''s default list'
    '^/version:0\.0$'                  = 'CMake default image version; MSBuild leaves it at 0.0 too'
    '^input:.*\.res$'                  = 'Compiled resource file name (<name>.rc.res in CMake)'
    '^lib:(<builddir>/)?lib[\\/]mapwingis\.core\.lib$' = 'Core import library from target_link_libraries (MSBuild: project reference)'
}

# Compile differences that are deliberate (docs/CMAKE-MIGRATION.md).
$explainedCompileDifferences = [ordered]@{
    '^/I<intdir>$'                                = 'MIDL output moved from the intermediate directory to <build>/.../midl (D3)'
    '^/I<builddir>/.*/midl$'                      = 'MIDL output directory (D3)'
    '^/I<vcpkg_installed>/[^/]+/include$'         = 'vcpkg auto-include; targets without a vcpkg dependency do not get it, the others get it from imported targets'
    '^/I<vcpkg_installed>/[^/]+/include/geos$'    = 'Interface include directory of GEOS::geos_c'
    '^/DGDAL_DEBUG$'                              = 'Interface definition of the debug GDAL::GDAL'
}

# MSBuild project names that differ from the CMake target names.
$targetNames = @{ 'Translate' = 'MapWinGIS.Samples.Translate' }

function Find-MSBuild {
    if ($MSBuild) { return $MSBuild }
    if ($env:MWG_MSBUILD) { return $env:MWG_MSBUILD }

    $onPath = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
            Select-Object -First 1
        if ($found) { return $found }
    }

    throw 'MSBuild was not found. Set MWG_MSBUILD, pass -MSBuild, or run from a Developer PowerShell.'
}

function Find-Dumpbin {
    $onPath = Get-Command dumpbin -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
        if ($found) { return $found }
    }
    return $null
}

# Splits a command line the way the MSVC tools do: whitespace separates
# arguments, double quotes group them, and \" is a literal quote.
function Split-CommandLine([string] $line) {
    $tokens = [System.Collections.Generic.List[string]]::new()
    $current = [System.Text.StringBuilder]::new()
    $inQuotes = $false
    $hasToken = $false
    for ($i = 0; $i -lt $line.Length; $i++) {
        $ch = $line[$i]
        if ($ch -eq '\') {
            # 2n backslashes before a quote are n backslashes and a delimiter,
            # 2n+1 are n backslashes and a literal quote; otherwise they are literal.
            $count = 0
            while ($i -lt $line.Length -and $line[$i] -eq '\') { $count++; $i++ }
            if ($i -lt $line.Length -and $line[$i] -eq '"') {
                [void]$current.Append([char]'\', [int][Math]::Floor($count / 2))
                if ($count % 2 -eq 1) { [void]$current.Append('"') } else { $inQuotes = -not $inQuotes }
            }
            else {
                [void]$current.Append([char]'\', $count)
                $i--
            }
            $hasToken = $true
            continue
        }
        if ($ch -eq '"') { $inQuotes = -not $inQuotes; $hasToken = $true; continue }
        if (-not $inQuotes -and [char]::IsWhiteSpace($ch)) {
            if ($hasToken) { $tokens.Add($current.ToString()); [void]$current.Clear(); $hasToken = $false }
            continue
        }
        [void]$current.Append($ch); $hasToken = $true
    }
    if ($hasToken) { $tokens.Add($current.ToString()) }
    return $tokens
}

function Get-NormalizedPath([string] $path, [string] $baseDir, [hashtable] $placeholders) {
    $path = $path.Trim().TrimEnd('\', '/')
    if (-not [System.IO.Path]::IsPathRooted($path) -and $baseDir) {
        $path = Join-Path $baseDir $path
    }
    try { $path = [System.IO.Path]::GetFullPath($path) } catch { }
    $path = $path.TrimEnd('\', '/')

    # Longest prefix first, so build directories win over the repository root.
    foreach ($key in ($placeholders.Keys | Sort-Object Length -Descending)) {
        if ($path.StartsWith($key, [System.StringComparison]::OrdinalIgnoreCase)) {
            $rest = $path.Substring($key.Length).TrimStart('\', '/')
            $value = $placeholders[$key]
            return $(if ($rest) { "$value/$($rest -replace '\\', '/')" } else { $value })
        }
    }
    return ($path -replace '\\', '/')
}

# Turns raw arguments into a sorted set of normalised options. Options with a
# separate argument (/D X, /I X) are joined, '-' is mapped to '/', and paths
# become placeholders such as <repo>, <vcpkg>, <intdir> or <bindir>.
function ConvertTo-OptionSet([string[]] $arguments, [string] $baseDir, [hashtable] $placeholders,
                             [string[]] $sourceExtensions) {
    $options = [System.Collections.Generic.List[string]]::new()
    $sources = [System.Collections.Generic.List[string]]::new()
    # Compiler and linker share the argument list here, so each prefix only
    # applies to the tool that owns it (/I is a compiler option, /INCREMENTAL is not a path).
    $isLink = $sourceExtensions -contains '.obj'
    $pathOptions = if ($isLink) { @('/LIBPATH:', '/DEF:', '/MANIFESTINPUT:', '/WHOLEARCHIVE:') }
                   else { @('/I', '/external:I', '/FI') }
    $joined = if ($isLink) { @() } else { @('/D', '/I', '/U', '/external:I', '/FI') }

    for ($i = 0; $i -lt $arguments.Count; $i++) {
        $arg = $arguments[$i]
        if ($arg.StartsWith('-')) { $arg = '/' + $arg.Substring(1) }
        if ($arg.StartsWith('@')) { continue }

        if ($joined -contains $arg -and $i + 1 -lt $arguments.Count) {
            $arg = $arg + $arguments[++$i]
        }

        if (-not $arg.StartsWith('/')) {
            $extension = [System.IO.Path]::GetExtension($arg)
            if ($sourceExtensions -contains $extension) {
                $sources.Add((Get-NormalizedPath $arg $baseDir $placeholders))
            }
            elseif ($extension -eq '.lib' -or $arg.EndsWith('*.lib')) {
                $name = if ([System.IO.Path]::IsPathRooted($arg)) {
                    Get-NormalizedPath $arg $baseDir $placeholders
                } else { $arg.ToLowerInvariant() }
                $options.Add("lib:$name")
            }
            elseif ($extension -in @('.res', '.def')) {
                $options.Add("input:$([System.IO.Path]::GetFileName($arg))")
            }
            continue
        }

        if ($ignoredOptions -contains $arg) { continue }
        $skip = $false
        foreach ($prefix in $ignoredOptionPrefixes) {
            if ($arg.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) { $skip = $true; break }
        }
        if ($skip) { continue }

        foreach ($prefix in $pathOptions) {
            if ($arg.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) -and $arg.Length -gt $prefix.Length) {
                $arg = $prefix + (Get-NormalizedPath $arg.Substring($prefix.Length) $baseDir $placeholders)
                break
            }
        }
        # Linker options are case-insensitive (/DEBUG and /debug are the same).
        if ($isLink) { $arg = $arg.ToLowerInvariant() }
        $options.Add($arg)
    }

    return [pscustomobject]@{
        Options = @($options | Sort-Object -Unique -CaseSensitive)
        Sources = @($sources)
    }
}

function Get-Placeholders([string] $intRoot, [string] $binDir, [string] $buildDir) {
    $map = @{}
    $map[$repoRoot.TrimEnd('\')] = '<repo>'
    foreach ($installed in @('vcpkg_installed')) {
        $dir = Join-Path $repoRoot $installed
        $map[$dir] = '<vcpkg_installed>'
    }
    if ($buildDir) {
        $map[$buildDir.TrimEnd('\')] = '<builddir>'
        $map[(Join-Path $buildDir 'vcpkg_installed')] = '<vcpkg_installed>'
    }
    if ($intRoot) { $map[$intRoot.TrimEnd('\')] = '<intdir>' }
    if ($binDir) { $map[$binDir.TrimEnd('\')] = '<bindir>' }
    return $map
}

# vcpkg install trees differ between the builds (vcpkg_installed\x64 for
# MSBuild, vcpkg_installed\cmake-x64 or similar for CMake). Collapse the
# platform folder so both report <vcpkg_installed>/<triplet>/...
function Merge-VcpkgPlatform([string[]] $options) {
    return @($options | ForEach-Object {
        $_ -replace '<vcpkg_installed>/(cmake-)?(x64|Win32|x86)/', '<vcpkg_installed>/'
    } | Sort-Object -Unique -CaseSensitive)
}

function Read-MSBuildCommands([string] $logPath, [string] $binDir) {
    $compiles = @{}
    $links = @{}
    $projectDirs = @{}
    Get-ChildItem -Path $repoRoot -Recurse -Filter *.vcxproj -File |
        Where-Object { $_.FullName -notmatch '\\(out|obj|bin|vcpkg_installed)\\' } |
        ForEach-Object { $projectDirs[$_.BaseName] = $_.DirectoryName }

    $objRoot = Join-Path $repoRoot 'obj'
    foreach ($line in [System.IO.File]::ReadLines($logPath)) {
        $match = [regex]::Match($line, '^\s*(?<exe>[A-Za-z]:\\.*?\\(?<tool>CL|link)\.exe)\s+(?<args>.*)$',
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        if (-not $match.Success) { continue }

        $arguments = Split-CommandLine $match.Groups['args'].Value
        $tool = $match.Groups['tool'].Value.ToLowerInvariant()

        if ($tool -eq 'cl') {
            $fo = $arguments | Where-Object { $_ -match '^[/-]Fo' } | Select-Object -First 1
            if (-not $fo) { continue }
            $intDir = $fo.Substring(3).TrimEnd('\')
            $project = ($intDir.Substring($objRoot.Length).TrimStart('\') -split '\\')[0]
            $baseDir = $projectDirs[$project]
            $target = if ($targetNames.ContainsKey($project)) { $targetNames[$project] } else { $project }
            $placeholders = Get-Placeholders $intDir $binDir $null
            $set = ConvertTo-OptionSet $arguments $baseDir $placeholders @('.c', '.cpp', '.cxx', '.cc')
            foreach ($source in $set.Sources) {
                # Keyed by target: the tests compile some ActiveX sources a second time.
                $compiles[(Get-SourceKey $target $source)] = [pscustomobject]@{
                    Target  = $target
                    Source  = $source
                    Options = Merge-VcpkgPlatform $set.Options
                }
            }
        }
        else {
            $out = $arguments | Where-Object { $_ -match '^[/-]OUT:' } | Select-Object -First 1
            if (-not $out) { continue }
            $binary = [System.IO.Path]::GetFileName($out.Substring(5))
            # Object files are logged on separate lines; any argument inside obj\<Project>\ names the project.
            $intFile = $arguments | ForEach-Object { $_ -replace '^[/-][A-Za-z]+:', '' } |
                Where-Object { $_.StartsWith($objRoot, [System.StringComparison]::OrdinalIgnoreCase) } |
                Select-Object -First 1
            $intDir = if ($intFile) { Split-Path $intFile -Parent } else { $null }
            $project = if ($intDir) { ($intDir.Substring($objRoot.Length).TrimStart('\') -split '\\')[0] } else { $null }
            $baseDir = if ($project) { $projectDirs[$project] } else { $null }
            $placeholders = Get-Placeholders $intDir $binDir $null
            $set = ConvertTo-OptionSet $arguments $baseDir $placeholders @('.obj')
            $links[$binary] = Merge-VcpkgPlatform $set.Options
        }
    }

    return [pscustomobject]@{ Compiles = $compiles; Links = $links }
}

function Read-CMakeCommands([string] $buildDir, [string] $logPath, [string] $binDir) {
    $compiles = @{}
    $links = @{}

    $database = Join-Path $buildDir 'compile_commands.json'
    if (-not (Test-Path -LiteralPath $database)) {
        throw "No compile_commands.json in $buildDir. Configure with CMAKE_EXPORT_COMPILE_COMMANDS=ON (Ninja generator)."
    }

    foreach ($entry in (Get-Content -Raw -LiteralPath $database | ConvertFrom-Json)) {
        # Resource compiles are in the database too; the MSBuild side has only cl.exe.
        if ($entry.file -match '\.rc$') { continue }
        $arguments = if ($entry.arguments) { @($entry.arguments | Select-Object -Skip 1) }
                     else { @(Split-CommandLine $entry.command | Select-Object -Skip 1) }
        $fo = $arguments | Where-Object { $_ -match '^[/-]Fo' } | Select-Object -First 1
        $intDir = if ($fo) { Split-Path ([System.IO.Path]::GetFullPath((Join-Path $entry.directory $fo.Substring(3)))) -Parent } else { $null }
        $placeholders = Get-Placeholders $intDir $binDir $buildDir
        $set = ConvertTo-OptionSet $arguments $entry.directory $placeholders @('.c', '.cpp', '.cxx', '.cc')
        $source = Get-NormalizedPath $entry.file $entry.directory $placeholders
        $target = if ($intDir -match '\\CMakeFiles\\(?<t>[^\\]+)\.dir') { $Matches['t'] } else { '' }
        $compiles[(Get-SourceKey $target $source)] = [pscustomobject]@{
            Target  = $target
            Source  = $source
            Options = Merge-VcpkgPlatform $set.Options
        }
    }

    if ($logPath) {
        foreach ($line in [System.IO.File]::ReadLines($logPath)) {
            # Ninja wraps links in "cmake -E vs_link_dll|vs_link_exe ... -- link.exe ..."
            $match = [regex]::Match($line, '(?:^|\s|")(?<exe>[^\s"]*link\.exe"?)\s+(?<args>.*)$',
                [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
            if (-not $match.Success) { continue }
            $arguments = Split-CommandLine $match.Groups['args'].Value
            $out = $arguments | Where-Object { $_ -match '^[/-]out:' } | Select-Object -First 1
            if (-not $out) { continue }
            $binary = [System.IO.Path]::GetFileName($out.Substring(5))
            $placeholders = Get-Placeholders $null $binDir $buildDir
            $set = ConvertTo-OptionSet $arguments $buildDir $placeholders @('.obj')
            $links[$binary] = Merge-VcpkgPlatform $set.Options
        }
    }

    return [pscustomobject]@{ Compiles = $compiles; Links = $links }
}

function Get-WarningCount([string] $logPath) {
    if (-not $logPath -or -not (Test-Path -LiteralPath $logPath)) { return $null }
    $unique = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($line in [System.IO.File]::ReadLines($logPath)) {
        $match = [regex]::Match($line, '(?<loc>[^\s>][^>]*?\(\d+(?:,\d+)?\))?\s*:\s*warning\s+(?<code>[A-Z]+\d+)\s*:(?<msg>[^\[]*)')
        if ($match.Success) {
            # Detailed logs repeat each warning in the project summary, so count unique ones.
            [void]$unique.Add(($match.Groups['loc'].Value.Trim() + '|' + $match.Groups['code'].Value + '|' + $match.Groups['msg'].Value.Trim()))
        }
    }
    return $unique.Count
}

function Get-BinarySummary([string] $dumpbin, [string] $path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }

    $dependents = @(& $dumpbin /nologo /dependents $path |
        Where-Object { $_ -match '^\s+\S+\.(dll|DLL|ocx|exe)\s*$' } |
        ForEach-Object { $_.Trim().ToLowerInvariant() } | Sort-Object -Unique)

    $exports = @(& $dumpbin /nologo /exports $path |
        ForEach-Object {
            $m = [regex]::Match($_, '^\s+\d+\s+[0-9A-F]+\s+(?:[0-9A-F]{8}\s+)?(?<name>\S+)')
            if ($m.Success) { $m.Groups['name'].Value }
        } | Sort-Object -Unique -CaseSensitive)

    $headers = & $dumpbin /nologo /headers $path
    $subsystem = ($headers | Where-Object { $_ -match 'subsystem \(' } | Select-Object -First 1)
    $characteristics = @($headers |
        Where-Object { $_ -match '^\s+(Dynamic base|NX compatible|High Entropy Virtual Addresses|Control Flow Guard|Guard|Terminal Server Aware|Application can handle large)' } |
        ForEach-Object { $_.Trim() } | Sort-Object -Unique)

    return [pscustomobject]@{
        Dependents         = $dependents
        Exports            = $exports
        Subsystem          = if ($subsystem) { $subsystem.Trim() } else { $null }
        DllCharacteristics = $characteristics
    }
}

function Compare-OptionSets($reference, $actual) {
    $lost = @($reference | Where-Object { $actual -cnotcontains $_ })
    $added = @($actual | Where-Object { $reference -cnotcontains $_ })
    return [pscustomobject]@{ Lost = $lost; Added = $added }
}

# Moves the differences that match a pattern into 'Explained', with the reason.
function Split-ExplainedDifferences($diff, $patterns) {
    $explained = [ordered]@{}
    $lost = @(); $added = @()
    foreach ($pair in @(@{ Items = $diff.Lost; Kind = 'lost' }, @{ Items = $diff.Added; Kind = 'added' })) {
        foreach ($option in $pair.Items) {
            $reason = $null
            foreach ($pattern in $patterns.Keys) {
                if ($option -match $pattern) { $reason = $patterns[$pattern]; break }
            }
            if ($reason) { $explained["$($pair.Kind) $option"] = $reason }
            elseif ($pair.Kind -eq 'lost') { $lost += $option }
            else { $added += $option }
        }
    }
    return [pscustomobject]@{ Lost = $lost; Added = $added; Explained = $explained }
}

# MIDL output lives in different directories in the two builds (D3); key the
# generated sources by file name.
function Get-SourceKey([string] $target, [string] $source) {
    if ($source -match '^<(intdir|builddir)>/(?:.*/)?(?<file>[^/]+_i\.c)$') { $source = "<generated>/$($Matches['file'])" }
    return "$target $source"
}

# --- MSBuild baseline ------------------------------------------------------

if (-not $MSBuildBinDirectory) {
    $MSBuildBinDirectory = Join-Path $repoRoot "bin\$Configuration\$Platform"
}

if (-not $MSBuildLog) {
    New-Item -ItemType Directory -Force -Path $parityDir | Out-Null
    $MSBuildLog = Join-Path $parityDir "msbuild-$Configuration-$Platform.log"
    $msbuildExe = Find-MSBuild
    Write-Host "MSBuild: $msbuildExe"
    # A rebuild guarantees that every translation unit appears in the log.
    & $msbuildExe (Join-Path $repoRoot 'MapWinGIS6.slnx') -restore '-t:Rebuild' "-p:Configuration=$Configuration" `
        "-p:Platform=$Platform" -m '-v:m' "-flp:logfile=$MSBuildLog;verbosity=detailed"
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE" }
}

$MSBuildLog = (Resolve-Path -LiteralPath $MSBuildLog).Path
$msbuildCommands = Read-MSBuildCommands $MSBuildLog $MSBuildBinDirectory

$report = [ordered]@{
    configuration = $Configuration
    platform      = $Platform
    msbuild       = [ordered]@{
        log      = $MSBuildLog
        warnings = Get-WarningCount $MSBuildLog
        compiles = $msbuildCommands.Compiles
        links    = $msbuildCommands.Links
    }
}

# --- CMake side and diff ---------------------------------------------------

$lostTotal = 0
if ($CMakeBuildDir) {
    $CMakeBuildDir = (Resolve-Path -LiteralPath $CMakeBuildDir).Path
    if (-not $CMakeBinDirectory) { $CMakeBinDirectory = Join-Path $CMakeBuildDir 'bin' }
    if ($CMakeBuildLog) { $CMakeBuildLog = (Resolve-Path -LiteralPath $CMakeBuildLog).Path }
    $cmakeCommands = Read-CMakeCommands $CMakeBuildDir $CMakeBuildLog $CMakeBinDirectory

    $compileDiff = [ordered]@{}
    # Targets that CMake does not build yet are skipped, so one phase can be
    # measured before the later targets exist.
    $cmakeTargets = @($cmakeCommands.Compiles.Values | ForEach-Object Target | Sort-Object -Unique)
    foreach ($key in ($msbuildCommands.Compiles.Keys | Sort-Object)) {
        $reference = $msbuildCommands.Compiles[$key]
        if ($cmakeTargets -notcontains $reference.Target) { continue }
        $actual = $cmakeCommands.Compiles[$key]
        if (-not $actual) {
            $compileDiff[$key] = 'missing in CMake'
            $lostTotal++
            continue
        }
        $diff = Compare-OptionSets $reference.Options $actual.Options
        if ($diff.Lost.Count -or $diff.Added.Count) {
            $split = Split-ExplainedDifferences $diff $explainedCompileDifferences
            $compileDiff[$key] = $split
            $lostTotal += $split.Lost.Count
        }
    }
    foreach ($key in ($cmakeCommands.Compiles.Keys | Where-Object { -not $msbuildCommands.Compiles.ContainsKey($_) })) {
        $compileDiff[$key] = 'only in CMake'
    }

    $linkDiff = [ordered]@{}
    foreach ($binary in ($msbuildCommands.Links.Keys | Sort-Object)) {
        if (-not $cmakeCommands.Links.ContainsKey($binary)) {
            # A binary that CMake builds but that was up to date has no link
            # line in the log; that is not parity.
            $cmakeTarget = [System.IO.Path]::GetFileNameWithoutExtension($binary)
            if ($CMakeBuildLog -and ($cmakeTargets -contains $cmakeTarget)) {
                Write-Warning "$binary was not linked in $CMakeBuildLog; delete it and rebuild to compare its link options."
            }
            continue
        }
        $diff = Compare-OptionSets $msbuildCommands.Links[$binary] $cmakeCommands.Links[$binary]
        if ($diff.Lost.Count -or $diff.Added.Count) {
            $split = Split-ExplainedDifferences $diff $explainedLinkDifferences
            $linkDiff[$binary] = $split
            $lostTotal += $split.Lost.Count
        }
    }

    $report.cmake = [ordered]@{
        buildDir = $CMakeBuildDir
        log      = $CMakeBuildLog
        warnings = Get-WarningCount $CMakeBuildLog
        compiles = $cmakeCommands.Compiles
        links    = $cmakeCommands.Links
    }
    $report.diff = [ordered]@{ compiles = $compileDiff; links = $linkDiff }
}

# --- Binaries --------------------------------------------------------------

# pwsh -File passes "a,b" as one string.
$Binaries = @($Binaries | ForEach-Object { $_ -split ',' } | ForEach-Object Trim | Where-Object { $_ })
if ($Binaries.Count) {
    $dumpbin = Find-Dumpbin
    if (-not $dumpbin) { throw 'dumpbin.exe was not found; install the MSVC build tools or run from a Developer PowerShell.' }
    $binaryReport = [ordered]@{}
    foreach ($binary in $Binaries) {
        $entry = [ordered]@{ msbuild = Get-BinarySummary $dumpbin (Join-Path $MSBuildBinDirectory $binary) }
        if ($CMakeBuildDir) { $entry.cmake = Get-BinarySummary $dumpbin (Join-Path $CMakeBinDirectory $binary) }
        $binaryReport[$binary] = $entry
    }
    $report.binaries = $binaryReport
}

# --- Output ----------------------------------------------------------------

if (-not $OutputPath) {
    New-Item -ItemType Directory -Force -Path $parityDir | Out-Null
    $OutputPath = Join-Path $parityDir "parity-$Configuration-$Platform.json"
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding utf8

Write-Host ''
Write-Host "Configuration:       $Configuration|$Platform"
Write-Host "MSBuild TUs/links:   $($msbuildCommands.Compiles.Count) / $($msbuildCommands.Links.Count)"
Write-Host "MSBuild warnings:    $($report.msbuild.warnings)"
if ($CMakeBuildDir) {
    Write-Host "CMake TUs/links:     $($report.cmake.compiles.Count) / $($report.cmake.links.Count)"
    Write-Host "CMake warnings:      $($report.cmake.warnings)"
    Write-Host "Lost options:        $lostTotal"
}
Write-Host "Report:              $OutputPath"

if ($FailOnLostFlags -and $lostTotal -gt 0) { exit 1 }
