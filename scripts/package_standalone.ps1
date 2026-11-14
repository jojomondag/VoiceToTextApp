param(
    [string]$BuildDir = "",
    [string]$Configuration = "Release",
    [string]$OutputDir = "",
    [ValidateSet("gguf-q8", "onnx-int4", "onnx-int8", "onnx-fp16", "onnx-fp32")]
    [string]$Model = "onnx-int8"
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $PSScriptRoot "..\build"
}
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $PSScriptRoot "..\dist\VoiceToText"
}

$binCandidates = @(
    (Join-Path $BuildDir "bin\$Configuration"),
    (Join-Path $BuildDir "bin")
)
$binDir = $binCandidates |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ "VoiceToText.exe") } |
    Select-Object -First 1
if (-not $binDir) {
    throw "VoiceToText.exe was not found in any expected build output: $($binCandidates -join ', '). Build the app first."
}

# Check the absolute target before replacing a package. Refuse any directory
# that contains source or build inputs, including a drive root.
$OutputDir = [IO.Path]::GetFullPath($OutputDir).TrimEnd('\', '/')
$repositoryDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..")).TrimEnd('\', '/')
$resolvedBinDir = [IO.Path]::GetFullPath($binDir).TrimEnd('\', '/')
$outputPrefix = $OutputDir + [IO.Path]::DirectorySeparatorChar
if ($repositoryDir.Equals($OutputDir, [StringComparison]::OrdinalIgnoreCase) -or
    $repositoryDir.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    $resolvedBinDir.Equals($OutputDir, [StringComparison]::OrdinalIgnoreCase) -or
    $resolvedBinDir.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    $OutputDir -eq [IO.Path]::GetPathRoot($OutputDir).TrimEnd('\', '/')) {
    throw "Unsafe package output directory: $OutputDir"
}
$repositoryPrefix = $repositoryDir + [IO.Path]::DirectorySeparatorChar
$distPrefix = (Join-Path $repositoryDir "dist") + [IO.Path]::DirectorySeparatorChar
$artifactsPrefix = (Join-Path $repositoryDir "artifacts") + [IO.Path]::DirectorySeparatorChar
if ($OutputDir.StartsWith($repositoryPrefix, [StringComparison]::OrdinalIgnoreCase) -and
    -not $OutputDir.StartsWith($distPrefix, [StringComparison]::OrdinalIgnoreCase) -and
    -not $OutputDir.StartsWith($artifactsPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Within the repository, use a package output below dist or artifacts: $OutputDir"
}
if (Test-Path -LiteralPath $OutputDir) {
    Remove-Item -LiteralPath $OutputDir -Recurse -Force
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $OutputDir "models") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $OutputDir "scripts") -Force | Out-Null

$coreFiles = @(
    "VoiceToText.exe",
    "transcribe.dll",
    "ggml.dll",
    "ggml-base.dll",
    "ggml-cpu-x64.dll",
    "ggml-cpu-sse42.dll",
    "ggml-vulkan.dll",
    "onnxruntime.dll"
)

foreach ($file in $coreFiles) {
    $source = Join-Path $binDir $file
    if (-not (Test-Path $source)) {
        throw "Required runtime file was not found: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDir $file) -Force
}

# Keep every CPU variant: the runtime selects one supported by the user's CPU.
$cpuBackends = @(Get-ChildItem -LiteralPath $binDir -Filter "ggml-cpu-*.dll" -File)
if ($cpuBackends.Count -eq 0) { throw "No portable GGML CPU backends were found in $binDir" }
foreach ($backend in $cpuBackends) {
    Copy-Item -LiteralPath $backend.FullName -Destination (Join-Path $OutputDir $backend.Name) -Force
}
foreach ($file in @(Get-ChildItem -LiteralPath $binDir -Filter "onnxruntime*.dll" -File)) {
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $OutputDir $file.Name) -Force
}
foreach ($licenseDir in @("licenses", "third-party-licenses")) {
    $licenseSource = Join-Path $binDir $licenseDir
    if (Test-Path -LiteralPath $licenseSource) {
        Copy-Item -LiteralPath $licenseSource -Destination (Join-Path $OutputDir $licenseDir) -Recurse -Force
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $OutputDir "licenses\Pianissimo") -PathType Container)) {
    throw "Pianissimo runtime license files were not found in $binDir"
}

foreach ($script in @("download_model.ps1", "download_gemma_model.ps1", "install_llama_server.ps1")) {
    $source = Join-Path $PSScriptRoot $script
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDir "scripts\$script") -Force
    }
}

$resourcesSource = Join-Path $PSScriptRoot "..\resources"
$resourcesDestination = Join-Path $OutputDir "resources"
New-Item -ItemType Directory -Path $resourcesDestination -Force | Out-Null
foreach ($resource in @(
    "overlay_voice_mode.png",
    "overlay_ai_language_mode.png",
    "overlay_cleanup_mode.png"
)) {
    $source = Join-Path $resourcesSource $resource
    if (-not (Test-Path $source)) {
        throw "Required runtime resource was not found: $source"
    }
    Copy-Item -LiteralPath $source -Destination $resourcesDestination -Force
}

# Include the selected Pianissimo variant only when all its files are present
# and match the pinned SHA-256 catalog. Keep Gemma models separately.
$modelDefinitions = @(& (Join-Path $PSScriptRoot "download_model.ps1") -Model $Model -ListFiles)
$modelSources = @(
    (Join-Path $binDir "models"),
    (Join-Path $PSScriptRoot "..\build\bin\$Configuration\models"),
    (Join-Path $PSScriptRoot "..\models")
)
$speechModelIncluded = $false
foreach ($modelSource in $modelSources) {
    if (-not (Test-Path -LiteralPath $modelSource)) { continue }
    $pianissimoSource = Join-Path $modelSource "Pianissimo"
    $availableFiles = @($modelDefinitions | Where-Object {
        Test-Path -LiteralPath (Join-Path $pianissimoSource $_.RelativePath) -PathType Leaf
    })
    if (-not $speechModelIncluded -and $availableFiles.Count -eq $modelDefinitions.Count) {
        & (Join-Path $PSScriptRoot "download_model.ps1") -Model $Model -OutputDir $pianissimoSource -VerifyOnly
        foreach ($definition in $modelDefinitions) {
            $destination = Join-Path (Join-Path $OutputDir "models\Pianissimo") $definition.RelativePath
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $pianissimoSource $definition.RelativePath) -Destination $destination -Force
        }
        $speechModelIncluded = $true
    }
    $gemmaSource = Join-Path $modelSource "llm"
    if (Test-Path -LiteralPath $gemmaSource) {
        Get-ChildItem -LiteralPath $gemmaSource -Filter "*.gguf" -File |
            ForEach-Object {
                $destination = Join-Path (Join-Path $OutputDir "models\llm") $_.Name
                New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
                Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
            }
    }
}

$configTemplate = Join-Path $PSScriptRoot "..\config.example.json"
$packageConfig = Get-Content -LiteralPath $configTemplate -Raw | ConvertFrom-Json
$packageConfig.model_id = $Model
$packageConfig.model_dir = "models/Pianissimo"
$packageConfig.model_path = "models/Pianissimo/pianissimo-sv-Q8_0.gguf"
$packageConfig | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $OutputDir "config.json") -Encoding UTF8

$llamaTools = Join-Path $PSScriptRoot "..\tools\llama.cpp"
if (Test-Path (Join-Path $llamaTools "llama-server.exe")) {
    New-Item -ItemType Directory -Path (Join-Path $OutputDir "tools\llama.cpp") -Force | Out-Null
    Copy-Item -Path (Join-Path $llamaTools "*") `
        -Destination (Join-Path $OutputDir "tools\llama.cpp") -Recurse -Force
}

$visualStudioRoots = @(
    (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio"),
    (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio")
)
$crtDir = $visualStudioRoots |
    Where-Object { Test-Path $_ } |
    ForEach-Object {
        Get-ChildItem -Path (Join-Path $_ "*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT") `
            -Directory -ErrorAction SilentlyContinue
    } |
    Sort-Object FullName -Descending |
    Select-Object -First 1

if ($crtDir) {
    $vcRuntimeDirs = @($crtDir)
    $vcRuntimeDirs += @(Get-ChildItem -LiteralPath $crtDir.Parent.FullName `
        -Directory -Filter "Microsoft.VC*.OpenMP" -ErrorAction SilentlyContinue)
    $vcRuntimeDirs |
        ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Filter "*.dll" -File } |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $OutputDir $_.Name) -Force
        }
} else {
    Write-Warning "Visual C++ app-local runtime was not found."
}

$modelFiles = @(Get-ChildItem -LiteralPath (Join-Path $OutputDir "models") -Recurse -File)
if (-not $speechModelIncluded) {
    Write-Warning "Selected Pianissimo $Model was not cached. The app will download its verified files on first use."
}
if ($modelFiles.Count -eq 0) {
    Write-Warning "No speech or Gemma model files were included."
} else {
    Write-Host "Included $($modelFiles.Count) model file(s)."
}

@"
VoiceToText standalone package

Run VoiceToText.exe.

The package selects Pianissimo $Model in config.json.
If the selected model is missing, the app downloads its checksum-verified
files into models\Pianissimo automatically. ONNX variants use PianissimoOnnx
inside that folder. Swedish is the supported speech language.

Press Ctrl+Shift+Space to start/stop. The Live dictation window displays the
transcript while speaking and lets you select text for spoken replacement.

If internal Gemma cleanup is enabled and llama-server.exe is missing, the app
uses scripts\install_llama_server.ps1 to install the llama.cpp runtime into
tools\llama.cpp.

This package includes the Pianissimo speech runtimes, available selected
speech model files, optional Gemma models, and available llama.cpp tools.
GGUF speech inference supports Vulkan GPU acceleration and portable CPU
backends. ONNX speech inference uses CPU. A compatible GPU driver is needed
for Vulkan. CUDA Toolkit is not needed for speech recognition.
"@ | Set-Content -LiteralPath (Join-Path $OutputDir "README_STANDALONE.txt") -Encoding UTF8

Write-Host "Standalone package created at: $OutputDir"
