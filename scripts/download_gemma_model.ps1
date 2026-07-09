param(
    [ValidateSet("auto", "e2b", "e4b", "12b", "26b-a4b", "31b")]
    [string]$Model = "e2b",
    [string]$OutputDir = "models/llm"
)

$ErrorActionPreference = "Stop"

$models = @{
    "e2b" = @{
        Repo = "ggml-org/gemma-4-E2B-it-GGUF"
        File = "gemma-4-E2B-it-Q8_0.gguf"
    }
    "e4b" = @{
        Repo = "ggml-org/gemma-4-E4B-it-GGUF"
        File = "gemma-4-E4B-it-Q4_K_M.gguf"
    }
    "12b" = @{
        Repo = "ggml-org/gemma-4-12B-it-GGUF"
        File = "gemma-4-12B-it-Q4_K_M.gguf"
    }
    "26b-a4b" = @{
        Repo = "ggml-org/gemma-4-26B-A4B-it-GGUF"
        File = "gemma-4-26B-A4B-it-Q4_K_M.gguf"
    }
    "31b" = @{
        Repo = "ggml-org/gemma-4-31B-it-GGUF"
        File = "gemma-4-31B-it-Q4_K_M.gguf"
    }
}

if ($Model -eq "auto") {
    $ramGb = [math]::Ceiling((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    $hasNvidia = $null -ne (Get-CimInstance Win32_VideoController |
        Where-Object { $_.Name -match "NVIDIA" } |
        Select-Object -First 1)
    $Model = if ($hasNvidia -and $ramGb -ge 24) { "e4b" } else { "e2b" }
    Write-Host "Auto selected Gemma $Model for this machine."
}

$entry = $models[$Model]
$url = "https://huggingface.co/$($entry.Repo)/resolve/main/$($entry.File)"
$targetDir = Resolve-Path -Path "." | ForEach-Object {
    Join-Path $_.Path $OutputDir
}
$target = Join-Path $targetDir $entry.File
$temp = "$target.download"

New-Item -ItemType Directory -Force -Path $targetDir | Out-Null

Write-Host "Downloading $($entry.File)"
Write-Host $url
Invoke-WebRequest -Uri $url -OutFile $temp

if (Test-Path $target) {
    Remove-Item -LiteralPath $target -Force
}
Move-Item -LiteralPath $temp -Destination $target

Write-Host "Saved to $target"
Write-Host "Set llm_provider to internal and llm_internal_model to $Model, or choose it from the tray menu."
