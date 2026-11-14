# Download one checksum-verified Pianissimo variant. Catalog mirrors TLH.
[CmdletBinding()]
param(
    [ValidateSet("gguf-q8", "onnx-int4", "onnx-int8", "onnx-fp16", "onnx-fp32")]
    [string]$Model = "onnx-int8",
    [string]$OutputDir = "",
    [switch]$VerifyOnly,
    # Packaging uses the same file definitions without triggering downloads.
    [switch]$ListFiles
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $PSScriptRoot "..\models\Pianissimo"
}
$onnxRoot = "https://huggingface.co/KlangAI/pianissimo-sv-onnx/resolve/63730c6021234f26b9bbae9a07a04fec39e7a52e"
function New-ModelFile([string]$Name, [long]$MinimumBytes, [string]$Sha256) {
    [pscustomobject]@{
        FileName = $Name
        RelativePath = "PianissimoOnnx/$Name"
        Url = "$onnxRoot/$Name"
        MinimumBytes = $MinimumBytes
        Sha256 = $Sha256
    }
}
$shared = @(
    (New-ModelFile "nemo128.onnx" 138824 "5b4a84c52eeaa615dc46d781cc7e4598f9b432184831d57493c48adcd01371a9"),
    (New-ModelFile "vocab.txt" 93939 "d58544679ea4bc6ac563d1f545eb7d474bd6cfa467f0a6e2c1dc1c7d37e3c35d")
)
$files = switch ($Model) {
    "gguf-q8" {
        [pscustomobject]@{
            FileName = "pianissimo-sv-Q8_0.gguf"
            RelativePath = "pianissimo-sv-Q8_0.gguf"
            Url = "https://huggingface.co/nub235/pianissimo-sv-gguf/resolve/main/pianissimo-sv-Q8_0.gguf"
            MinimumBytes = 650000000L
            Sha256 = "350bd38ea3849e0d6c05e680595ef81c185678e2d28eec0524ef704f4c345a4f"
        }
    }
    "onnx-int4" {
        $shared
        New-ModelFile "decoder_joint-model.int4.onnx" 28378227 "bf9b99051419623751e34d542c0076ab50f84bbde4f68f095850b2d4cefdafcd"
        New-ModelFile "encoder-model.int4.onnx" 486421211 "689b18b1c58df738e4a309757a817f4a8fcdad8e99d770f6810b3449be95b729"
    }
    "onnx-int8" {
        $shared
        New-ModelFile "decoder_joint-model.int8.onnx" 30025652 "0f9213242acd8874f2717c5e3cdb888d4e7673ddf3abb9ee3a2fd2eaf0acdd03"
        New-ModelFile "encoder-model.int8.onnx" 630313965 "13288a5f4f009bf6bf8a3260044d098f6922e8caf1c564521d4962fa5749f66c"
    }
    "onnx-fp16" {
        $shared
        New-ModelFile "decoder_joint-model.fp16.onnx" 36267462 "a3df1639600cfee598f3a003f4bdee0f76170d0f104effb88885236749b0c4aa"
        New-ModelFile "encoder-model.fp16.onnx" 1246604787 "1e7c36488dc3946eb2f0ea487135beaf1408b6f1967bad1be56908a7f12c3466"
    }
    "onnx-fp32" {
        $shared
        New-ModelFile "decoder_joint-model.onnx" 72520893 "1b796d178bd5f0e59fe4dd636f2a0d9be027c935d105ceedda3babc15c06d2ac"
        New-ModelFile "encoder-model.onnx" 356069854 "8939fa038d96baef7567e8e07de56acbec28815ab0ebcbefb9eff009ccf70261"
        New-ModelFile "encoder-model.onnx.data" 2133430272 "266f27c808431df1316bdb773bbf8284ffb8fae40dd847afba52da0bcfe656de"
    }
}

if ($ListFiles) {
    $files
    return
}

function Test-ModelFile([string]$Path, $Definition) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    if ((Get-Item -LiteralPath $Path).Length -lt $Definition.MinimumBytes) { return $false }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Definition.Sha256
}

foreach ($file in $files) {
    $destination = Join-Path $OutputDir $file.RelativePath
    if (Test-ModelFile $destination $file) {
        Write-Host "Verified $destination" -ForegroundColor Green
        continue
    }
    if ($VerifyOnly) { throw "Missing or damaged $Model model file: $destination" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    $temporary = "$destination.downloading"
    $previousProgress = $ProgressPreference
    try {
        Write-Host "Downloading $Model / $($file.FileName)..." -ForegroundColor Cyan
        $ProgressPreference = "SilentlyContinue"
        Invoke-WebRequest -Uri $file.Url -OutFile $temporary -UseBasicParsing
        if (-not (Test-ModelFile $temporary $file)) {
            throw "Checksum or size mismatch for $($file.FileName). The existing model was kept."
        }
        Move-Item -LiteralPath $temporary -Destination $destination -Force
        Write-Host "Verified $destination" -ForegroundColor Green
    } finally {
        $ProgressPreference = $previousProgress
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

Write-Host "Pianissimo $Model is ready in $OutputDir. Select it in the tray's Pianissimo Model menu."
