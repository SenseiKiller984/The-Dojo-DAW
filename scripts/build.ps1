<#
.SYNOPSIS
    Configure, build and test The Dojo on Windows using the CMake presets.

.DESCRIPTION
    Requires Visual Studio 2022 or newer with the "Desktop development with C++" workload
    (which includes CMake), plus Git. Run from any PowerShell prompt; no Developer Prompt needed.

.EXAMPLE
    .\scripts\build.ps1                    # Release
    .\scripts\build.ps1 -Config Debug      # Debug (RT allocation trap enabled in the app)
    .\scripts\build.ps1 -Config Debug -Run
    .\scripts\build.ps1 -NoTests
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string] $Config = 'Release',
    [switch] $NoTests,
    [switch] $Run
)

$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

function Invoke-Step([string] $Description, [scriptblock] $Command) {
    Write-Host "==> $Description" -ForegroundColor Cyan
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$Description failed with exit code $LASTEXITCODE" }
}

Invoke-Step 'Configure (windows-msvc)' { cmake --preset windows-msvc }
Invoke-Step "Build ($Config)" { cmake --build build/windows-msvc --config $Config --parallel }

if (-not $NoTests) {
    Invoke-Step "Test ($Config)" { ctest --test-dir build/windows-msvc -C $Config --output-on-failure }
}

$app = "build/windows-msvc/src/app/TheDojo_artefacts/$Config/TheDojo.exe"
Write-Host ""
Write-Host "==> Built: $app" -ForegroundColor Green

if ($Run) { Start-Process -FilePath $app }
