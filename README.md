# VoiceToText

VoiceToText is a local Windows dictation app built in C++. It records from your
microphone, transcribes speech with whisper.cpp, optionally cleans the result
through a local OpenAI-compatible LLM server, and inserts the final text into
the app you are already using.

The app is built for fast everyday dictation: start it once, leave it in the
system tray, press a hotkey, speak, and get text back in the focused window.

## Highlights

- Swedish-first KB-Whisper model support.
- Optional CUDA acceleration with automatic CPU/GPU selection.
- Microphone capture with voice activity detection and silence trimming.
- Optional local LLM cleanup and profile-based rewriting.
- Transcript history with raw, cleaned, and AI Language views.
- System tray controls and a floating status overlay.

## Requirements

- Windows 10/11, 64-bit.
- Visual Studio 2022 or Build Tools with the C++ workload.
- CMake 3.20 or newer.
- A microphone.
- Optional: NVIDIA GPU and CUDA runtime/backend files for GPU acceleration.

CPU builds are supported. CUDA is recommended for larger models.

## Quick Start

Clone the repository:

```powershell
git clone https://github.com/jojomondag/VoiceToTextApp.git
cd VoiceToTextApp
```

Download the default KB-Whisper model:

```powershell
.\scripts\download_model.ps1
```

Available model sizes are listed in [Models](#models).

Build and run:

```powershell
cmake -B build -DGGML_CUDA=ON
cmake --build build --config Release
.\build\bin\Release\VoiceToText.exe
```

The app starts in the system tray by default.

## Usage

Default hotkeys:

| Shortcut | Action |
| --- | --- |
| `Ctrl+Shift+Space` | Start or stop recording |
| `Escape` | Cancel the current recording while recording |
| `Ctrl+Shift+L` | Cycle language: Swedish, English, Auto |

Typical flow:

1. Put the cursor in any app where you want text.
2. Press `Ctrl+Shift+Space`.
3. Speak naturally.
4. Press `Ctrl+Shift+Space` again.
5. The final text appears in the focused app.

You can also use the tray menu to start recording, switch language, change
model size, change compute mode, open History, or exit.

## History And Profiles

The History window keeps recent dictations and shows:

- Recorded voice: raw Whisper output.
- Clean transcript: text after cleanup/profile transformation.
- AI Language: compact AI-readable version for prompt-heavy workflows.

Each history row includes copy buttons and profile controls. Clicking the
profile text or profile icon opens the profile prompt editor. From there you can
edit how each profile transforms cleaned text, create new profiles, and rename
profiles.

History is stored in `transcript_history.json` next to the executable unless
configured otherwise.

## Configuration

On first run, the app creates `config.json` next to the executable. The full
example lives in [config.example.json](config.example.json).

Common settings are:

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
| `llm_endpoint` | OpenAI-compatible chat completions endpoint. |
| `llm_model` | Model name sent to the LLM endpoint. |
| `llm_profiles` | Editable intent profiles and their instructions. |
| `show_overlay` | Shows the floating status overlay. |
| `start_minimized` | Starts in the tray instead of showing a window. |
| `transcript_history_path` | History JSON path. |
| `transcript_history_max_entries` | Number of history entries to keep. |

## Local LLM Cleanup

VoiceToText can send transcribed text to a local OpenAI-compatible chat
completions server before insertion. LM Studio works well for this.

Basic setup:

1. Start LM Studio.
2. Load a local chat model.
3. Start the local server, usually on `http://127.0.0.1:1234`.
4. In `config.json`, set:

```json
{
  "llm_cleanup_enabled": true,
  "llm_endpoint": "http://127.0.0.1:1234/v1/chat/completions",
  "llm_model": "local-model"
}
```

If the LLM request fails, the app falls back to the transcription instead of
blocking dictation.

## Models

The download script fetches KB-Whisper GGML models from KBLab. To choose a
specific size, pass `-Size`:

```powershell
.\scripts\download_model.ps1 -Size small
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

## Building

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

The executable is written to:

```text
build\bin\Release\VoiceToText.exe
```

## Packaging

There is a helper script for creating a standalone folder:

```powershell
.\scripts\package_standalone.ps1
```

The package script copies the executable, runtime DLLs, model/config files where
available, and standalone notes into `dist\VoiceToText`.

## Dependencies

CMake fetches or wires the required dependencies:

- [whisper.cpp](https://github.com/ggerganov/whisper.cpp)
- [miniaudio](https://github.com/mackron/miniaudio)
- [nlohmann/json](https://github.com/nlohmann/json)

## Repository Notes

Generated build folders such as `build` and `build-cuda` are local build
artifacts. They are not required to understand or modify the source code.

## License

MIT License. See [LICENSE](LICENSE).
