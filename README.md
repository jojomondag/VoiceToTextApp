# VoiceToText

VoiceToText is a local Windows dictation app built in C++. It records from your
microphone, transcribes Swedish speech with Pianissimo, and writes it live at
the text cursor in the app you are already using. Select Städa text or AI-språk
to transform the current dictation and continue processing new speech in that
mode with local Gemma. Original and processed versions are also available in
History.

The app is built for fast everyday dictation: start it once, leave it in the
system tray, press a hotkey, speak, and get text back in the focused window.

## Highlights

- Pianissimo Swedish with GGUF Q8 and ONNX int4, int8, fp16, and fp32 model choices.
- Live dictation into the focused text field, including spoken selection replacement.
- Optional transcript window for reviewing and copying the draft.
- Rösttext, Städa text, and AI-språk buttons with explanatory hover tooltips.
- Internal Gemma for background proofreading and compact instruction rewriting.
- Vulkan GPU acceleration for GGUF and portable CPU speech runtimes.
- Microphone capture with voice activity detection and silence trimming.
- Optional local LLM cleanup and profile-based rewriting.
- Transcript history with raw, cleaned, and AI Language views.
- System tray controls and a floating status overlay.

## Quick Start

Clone the repository:

```powershell
git clone https://github.com/jojomondag/VoiceToTextApp.git
cd VoiceToTextApp
```

Download the default official Pianissimo ONNX int8 model:

```powershell
.\scripts\download_model.ps1
```

Continue with [Setup And Configuration](docs/SETUP_AND_CONFIGURATION.md) for
build, run, packaging, and configuration details.

## Installers and Releases

Tagged GitHub releases build a Windows installer automatically. Download
`VoiceToText-Windows-Setup.exe` from the repository's Releases page and run it;
the app installs per-user and downloads its speech model on first use.

The application uses Win32 APIs and currently supports Windows only.

VoiceToText runs in the Windows notification area rather than as a normal
taskbar window. If Windows has collapsed notification icons, open the hidden
icons menu near the clock and enable VoiceToText under taskbar settings.

![VoiceToText MarketGraphic](assets/MarketGraphic.png)

## Usage

Default hotkeys:

| Shortcut | Action |
| --- | --- |
| `Ctrl+Shift+Space` | Start or stop recording |
| `Escape` | Stop recording; text already written stays in place |

Typical flow:

1. Put the cursor in the text field where you want to write.
2. Press `Ctrl+Shift+Space`.
3. Speak and watch the words appear there. Select a passage in that field and
   keep speaking to replace it. Move the cursor to continue somewhere else.
4. Click a mode on the floating overlay to transform the current dictation and
   keep using that mode as you speak; hover over its icons for explanations.
5. Press `Ctrl+Shift+Space` again to finish the remaining speech and selected
   processing before saving History.

| Live mode | Result |
| --- | --- |
| **Rösttext** | Restores the original transcript of the current unmodified dictation span when safe replacement is supported, then continues with original speech. |
| **Städa text** | Proofreads the current dictation and new speech for spelling, punctuation, capitalization, and grammar. |
| **AI-språk** | Builds a ready-to-send AI prompt from the current dictation and new speech, organizing the supplied goal, tasks, and requirements. |

The selected mode stays active while you speak. Stable original words appear at
the current cursor first; in Städa text or AI-språk, Gemma then updates the
verified active dictation span in the background. New words can therefore appear
briefly as original speech before processing catches up. Words spoken while a
result is being prepared are preserved and included in the next update. Switch
to another mode at any time; transformations use the original transcript so
repeated changes do not accumulate rewrites.

The overlay shows **AI bearbetar...** while any part of the current text is still
waiting for AI, and **AI klar** only after the whole current span is processed
and its editor write is confirmed. Wait for that indication before selecting
or copying the text. Stop also flushes short remaining phrases. Selecting or
editing the text cancels late replacements and shows **Texten behålls**.

Live proofreading collects a phrase or sentence before submitting it. Automatic
AI-språk updates process completed sentences and retain unfinished speech as a
raw tail until it is completed. Stop or an explicit mode click processes the
whole current source, including unfinished phrases. Both modes retain the
complete original speech in the active span as new words arrive. AI-språk
aims to make the user's intent actionable in one attempt: it organizes relevant
context, tasks, constraints, and the requested result, without adding missing
requirements. Short requests stay short. Unclear or unfinished parts are retained
as quotations that need clarification instead of being completed by guessing.
AI-språk uses paragraphs and numbered requirements with short bold Markdown
labels. Related subpoints use four-space indentation. Simple requests stay
compact. Live insertion preserves that layout without sending
Enter or Tab keys. Browser fields use a temporary text paste; the previous
clipboard is restored after the write is confirmed, unless you have copied
something newer. If its contents cannot be preserved, the current text stays
unchanged. Both AI versions use the original
transcript, so proofreading guesses do not become prompt requirements. Delayed
editor acknowledgements keep new speech queued until the earlier write is verified.

Processing is limited to a current span of up to 3000 characters. Moving the
cursor, selecting text, typing, or changing fields ends ownership of the old
span and cancels any pending replacement. New speech follows the current cursor
and replaces an existing user selection like ordinary typing.

Some apps do not expose enough editor information for safe replacement. Speech
still writes original words directly in those fields. Clicking a mode button
leaves the written text intact and explains that a safe switch is unavailable.
Processed versions remain available in History after the session.

Choosing Städa text or AI-språk enables AI processing. A configured, available
language model is still required. The prepared local test build uses internal
Gemma E4B. AI-språk allows the model to reason before returning the prompt; only
the final text is inserted. This can take a few seconds while speech continues.
Proofreading keeps the shorter, non-reasoning request. If processing fails, the
speech transcript is kept. Review rewritten text when wording matters; the model
can still misinterpret ambiguous speech or fail to flag an unclear fragment.
Stopping finishes the remaining speech and its selected processing before
saving History. `Escape` cancels further dictation and pending processing,
leaving text already written in place.

The separate dictation window stays closed unless opened from **Live dictation**
in the tray. It provides an original draft to review or copy when needed. Gemma
cleanup, editable History profiles, and AI Language results remain available in
History alongside the live mode behavior.

Local dictation diagnostics are enabled to help investigate cursor and insertion
problems. `diagnostics/dictation.jsonl`, next to `config.json`, contains dictated
text and insertion/cursor events, including `mode_switch_requested`,
`mode_switch_completed`, `revision_applied`, and `revision_skipped` for live
mode changes and updates. It stays on this computer and is limited to 2 MiB, plus one 2 MiB
previous file. Set `dictation_debug_logging` to `false` in
`config.json` and restart to stop logging. See
[diagnostics details](docs/SETUP_AND_CONFIGURATION.md#local-dictation-diagnostics).

You can also use the tray menu to start recording, reopen Live dictation,
change the Pianissimo model, choose a Gemma cleanup model, change compute mode,
open History, or exit. Pianissimo speech recognition supports Swedish.

## History And Profiles

The History window keeps recent dictations and shows:

- Recorded voice: the dictation transcript before optional cleanup.
- Clean transcript: text after cleanup/profile transformation.
- AI Language: compact AI-readable version for prompt-heavy workflows.

Each history row includes copy buttons and profile controls. Clicking the
profile text or profile icon switches the History window into the profile
editor. From there you can edit how each profile transforms cleaned text,
create new profiles, rename profiles, and go back to history.

History is stored in `transcript_history.json` next to the executable unless
configured otherwise.

## Setup And Configuration

See [Setup And Configuration](docs/SETUP_AND_CONFIGURATION.md) for requirements,
model downloads, build commands, packaging, configuration settings, and local
LLM setup.

## Repository Notes

Generated build folders such as `build` and `build-cuda` are local build
artifacts. They are not required to understand or modify the source code.

## License

MIT License. See [LICENSE](LICENSE).
