# build.ps1 — incremental build for VoiceToText
#
# Usage:  pwsh ./myBuild/build.ps1
#
# Imports the MSVC build environment via vcvarsall.bat, then invokes CMake
# in --build mode against the existing build/ directory at the project root.
# For a clean (re)configure, use rebuild.bat in the project root instead.

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir    = Join-Path $projectRoot 'build'

$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat'
$cmake  = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

if (-not (Test-Path $buildDir)) {
    Write-Error "Build directory not found: $buildDir`nRun rebuild.bat first to configure CMake."
    exit 1
}

# Import the MSVC env into the current PowerShell session so cl.exe / link.exe
# are on PATH when ninja invokes them.
$envDump = cmd /c "`"$vcvars`" amd64 > nul 2>&1 && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "Env:$($matches[1])" -Value $matches[2]
    }
}

$jobs = $env:NUMBER_OF_PROCESSORS
& $cmake --build $buildDir --config Release --parallel $jobs
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

Write-Host "Build succeeded." -ForegroundColor Green
