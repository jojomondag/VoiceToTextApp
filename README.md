# VoiceToText

A fast, local voice-to-text application for Windows built in C++ with GPU acceleration. Uses **KB-Whisper** models fine-tuned for Swedish by the National Library of Sweden (KBLab), running on **whisper.cpp** with CUDA support.

Inspired by [Wispr Flow](https://wisprflow.ai) - designed for dictating thoughts directly into any application at high speed.

## Features

- **Local & Private** - All processing happens on your GPU. No data leaves your machine.
- **GPU Accelerated** - CUDA support via whisper.cpp for fast transcription
- **KB-Whisper Swedish** - State-of-the-art Swedish speech recognition (47% lower error rate than OpenAI Whisper)
- **Multi-language** - Swedish, English, and auto-detect. Switch with Ctrl+Shift+L
- **Works Everywhere** - Transcribed text is injected into any focused application via clipboard/keystrokes
- **Optional LM Studio cleanup** - Send the finished transcription through a local OpenAI-compatible LM Studio model before insertion
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
    "llm_cleanup_enabled": false,
    "llm_endpoint": "http://127.0.0.1:1234/v1/chat/completions",
    "llm_model": "local-model",
    "llm_pre_prompt": "Du städar upp dikterade instruktioner innan de skickas till en AI-kodmodell. Rätta transkriberingsfel, fel ord som tydligt inte passar i sammanhanget, skiljetecken, versaler, grammatik och struktur. Gör texten tydlig, konkret och lätt för en kodmodell att agera på. Använd kodkontext när det är uppenbart, till exempel LM Studio, app.cpp, C++, CMake, PowerShell, JSON, API, endpoint och config. Bevara användarens avsikt, språk, ton, namn, sökvägar, kommandon, kodidentifierare, siffror och tekniska termer. Lägg inte till nya krav, antaganden eller förklaringar. Svara endast med den städade instruktionen.",
    "llm_timeout_ms": 30000,
    "llm_max_tokens": 4096,
    "llm_corrections_enabled": true,
    "llm_corrections_path": "transcription_corrections.json",
    "llm_corrections_max_entries": 200,
    "llm_corrections_prompt_limit": 20,
    "llm_corrections_min_count": 1,
    "llm_ai_language_enabled": true,
    "llm_ai_language_prompt": "Skapa en ultrakompakt AI-läsbar instruktionsdialekt från användarens städade instruktion. Komprimera aggressivt men bevara avsikt, krav, begränsningar, entiteter, kodnamn, sökvägar, siffror och åtgärdsordning. Identifiera och ta bort upprepningar, utfyllnad och otydlighet. Strukturera tätt med etiketter som Goal, Context, Tasks, Constraints och Output när det hjälper. Använd förkortningar bara när en kodmodell entydigt förstår dem. Lägg inte till nya krav eller antaganden. Svara endast med AI-språkversionen.",
    "llm_intent_profiles_enabled": true,
    "llm_intent_detection_prompt": "Identifiera användarens avsikt från texten och välj exakt en profil: coding, email, discussion, translation eller general. Svara endast med strikt JSON: {\"profile\":\"coding|email|discussion|translation|general\"}.",
    "llm_profile_transform_prompt": "Transformera texten enligt vald profil. coding: gör texten konkret, åtgärdsbar och tydlig för en kodmodell. email: gör den till ett tydligt mailutkast med passande ton. Lägg inte till placeholders, signatur eller ämnesrad om användaren inte bad om det. discussion: bevara samtalston och gör resonemanget klart. translation: översätt eller förbered översättning enligt användarens begäran och bevara betydelsen. general: städa texten utan särskild formatering. Lägg inte till nya krav. Svara endast med den transformerade texten.",
    "show_overlay": true,
    "start_minimized": true,
    "auto_start": false,
    "transcript_history_path": "transcript_history.json",
    "transcript_history_max_entries": 100
}
```

### Key settings

- **`model_path`** - Path to the GGML model file
- **`language`** - `"sv"` (Swedish), `"en"` (English), or `"auto"` (auto-detect)
- **`use_gpu`** - `true` for CUDA GPU acceleration, `false` for CPU only
- **`n_threads`** - CPU threads for inference (when using CPU)
- **`vad_threshold`** - Voice activity detection sensitivity (lower = more sensitive)
- **`injection_method`** - `"auto"`, `"clipboard"`, or `"keystrokes"`
- **`llm_cleanup_enabled`** - `true` to clean transcription text through LM Studio before insertion
- **`llm_endpoint`** - LM Studio OpenAI-compatible chat completions URL, usually `http://127.0.0.1:1234/v1/chat/completions`
- **`llm_model`** - Model identifier shown by LM Studio for your loaded Gemma model
- **`llm_pre_prompt`** - Instruction that tells the local LLM how to clean dictated text before insertion
- **`llm_timeout_ms`** - How long to wait for cleanup before falling back to raw transcription
- **`llm_corrections_enabled`** - `true` to learn recurring raw-to-cleaned transcription corrections
- **`llm_corrections_path`** - JSON file where learned corrections are stored next to the executable
- **`llm_corrections_prompt_limit`** - Maximum learned corrections included in each LM Studio prompt
- **`llm_ai_language_enabled`** - `true` to generate a third compact AI-readable history version
- **`llm_ai_language_prompt`** - Prompt used to compress cleaned text into AI Language
- **`llm_intent_profiles_enabled`** - `true` to auto-detect intent and apply a profile during cleanup
- **`llm_intent_detection_prompt`** - Prompt used for the second pass that classifies the user's intent
- **`llm_profile_transform_prompt`** - Prompt used for the third pass that transforms text according to the detected profile
- **`transcript_history_path`** - JSON file where raw and cleaned transcription history is stored
- **`transcript_history_max_entries`** - Maximum number of history entries kept for the History window

### LM Studio cleanup

1. Start LM Studio and load your Gemma model.
2. Open the Local Server tab and start the OpenAI-compatible server on port `1234`.
3. In `config.json` next to `VoiceToText.exe`, set:

```json
{
    "llm_cleanup_enabled": true,
    "llm_endpoint": "http://127.0.0.1:1234/v1/chat/completions",
    "llm_model": "local-model",
    "llm_pre_prompt": "Du städar upp dikterade instruktioner innan de skickas till en AI-kodmodell. Rätta transkriberingsfel, fel ord som tydligt inte passar i sammanhanget, skiljetecken, versaler, grammatik och struktur. Gör texten tydlig, konkret och lätt för en kodmodell att agera på. Använd kodkontext när det är uppenbart, till exempel LM Studio, app.cpp, C++, CMake, PowerShell, JSON, API, endpoint och config. Bevara användarens avsikt, språk, ton, namn, sökvägar, kommandon, kodidentifierare, siffror och tekniska termer. Lägg inte till nya krav, antaganden eller förklaringar. Svara endast med den städade instruktionen.",
    "llm_corrections_enabled": true,
    "llm_corrections_path": "transcription_corrections.json",
    "llm_ai_language_enabled": true
}
```

If LM Studio is not running or the request fails, VoiceToText writes the raw Whisper transcription instead.

## How It Works

1. **Press Ctrl+Shift+Space** → Recording starts, overlay shows blue "Listening..." indicator
2. **Speak naturally** → Audio is captured at 16kHz from your microphone
3. **Press Ctrl+Shift+Space again** → Recording stops, overlay shows yellow "Processing..."
4. **Whisper processes audio** → KB-Whisper model runs inference on your GPU
5. **Optional LM Studio cleanup** → Text is polished locally through your loaded model
6. **Optional AI Language pass** → A compact model-readable version is stored in history
7. **Text is injected** → Transcribed text appears in whatever application has focus

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
