# download_model.ps1 - Download KB-Whisper GGML model for VoiceToText
#
# Usage:
#   .\scripts\download_model.ps1                  # Downloads medium (default)
#   .\scripts\download_model.ps1 -Size large      # Downloads large
#   .\scripts\download_model.ps1 -Size small -Quantized $false  # Non-quantized small

param(
    [ValidateSet("tiny", "base", "small", "medium", "large")]
    [string]$Size = "medium",

    [bool]$Quantized = $true
)

$ErrorActionPreference = "Stop"

$modelsDir = Join-Path $PSScriptRoot "..\models"
if (-not (Test-Path $modelsDir)) {
    New-Item -ItemType Directory -Path $modelsDir -Force | Out-Null
}

$baseUrl = "https://huggingface.co/KBLab/kb-whisper-$Size/resolve/main"

# Remote (HuggingFace) filenames are the same across sizes; we rename on disk
# to include the size so multiple variants can coexist in models/.
if ($Quantized) {
    $remoteName = "ggml-model-q5_0.bin"
    $localName  = "ggml-model-$Size-q5_0.bin"
} else {
    $remoteName = "ggml-model.bin"
    $localName  = "ggml-model-$Size.bin"
}

$url = "$baseUrl/$remoteName"
$outputPath = Join-Path $modelsDir $localName

Write-Host "============================================" -ForegroundColor Cyan
Write-Host " VoiceToText - KB-Whisper Model Downloader" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Model:      kb-whisper-$Size" -ForegroundColor Yellow
Write-Host "Quantized:  $Quantized" -ForegroundColor Yellow
Write-Host "URL:        $url" -ForegroundColor Gray
Write-Host "Output:     $outputPath" -ForegroundColor Gray
Write-Host ""

if (Test-Path $outputPath) {
    Write-Host "Model file already exists at: $outputPath" -ForegroundColor Green
    $response = Read-Host "Download again? (y/N)"
    if ($response -ne "y" -and $response -ne "Y") {
        Write-Host "Skipping download." -ForegroundColor Yellow
        exit 0
    }
}

Write-Host "Downloading model... (this may take a while)" -ForegroundColor Cyan

try {
    $ProgressPreference = 'SilentlyContinue'  # Speed up download
    Invoke-WebRequest -Uri $url -OutFile $outputPath -UseBasicParsing
    $ProgressPreference = 'Continue'

    $fileSize = (Get-Item $outputPath).Length / 1MB
    Write-Host ""
    Write-Host "Download complete!" -ForegroundColor Green
    Write-Host "File size: $([math]::Round($fileSize, 1)) MB" -ForegroundColor Green
    Write-Host "Saved to: $outputPath" -ForegroundColor Green
} catch {
    Write-Host "ERROR: Failed to download model." -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Available model sizes and approximate download sizes:" -ForegroundColor Cyan
Write-Host "  tiny     ~75 MB   (fastest, lowest quality)" -ForegroundColor Gray
Write-Host "  base     ~150 MB  (fast, decent quality)" -ForegroundColor Gray
Write-Host "  small    ~500 MB  (good balance)" -ForegroundColor Gray
Write-Host "  medium   ~1.5 GB  (high quality)" -ForegroundColor Gray
Write-Host "  large    ~3 GB    (best quality, needs good GPU)" -ForegroundColor Gray
Write-Host ""
Write-Host "To download a different size, run:" -ForegroundColor Gray
Write-Host "  .\scripts\download_model.ps1 -Size <size>" -ForegroundColor White
