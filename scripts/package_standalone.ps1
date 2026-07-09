param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [string]$Configuration = "Release",
    [string]$OutputDir = (Join-Path $PSScriptRoot "..\dist\VoiceToText")
)

$ErrorActionPreference = "Stop"

$binDir = Join-Path $BuildDir "bin\$Configuration"
if (-not (Test-Path (Join-Path $binDir "VoiceToText.exe"))) {
    throw "VoiceToText.exe was not found in $binDir. Build the app first."
}

if (Test-Path $OutputDir) {
    Remove-Item -LiteralPath $OutputDir -Recurse -Force
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $OutputDir "models") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $OutputDir "scripts") -Force | Out-Null

$files = @(
    "VoiceToText.exe",
    "whisper.dll",
    "parakeet.dll",
    "ggml.dll",
    "ggml-base.dll",
    "ggml-cpu.dll",
    "ggml-cuda.dll",
    "cublas64_13.dll",
    "cublasLt64_13.dll",
    "cublas64_12.dll",
    "cublasLt64_12.dll"
)

foreach ($file in $files) {
    $source = Join-Path $binDir $file
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDir $file) -Force
    }
}

foreach ($script in @("download_model.ps1", "download_gemma_model.ps1", "install_llama_server.ps1")) {
    $source = Join-Path $PSScriptRoot $script
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDir "scripts\$script") -Force
    }
}

$resourcesSource = Join-Path $PSScriptRoot "..\resources"
if (Test-Path $resourcesSource) {
    Copy-Item -LiteralPath $resourcesSource `
        -Destination (Join-Path $OutputDir "resources") -Recurse -Force
}

$llamaTools = Join-Path $PSScriptRoot "..\tools\llama.cpp"
if (Test-Path (Join-Path $llamaTools "llama-server.exe")) {
    New-Item -ItemType Directory -Path (Join-Path $OutputDir "tools\llama.cpp") -Force | Out-Null
    Copy-Item -Path (Join-Path $llamaTools "*") `
        -Destination (Join-Path $OutputDir "tools\llama.cpp") -Recurse -Force
}

$vcRedistRoot = Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\18\Community\VC\Redist\MSVC"
if (Test-Path $vcRedistRoot) {
    $crtDir = Get-ChildItem -LiteralPath $vcRedistRoot -Directory |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\Microsoft.VC145.CRT" } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1

    if ($crtDir) {
        foreach ($file in @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
            $source = Join-Path $crtDir $file
            if (Test-Path $source) {
                Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDir $file) -Force
            }
        }
    }
}

@"
VoiceToText standalone package

Run VoiceToText.exe.

On first run the app creates config.json next to the executable.
If the selected KB Whisper model is missing, the app downloads it into the
models folder automatically.

If internal Gemma cleanup is enabled and llama-server.exe is missing, the app
uses scripts\install_llama_server.ps1 to install the llama.cpp runtime into
tools\llama.cpp.

GPU mode is used when all of these are true:
- an NVIDIA driver is installed
- ggml-cuda.dll is included in this folder
- the required cuBLAS DLLs are included in this folder

If GPU is unavailable, the app falls back to CPU.
"@ | Set-Content -LiteralPath (Join-Path $OutputDir "README_STANDALONE.txt") -Encoding UTF8

Write-Host "Standalone package created at: $OutputDir"
