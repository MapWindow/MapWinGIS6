param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [ValidateSet('Win32', 'x64')]
    [string]$Platform = 'x64',

    [switch]$Automated,

    [string]$ResultsPath,

    [ValidateRange(5, 300)]
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..\..\..')
)
$outputDirectory = Join-Path $repositoryRoot "bin\$Configuration\$Platform"
$controlPath = Join-Path $outputDirectory 'MapWinGIS.ocx'
$hostPath = Join-Path $outputDirectory 'ActiveXTestHost.exe'
$corePath = Join-Path $outputDirectory 'MapWinGIS.Core.dll'

foreach ($requiredFile in @($controlPath, $hostPath, $corePath)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required file was not found: '$requiredFile'. Build the solution for $Configuration|$Platform first."
    }
}

$registrationPaths = @(
    'Software\Classes\CLSID\{2F0AB6FC-F7C0-4E42-A002-A61F9C6F294F}'
    'Software\Classes\MapWinGIS6.Control.6'
    'Software\Classes\MapWinGIS6.Control'
)
$existingRegistration = @()
foreach ($hive in @(
        [Microsoft.Win32.RegistryHive]::CurrentUser,
        [Microsoft.Win32.RegistryHive]::LocalMachine
    )) {
    foreach ($view in @(
            [Microsoft.Win32.RegistryView]::Registry32,
            [Microsoft.Win32.RegistryView]::Registry64
        )) {
        $registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, $view)
        try {
            foreach ($registrationPath in $registrationPaths) {
                $key = $registry.OpenSubKey($registrationPath)
                if ($null -ne $key) {
                    $key.Dispose()
                    $existingRegistration += "$hive\$view\$registrationPath"
                }
            }
        }
        finally {
            $registry.Dispose()
        }
    }
}

if ($existingRegistration.Count -gt 0) {
    throw "COM registration already exists ($($existingRegistration -join ', ')). Refusing to replace or remove another build's registration."
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

$registrationTool = if ($Platform -eq 'x64') {
    Join-Path $env:SystemRoot 'System32\regsvr32.exe'
} else {
    Join-Path $env:SystemRoot 'SysWOW64\regsvr32.exe'
}

$registerResult = Start-Process -FilePath $registrationTool `
    -ArgumentList @('/s', '/n', '/i:user', "`"$controlPath`"") `
    -Wait -PassThru
if ($registerResult.ExitCode -ne 0) {
    throw "Per-user registration failed with exit code $($registerResult.ExitCode)."
}

$hostExitCode = 1
$hostProcess = $null
try {
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

    $hostProcess.WaitForExit()
    $hostExitCode = $hostProcess.ExitCode
}
finally {
    if ($null -ne $hostProcess) {
        $hostProcess.Dispose()
    }

    $unregisterResult = Start-Process -FilePath $registrationTool `
        -ArgumentList @('/s', '/u', '/n', '/i:user', "`"$controlPath`"") `
        -Wait -PassThru
    if ($unregisterResult.ExitCode -ne 0) {
        throw "Per-user unregistration failed with exit code $($unregisterResult.ExitCode)."
    }
}

exit $hostExitCode
