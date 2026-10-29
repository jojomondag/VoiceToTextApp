param(
    [ValidateSet("cpu", "cuda")]
    [string]$Variant = "cpu",
    [string]$InstallDir = (Join-Path $PSScriptRoot "..\tools\llama.cpp"),
    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Select-LlamaAsset {
    param(
        [Parameter(Mandatory = $true)] $Release,
        [Parameter(Mandatory = $true)] [string] $Variant
    )

    $assets = @($Release.assets | Where-Object {
        $_.name -match '(?i)win.*x64.*\.zip$' -or
        $_.name -match '(?i)x64.*win.*\.zip$'
    })

    if ($Variant -eq "cuda") {
        $cuda = @($assets | Where-Object {
            $_.name -match '(?i)^llama-.*win-cuda.*x64.*\.zip$'
        } | Sort-Object {
            if ($_.name -match '(?i)cuda-(\d+)\.(\d+)') {
                [version]("{0}.{1}" -f $Matches[1], $Matches[2])
            } else {
                [version]"0.0"
            }
        } -Descending |
            Select-Object -First 1)
        if ($cuda.Count -gt 0) { return $cuda[0] }
    }

    $cpu = @($assets | Where-Object {
        $_.name -match '(?i)cpu' -and $_.name -notmatch '(?i)cuda|cublas|vulkan|opencl'
    } | Sort-Object name | Select-Object -First 1)
    if ($cpu.Count -gt 0) { return $cpu[0] }

    $fallback = @($assets | Where-Object {
        $_.name -notmatch '(?i)cuda|cublas|vulkan|opencl'
    } | Sort-Object name | Select-Object -First 1)
    if ($fallback.Count -gt 0) { return $fallback[0] }

    throw "Could not find a Windows x64 llama.cpp release asset."
}

function Select-CudaRuntimeAsset {
    param(
        [Parameter(Mandatory = $true)] $Release,
        [Parameter(Mandatory = $true)] $LlamaAsset
    )

    $cudaVersion = ""
    if ($LlamaAsset.name -match '(cuda-\d+\.\d+)') {
        $cudaVersion = $Matches[1]
    }

    $runtime = @($Release.assets | Where-Object {
        $_.name -match '(?i)^cudart-.*win.*x64.*\.zip$' -and
        ($cudaVersion -eq "" -or $_.name -match [regex]::Escape($cudaVersion))
    } | Sort-Object name | Select-Object -First 1)

    if ($runtime.Count -gt 0) { return $runtime[0] }
    return $null
}

function Expand-LlamaAsset {
    param(
        [Parameter(Mandatory = $true)] $Asset,
        [Parameter(Mandatory = $true)] [string] $TempRoot,
        [Parameter(Mandatory = $true)] [string] $InstallPath
    )

    $zipPath = Join-Path $TempRoot $Asset.name
    $extractPath = Join-Path $TempRoot ("extract-" + [System.IO.Path]::GetFileNameWithoutExtension($Asset.name))
    New-Item -ItemType Directory -Force -Path $extractPath | Out-Null

    Write-Host "Downloading $($Asset.name)"
    Invoke-WebRequest -Uri $Asset.browser_download_url -OutFile $zipPath

    Write-Host "Extracting $($Asset.name)..."
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractPath -Force

    $server = Get-ChildItem -LiteralPath $extractPath -Recurse -Filter "llama-server.exe" |
        Select-Object -First 1
    if ($server) {
        $sourceDir = $server.Directory.FullName
    } else {
        $dll = Get-ChildItem -LiteralPath $extractPath -Recurse -Filter "*.dll" |
            Select-Object -First 1
        if (-not $dll) {
            throw "The archive did not contain llama-server.exe or runtime DLLs."
        }
        $sourceDir = $dll.Directory.FullName
    }

    Copy-Item -Path (Join-Path $sourceDir "*") -Destination $InstallPath -Recurse -Force
}

if ([System.IO.Path]::IsPathRooted($InstallDir)) {
    $installPath = [System.IO.Path]::GetFullPath($InstallDir)
} else {
    $installPath = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $InstallDir))
}
$serverPath = Join-Path $installPath "llama-server.exe"

if ((Test-Path $serverPath) -and -not $Force) {
    Write-Host "llama-server.exe already installed at $serverPath"
    exit 0
}

$releaseUrl = "https://api.github.com/repos/ggml-org/llama.cpp/releases/latest"
Write-Host "Checking latest llama.cpp release..."
$release = Invoke-RestMethod -Uri $releaseUrl -Headers @{
    "User-Agent" = "VoiceToTextApp"
}
$asset = Select-LlamaAsset -Release $release -Variant $Variant
$runtimeAsset = if ($Variant -eq "cuda") {
    Select-CudaRuntimeAsset -Release $release -LlamaAsset $asset
} else {
    $null
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("vtt-llama-" + [System.Guid]::NewGuid())

New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
if ($Force -and (Test-Path $installPath)) {
    Remove-Item -LiteralPath $installPath -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $installPath | Out-Null

try {
    Expand-LlamaAsset -Asset $asset -TempRoot $tempRoot -InstallPath $installPath
    if ($runtimeAsset) {
        Expand-LlamaAsset -Asset $runtimeAsset -TempRoot $tempRoot -InstallPath $installPath
    }

    if (-not (Test-Path $serverPath)) {
        throw "llama-server.exe was not installed to $serverPath."
    }

    Write-Host "Installed llama-server.exe to $serverPath"
} finally {
    if (Test-Path $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
