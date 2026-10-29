# Setup And Configuration

This page contains the detailed setup, build, model, packaging, and
configuration notes for VoiceToText.

## Requirements

- Windows 10/11, 64-bit.
- Visual Studio 2022 or Build Tools with the C++ workload.
- CMake 3.20 or newer.
- A microphone.
- Optional: a GPU with a compatible Vulkan driver for GGUF speech acceleration.

Portable CPU backends and the CPU ONNX Runtime are bundled. GGUF speech uses
Vulkan when selected and available; ONNX variants currently run on CPU.

## Models

Download the default official Pianissimo ONNX int8 model:

```powershell
.\scripts\download_model.ps1
```

Choose a specific model with `-Model`:

```powershell
.\scripts\download_model.ps1 -Model gguf-q8
.\scripts\download_model.ps1 -Model onnx-int4
.\scripts\download_model.ps1 -Model onnx-int8
.\scripts\download_model.ps1 -Model onnx-fp16
.\scripts\download_model.ps1 -Model onnx-fp32
```

| Model ID | Approx. download | Runtime |
| --- | ---: | --- |
| `gguf-q8` | 740 MB | Optional; Vulkan GPU or CPU. |
| `onnx-int4` | 515 MB | Smallest download; CPU. |
| `onnx-int8` | 660 MB | Default; official CPU variant. |
| `onnx-fp16` | 1.28 GB | Higher precision; CPU. |
| `onnx-fp32` | 2.56 GB | Full precision; CPU. |

Higher precision does not guarantee better transcription. Download size is
different from runtime memory use. These are Swedish speech models.

The tray's **Pianissimo Model** submenu selects a variant. Its missing files are
downloaded and verified before use. Files are cached under `models/Pianissimo`;
GGUF lives directly there and ONNX variants share `PianissimoOnnx` inside it.
The fp32 encoder requires both `encoder-model.onnx` and
`encoder-model.onnx.data`.

The downloader and app verify model size and SHA-256, reuse valid cached files,
and promote temporary downloads only after verification. ONNX downloads use
the pinned revision in the model catalog. Verify an existing cache without
downloading:

```powershell
.\scripts\download_model.ps1 -Model onnx-int8 -VerifyOnly
```

An existing KB-Whisper configuration migrates to Pianissimo ONNX int8 automatically.
The previous `.bin` files remain on disk and are not loaded as Pianissimo.
The `model_id` and `model_dir` settings replace legacy model-size selection.

Model sources:

- [Pianissimo Swedish](https://huggingface.co/KlangAI/pianissimo-sv)
- [Official Pianissimo ONNX variants](https://huggingface.co/KlangAI/pianissimo-sv-onnx)
- [Pianissimo GGUF Q8](https://huggingface.co/nub235/pianissimo-sv-gguf)

## Internal Gemma Models

VoiceToText can run Gemma internally for cleanup, profile transforms, and AI
Language output, including live background processing of dictation. This uses
a local `llama-server.exe` process and an OpenAI-compatible endpoint inside the
app. Inference requests disable thinking output and request only the processed
text. Model startup and live processing run in the background.

The currently prepared local test build is configured for internal Gemma E4B
with AI processing enabled. This is separate from the generic example
configuration and the hardware-based model recommendation.

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

Build the app with the bundled speech runtimes:

```powershell
cmake -B build
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

The app starts in the system tray by default. Place the cursor in the text field
where you want to write, then press `Ctrl+Shift+Space`. Speech appears directly
there while you talk. Select text in that field to dictate its replacement, or
move the cursor to continue elsewhere. The separate dictation window does not
open automatically; the tray's **Live dictation** item opens its draft view.
Preview updates reprocess growing audio windows, so the current phrase can
change until it is finalized. A pause of about 600 ms finalizes the phrase;
silence is handled automatically between phrases.
Words that agree across consecutive previews are written incrementally at the
active cursor; Stop flushes the remaining phrase. Recognition preview changes
do not rewrite previously inserted words. In Städa text or AI-språk, Gemma then
transforms the app's verified, unedited dictation span in the background and
continues processing new speech in the selected mode.
Selecting text, moving the cursor, or typing directs new speech to the current
position and prevents delayed AI results from changing the old span. The next
words replace a selection just as ordinary typing would. The optional dictation
window can still revise its own original draft as recognition improves.

**Stop** or the recording hotkey finishes the remaining speech and waits for its
selected processing, without pasting the whole transcript again. It then saves
the original and processed versions in History. `Escape` stops further speech input and cancels outstanding live
processing; text already written remains in place.
The draft is also available in the optional dictation window for review or
copying. Original, cleaned/profile-transformed, and AI Language versions are
saved in History.

## Live Language Modes

The floating overlay has three mode buttons. Hover over an icon to see its
tooltip. These buttons control the external text field where you are dictating:

| Mode | Behavior |
| --- | --- |
| **Rösttext** (`recorded`) | Restores the original of the current unmodified dictation span when replacement can be verified and continues with original speech. |
| **Städa text** (`clean`) | Proofreads the current dictation and new speech for spelling, punctuation, capitalization, and grammar. |
| **AI-språk** (`ai`) | Builds an actionable AI prompt from the current dictation and new speech using `llm_ai_language_prompt`. |

Clicking Städa text or AI-språk enables `llm_cleanup_enabled` and
`llm_ai_language_enabled` and saves the selected mode. A working configured LLM
is required; see [Local LLM Cleanup](#local-llm-cleanup). Live proofreading uses
a conservative proofreading prompt, while the editable intent profiles are
used for the versions saved in History. AI text can change wording, so review
its output when exact phrasing matters.

The selected mode remains active for subsequent speech. All modes first write
stable original speech at the current cursor. In Städa text or AI-språk, Gemma
then processes the current unmodified dictation span and updates it when the
text and caret still match what the app last wrote. New words may briefly appear
as original speech before the selected transformation is ready.

Background proofreading collects a phrase or sentence instead of processing
isolated words. Automatic AI updates submit only completed sentence prefixes;
the unfinished tail remains visible as original speech until the next sentence
boundary. Stop and explicit mode clicks process the entire current source.
Before submission, new words coalesce into the latest original
source; after submission, that exact prefix stays fixed and later words are
included in the next update. Briefly delayed editor readback does not discard
this source. Stop and explicit mode clicks also process shorter remaining text.

AI-språk organizes the supplied goal, tasks, relevant context, constraints, and
requested output into a prompt intended to help an AI complete the request in
one attempt. It keeps simple requests short, preserves facts and named entities,
and quotes unclear or unfinished fragments for clarification without inventing
their meaning. It does not carry out or answer the dictated request. Useful
sections have blank lines between them, independent requirements use numbered
lines with short bold Markdown labels, and related subpoints use four-space
indentation. Simple requests stay compact. In a plain text field, the Markdown
markers remain visible; a Markdown preview renders them as bold text.
Live output and History preserve that layout. Native text fields receive text
directly; browser fields receive multiline text through a temporary clipboard
paste, without Enter or Tab keypresses. The previous clipboard formats are
restored only after the write is confirmed and only if no newer copy exists.
If the app exits while a paste is still unconfirmed, the AI text remains on the
clipboard so a delayed paste cannot insert unrelated previous clipboard data.
Unreadable, unsupported, or oversized clipboard contents leave the current
dictation unchanged instead of risking their loss. The explicit keystrokes
method for manual History insertion still flattens line breaks for safety.
Previous shipped AI prompts migrate automatically on load; customized prompts
are preserved. Truncated output and recognizable invented placeholder notation
are rejected, leaving the existing speech text intact. Both live and History AI
versions use the original speech, independently of proofreading results, with the
same configured AI prompt. AI rewriting avoids automatically learned phrase
substitutions: those may encode edits that only make sense in an earlier
sentence. Learned corrections remain available to proofreading.

Internal Gemma uses reasoning for AI prompt construction, with the configured
token budget (up to 4096 for live prompts) and timeout (up to 30 seconds). The
reasoning is not inserted or stored as the AI text. Proofreading and warmup keep
their shorter non-reasoning requests. AI rewrites may therefore take a few
seconds while original words continue to appear. Ambiguous speech can still be
misinterpreted or left unmarked; review the prompt before using it.

Clicking a mode transforms the current owned dictation immediately or starts
processing it if a result is not yet ready. Words spoken during processing are
preserved as a suffix and included in the next update automatically. The app
keeps the original speech separately from the visible version, so switching
modes always uses that source. A newer mode selection supersedes pending
results from the previous mode. Selecting a mode before speaking also applies
that mode to the upcoming dictation.

The active processing window is limited to 3000 characters to bound local
inference and replacement work. Earlier text is left in place. Moving the
caret, selecting text, manually editing, or changing fields revokes permission
to rewrite the old insertion span and cancels pending updates for that span.
The next spoken words follow the new caret and continue in the selected mode;
the app does not rewrite user edits or the surrounding document. Stop finishes
the remaining speech and selected processing. Escape cancels pending updates
and preserves the visible text.

Some editors do not expose readable text and selection through Windows. In
those fields the app cannot safely change text it has already inserted. All
modes continue writing stable original speech directly. Clicking a mode leaves
that written text intact and reports that a safe replacement is unavailable.
Processed versions can be copied from History after the session. There is no
blind selection or delayed processed-chunk insertion into those fields.

If processing fails or a replacement cannot be verified, the written version
is preserved rather than applying an unconfirmed rewrite. Editor capabilities
determine whether the selected mode can safely replace the active span; replacement
is not available in every app.

## Packaging

Create a standalone folder:

```powershell
.\scripts\package_standalone.ps1
# Or package a specific selected variant:
.\scripts\package_standalone.ps1 -Model onnx-int8
```

The package script copies the executable, all portable CPU backends, Vulkan and
ONNX runtime DLLs, licenses, resources, and a matching configuration into
`dist\VoiceToText`. It includes a complete, checksum-verified cache of the
selected Pianissimo variant when available, plus available Gemma models and
llama.cpp tools. Partial speech caches and old Whisper `.bin` models are
excluded. Without a cached speech model, first use downloads the selected
variant. CUDA Toolkit is not needed for speech recognition.

## Configuration File

On first run, the app creates `config.json` next to the executable. The full
example lives in [../config.example.json](../config.example.json).

Common settings:

| Setting | Meaning |
| --- | --- |
| `model_id` | `gguf-q8`, `onnx-int4`, `onnx-int8`, `onnx-fp16`, or `onnx-fp32`. Default `onnx-int8`. |
| `model_dir` | Speech cache root; default `models/Pianissimo`. ONNX files use its `PianissimoOnnx` subfolder. |
| `model_path` | Compatibility alias for the GGUF file; select models with `model_id`. |
| `language` | `sv`; Pianissimo speech recognition supports Swedish. |
| `use_gpu` | Enables Vulkan GPU use for GGUF when available. |
| `compute_mode` | `auto`, `cpu`, or `gpu`; ONNX uses CPU. |
| `n_threads` | CPU inference threads. `0` means auto. |
| `whisper_fast_mode` | Legacy compatibility setting; no effect with Pianissimo. |
| `warm_up_model_on_load` | Runs a small warmup after loading the model. |
| `audio_device` | Optional microphone device index as a string. Empty means Windows default. |
| `vad_threshold` | Voice activity sensitivity. Lower is more sensitive. |
| `trim_silence_before_transcription` | Legacy compatibility setting; live dictation handles silence automatically. |
| `trim_silence_padding_ms` | Legacy compatibility setting; no effect with live Pianissimo dictation. |
| `toggle_key` | Virtual-key code for the recording hotkey key. |
| `cancel_key` | Virtual-key code to stop further dictation; text already written stays in place. |
| `injection_method` | `auto`, `clipboard`, or `keystrokes`. |
| `paste_output_mode` | Active live mode: `recorded` (Rösttext), `clean` (Städa text), or `ai` (AI-språk). The selected mode transforms the current owned dictation and subsequent speech. Also chooses the optional manual insertion version. |
| `dictation_debug_logging` | Local dictation/insertion JSONL diagnostics. Default `true`; set `false` and restart to disable. Contains dictated text. |
| `llm_cleanup_enabled` | Enables LLM processing; choosing Städa text or AI-språk enables it automatically. |
| `llm_provider` | `internal` for Gemma, or `lmstudio` for an external server. |
| `llm_endpoint` | OpenAI-compatible chat completions endpoint. |
| `llm_model` | Model name sent to the LLM endpoint. |
| `llm_internal_model` | `auto`, `e2b`, `e4b`, `12b`, `26b-a4b`, or `31b`. |
| `llm_internal_model_path` | Gemma GGUF path used by internal mode. |
| `llm_internal_server_path` | Path to `llama-server.exe`. |
| `llm_internal_port` | Local port for the internal OpenAI-compatible server. |
| `llm_max_tokens` | Dynamic output-token ceiling; short dictation uses less, while long dictation can scale up to this value. Default `4096`. |
| `llm_ai_language_enabled` | Enables the AI Language History version; choosing an AI live mode enables this setting. |
| `llm_ai_language_prompt` | Instructions for organizing speech into a faithful, actionable AI prompt. |
| `llm_profiles` | Editable intent profiles and their instructions. |
| `show_overlay` | Shows the floating status overlay. |
| `transcript_history_path` | History JSON path. |
| `transcript_history_max_entries` | Number of history entries to keep. |

## Local Dictation Diagnostics

Dictation diagnostics are enabled by default to help troubleshoot text appearing
at the wrong cursor or replacing a selection. The log is stored in the
`diagnostics` folder beside the active `config.json`:

```text
diagnostics\dictation.jsonl
diagnostics\dictation.previous.jsonl
```

Each line is JSON with a UTC timestamp, logger session ID, sequence number,
event name, and event fields. Events describe recognition updates, capture
start/stop, attempted text insertion, selection/cursor metadata, and insertion
results where the editor exposes them. `mode_switch_requested` records an
explicit button click, mode, and source length. `mode_switch_completed`
records whether a requested mode update was applied. `revision_applied` records a verified
replacement and the preserved raw suffix length, while `revision_skipped`
explains a rejected operation, such as a changed caret or unavailable exact
range. In a selected processing mode, background results can update the owned span.
Text fields contain recognized speech
or the app's own inserted text. The logger does not collect surrounding document
contents, window titles, or password-field contents. Files stay on this
computer and are not transmitted automatically.

The current file is capped at 2 MiB. Rotation keeps one previous file of up to
2 MiB, for a maximum of 4 MiB. Individual strings are limited to 4096 UTF-8
bytes; large or deeply nested records are shortened. Each record is flushed
immediately, and a logging failure does not interrupt dictation.

To disable new records, set `"dictation_debug_logging": false` in `config.json`
and restart the app. Existing files are retained until you delete them. If
reporting a problem, include the approximate local time, what you selected or
where you moved the cursor, and the relevant event lines. Review dictated text
before sharing a log.

## Local LLM Cleanup

VoiceToText processes active dictation in the selected mode and creates cleaned
versions in History. Live updates follow the safety and editor capability rules in
[Live Language Modes](#live-language-modes); manually edited text is not eligible
for replacement.

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

The prepared local test build uses `"llm_internal_model": "e4b"` with its
downloaded GGUF path. The `auto` example above selects according to the app's
hardware recommendation. Mode buttons enable processing but do not install or
replace a missing model by themselves.

If the LLM request fails, the speech transcript is preserved for live output and
remains available in History. Incomplete or token-truncated responses are not
used to replace the active dictation.

## Implementation reference: TeachersLittleHelper

The integration follows the actual TeachersLittleHelper source:

| TeachersLittleHelper source | VoiceToText implementation |
| --- | --- |
| `SpeechModelProvisioning.cs` | `speech_model_catalog.cpp`: the five variants, pinned ONNX revision, SHA-256 validation and atomic downloads. |
| `PianissimoOnnxModel.cs` | `pianissimo_engine.cpp`: NeMo preprocessing, encoder and TDT decoder through ONNX Runtime. |
| `TranscriptionService.cs` dictation loop | `dictation_stream.cpp`: first preview after 400 ms, 320 ms refresh steps, eight-second windows and timestamp-filtered overlap. |
| `DictationTextInsertion.cs` and `DictationButtonBase.cs` | `external_dictation_target.cpp`: append stable raw speech at the active caret and replace user selections. `mode_switch_policy.cpp` keeps the selected processing mode active for new speech; `external_dictation_target.cpp` verifies the unmodified span and preserves later raw speech. Opaque editors retain raw text. `live_mode_processor.cpp` processes the selected version in the background; `dictation_window.cpp` retains the optional original draft view. |

The native app uses the same transcribe.cpp provider for GGUF and ONNX Runtime
for the official variants, without requiring .NET. This port ends phrases after
approximately 600 ms of low audio energy; TeachersLittleHelper uses its separate
VAD service. Only the remaining tail is finalized on Stop. The transcript enters
the cleanup/history pipeline without pasting a second copy over the live text.

ONNX int8 is the default here because the optional GGUF download returned
HTTP 401 during integration verification on 2026-10-15. Explicit model choices
never silently switch to a different speech model.

## Dependencies

CMake fetches or wires the required dependencies:

- [transcribe.cpp](https://github.com/handy-computer/transcribe.cpp) GGUF speech runtime, matching TeachersLittleHelper
- [ONNX Runtime](https://github.com/microsoft/onnxruntime)
- [miniaudio](https://github.com/mackron/miniaudio)
- [nlohmann/json](https://github.com/nlohmann/json)
