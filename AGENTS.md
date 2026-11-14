# VoiceToText project instructions

## Building

Linux (the build the user runs daily), from the repository root:

```bash
cmake -S . -B build -G Ninja
cmake --build build
cmake --install build --component app --prefix ~/.local
```

The install step puts the program in `~/.local/lib/voicetotext`, links it as
`~/.local/bin/VoiceToText` and adds the launcher entry. Quit the running app
first (`VoiceToText --quit`) and start it again afterwards.

Windows:

```powershell
cmake -S . -B build
cmake --build build --config Release --target VoiceToText
```

Verify that the build succeeds before reporting completion. If it fails or the
executable is locked, report the actual problem.

## Layout

- `src/` holds the shared code and the Win32 UI. Each Win32 UI header starts
  with `#ifndef _WIN32 #include "linux/<same name>.h"`; the Linux (Qt 6,
  Wayland, Hyprland) counterparts live in `src/linux/`.
- `src/app.cpp` keeps its Win32 shape on both platforms; on Linux it runs on
  `src/linux/win_compat.{h,cpp}`.
- `tests/CMakeLists.txt` builds the Windows regression tests;
  `tests/linux_speech_smoke/` is a standalone Linux check of the speech engine.

A change to shared code must still compile on the other platform.
