# VoiceToText

A fast, local voice-to-text application for Windows built in C++ with GPU acceleration. Uses **KB-Whisper** models fine-tuned for Swedish by the National Library of Sweden (KBLab), running on **whisper.cpp** with CUDA support.

Inspired by [Wispr Flow](https://wisprflow.ai) - designed for dictating thoughts directly into any application at high speed.

## Features

- **Local & Private** - All processing happens on your GPU. No data leaves your machine.
- **GPU Accelerated** - CUDA support via whisper.cpp for fast transcription
- **KB-Whisper Swedish** - State-of-the-art Swedish speech recognition (47% lower error rate than OpenAI Whisper)
- **Multi-language** - Swedish, English, and auto-detect. Switch with Ctrl+Shift+L
- **Works Everywhere** - Transcribed text is injected into any focused application via clipboard/keystrokes
- **Press-to-Talk** - Hold Ctrl+Shift+Space to record, release to transcribe
- **Voice Activity Detection** - Energy-based VAD with configurable thresholds
- **System Tray** - Runs quietly in the background with status notifications
- **Status Overlay** - Small floating indicator showing recording/processing state
- **Configurable** - JSON config file for all settings

## Requirements

- **Windows 10/11** (64-bit)
- **NVIDIA GPU** with CUDA support (for GPU acceleration)
- **CUDA Toolkit 12.x** installed
- **CMake 3.20+**
- **Visual Studio 2022** (or Build Tools with C++ workload)
- **A microphone**

## Quick Start

### 1. Clone the repository

```bash
git clone https://github.com/YOUR_USERNAME/VoiceToTextApp.git
cd VoiceToTextApp
```

### 2. Download a KB-Whisper model

```powershell
# Download medium model (recommended, ~1.5 GB)
.\scripts\download_model.ps1

# Or choose a specific size:
.\scripts\download_model.ps1 -Size small    # 500 MB, fast, good quality
.\scripts\download_model.ps1 -Size large    # 3 GB, best quality
.\scripts\download_model.ps1 -Size tiny     # 75 MB, fastest, lower quality
```

Available models from [KBLab on Hugging Face](https://huggingface.co/KBLab):

| Model | Size | Quality | Speed |
|-------|------|---------|-------|
| `tiny` | ~75 MB | Basic | Fastest |
| `base` | ~150 MB | Decent | Fast |
| `small` | ~500 MB | Good | Good balance |
| `medium` | ~1.5 GB | High | Moderate |
| `large` | ~3 GB | Best | Slower |

### 3. Build

```bash
cmake -B build -DGGML_CUDA=ON
cmake --build build --config Release
```

The executable will be at `build/bin/Release/VoiceToText.exe`.

### 4. Run

```bash
.\build\bin\Release\VoiceToText.exe
```

The app starts minimized in the system tray.

## Hotkeys

| Shortcut | Action |
|----------|--------|
| `Ctrl+Shift+Space` | Toggle recording (press to start, press again to stop & transcribe) |
| `Escape` | Cancel current recording |
| `Ctrl+Shift+L` | Cycle language (Swedish → English → Auto-detect) |

## Configuration

Edit `config.json` (created automatically next to the executable):

```json
{
    "model_path": "models/ggml-model-q5_0.bin",
    "language": "sv",
    "use_gpu": true,
    "n_threads": 4,
    "audio_device": "",
    "vad_threshold": 0.01,
    "silence_duration_ms": 1500,
    "speech_min_duration_ms": 200,
    "injection_method": "auto",
    "keystroke_delay_ms": 5,
    "show_overlay": true,
    "start_minimized": true,
    "auto_start": false
}
```

### Key settings

- **`model_path`** - Path to the GGML model file
- **`language`** - `"sv"` (Swedish), `"en"` (English), or `"auto"` (auto-detect)
- **`use_gpu`** - `true` for CUDA GPU acceleration, `false` for CPU only
- **`n_threads`** - CPU threads for inference (when using CPU)
- **`vad_threshold`** - Voice activity detection sensitivity (lower = more sensitive)
- **`injection_method`** - `"auto"`, `"clipboard"`, or `"keystrokes"`

## How It Works

1. **Press Ctrl+Shift+Space** → Recording starts, overlay shows blue "Listening..." indicator
2. **Speak naturally** → Audio is captured at 16kHz from your microphone
3. **Press Ctrl+Shift+Space again** → Recording stops, overlay shows yellow "Processing..."
4. **Whisper processes audio** → KB-Whisper model runs inference on your GPU
5. **Text is injected** → Transcribed text appears in whatever application has focus

## Architecture

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  Hotkey Mgr  │────▶│     App      │◀────│  System Tray │
└──────────────┘     └──────┬───────┘     └──────────────┘
                            │
                 ┌──────────┼──────────┐
                 ▼          ▼          ▼
          ┌───────────┐ ┌────────┐ ┌───────────┐
          │  Audio    │ │Whisper │ │   Text    │
          │ Capture   │ │ Engine │ │ Injector  │
          │(miniaudio)│ │(.cpp)  │ │(SendInput)│
          └─────┬─────┘ └────────┘ └───────────┘
                │
          ┌─────▼─────┐
          │   VAD     │
          │(energy)   │
          └───────────┘
```

## Building Without GPU

To build CPU-only (no CUDA required):

```bash
cmake -B build -DGGML_CUDA=OFF
cmake --build build --config Release
```

## Dependencies

All dependencies are automatically fetched via CMake FetchContent:

- [whisper.cpp](https://github.com/ggerganov/whisper.cpp) - C/C++ Whisper inference
- [miniaudio](https://github.com/mackron/miniaudio) - Audio capture (single header)
- [nlohmann/json](https://github.com/nlohmann/json) - JSON configuration

## KB-Whisper Models

This application uses [KB-Whisper](https://kb-labb.github.io/posts/2025-03-07-welcome-KB-Whisper/) models from the National Library of Sweden. These models achieve an average **47% reduction in Word Error Rate** compared to OpenAI's Whisper-large-v3 for Swedish speech.

Model repositories on Hugging Face:
- [kb-whisper-tiny](https://huggingface.co/KBLab/kb-whisper-tiny)
- [kb-whisper-base](https://huggingface.co/KBLab/kb-whisper-base)
- [kb-whisper-small](https://huggingface.co/KBLab/kb-whisper-small)
- [kb-whisper-medium](https://huggingface.co/KBLab/kb-whisper-medium)
- [kb-whisper-large](https://huggingface.co/KBLab/kb-whisper-large)

## License

MIT License
