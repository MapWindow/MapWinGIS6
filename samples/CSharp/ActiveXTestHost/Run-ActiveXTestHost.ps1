param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [ValidateSet('Win32', 'x64')]
    [string]$Platform = 'x64',

    [string]$BinDirectory,

    [switch]$Automated,

    [string]$ResultsPath,

    [ValidateRange(5, 300)]
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..\..\..')
)
if ([string]::IsNullOrWhiteSpace($BinDirectory)) {
    $outputDirectory = Join-Path $repositoryRoot "bin\$Configuration\$Platform"
}
else {
    $outputDirectory = [System.IO.Path]::GetFullPath($BinDirectory)
}
$controlPath = Join-Path $outputDirectory 'MapWinGIS.ocx'
$hostPath = Join-Path $outputDirectory 'ActiveXTestHost.exe'
$corePath = Join-Path $outputDirectory 'MapWinGIS.Core.dll'

foreach ($requiredFile in @($controlPath, $hostPath, $corePath)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required file was not found: '$requiredFile'. Build the host and native control for $Configuration|$Platform first."
    }
}

$hostArguments = @()
if ($Automated) {
    $hostArguments += '--automated'
    $hostArguments += '--timeout-seconds'
    $hostArguments += "$TimeoutSeconds"
    if ([string]::IsNullOrWhiteSpace($ResultsPath)) {
        $ResultsPath = Join-Path $outputDirectory 'ActiveXTestHost-results.json'
    }
    $hostArguments += '--results'
    $hostArguments += [System.IO.Path]::GetFullPath($ResultsPath)
} elseif (-not [string]::IsNullOrWhiteSpace($ResultsPath)) {
    throw '-ResultsPath can only be used with -Automated.'
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new($hostPath)
$startInfo.WorkingDirectory = $outputDirectory
$startInfo.UseShellExecute = $false
foreach ($argument in $hostArguments) {
    $startInfo.ArgumentList.Add($argument)
}

$hostProcess = [System.Diagnostics.Process]::Start($startInfo)
if ($null -eq $hostProcess) {
    throw 'The ActiveX test host process could not be started.'
}

try {
    $hostProcess.WaitForExit()
    exit $hostProcess.ExitCode
}
finally {
    $hostProcess.Dispose()
}
