param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [ValidateSet('Win32', 'x64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'

$expectedPointerSize = if ($Platform -eq 'x64') { 8 } else { 4 }
if ([IntPtr]::Size -ne $expectedPointerSize) {
    throw "Run this script in a $Platform PowerShell process to match the control."
}

$controlPath = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot "..\..\bin\$Configuration\$Platform\MapWinGIS.ocx")
)
if (-not (Test-Path -LiteralPath $controlPath -PathType Leaf)) {
    throw "The ActiveX control was not found at '$controlPath'. Build the solution first."
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

try {
    $control = New-Object -ComObject 'MapWinGIS6.Control'
    if ($null -eq $control) {
        throw 'COM activation returned no control instance.'
    }

    if ($control.CoreInitialized -isnot [bool]) {
        throw 'The CoreInitialized automation property returned an unexpected value.'
    }

    [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($control)
    $control = $null
}
finally {
    $unregisterResult = Start-Process -FilePath $registrationTool `
        -ArgumentList @('/s', '/u', '/n', '/i:user', "`"$controlPath`"") `
        -Wait -PassThru
    if ($unregisterResult.ExitCode -ne 0) {
        throw "Per-user unregistration failed with exit code $($unregisterResult.ExitCode)."
    }
}
