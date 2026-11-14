param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [ValidateSet('Win32', 'x64')]
    [string]$Platform = 'x64',

    [string]$BinDirectory = '',

    [string]$TestExecutable = 'MapWinGIS.ActiveX.Tests.exe'
)

$ErrorActionPreference = 'Stop'

$expectedPointerSize = if ($Platform -eq 'x64') { 8 } else { 4 }
if ([IntPtr]::Size -ne $expectedPointerSize) {
    throw "Run this script in a $Platform PowerShell process to match the control."
}

$binDirectory = if ([string]::IsNullOrWhiteSpace($BinDirectory)) {
    Join-Path $PSScriptRoot "..\..\bin\$Configuration\$Platform"
} else {
    $BinDirectory
}
$binDirectory = [System.IO.Path]::GetFullPath($binDirectory)

$controlPath = Join-Path $binDirectory 'MapWinGIS.ocx'
if (-not (Test-Path -LiteralPath $controlPath -PathType Leaf)) {
    throw "The ActiveX control was not found at '$controlPath'. Build the solution first."
}

$registrationTool = if ($Platform -eq 'x64') {
    Join-Path $env:SystemRoot 'System32\regsvr32.exe'
} else {
    Join-Path $env:SystemRoot 'SysWOW64\regsvr32.exe'
}

$registrationPaths = @(
    'Software\Classes\CLSID\{2F0AB6FC-F7C0-4E42-A002-A61F9C6F294F}'
    'Software\Classes\MapWinGIS6.Control.6'
    'Software\Classes\MapWinGIS6.Control'
)
$testReport = Join-Path ([System.IO.Path]::GetTempPath()) (
    "MapWinGIS.ActiveX.Tests.$([guid]::NewGuid().ToString('N')).xml")
$existingRegistration = @()
foreach ($view in @(
        [Microsoft.Win32.RegistryView]::Registry32,
        [Microsoft.Win32.RegistryView]::Registry64
    )) {
    $userRegistry = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::CurrentUser,
        $view
    )
    try {
        foreach ($registrationPath in $registrationPaths) {
            $registrationKey = $userRegistry.OpenSubKey($registrationPath)
            if ($null -ne $registrationKey) {
                $registrationKey.Dispose()
                $existingRegistration += "$view\$registrationPath"
            }
        }
    }
    finally {
        $userRegistry.Dispose()
    }
}

if ($existingRegistration.Count -gt 0) {
    throw "Per-user COM registration already exists ($($existingRegistration -join ', ')). Refusing to replace or remove it."
}

$registerResult = Start-Process -FilePath $registrationTool `
    -ArgumentList @('/s', '/n', '/i:user', "`"$controlPath`"") `
    -Wait -PassThru
if ($registerResult.ExitCode -ne 0) {
    throw "Per-user registration failed with exit code $($registerResult.ExitCode)."
}

try {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace MapWinGIS.ActiveX.Tests {
    public static class OleRegistration {
        [DllImport("ole32.dll", ExactSpelling = true)]
        public static extern int OleRegGetMiscStatus(
            ref Guid clsid, uint aspect, out uint miscStatus);
    }
}
'@
    $controlClsid = [Guid]'2F0AB6FC-F7C0-4E42-A002-A61F9C6F294F'
    [uint32]$miscStatus = 0
    $miscStatusResult = [MapWinGIS.ActiveX.Tests.OleRegistration]::OleRegGetMiscStatus(
        [ref]$controlClsid, 1, [ref]$miscStatus)
    if ($miscStatusResult -ne 0 -or $miscStatus -ne 913) {
        throw "OleRegGetMiscStatus returned HRESULT 0x$('{0:X8}' -f $miscStatusResult) and flags $miscStatus; expected 913."
    }

    $control = New-Object -ComObject 'MapWinGIS6.Control'
    if ($null -eq $control) {
        throw 'COM activation returned no control instance.'
    }

    if ($control.CoreInitialized -isnot [bool]) {
        throw 'The CoreInitialized automation property returned an unexpected value.'
    }

    [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($control)
    $control = $null

    $testExecutable = Join-Path $binDirectory $TestExecutable
    if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
        throw "The ActiveX hosting test executable was not found at '$testExecutable'. Build the solution first."
    }

    $previousHostTest = $env:MWG_TEST_ACTIVEX_HOST
    $env:MWG_TEST_ACTIVEX_HOST = '1'
    try {
        & $testExecutable `
            '--gtest_filter=ActiveXHosting.ResizesRepaintsAndRecreatesControl' `
            "--gtest_output=xml:$testReport"
        if ($LASTEXITCODE -ne 0) {
            throw "The ActiveX hosting test failed with exit code $LASTEXITCODE."
        }

        if (-not (Test-Path -LiteralPath $testReport -PathType Leaf)) {
            throw 'The ActiveX hosting test did not produce its GoogleTest report.'
        }
        [xml]$testResults = Get-Content -LiteralPath $testReport -Raw
        if ([int]$testResults.DocumentElement.GetAttribute('tests') -lt 1) {
            throw 'The ActiveX hosting test filter selected no tests.'
        }
    }
    finally {
        if ($null -eq $previousHostTest) {
            Remove-Item Env:\MWG_TEST_ACTIVEX_HOST -ErrorAction SilentlyContinue
        } else {
            $env:MWG_TEST_ACTIVEX_HOST = $previousHostTest
        }
    }
}
finally {
    Remove-Item -LiteralPath $testReport -Force -ErrorAction SilentlyContinue
    $unregisterResult = Start-Process -FilePath $registrationTool `
        -ArgumentList @('/s', '/u', '/n', '/i:user', "`"$controlPath`"") `
        -Wait -PassThru
    if ($unregisterResult.ExitCode -ne 0) {
        throw "Per-user unregistration failed with exit code $($unregisterResult.ExitCode)."
    }
}
