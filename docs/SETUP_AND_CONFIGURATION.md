# Setup And Configuration

This page contains the detailed setup, build, model, packaging, and
configuration notes for VoiceToText.

## Requirements

- Windows 10/11, 64-bit.
- Visual Studio 2022 or Build Tools with the C++ workload.
- CMake 3.20 or newer.
- A microphone.
- Optional: NVIDIA GPU and CUDA runtime/backend files for GPU acceleration.

CPU builds are supported. CUDA is recommended for larger models.

## Models

Download the default KB-Whisper model:

```powershell
.\scripts\download_model.ps1
```

Choose a specific model size with `-Size`:

```powershell
.\scripts\download_model.ps1 -Size tiny
.\scripts\download_model.ps1 -Size base
.\scripts\download_model.ps1 -Size small
.\scripts\download_model.ps1 -Size medium
.\scripts\download_model.ps1 -Size large
```

| Size | Approx. file size | Use case |
| --- | ---: | --- |
| `tiny` | 29 MB | Fast tests and very low latency. |
| `base` | 53 MB | Lightweight dictation. |
| `small` | 167 MB | Good everyday balance. |
| `medium` | 514 MB | Better quality, slower. |
| `large` | 1.01 GB | Best quality, highest resource use. |

Model project:

- [KB-Whisper](https://kb-labb.github.io/posts/2025-03-07-welcome-KB-Whisper/)
- [KBLab on Hugging Face](https://huggingface.co/KBLab)

## Internal Gemma Models

VoiceToText can run Gemma internally for cleanup, profile transforms, and AI
Language output. This uses a local `llama-server.exe` process and an
OpenAI-compatible endpoint inside the app.

Download the recommended Gemma model:

```powershell
.\scripts\download_gemma_model.ps1 -Model e2b
```

Let the script choose based on hardware:

```powershell
.\scripts\download_gemma_model.ps1 -Model auto
```

| Model | Use case |
| --- | --- |
| `e2b` | Recommended default for this app. |
| `e4b` | Higher quality when the machine has enough RAM/GPU headroom. |
| `12b` | Large; usually unnecessary for dictation cleanup. |
| `26b-a4b` | Very large workstation option. |
| `31b` | Experimental/overkill for normal use. |

The tray menu also has a Gemma Model submenu. If a selected model is missing,
VoiceToText downloads it into `models/llm` and switches to it when ready.

VoiceToText looks for `llama-server.exe` at the configured path, next to the
executable, or under `tools\llama.cpp`. If it is missing, the app can run
`scripts\install_llama_server.ps1` automatically and install the CPU runtime
under `tools\llama.cpp`.

Install the runtime manually:

```powershell
.\scripts\install_llama_server.ps1
```

## Build

CUDA build:

```powershell
cmake -B build -DGGML_CUDA=ON
cmake --build build --config Release
```

CPU-only build:

```powershell
cmake -B build -DGGML_CUDA=OFF
cmake --build build --config Release
```

Debug build:

```powershell
cmake --build build --config Debug
```

The Release executable is written to:

```text
build\bin\Release\VoiceToText.exe
```

## Run

```powershell
.\build\bin\Release\VoiceToText.exe
```

The app starts in the system tray by default.

## Packaging

Create a standalone folder:

```powershell
.\scripts\package_standalone.ps1
```

The package script copies the executable, runtime DLLs, model/config files where
available, and standalone notes into `dist\VoiceToText`.

## Configuration File

On first run, the app creates `config.json` next to the executable. The full
example lives in [../config.example.json](../config.example.json).

Common settings:

| Setting | Meaning |
| --- | --- |
| `model_path` | GGML model file used by whisper.cpp. |
| `language` | `sv`, `en`, or `auto`. |
| `use_gpu` | Enables GPU use when available. |
| `compute_mode` | `auto`, `cpu`, or `gpu`. |
| `n_threads` | CPU inference threads. `0` means auto. |
| `whisper_fast_mode` | Faster dictation-oriented Whisper settings. |
| `warm_up_model_on_load` | Runs a small warmup after loading the model. |
| `audio_device` | Optional microphone device name. Empty means default. |
| `vad_threshold` | Voice activity sensitivity. Lower is more sensitive. |
| `silence_duration_ms` | Silence duration used to detect speech end. |
| `speech_min_duration_ms` | Minimum speech duration before accepting input. |
| `trim_silence_before_transcription` | Trims leading/trailing silence. |
| `toggle_key` | Virtual-key code for the recording hotkey key. |
| `cancel_key` | Virtual-key code for cancel while recording. |
| `injection_method` | `auto`, `clipboard`, or `keystrokes`. |
| `llm_cleanup_enabled` | Enables local LLM processing. |
| `llm_provider` | `internal` for Gemma, or `lmstudio` for an external server. |
| `llm_endpoint` | OpenAI-compatible chat completions endpoint. |
| `llm_model` | Model name sent to the LLM endpoint. |
| `llm_internal_model` | `auto`, `e2b`, `e4b`, `12b`, `26b-a4b`, or `31b`. |
| `llm_internal_model_path` | Gemma GGUF path used by internal mode. |
| `llm_internal_server_path` | Path to `llama-server.exe`. |
| `llm_internal_port` | Local port for the internal OpenAI-compatible server. |
| `llm_max_tokens` | Dynamic output-token ceiling; short dictation uses less, while long dictation can scale up to this value. Default `4096`. |
| `llm_profiles` | Editable intent profiles and their instructions. |
| `show_overlay` | Shows the floating status overlay. |
| `start_minimized` | Starts in the tray instead of showing a window. |
| `transcript_history_path` | History JSON path. |
| `transcript_history_max_entries` | Number of history entries to keep. |

## Local LLM Cleanup

VoiceToText can clean transcribed text before insertion.

Internal Gemma setup:

```json
{
  "llm_cleanup_enabled": true,
  "llm_provider": "internal",
  "llm_internal_model": "auto",
  "llm_internal_server_path": "llama-server.exe"
}
```

External LM Studio setup:

1. Start LM Studio and load a local chat model.
2. Start the local server, usually on `http://127.0.0.1:1234`.
3. In `config.json`, set:

```json
{
  "llm_cleanup_enabled": true,
  "llm_provider": "lmstudio",
  "llm_endpoint": "http://127.0.0.1:1234/v1/chat/completions",
  "llm_model": "local-model"
}
```

If the LLM request fails, the app falls back to the transcription instead of
blocking dictation.

## Dependencies

CMake fetches or wires the required dependencies:

- [whisper.cpp](https://github.com/ggerganov/whisper.cpp)
- [miniaudio](https://github.com/mackron/miniaudio)
- [nlohmann/json](https://github.com/nlohmann/json)
